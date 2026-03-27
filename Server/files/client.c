/*
 * client.c — Client de communication (Linux & Windows)
 *
 * Compile Linux   : gcc -o client_linux client.c -lpthread
 * Compile Windows : gcc -o client.exe client.c -lws2_32
 *
 * Usage : ./client_linux <IP_DU_SERVEUR>
 *         .\client.exe  <IP_DU_SERVEUR>
 *
 * Fonctionnement :
 *   1. Se connecte au serveur (TCP :9000) pour découverte (optionnel)
 *   2. S'enregistre sur le registre (UDP :9001) avec un pseudo
 *   3. Affiche la liste des clients connectés
 *   4. Communique directement en peer-to-peer avec n'importe quel client
 *      via ton protocol original (UDP :9002)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <windows.h>
    #include <process.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef int socklen_t;
    #define SLEEP(ms)    Sleep(ms)
    #define THREAD_RET   unsigned __stdcall
    #define THREAD_CALL  __stdcall
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <pthread.h>
    #define SOCKET       int
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR   -1
    #define closesocket  close
    #define SLEEP(ms)    usleep((ms) * 1000)
    #define THREAD_RET   void*
    #define THREAD_CALL
#endif

#include "protocol.h"

// ── État global ────────────────────────────────────────────────────────────────
static SOCKET         g_comm_sock;     // UDP :9002 — communication p2p
static SOCKET         g_reg_sock;      // UDP :9001 — registre
static char           g_server_ip[MAX_IP_LEN] = "127.0.0.1";
static char           g_my_name[MAX_NAME_LEN] = "Anonymous";
static struct ClientEntry g_peers[MAX_CLIENTS];
static int            g_peer_count = 0;

// ── Utilitaires ────────────────────────────────────────────────────────────────
static const char *get_basename(const char *path) {
    const char *a = strrchr(path, '/');
    const char *b = strrchr(path, '\\');
    if (a && b) return (a > b) ? a + 1 : b + 1;
    if (a) return a + 1;
    if (b) return b + 1;
    return path;
}

static void print_peers(void) {
    printf("\n┌─────────────────────────────────────────────┐\n");
    printf("│  Clients connectés (%d)                      │\n", g_peer_count);
    printf("├────┬─────────────────┬───────────────────────┤\n");
    printf("│ #  │ Pseudo          │ IP                    │\n");
    printf("├────┼─────────────────┼───────────────────────┤\n");
    for (int i = 0; i < g_peer_count; i++) {
        printf("│ %-2d │ %-15s │ %-21s │\n",
               i + 1, g_peers[i].name, g_peers[i].ip);
    }
    printf("└────┴─────────────────┴───────────────────────┘\n\n");
}

// ══════════════════════════════════════════════════════════════════════════════
//  COMMUNICATION PEER-TO-PEER (ton protocol original, UDP :9002)
// ══════════════════════════════════════════════════════════════════════════════

static void send_message_to(const char *target_ip, const char *msg) {
    uint8_t buf[BUFFER_SIZE] = {0};
    struct Header *h = (struct Header *)buf;
    h->type = TYPE_MSG;
    h->seq  = htonl(1);
    snprintf((char *)(buf + sizeof(struct Header)), PAYLOAD_SIZE, "%s", msg);

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port   = htons(PORT_COMM),
        .sin_addr.s_addr = inet_addr(target_ip)
    };
    sendto(g_comm_sock, (char *)buf,
           (int)(sizeof(struct Header) + strlen(msg)), 0,
           (struct sockaddr *)&dest, sizeof(dest));
    printf("[→ %s] Message envoyé.\n", target_ip);
}

static void send_file_to(const char *target_ip, const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) { printf("[!] Impossible d'ouvrir '%s'\n", filepath); return; }

    uint8_t buf[BUFFER_SIZE];
    struct Header *h = (struct Header *)buf;
    const char *filename = get_basename(filepath);

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port   = htons(PORT_COMM),
        .sin_addr.s_addr = inet_addr(target_ip)
    };

    // FILE_START
    memset(buf, 0, BUFFER_SIZE);
    h->type = TYPE_FILE_START;
    strncpy((char *)(buf + sizeof(struct Header)), filename, PAYLOAD_SIZE - 1);
    sendto(g_comm_sock, (char *)buf, BUFFER_SIZE, 0,
           (struct sockaddr *)&dest, sizeof(dest));
    printf("[→ %s] Envoi de %s...\n", target_ip, filename);

    // FILE_DATA
    uint32_t seq = 0;
    size_t n;
    while ((n = fread(buf + sizeof(struct Header), 1, PAYLOAD_SIZE, f)) > 0) {
        h->type = TYPE_FILE_DATA;
        h->seq  = htonl(++seq);
        sendto(g_comm_sock, (char *)buf, (int)(sizeof(struct Header) + n), 0,
               (struct sockaddr *)&dest, sizeof(dest));
        SLEEP(1);
    }

    // FILE_END
    h->type = TYPE_FILE_END;
    sendto(g_comm_sock, (char *)buf, sizeof(struct Header), 0,
           (struct sockaddr *)&dest, sizeof(dest));
    printf("[→ %s] Transfert terminé.\n", target_ip);
    fclose(f);
}

// ══════════════════════════════════════════════════════════════════════════════
//  THREAD RÉCEPTION  (écoute UDP :9002 pour messages/fichiers + :9001 pour notifs)
// ══════════════════════════════════════════════════════════════════════════════

static THREAD_RET THREAD_CALL receiver_thread(void *arg) {
    (void)arg;
    uint8_t buf[BUFFER_SIZE];
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    FILE *f_recv = NULL;

    // On surveille les deux sockets : g_comm_sock et g_reg_sock
    // Utilise select() pour multiplexer
    while (1) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(g_comm_sock, &fds);
        FD_SET(g_reg_sock,  &fds);
        int maxfd = (g_comm_sock > g_reg_sock ? g_comm_sock : g_reg_sock) + 1;
        struct timeval tv = {1, 0};
        int ready = select(maxfd, &fds, NULL, NULL, &tv);
        if (ready <= 0) continue;

        // ── Socket de communication (UDP :9002) ──────────────────────────────
        if (FD_ISSET(g_comm_sock, &fds)) {
            int n = recvfrom(g_comm_sock, (char *)buf, BUFFER_SIZE, 0,
                             (struct sockaddr *)&from, &flen);
            if (n > 0) {
                struct Header *h = (struct Header *)buf;
                char *payload    = (char *)(buf + sizeof(struct Header));
                int  plen        = n - (int)sizeof(struct Header);
                char from_ip[MAX_IP_LEN];
                strncpy(from_ip, inet_ntoa(from.sin_addr), MAX_IP_LEN - 1);

                switch (h->type) {
                    case TYPE_MSG:
                        if (plen > 0 && (size_t)plen < PAYLOAD_SIZE) payload[plen] = '\0';
                        printf("\n\033[1;36m>>> [MSG de %s] : %s\033[0m\n> ", from_ip, payload);
                        fflush(stdout);
                        break;

                    case TYPE_FILE_START: {
                        payload[PAYLOAD_SIZE - 1] = '\0';
                        printf("\n>>> [FICHIER de %s] : %s\n", from_ip, payload);
                        char path[300];
                        snprintf(path, sizeof(path), "recu_%.*s", 250, payload);
                        f_recv = fopen(path, "wb");
                        break;
                    }

                    case TYPE_FILE_DATA:
                        if (f_recv) fwrite(payload, 1, plen, f_recv);
                        break;

                    case TYPE_FILE_END:
                        if (f_recv) {
                            fclose(f_recv); f_recv = NULL;
                            printf(">>> [OK] Fichier enregistré.\n> ");
                            fflush(stdout);
                        }
                        break;
                }
            }
        }

        // ── Socket registre (UDP :9001) — notifications et liste ─────────────
        if (FD_ISSET(g_reg_sock, &fds)) {
            int n = recvfrom(g_reg_sock, (char *)buf, BUFFER_SIZE, 0,
                             (struct sockaddr *)&from, &flen);
            if (n > 0) {
                struct Header *h = (struct Header *)buf;

                switch (h->type) {
                    case REG_LIST_RESP: {
                        struct ClientListPacket *pkt = (struct ClientListPacket *)buf;
                        g_peer_count = pkt->count;
                        memcpy(g_peers, pkt->clients, g_peer_count * sizeof(struct ClientEntry));
                        break;
                    }
                    case REG_NOTIFY: {
                        char *msg = (char *)(buf + sizeof(struct Header));
                        msg[PAYLOAD_SIZE - 1] = '\0';
                        printf("\n\033[1;33m[RÉSEAU] %s\033[0m\n> ", msg);
                        fflush(stdout);
                        // Redemande la liste à jour
                        struct Header req = {REG_LIST_REQ, 0};
                        struct sockaddr_in srv = {
                            .sin_family = AF_INET,
                            .sin_port   = htons(PORT_REGISTRY),
                            .sin_addr.s_addr = inet_addr(g_server_ip)
                        };
                        sendto(g_reg_sock, (const char *)&req, sizeof(req), 0,
                               (struct sockaddr *)&srv, sizeof(srv));
                        break;
                    }
                }
            }
        }
    }
    return 0;
}

// ══════════════════════════════════════════════════════════════════════════════
//  THREAD HEARTBEAT  (envoie un keepalive au serveur toutes les 10s)
// ══════════════════════════════════════════════════════════════════════════════

static THREAD_RET THREAD_CALL heartbeat_thread(void *arg) {
    (void)arg;
    struct sockaddr_in srv = {
        .sin_family = AF_INET,
        .sin_port   = htons(PORT_REGISTRY),
        .sin_addr.s_addr = inet_addr(g_server_ip)
    };
    struct Header hb = {REG_HEARTBEAT, 0};
    while (1) {
        sendto(g_reg_sock, (const char *)&hb, sizeof(hb), 0, (struct sockaddr *)&srv, sizeof(srv));
        SLEEP(10000);
    }
    return 0;
}

// ══════════════════════════════════════════════════════════════════════════════
//  ENREGISTREMENT AUPRÈS DU SERVEUR
// ══════════════════════════════════════════════════════════════════════════════

static void register_to_server(void) {
    struct RegisterPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = REG_REGISTER;
    snprintf(pkt.name, MAX_NAME_LEN, "%s", g_my_name);
    pkt.comm_port = htons(PORT_COMM);

    struct sockaddr_in srv = {
        .sin_family = AF_INET,
        .sin_port   = htons(PORT_REGISTRY),
        .sin_addr.s_addr = inet_addr(g_server_ip)
    };
    sendto(g_reg_sock, (const char *)&pkt, sizeof(pkt), 0, (struct sockaddr *)&srv, sizeof(srv));
    printf("[✓] Enregistré sur le serveur sous le nom '%s'\n", g_my_name);
}

static void unregister_from_server(void) {
    struct Header h = {REG_UNREGISTER, 0};
    struct sockaddr_in srv = {
        .sin_family = AF_INET,
        .sin_port   = htons(PORT_REGISTRY),
        .sin_addr.s_addr = inet_addr(g_server_ip)
    };
    sendto(g_reg_sock, (const char *)&h, sizeof(h), 0, (struct sockaddr *)&srv, sizeof(srv));
}

// ══════════════════════════════════════════════════════════════════════════════
//  MAIN
// ══════════════════════════════════════════════════════════════════════════════

int main(int argc, char *argv[]) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    if (argc > 1) strncpy(g_server_ip, argv[1], MAX_IP_LEN - 1);

    // Demande le pseudo
    printf("\n=== CLIENT DE COMMUNICATION ===\n");
    printf("Serveur : %s\n\n", g_server_ip);
    printf("Ton pseudo : ");
    { char *_r = fgets(g_my_name, MAX_NAME_LEN, stdin); (void)_r; }
    g_my_name[strcspn(g_my_name, "\r\n")] = '\0';
    if (strlen(g_my_name) == 0) strncpy(g_my_name, "Anonyme", MAX_NAME_LEN - 1);

    // Crée les sockets
    g_comm_sock = socket(AF_INET, SOCK_DGRAM, 0);
    g_reg_sock  = socket(AF_INET, SOCK_DGRAM, 0);

    // Bind comm socket sur PORT_COMM pour recevoir les messages p2p
    struct sockaddr_in bind_comm = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(PORT_COMM)
    };
    if (bind(g_comm_sock, (struct sockaddr *)&bind_comm, sizeof(bind_comm)) < 0) {
        printf("[!] Port %d déjà utilisé (un autre client tourne ?)\n", PORT_COMM);
    }

    // Bind reg socket sur PORT_REGISTRY pour recevoir les notifs/listes
    struct sockaddr_in bind_reg = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(PORT_REGISTRY)
    };
    bind(g_reg_sock, (struct sockaddr *)&bind_reg, sizeof(bind_reg));

    // Lance les threads de fond
#ifdef _WIN32
    _beginthreadex(NULL, 0, receiver_thread,  NULL, 0, NULL);
    _beginthreadex(NULL, 0, heartbeat_thread, NULL, 0, NULL);
#else
    pthread_t t_recv, t_hb;
    pthread_create(&t_recv, NULL, receiver_thread,  NULL);
    pthread_create(&t_hb,   NULL, heartbeat_thread, NULL);
    pthread_detach(t_recv);
    pthread_detach(t_hb);
#endif

    // Enregistrement
    register_to_server();
    SLEEP(500); // laisse le temps de recevoir la liste initiale

    // ── Boucle principale ─────────────────────────────────────────────────────
    char target_ip[MAX_IP_LEN] = "";

    while (1) {
        printf("\n");
        printf("══════════════════════════════════════════\n");
        printf("  %s  |  Serveur : %s\n", g_my_name, g_server_ip);
        if (strlen(target_ip) > 0)
            printf("  Cible actuelle : %s\n", target_ip);
        printf("══════════════════════════════════════════\n");
        printf("  1. Envoyer un message\n");
        printf("  2. Envoyer un fichier\n");
        printf("  3. Voir les clients connectés\n");
        printf("  4. Choisir une cible dans la liste\n");
        printf("  5. Saisir une IP manuellement\n");
        printf("  0. Quitter\n");
        printf("> ");

        int choix;
        if (scanf("%d", &choix) != 1) { getchar(); continue; }
        getchar();

        switch (choix) {
            case 1: {
                if (strlen(target_ip) == 0) {
                    printf("[!] Aucune cible. Utilise option 4 ou 5.\n");
                    break;
                }
                char msg[PAYLOAD_SIZE];
                printf("Message : ");
                { char *_r = fgets(msg, PAYLOAD_SIZE, stdin); (void)_r; }
                msg[strcspn(msg, "\r\n")] = '\0';
                send_message_to(target_ip, msg);
                break;
            }
            case 2: {
                if (strlen(target_ip) == 0) {
                    printf("[!] Aucune cible. Utilise option 4 ou 5.\n");
                    break;
                }
                char path[256];
                printf("Chemin du fichier : ");
                { char *_r = fgets(path, sizeof(path), stdin); (void)_r; }
                path[strcspn(path, "\r\n")] = '\0';
                send_file_to(target_ip, path);
                break;
            }
            case 3:
                print_peers();
                break;

            case 4: {
                print_peers();
                if (g_peer_count == 0) { printf("[!] Aucun client connecté.\n"); break; }
                printf("Numéro de la cible : ");
                int idx;
                if (scanf("%d", &idx) == 1 && idx >= 1 && idx <= g_peer_count) {
                    strncpy(target_ip, g_peers[idx - 1].ip, MAX_IP_LEN - 1);
                    printf("[✓] Cible : %s (%s)\n", g_peers[idx - 1].name, target_ip);
                } else {
                    printf("[!] Numéro invalide.\n");
                }
                getchar();
                break;
            }
            case 5:
                printf("IP : ");
                { int _r = scanf("%15s", target_ip); (void)_r; }
                getchar();
                printf("[✓] Cible : %s\n", target_ip);
                break;

            case 0:
                unregister_from_server();
                closesocket(g_comm_sock);
                closesocket(g_reg_sock);
#ifdef _WIN32
                WSACleanup();
#endif
                printf("Au revoir !\n");
                return 0;
        }
    }
}