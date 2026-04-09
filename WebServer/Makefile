CC      = gcc
CFLAGS  = -Wall -Wextra -O2
LIBS    = -lpthread
SRC     = server.c
BIN     = server
DIST    = ./dist

.PHONY: all clean install deps

all: deps $(BIN) dist

deps:
	@which openssl > /dev/null 2>&1 || (echo "Installation de libssl-dev..." && sudo apt-get install -y libssl-dev)

$(BIN): $(SRC) protocol.h
	$(CC) $(CFLAGS) -o $(BIN) $(SRC) $(LIBS)
	@echo "Serveur compilé : ./$(BIN)"

dist: $(BIN)
	@mkdir -p $(DIST)
	@cp client.py $(DIST)/
	@echo "Dist prêt : $(DIST)/"
	@ls -lh $(DIST)/

clean:
	rm -f $(BIN)
	rm -rf $(DIST)

install: all
	@mkdir -p /opt/comserver/dist
	@cp $(BIN) /opt/comserver/
	@cp $(DIST)/* /opt/comserver/dist/
	@printf '[Unit]\nDescription=Serveur WebSocket\nAfter=network-online.target\nWants=network-online.target\n\n[Service]\nType=simple\nUser=nobody\nWorkingDirectory=/opt/comserver\nExecStart=/opt/comserver/server\nRestart=always\nRestartSec=5\n\n[Install]\nWantedBy=multi-user.target\n' > /etc/systemd/system/comserver.service
	@systemctl daemon-reload
	@systemctl enable comserver
	@systemctl start comserver
	@echo "Service installé et démarré !"
