#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

// =============================================
//  PORTS
// =============================================
#define PORT_DISCOVERY   9000   // TCP : découverte, le serveur envoie les instructions
#define PORT_HTTP        8080   // HTTP : téléchargement des fichiers
#define PORT_REGISTRY    9001   // UDP : enregistrement des clients + liste
#define PORT_COMM        9002   // UDP : communication entre clients (ton protocol)

// =============================================
//  TAILLES
// =============================================
#define BUFFER_SIZE      4096
#define PAYLOAD_SIZE     (BUFFER_SIZE - sizeof(struct Header))
#define MAX_CLIENTS      64
#define MAX_NAME_LEN     32
#define MAX_IP_LEN       16

// =============================================
//  TYPES DE PAQUETS (PORT_COMM - ton protocol)
// =============================================
#define TYPE_MSG         0x01   // Message texte
#define TYPE_FILE_START  0x02   // Début de fichier
#define TYPE_FILE_DATA   0x03   // Chunk de fichier
#define TYPE_FILE_END    0x04   // Fin de fichier

// =============================================
//  TYPES DE PAQUETS (PORT_REGISTRY)
// =============================================
#define REG_REGISTER     0x10   // Client s'enregistre avec son pseudo
#define REG_UNREGISTER   0x11   // Client se déconnecte
#define REG_LIST_REQ     0x12   // Client demande la liste
#define REG_LIST_RESP    0x13   // Serveur répond avec la liste
#define REG_HEARTBEAT    0x14   // Keepalive du client
#define REG_NOTIFY       0x15   // Serveur notifie les autres d'un join/leave

// =============================================
//  STRUCTURES
// =============================================

// Header universel pour tous les paquets UDP
struct Header {
    uint8_t  type;
    uint32_t seq;
} __attribute__((packed));

// Une entrée client dans la liste du serveur
struct ClientEntry {
    char     name[MAX_NAME_LEN];
    char     ip[MAX_IP_LEN];
    uint16_t port;               // port COMM du client
    uint32_t last_seen;          // timestamp (secondes)
} __attribute__((packed));

// Paquet de liste de clients (envoyé par le serveur en REG_LIST_RESP)
struct ClientListPacket {
    struct Header     header;
    uint8_t           count;
    struct ClientEntry clients[MAX_CLIENTS];
} __attribute__((packed));

// Paquet d'enregistrement / notification
struct RegisterPacket {
    struct Header header;
    char          name[MAX_NAME_LEN];
    uint16_t      comm_port;     // port sur lequel le client écoute en COMM
} __attribute__((packed));

#endif // PROTOCOL_H
