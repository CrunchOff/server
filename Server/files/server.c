/*
 * server.c — Serveur central (Debian A)
 *
 * Compile : gcc -o server server.c -lpthread
 * Lancer  : ./server
 *
 * Ports ouverts :
 *   9000 TCP  → Découverte  : envoie les instructions de téléchargement
 *   8080 TCP  → HTTP        : sert les fichiers (linux, windows, sources)
 *   9001 UDP  → Registry    : liste des clients connectés + heartbeat
 *   9002 UDP  → Relay COMM  : relaie les messages entre clients si besoin
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>

#include "protocol.h"

// ── Configuration ──────────────────────────────────────────────────────────────
#define DIST_DIR        "./dist"          // dossier contenant les fichiers à servir
#define HEARTBEAT_TTL   30                // secondes avant qu'un client soit considéré mort
#define SERVER_VERSION  "1.0"

// ── Registre des clients ───────────────────────────────────────────────────────
static struct ClientEntry  g_clients[MAX_CLIENTS];
static int                 g_client_count = 0;
static pthread_mutex_t     g_clients_mutex = PTHREAD_MUTEX_INITIALIZER;

// ── Utilitaires ────────────────────────────────────────────────────────────────
static uint32_t now_sec(void) {
    return (uint32_t)time(NULL);
}

static void log_msg(const char *tag, const char *msg) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    printf("[%02d:%02d:%02d] [%s] %s\n", tm->tm_hour, tm->tm_min, tm->tm_sec, tag, msg);
    fflush(stdout);
}

// ══════════════════════════════════════════════════════════════════════════════
//  THREAD DÉCOUVERTE  (TCP :9000)
//  → Le client se connecte, le serveur lui envoie un message d'accueil
//    avec les instructions pour télécharger le client
// ══════════════════════════════════════════════════════════════════════════════

static void *handle_discovery_client(void *arg) {
    int client_fd = *(int *)arg;
    free(arg);

    // Récupère l'IP du client pour personnaliser le message
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    getpeername(client_fd, (struct sockaddr *)&peer, &plen);
    char client_ip[MAX_IP_LEN];
    strncpy(client_ip, inet_ntoa(peer.sin_addr), MAX_IP_LEN - 1);

    // Récupère l'IP locale du serveur (celle vue par ce client)
    struct sockaddr_in local;
    socklen_t llen = sizeof(local);
    getsockname(client_fd, (struct sockaddr *)&local, &llen);
    char server_ip[MAX_IP_LEN];
    strncpy(server_ip, inet_ntoa(local.sin_addr), MAX_IP_LEN - 1);

    char msg[2048];
    snprintf(msg, sizeof(msg),
        "\r\n"
        "╔══════════════════════════════════════════════════╗\r\n"
        "║         SERVEUR DE COMMUNICATION v%s           ║\r\n"
        "║                                                  ║\r\n"
        "║  Bienvenue ! (%s)                   ║\r\n"
        "║                                                  ║\r\n"
        "║  Pour rejoindre le réseau, télécharge le client  ║\r\n"
        "║  correspondant à ton système :                   ║\r\n"
        "║                                                  ║\r\n"
        "║  🐧 Linux :                                      ║\r\n"
        "║  wget http://%s:%d/client_linux         ║\r\n"
        "║  chmod +x client_linux                           ║\r\n"
        "║  ./client_linux %s                  ║\r\n"
        "║                                                  ║\r\n"
        "║  🪟 Windows (PowerShell) :                       ║\r\n"
        "║  Invoke-WebRequest http://%s:%d/client.exe -OutFile client.exe\r\n"
        "║  .\\client.exe %s                    ║\r\n"
        "║                                                  ║\r\n"
        "║  📦 Sources C :                                  ║\r\n"
        "║  wget http://%s:%d/sources.zip        ║\r\n"
        "║                                                  ║\r\n"
        "╚══════════════════════════════════════════════════╝\r\n"
        "\r\n",
        SERVER_VERSION,
        client_ip,
        server_ip, PORT_HTTP, server_ip,
        server_ip, PORT_HTTP, server_ip,
        server_ip, PORT_HTTP
    );

    send(client_fd, msg, strlen(msg), 0);
    close(client_fd);

    char logbuf[64];
    snprintf(logbuf, sizeof(logbuf), "Client découverte : %s", client_ip);
    log_msg("DISCOVERY", logbuf);
    return NULL;
}

static void *thread_discovery(void *arg) {
    (void)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(PORT_DISCOVERY)
    };
    bind(srv, (struct sockaddr *)&addr, sizeof(addr));
    listen(srv, 16);
    log_msg("DISCOVERY", "Écoute sur TCP :9000");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t clen = sizeof(client_addr);
        int *cfd = malloc(sizeof(int));
        *cfd = accept(srv, (struct sockaddr *)&client_addr, &clen);
        if (*cfd < 0) { free(cfd); continue; }
        pthread_t t;
        pthread_create(&t, NULL, handle_discovery_client, cfd);
        pthread_detach(t);
    }
    return NULL;
}

// ══════════════════════════════════════════════════════════════════════════════
//  THREAD HTTP  (TCP :8080)
//  → Serveur HTTP minimaliste pour distribuer les fichiers du dossier ./dist
// ══════════════════════════════════════════════════════════════════════════════

static const char *mime_type(const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (!ext) return "application/octet-stream";
    if (!strcmp(ext, ".exe"))  return "application/octet-stream";
    if (!strcmp(ext, ".zip"))  return "application/zip";
    if (!strcmp(ext, ".c"))    return "text/plain";
    if (!strcmp(ext, ".h"))    return "text/plain";
    if (!strcmp(ext, ".txt"))  return "text/plain";
    if (!strcmp(ext, ".html")) return "text/html";
    return "application/octet-stream";
}

static void http_send_file(int cfd, const char *filepath, const char *filename) {
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        const char *r404 = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nNot Found";
        send(cfd, r404, strlen(r404), 0);
        return;
    }
    struct stat st;
    fstat(fd, &st);
    char header[512];
    snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "\r\n",
        mime_type(filename), (long)st.st_size, filename);
    send(cfd, header, strlen(header), 0);

    char buf[8192];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        send(cfd, buf, n, 0);
    }
    close(fd);
}

static void http_send_index(int cfd) {
    // Liste les fichiers disponibles dans ./dist
    char body[8192];
    int blen = 0;
    blen += snprintf(body + blen, sizeof(body) - blen,
        "<html><head><title>Distribution</title></head><body>"
        "<h2>Fichiers disponibles</h2><ul>");

    DIR *d = opendir(DIST_DIR);
    if (d) {
        struct dirent *entry;
        while ((entry = readdir(d)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            blen += snprintf(body + blen, sizeof(body) - blen,
                "<li><a href=\"/%s\">%s</a></li>", entry->d_name, entry->d_name);
        }
        closedir(d);
    }
    blen += snprintf(body + blen, sizeof(body) - blen, "</ul></body></html>");

    char header[256];
    snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %d\r\n\r\n", blen);
    send(cfd, header, strlen(header), 0);
    send(cfd, body, blen, 0);
}

static void *handle_http_client(void *arg) {
    int cfd = *(int *)arg;
    free(arg);

    char req[1024] = {0};
    recv(cfd, req, sizeof(req) - 1, 0);

    // Parse la première ligne : GET /filename HTTP/1.1
    char method[8], path[256];
    sscanf(req, "%7s %255s", method, path);

    if (strcmp(path, "/") == 0 || strcmp(path, "") == 0) {
        http_send_index(cfd);
    } else {
        // Sécurité basique : interdit ".."
        if (strstr(path, "..")) {
            const char *r403 = "HTTP/1.1 403 Forbidden\r\nContent-Length: 9\r\n\r\nForbidden";
            send(cfd, r403, strlen(r403), 0);
        } else {
            char filepath[512];
            snprintf(filepath, sizeof(filepath), "%s%s", DIST_DIR, path);
            const char *filename = path + 1; // sans le '/'
            http_send_file(cfd, filepath, filename);
        }
    }
    close(cfd);
    return NULL;
}

static void *thread_http(void *arg) {
    (void)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(PORT_HTTP)
    };
    bind(srv, (struct sockaddr *)&addr, sizeof(addr));
    listen(srv, 32);
    log_msg("HTTP", "Écoute sur TCP :8080  (distribution de fichiers)");

    while (1) {
        struct sockaddr_in ca;
        socklen_t clen = sizeof(ca);
        int *cfd = malloc(sizeof(int));
        *cfd = accept(srv, (struct sockaddr *)&ca, &clen);
        if (*cfd < 0) { free(cfd); continue; }
        pthread_t t;
        pthread_create(&t, NULL, handle_http_client, cfd);
        pthread_detach(t);
    }
    return NULL;
}

// ══════════════════════════════════════════════════════════════════════════════
//  THREAD REGISTRY  (UDP :9001)
//  → Gère l'enregistrement, les heartbeats et la liste des clients
// ══════════════════════════════════════════════════════════════════════════════

static int find_client_by_ip(const char *ip) {
    for (int i = 0; i < g_client_count; i++) {
        if (strcmp(g_clients[i].ip, ip) == 0) return i;
    }
    return -1;
}

static void remove_client(int idx) {
    if (idx < 0 || idx >= g_client_count) return;
    g_clients[idx] = g_clients[g_client_count - 1];
    g_client_count--;
}

static void purge_dead_clients(void) {
    uint32_t now = now_sec();
    pthread_mutex_lock(&g_clients_mutex);
    for (int i = g_client_count - 1; i >= 0; i--) {
        if (now - g_clients[i].last_seen > HEARTBEAT_TTL) {
            char logbuf[64];
            snprintf(logbuf, sizeof(logbuf), "Timeout client : %s (%s)", g_clients[i].name, g_clients[i].ip);
            log_msg("REGISTRY", logbuf);
            remove_client(i);
        }
    }
    pthread_mutex_unlock(&g_clients_mutex);
}

static void send_client_list(int sock, struct sockaddr_in *dest) {
    struct ClientListPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = REG_LIST_RESP;
    pthread_mutex_lock(&g_clients_mutex);
    pkt.count = (uint8_t)g_client_count;
    memcpy(pkt.clients, g_clients, g_client_count * sizeof(struct ClientEntry));
    pthread_mutex_unlock(&g_clients_mutex);
    sendto(sock, &pkt, sizeof(pkt), 0, (struct sockaddr *)dest, sizeof(*dest));
}

static void notify_all(int sock, const char *joined_name, const char *joined_ip, int joined) {
    // Envoie une notification à tous les clients enregistrés
    uint8_t buf[BUFFER_SIZE] = {0};
    struct Header *h = (struct Header *)buf;
    h->type = REG_NOTIFY;
    char *payload = (char *)(buf + sizeof(struct Header));
    snprintf(payload, PAYLOAD_SIZE - 1, "%s %s (%s)",
             joined ? "+" : "-", joined_name, joined_ip);

    pthread_mutex_lock(&g_clients_mutex);
    for (int i = 0; i < g_client_count; i++) {
        struct sockaddr_in dest;
        dest.sin_family = AF_INET;
        dest.sin_port = htons(PORT_REGISTRY);
        dest.sin_addr.s_addr = inet_addr(g_clients[i].ip);
        sendto(sock, buf, sizeof(struct Header) + strlen(payload), 0,
               (struct sockaddr *)&dest, sizeof(dest));
    }
    pthread_mutex_unlock(&g_clients_mutex);
}

static void *thread_registry(void *arg) {
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(PORT_REGISTRY)
    };
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));
    log_msg("REGISTRY", "Écoute sur UDP :9001  (registre des clients)");

    uint8_t buf[BUFFER_SIZE];
    struct sockaddr_in client;
    socklen_t clen = sizeof(client);
    uint32_t last_purge = now_sec();

    while (1) {
        // Timeout recvfrom pour faire la purge périodiquement
        struct timeval tv = {5, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&client, &clen);

        // Purge périodique des clients morts
        if (now_sec() - last_purge > 10) {
            purge_dead_clients();
            last_purge = now_sec();
        }

        if (n <= 0) continue;

        struct Header *h = (struct Header *)buf;
        char client_ip[MAX_IP_LEN];
        strncpy(client_ip, inet_ntoa(client.sin_addr), MAX_IP_LEN - 1);

        switch (h->type) {

            case REG_REGISTER: {
                struct RegisterPacket *reg = (struct RegisterPacket *)buf;
                reg->name[MAX_NAME_LEN - 1] = '\0';
                pthread_mutex_lock(&g_clients_mutex);
                int idx = find_client_by_ip(client_ip);
                if (idx < 0 && g_client_count < MAX_CLIENTS) {
                    idx = g_client_count++;
                    strncpy(g_clients[idx].ip, client_ip, MAX_IP_LEN - 1);
                }
                if (idx >= 0) {
                    strncpy(g_clients[idx].name, reg->name, MAX_NAME_LEN - 1);
                    g_clients[idx].port = ntohs(reg->comm_port);
                    g_clients[idx].last_seen = now_sec();
                }
                pthread_mutex_unlock(&g_clients_mutex);

                char logbuf[80];
                snprintf(logbuf, sizeof(logbuf), "Enregistré : %s (%s)", reg->name, client_ip);
                log_msg("REGISTRY", logbuf);

                notify_all(sock, reg->name, client_ip, 1);
                send_client_list(sock, &client);
                break;
            }

            case REG_HEARTBEAT: {
                pthread_mutex_lock(&g_clients_mutex);
                int idx = find_client_by_ip(client_ip);
                if (idx >= 0) g_clients[idx].last_seen = now_sec();
                pthread_mutex_unlock(&g_clients_mutex);
                break;
            }

            case REG_LIST_REQ:
                send_client_list(sock, &client);
                break;

            case REG_UNREGISTER: {
                pthread_mutex_lock(&g_clients_mutex);
                int idx = find_client_by_ip(client_ip);
                char name[MAX_NAME_LEN] = "inconnu";
                if (idx >= 0) {
                    strncpy(name, g_clients[idx].name, MAX_NAME_LEN - 1);
                    remove_client(idx);
                }
                pthread_mutex_unlock(&g_clients_mutex);
                notify_all(sock, name, client_ip, 0);

                char logbuf[80];
                snprintf(logbuf, sizeof(logbuf), "Déconnecté : %s (%s)", name, client_ip);
                log_msg("REGISTRY", logbuf);
                break;
            }
        }
    }
    return NULL;
}

// ══════════════════════════════════════════════════════════════════════════════
//  MAIN
// ══════════════════════════════════════════════════════════════════════════════

int main(void) {
    // Crée le dossier dist s'il n'existe pas
    mkdir(DIST_DIR, 0755);

    printf("\n");
    printf("  ╔══════════════════════════════════════╗\n");
    printf("  ║     SERVEUR DE COMMUNICATION v%s    ║\n", SERVER_VERSION);
    printf("  ╠══════════════════════════════════════╣\n");
    printf("  ║  Ports:                              ║\n");
    printf("  ║   TCP  %d  →  Découverte           ║\n", PORT_DISCOVERY);
    printf("  ║   TCP  %d  →  Distribution HTTP    ║\n", PORT_HTTP);
    printf("  ║   UDP  %d  →  Registre clients     ║\n", PORT_REGISTRY);
    printf("  ║   UDP  %d  →  Communication        ║\n", PORT_COMM);
    printf("  ╚══════════════════════════════════════╝\n\n");
    printf("  Placez les fichiers à distribuer dans : %s/\n\n", DIST_DIR);

    pthread_t t_discovery, t_http, t_registry;
    pthread_create(&t_discovery, NULL, thread_discovery, NULL);
    pthread_create(&t_http,      NULL, thread_http,      NULL);
    pthread_create(&t_registry,  NULL, thread_registry,  NULL);

    // Attendre tous les threads (infini)
    pthread_join(t_discovery, NULL);
    pthread_join(t_http,      NULL);
    pthread_join(t_registry,  NULL);
    return 0;
}
