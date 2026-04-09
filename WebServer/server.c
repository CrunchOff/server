/*
 * server.c — Serveur WebSocket (port unique 8080)
 *
 * Compile : gcc -o server server.c -lpthread -lssl -lcrypto
 * Lancer  : ./server
 *
 * Routes HTTP :
 *   GET  /          → page d'accueil avec instructions
 *   GET  /dl/<file> → téléchargement depuis ./dist/
 *   GET  /ws        → upgrade WebSocket
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdarg.h>
#include <dirent.h>
/* SHA1 + Base64 intégrés — pas besoin de libssl */

/* ── SHA1 minimal (RFC 3174) ─────────────────────────────────────────────── */
#define SHA1_ROTL(x,n) (((x)<<(n))|((x)>>(32-(n))))
typedef struct { uint32_t state[5]; uint64_t count; uint8_t buf[64]; } SHA1_CTX;
static void sha1_transform(SHA1_CTX *c) {
    uint32_t W[80], a,b,cc,d,e,t,i;
    for(i=0;i<16;i++) W[i]=((uint32_t)c->buf[i*4]<<24)|((uint32_t)c->buf[i*4+1]<<16)|((uint32_t)c->buf[i*4+2]<<8)|c->buf[i*4+3];
    for(i=16;i<80;i++) W[i]=SHA1_ROTL(W[i-3]^W[i-8]^W[i-14]^W[i-16],1);
    a=c->state[0];b=c->state[1];cc=c->state[2];d=c->state[3];e=c->state[4];
    for(i=0;i<80;i++){
        if(i<20)      t=SHA1_ROTL(a,5)+((b&cc)|(~b&d))+e+W[i]+0x5A827999;
        else if(i<40) t=SHA1_ROTL(a,5)+(b^cc^d)+e+W[i]+0x6ED9EBA1;
        else if(i<60) t=SHA1_ROTL(a,5)+((b&cc)|(b&d)|(cc&d))+e+W[i]+0x8F1BBCDC;
        else          t=SHA1_ROTL(a,5)+(b^cc^d)+e+W[i]+0xCA62C1D6;
        e=d;d=cc;cc=SHA1_ROTL(b,30);b=a;a=t;
    }
    c->state[0]+=a;c->state[1]+=b;c->state[2]+=cc;c->state[3]+=d;c->state[4]+=e;
}
static void sha1_init(SHA1_CTX *c){
    c->state[0]=0x67452301;c->state[1]=0xEFCDAB89;c->state[2]=0x98BADCFE;
    c->state[3]=0x10325476;c->state[4]=0xC3D2E1F0;c->count=0;
}
static void sha1_update(SHA1_CTX *c,const uint8_t *d,size_t l){
    size_t i;
    for(i=0;i<l;i++){
        c->buf[c->count%64]=d[i];
        if((++c->count)%64==0) sha1_transform(c);
    }
}
static void sha1_final(SHA1_CTX *c,uint8_t *out){
    uint64_t bc=c->count*8; uint8_t pad=0x80;
    sha1_update(c,&pad,1);
    pad=0; while(c->count%64!=56) sha1_update(c,&pad,1);
    uint8_t len[8];
    for(int i=7;i>=0;i--){len[i]=(uint8_t)(bc&0xFF);bc>>=8;}
    sha1_update(c,len,8);
    for(int i=0;i<5;i++){out[i*4]=(c->state[i]>>24)&0xFF;out[i*4+1]=(c->state[i]>>16)&0xFF;out[i*4+2]=(c->state[i]>>8)&0xFF;out[i*4+3]=c->state[i]&0xFF;}
}
static void SHA1(const unsigned char *d,size_t l,unsigned char *out){SHA1_CTX c;sha1_init(&c);sha1_update(&c,d,l);sha1_final(&c,out);}

/* ── Base64 minimal ──────────────────────────────────────────────────────── */
static const char B64[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void base64_encode(const unsigned char *in,int inlen,char *out,int outlen){
    int i=0,o=0;
    for(;i<inlen&&o<outlen-4;i+=3){
        uint32_t v=((uint32_t)in[i]<<16)|((i+1<inlen)?(uint32_t)in[i+1]<<8:0)|((i+2<inlen)?(uint32_t)in[i+2]:0);
        out[o++]=B64[(v>>18)&63];out[o++]=B64[(v>>12)&63];
        out[o++]=(i+1<inlen)?B64[(v>>6)&63]:'=';
        out[o++]=(i+2<inlen)?B64[v&63]:'=';
    }
    out[o]='\0';
}
/* ─────────────────────────────────────────────────────────────────────────── */


#include "protocol.h"

#define SERVER_VERSION  "2.0"
#define DIST_DIR        "./dist"
#define HEARTBEAT_TTL   45

// ── Structure client WebSocket ─────────────────────────────────────────────────
typedef struct {
    int         fd;
    char        ip[MAX_IP_LEN];
    char        name[MAX_NAME_LEN];
    int         registered;
    uint32_t    last_seen;
    pthread_t   thread;
} WSClient;

static WSClient        g_clients[MAX_CLIENTS];
static int             g_client_count = 0;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

// ══════════════════════════════════════════════════════════════════════════════
//  UTILITAIRES
// ══════════════════════════════════════════════════════════════════════════════

static uint32_t now_sec(void) { return (uint32_t)time(NULL); }

static void logf_msg(const char *tag, const char *fmt, ...) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    printf("[%02d:%02d:%02d] [%s] ", tm->tm_hour, tm->tm_min, tm->tm_sec, tag);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

// JSON minimal — extrait la valeur d'une clé string
// ex: json_get("{"name":"Bob","x":1}", "name", buf, sizeof(buf))
static int json_get(const char *json, const char *key, char *out, int outlen) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ' || *p == ':') p++;
    if (*p == '"') {
        p++;
        int i = 0;
        while (*p && *p != '"' && i < outlen - 1) out[i++] = *p++;
        out[i] = '\0';
        return 1;
    }
    return 0;
}



// ══════════════════════════════════════════════════════════════════════════════
//  BASE64 (pour le handshake WebSocket)
// ══════════════════════════════════════════════════════════════════════════════



// ══════════════════════════════════════════════════════════════════════════════
//  WEBSOCKET — FRAMING (RFC 6455)
// ══════════════════════════════════════════════════════════════════════════════

// Envoie un frame WebSocket texte
static int ws_send_text(int fd, const char *data, size_t len) {
    uint8_t header[10];
    int hlen = 0;
    header[hlen++] = 0x81; // FIN + opcode TEXT
    if (len <= 125) {
        header[hlen++] = (uint8_t)len;
    } else if (len <= 65535) {
        header[hlen++] = 126;
        header[hlen++] = (len >> 8) & 0xFF;
        header[hlen++] = len & 0xFF;
    } else {
        header[hlen++] = 127;
        for (int i = 7; i >= 0; i--)
            header[hlen++] = (len >> (i * 8)) & 0xFF;
    }
    if (send(fd, header, hlen, MSG_NOSIGNAL) < 0) return -1;
    if (send(fd, data, len, MSG_NOSIGNAL) < 0) return -1;
    return 0;
}

// Envoie un frame WebSocket binaire
static int ws_send_binary(int fd, const uint8_t *data, size_t len) {
    uint8_t header[10];
    int hlen = 0;
    header[hlen++] = 0x82; // FIN + opcode BINARY
    if (len <= 125) {
        header[hlen++] = (uint8_t)len;
    } else if (len <= 65535) {
        header[hlen++] = 126;
        header[hlen++] = (len >> 8) & 0xFF;
        header[hlen++] = len & 0xFF;
    } else {
        header[hlen++] = 127;
        for (int i = 7; i >= 0; i--)
            header[hlen++] = (len >> (i * 8)) & 0xFF;
    }
    if (send(fd, header, hlen, MSG_NOSIGNAL) < 0) return -1;
    if (send(fd, data, len, MSG_NOSIGNAL) < 0) return -1;
    return 0;
}

// Lit un frame WebSocket, retourne la taille du payload (-1 = erreur/close)
// Démasque automatiquement si le frame est masqué (client→serveur)
static ssize_t ws_recv_frame(int fd, uint8_t *buf, size_t bufsize, int *opcode) {
    uint8_t h[2];
    if (recv(fd, h, 2, MSG_WAITALL) != 2) return -1;

    *opcode = h[0] & 0x0F;
    int masked = (h[1] & 0x80) != 0;
    uint64_t payload_len = h[1] & 0x7F;

    if (payload_len == 126) {
        uint8_t ext[2];
        if (recv(fd, ext, 2, MSG_WAITALL) != 2) return -1;
        payload_len = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (payload_len == 127) {
        uint8_t ext[8];
        if (recv(fd, ext, 8, MSG_WAITALL) != 8) return -1;
        payload_len = 0;
        for (int i = 0; i < 8; i++)
            payload_len = (payload_len << 8) | ext[i];
    }

    uint8_t mask[4] = {0};
    if (masked) {
        if (recv(fd, mask, 4, MSG_WAITALL) != 4) return -1;
    }

    if (payload_len > bufsize) return -1;

    ssize_t got = 0;
    while ((size_t)got < payload_len) {
        ssize_t n = recv(fd, buf + got, payload_len - got, 0);
        if (n <= 0) return -1;
        got += n;
    }

    if (masked) {
        for (uint64_t i = 0; i < payload_len; i++)
            buf[i] ^= mask[i % 4];
    }

    return (ssize_t)payload_len;
}

// ══════════════════════════════════════════════════════════════════════════════
//  WEBSOCKET — HANDSHAKE
// ══════════════════════════════════════════════════════════════════════════════

static const char *WS_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

static int ws_handshake(int fd, const char *request) {
    // Extrait Sec-WebSocket-Key
    const char *key_header = strstr(request, "Sec-WebSocket-Key:");
    if (!key_header) return -1;
    key_header += 18;
    while (*key_header == ' ') key_header++;
    char ws_key[64] = {0};
    int i = 0;
    while (*key_header && *key_header != '\r' && *key_header != '\n' && i < 63)
        ws_key[i++] = *key_header++;
    ws_key[i] = '\0';

    // Calcule l'accept key : SHA1(key + magic) en base64
    char combined[128];
    snprintf(combined, sizeof(combined), "%s%s", ws_key, WS_MAGIC);
    unsigned char sha1[20];
    SHA1((unsigned char *)combined, strlen(combined), sha1);
    char accept[64];
    base64_encode(sha1, 20, accept, sizeof(accept));

    // Envoie la réponse
    char response[512];
    snprintf(response, sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n", accept);

    return send(fd, response, strlen(response), 0) > 0 ? 0 : -1;
}

// ══════════════════════════════════════════════════════════════════════════════
//  REGISTRE — gestion des clients
// ══════════════════════════════════════════════════════════════════════════════

static void broadcast_json(const char *json, int exclude_fd) {
    pthread_mutex_lock(&g_mutex);
    for (int i = 0; i < g_client_count; i++) {
        if (g_clients[i].fd != exclude_fd && g_clients[i].registered)
            ws_send_text(g_clients[i].fd, json, strlen(json));
    }
    pthread_mutex_unlock(&g_mutex);
}

static void send_client_list(int fd) {
    char buf[MAX_CLIENTS * 80 + 64];
    int n = snprintf(buf, sizeof(buf), "{\"type\":\"clients\",\"list\":[");
    pthread_mutex_lock(&g_mutex);
    int first = 1;
    for (int i = 0; i < g_client_count; i++) {
        if (!g_clients[i].registered) continue;
        n += snprintf(buf + n, sizeof(buf) - n,
            "%s{\"name\":\"%.31s\",\"ip\":\"%.45s\"}",
            first ? "" : ",",
            g_clients[i].name, g_clients[i].ip);
        first = 0;
    }
    pthread_mutex_unlock(&g_mutex);
    n += snprintf(buf + n, sizeof(buf) - n, "]}");
    ws_send_text(fd, buf, n);
}

static WSClient *find_client_by_ip(const char *ip) {
    for (int i = 0; i < g_client_count; i++)
        if (strcmp(g_clients[i].ip, ip) == 0) return &g_clients[i];
    return NULL;
}

static void remove_client(int fd) {
    pthread_mutex_lock(&g_mutex);
    for (int i = 0; i < g_client_count; i++) {
        if (g_clients[i].fd == fd) {
            // Notifie les autres si le client était enregistré
            if (g_clients[i].registered) {
                char notif[128];
                snprintf(notif, sizeof(notif),
                    "{\"type\":\"notify\",\"event\":\"leave\",\"name\":\"%.31s\",\"ip\":\"%.45s\"}",
                    g_clients[i].name, g_clients[i].ip);
                pthread_mutex_unlock(&g_mutex);
                broadcast_json(notif, fd);
                pthread_mutex_lock(&g_mutex);
                logf_msg("REGISTRY", "Déconnecté : %s (%s)", g_clients[i].name, g_clients[i].ip);
            }
            close(g_clients[i].fd);
            g_clients[i] = g_clients[g_client_count - 1];
            g_client_count--;
            break;
        }
    }
    pthread_mutex_unlock(&g_mutex);
}

// ══════════════════════════════════════════════════════════════════════════════
//  THREAD PAR CLIENT WebSocket
// ══════════════════════════════════════════════════════════════════════════════

static void *ws_client_thread(void *arg) {
    WSClient *cli = (WSClient *)arg;
    int fd = cli->fd;

    uint8_t *frame_buf = malloc(WS_BUF_SIZE);
    if (!frame_buf) { remove_client(fd); return NULL; }

    // Timeout de réception
    struct timeval tv = {60, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (1) {
        int opcode = 0;
        ssize_t len = ws_recv_frame(fd, frame_buf, WS_BUF_SIZE, &opcode);

        if (len < 0 || opcode == 8) break; // erreur ou close frame

        cli->last_seen = now_sec();

        if (opcode == 9) {
            // Ping → Pong
            uint8_t pong[2] = {0x8A, 0x00};
            send(fd, pong, 2, MSG_NOSIGNAL);
            continue;
        }

        if (opcode == 1) {
            // Frame TEXT — message JSON
            frame_buf[len] = '\0';
            char *json = (char *)frame_buf;

            char type[32] = {0};
            json_get(json, "type", type, sizeof(type));

            if (strcmp(type, MSG_REGISTER) == 0) {
                char name[MAX_NAME_LEN] = {0};
                json_get(json, "name", name, sizeof(name));
                if (strlen(name) == 0) strncpy(name, "Anonyme", MAX_NAME_LEN - 1);

                pthread_mutex_lock(&g_mutex);
                snprintf(cli->name, MAX_NAME_LEN, "%.31s", name);
                cli->registered = 1;
                pthread_mutex_unlock(&g_mutex);

                logf_msg("REGISTRY", "Enregistré : %s (%s)", name, cli->ip);

                // Notifie tout le monde
                char notif[128];
                snprintf(notif, sizeof(notif),
                    "{\"type\":\"notify\",\"event\":\"join\",\"name\":\"%.31s\",\"ip\":\"%.45s\"}",
                    name, cli->ip);
                broadcast_json(notif, fd);

                // Envoie la liste au nouveau client
                send_client_list(fd);

            } else if (strcmp(type, MSG_LIST_REQ) == 0) {
                send_client_list(fd);

            } else if (strcmp(type, MSG_HEARTBEAT) == 0) {
                char pong[] = "{\"type\":\"pong\"}";
                ws_send_text(fd, pong, strlen(pong));

            } else if (strcmp(type, MSG_UNREGISTER) == 0) {
                break;

            } else if (strcmp(type, MSG_CHAT) == 0) {
                // Relay du message vers le destinataire
                char to_ip[MAX_IP_LEN] = {0};
                json_get(json, "to", to_ip, sizeof(to_ip));

                pthread_mutex_lock(&g_mutex);
                WSClient *dest = find_client_by_ip(to_ip);
                if (dest) ws_send_text(dest->fd, json, len);
                pthread_mutex_unlock(&g_mutex);

            } else if (strcmp(type, MSG_FILE_START) == 0 || strcmp(type, MSG_FILE_END) == 0) {
                // Relay vers destinataire
                char to_ip[MAX_IP_LEN] = {0};
                json_get(json, "to", to_ip, sizeof(to_ip));
                pthread_mutex_lock(&g_mutex);
                WSClient *dest = find_client_by_ip(to_ip);
                if (dest) ws_send_text(dest->fd, json, len);
                pthread_mutex_unlock(&g_mutex);
            }

        } else if (opcode == 2) {
            // Frame BINAIRE — chunk de fichier
            // Format : 2 octets big-endian = longueur du header JSON
            //          N octets = header JSON {"type":"file_data","to":"ip",...}
            //          reste = données binaires
            if (len < 2) continue;
            uint16_t hdr_len = ((uint16_t)frame_buf[0] << 8) | frame_buf[1];
            if ((size_t)hdr_len + 2 > (size_t)len) continue;

            char hdr_json[512] = {0};
            memcpy(hdr_json, frame_buf + 2, hdr_len < 511 ? hdr_len : 511);

            char to_ip[MAX_IP_LEN] = {0};
            json_get(hdr_json, "to", to_ip, sizeof(to_ip));

            pthread_mutex_lock(&g_mutex);
            WSClient *dest = find_client_by_ip(to_ip);
            if (dest) ws_send_binary(dest->fd, frame_buf, len);
            pthread_mutex_unlock(&g_mutex);
        }
    }

    free(frame_buf);
    remove_client(fd);
    return NULL;
}

// ══════════════════════════════════════════════════════════════════════════════
//  HTTP — page d'accueil + téléchargement
// ══════════════════════════════════════════════════════════════════════════════

static void http_serve_file(int fd, const char *filepath, const char *filename) {
    int ffd = open(filepath, O_RDONLY);
    if (ffd < 0) {
        const char *r = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nNot Found";
        send(fd, r, strlen(r), 0);
        return;
    }
    struct stat st;
    fstat(ffd, &st);

    const char *mime = "application/octet-stream";
    const char *ext = strrchr(filename, '.');
    if (ext) {
        if (!strcmp(ext, ".py"))  mime = "text/plain";
        if (!strcmp(ext, ".txt")) mime = "text/plain";
        if (!strcmp(ext, ".zip")) mime = "application/zip";
    }

    char header[512];
    snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "Connection: close\r\n\r\n",
        mime, (long)st.st_size, filename);
    send(fd, header, strlen(header), 0);

    char buf[8192];
    ssize_t n;
    while ((n = read(ffd, buf, sizeof(buf))) > 0)
        send(fd, buf, n, 0);
    close(ffd);
}

static void http_serve_index(int fd, const char *server_ip) {
    // Liste les fichiers dispo
    char files_html[4096] = "";
    int flen = 0;
    DIR *d = opendir(DIST_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            flen += snprintf(files_html + flen, sizeof(files_html) - flen,
                "<li><a href=\"/dl/%s\">%s</a></li>", e->d_name, e->d_name);
        }
        closedir(d);
    }

    char body[8192];
    int blen = snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<title>Serveur v%s</title>"
        "<style>"
        "body{font-family:monospace;background:#1a1a2e;color:#eee;padding:2em;max-width:700px;margin:auto}"
        "h1{color:#00d4ff}h2{color:#a29bfe;margin-top:1.5em}"
        "pre{background:#16213e;padding:1em;border-radius:8px;color:#00ff88;overflow:auto}"
        "a{color:#00d4ff} ul{line-height:2}"
        "</style></head><body>"
        "<h1>&#128225; Serveur de communication v%s</h1>"
        "<p>Pour rejoindre le réseau, télécharge et lance le client :</p>"
        "<h2>&#127823; Linux / Mac</h2><pre>"
        "# Télécharger\n"
        "curl -O http://%s:%d/dl/client.py\n\n"
        "# Installer les dépendances (une seule fois)\n"
        "pip install websockets\n\n"
        "# Lancer\n"
        "python3 client.py %s"
        "</pre>"
        "<h2>&#129695; Windows (PowerShell)</h2><pre>"
        "# Télécharger\n"
        "Invoke-WebRequest http://%s:%d/dl/client.py -OutFile client.py\n\n"
        "# Installer les dépendances (une seule fois)\n"
        "pip install websockets\n\n"
        "# Lancer\n"
        "python client.py %s"
        "</pre>"
        "<h2>&#128230; Fichiers disponibles</h2><ul>%s</ul>"
        "</body></html>",
        SERVER_VERSION, SERVER_VERSION,
        server_ip, PORT_HTTP, server_ip,
        server_ip, PORT_HTTP, server_ip,
        files_html
    );

    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n", blen);
    send(fd, header, hlen, 0);
    send(fd, body, blen, 0);
}

// ══════════════════════════════════════════════════════════════════════════════
//  THREAD PRINCIPAL — accepte les connexions HTTP/WS
// ══════════════════════════════════════════════════════════════════════════════

static void *handle_connection(void *arg) {
    int fd = *(int *)arg;
    free(arg);

    // Récupère l'IP du client
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    getpeername(fd, (struct sockaddr *)&peer, &plen);
    char client_ip[MAX_IP_LEN];
    strncpy(client_ip, inet_ntoa(peer.sin_addr), MAX_IP_LEN - 1);

    // IP du serveur vue par ce client
    struct sockaddr_in local;
    socklen_t llen = sizeof(local);
    getsockname(fd, (struct sockaddr *)&local, &llen);
    char server_ip[MAX_IP_LEN];
    strncpy(server_ip, inet_ntoa(local.sin_addr), MAX_IP_LEN - 1);

    // Lit la requête HTTP
    char req[4096] = {0};
    ssize_t n = recv(fd, req, sizeof(req) - 1, 0);
    if (n <= 0) { close(fd); return NULL; }

    // Parse méthode + path
    char method[8] = {0}, path[256] = {0};
    sscanf(req, "%7s %255s", method, path);

    // WebSocket upgrade ?
    if (strstr(req, "Upgrade: websocket") || strstr(req, "Upgrade: WebSocket")) {
        if (ws_handshake(fd, req) < 0) { close(fd); return NULL; }

        // Enregistre le client
        pthread_mutex_lock(&g_mutex);
        if (g_client_count >= MAX_CLIENTS) {
            pthread_mutex_unlock(&g_mutex);
            close(fd);
            return NULL;
        }
        WSClient *cli = &g_clients[g_client_count++];
        memset(cli, 0, sizeof(WSClient));
        cli->fd = fd;
        snprintf(cli->ip, MAX_IP_LEN, "%s", client_ip);
        cli->last_seen = now_sec();
        pthread_mutex_unlock(&g_mutex);

        logf_msg("WS", "Connexion : %s", client_ip);

        // Lance le thread de gestion
        pthread_create(&cli->thread, NULL, ws_client_thread, cli);
        pthread_detach(cli->thread);
        return NULL;
    }

    // Téléchargement de fichier
    if (strncmp(path, "/dl/", 4) == 0) {
        const char *filename = path + 4;
        // Sécurité : interdit ".."
        if (strstr(filename, "..") || strchr(filename, '/')) {
            const char *r = "HTTP/1.1 403 Forbidden\r\nContent-Length: 9\r\n\r\nForbidden";
            send(fd, r, strlen(r), 0);
        } else {
            char filepath[512];
            snprintf(filepath, sizeof(filepath), "%s/%s", DIST_DIR, filename);
            http_serve_file(fd, filepath, filename);
            logf_msg("HTTP", "Download : %s → %s", filename, client_ip);
        }
        close(fd);
        return NULL;
    }

    // Page d'accueil
    http_serve_index(fd, server_ip);
    close(fd);
    return NULL;
}

// ══════════════════════════════════════════════════════════════════════════════
//  THREAD PURGE — retire les clients inactifs
// ══════════════════════════════════════════════════════════════════════════════

static void *purge_thread(void *arg) {
    (void)arg;
    while (1) {
        sleep(15);
        uint32_t now = now_sec();
        pthread_mutex_lock(&g_mutex);
        for (int i = g_client_count - 1; i >= 0; i--) {
            if (now - g_clients[i].last_seen > HEARTBEAT_TTL) {
                logf_msg("REGISTRY", "Timeout : %s (%s)",
                    g_clients[i].name[0] ? g_clients[i].name : "?",
                    g_clients[i].ip);
                close(g_clients[i].fd);
                g_clients[i] = g_clients[g_client_count - 1];
                g_client_count--;
            }
        }
        pthread_mutex_unlock(&g_mutex);
    }
    return NULL;
}

// ══════════════════════════════════════════════════════════════════════════════
//  MAIN
// ══════════════════════════════════════════════════════════════════════════════

int main(void) {
    mkdir(DIST_DIR, 0755);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(PORT_HTTP)
    };
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    listen(srv, 32);

    printf("\n");
    printf("  ╔══════════════════════════════════════╗\n");
    printf("  ║   SERVEUR DE COMMUNICATION v%s     ║\n", SERVER_VERSION);
    printf("  ╠══════════════════════════════════════╣\n");
    printf("  ║  TCP %d  →  HTTP + WebSocket        ║\n", PORT_HTTP);
    printf("  ║                                      ║\n");
    printf("  ║  Routes :                            ║\n");
    printf("  ║   /      →  Page d'accueil           ║\n");
    printf("  ║   /dl/*  →  Téléchargements          ║\n");
    printf("  ║   /ws    →  WebSocket                ║\n");
    printf("  ╚══════════════════════════════════════╝\n\n");
    printf("  Fichiers à distribuer : %s/\n\n", DIST_DIR);

    pthread_t t_purge;
    pthread_create(&t_purge, NULL, purge_thread, NULL);
    pthread_detach(t_purge);

    logf_msg("SERVER", "Prêt sur TCP :%d", PORT_HTTP);

    while (1) {
        struct sockaddr_in ca;
        socklen_t clen = sizeof(ca);
        int *cfd = malloc(sizeof(int));
        *cfd = accept(srv, (struct sockaddr *)&ca, &clen);
        if (*cfd < 0) { free(cfd); continue; }
        pthread_t t;
        pthread_create(&t, NULL, handle_connection, cfd);
        pthread_detach(t);
    }
    return 0;
}
