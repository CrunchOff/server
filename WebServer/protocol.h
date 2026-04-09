#ifndef PROTOCOL_H
#define PROTOCOL_H

// =============================================
//  PORTS
// =============================================
#define PORT_HTTP       8080    // HTTP + WebSocket upgrade

// =============================================
//  TAILLES
// =============================================
#define MAX_CLIENTS     64
#define MAX_NAME_LEN    32
#define MAX_IP_LEN      46      // IPv6 ready
#define WS_BUF_SIZE     (1024 * 1024 * 11)  // 11 MB max par message

// =============================================
//  TYPES DE MESSAGES JSON  (champ "type")
// =============================================
// Client → Serveur
#define MSG_REGISTER    "register"    // {"type":"register","name":"..."}
#define MSG_LIST_REQ    "list"        // {"type":"list"}
#define MSG_HEARTBEAT   "ping"        // {"type":"ping"}
#define MSG_UNREGISTER  "bye"         // {"type":"bye"}

// Serveur → Client
#define MSG_LIST_RESP   "clients"     // {"type":"clients","list":[...]}
#define MSG_NOTIFY      "notify"      // {"type":"notify","msg":"..."}
#define MSG_PONG        "pong"        // {"type":"pong"}
#define MSG_ERROR       "error"       // {"type":"error","msg":"..."}

// Client → Client (relayé par le serveur)
#define MSG_CHAT        "chat"        // {"type":"chat","to":"ip","from":"ip","name":"...","text":"..."}
#define MSG_FILE_START  "file_start"  // {"type":"file_start","to":"ip","from":"ip","name":"...","size":N}
#define MSG_FILE_DATA   "file_data"   // binaire préfixé par header JSON (voir protocol)
#define MSG_FILE_END    "file_end"    // {"type":"file_end","to":"ip","from":"ip","name":"..."}

#endif
