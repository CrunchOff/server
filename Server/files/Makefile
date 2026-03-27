CC      = gcc
CFLAGS  = -Wall -Wextra -O2
LDFLAGS = -lpthread

SERVER_SRC = server.c
CLIENT_SRC = client.c
SERVER_BIN = server
CLIENT_BIN = client_linux
DIST_DIR   = ./dist

.PHONY: all server client clean install dist

all: server client dist

server: $(SERVER_SRC) protocol.h
	$(CC) $(CFLAGS) -o $(SERVER_BIN) $(SERVER_SRC) $(LDFLAGS)
	@echo "Serveur compile : ./$(SERVER_BIN)"

client: $(CLIENT_SRC) protocol.h
	$(CC) $(CFLAGS) -o $(CLIENT_BIN) $(CLIENT_SRC) $(LDFLAGS)
	@echo "Client Linux compile : ./$(CLIENT_BIN)"

dist: server client
	@mkdir -p $(DIST_DIR)
	@cp $(CLIENT_BIN) $(DIST_DIR)/
	@zip -j $(DIST_DIR)/sources.zip client.c server.c protocol.h Makefile 2>/dev/null || \
		tar -czf $(DIST_DIR)/sources.tar.gz client.c server.c protocol.h Makefile
	@echo "Dossier dist/ pret"
	@ls -lh $(DIST_DIR)/

clean:
	rm -f $(SERVER_BIN) $(CLIENT_BIN)
	rm -rf $(DIST_DIR)

install: server dist
	@echo "Installation du service systemd..."
	@mkdir -p /opt/comserver/dist
	@cp $(SERVER_BIN) /opt/comserver/
	@cp $(DIST_DIR)/* /opt/comserver/dist/ 2>/dev/null || true
	@cp protocol.h client.c server.c /opt/comserver/
	@printf '[Unit]\nDescription=Serveur de communication\nAfter=network-online.target\nWants=network-online.target\n\n[Service]\nType=simple\nUser=nobody\nWorkingDirectory=/opt/comserver\nExecStart=/opt/comserver/server\nRestart=always\nRestartSec=5\n\n[Install]\nWantedBy=multi-user.target\n' > /etc/systemd/system/comserver.service
	@systemctl daemon-reload
	@systemctl enable comserver
	@systemctl start comserver
	@echo "Service installe et demarre !"
	@systemctl status comserver