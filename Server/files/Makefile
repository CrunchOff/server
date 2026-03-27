# ── Makefile ──────────────────────────────────────────────────────────────────
# Compile : make          → server + client_linux
#           make server   → serveur seulement
#           make client   → client Linux seulement
#           make clean    → nettoie
#           make install  → installe le serveur en service systemd
# ─────────────────────────────────────────────────────────────────────────────

CC      = gcc
CFLAGS  = -Wall -Wextra -O2
LDFLAGS = -lpthread

SERVER_SRC = server.c
CLIENT_SRC = client.c

SERVER_BIN = server
CLIENT_BIN = client_linux

DIST_DIR = ./dist

.PHONY: all server client clean install dist

all: server client dist

server: $(SERVER_SRC) protocol.h
	$(CC) $(CFLAGS) -o $(SERVER_BIN) $(SERVER_SRC) $(LDFLAGS)
	@echo "✓ Serveur compilé : ./$(SERVER_BIN)"

client: $(CLIENT_SRC) protocol.h
	$(CC) $(CFLAGS) -o $(CLIENT_BIN) $(CLIENT_SRC) $(LDFLAGS)
	@echo "✓ Client Linux compilé : ./$(CLIENT_BIN)"

# Prépare le dossier de distribution avec les fichiers compilés + sources
dist: server client
	@mkdir -p $(DIST_DIR)
	@cp $(CLIENT_BIN) $(DIST_DIR)/
	@zip -j $(DIST_DIR)/sources.zip client.c server.c protocol.h Makefile 2>/dev/null || \
		tar -czf $(DIST_DIR)/sources.tar.gz client.c server.c protocol.h Makefile
	@echo "✓ Dossier dist/ prêt"
	@ls -lh $(DIST_DIR)/

clean:
	rm -f $(SERVER_BIN) $(CLIENT_BIN)
	rm -rf $(DIST_DIR)

# Installe le serveur comme service systemd (lance au démarrage)
install: server dist
	@echo "Installation du service systemd..."
	@mkdir -p /opt/comserver/dist
	@cp $(SERVER_BIN) /opt/comserver/
	@cp $(DIST_DIR)/* /opt/comserver/dist/ 2>/dev/null || true
	@cp protocol.h client.c server.c /opt/comserver/
	@cat > /etc/systemd/system/comserver.service << 'EOF'
[Unit]
Description=Serveur de communication
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=nobody
WorkingDirectory=/opt/comserver
ExecStart=/opt/comserver/server
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF
	@systemctl daemon-reload
	@systemctl enable comserver
	@systemctl start comserver
	@echo "✓ Service installé et démarré !"
	@systemctl status comserver
