#!/usr/bin/env python3
"""
client.py — Client de communication WebSocket
 
Dépendances : pip install websockets
Usage       : python3 client.py <IP_DU_SERVEUR>
              python3 client.py <IP_DU_SERVEUR> <PORT>   (défaut: 8080)
              python3 client.py wss://mon-serveur.ts.net  (Tailscale Funnel)
"""

import asyncio
import sys
import os
import json
import struct
import base64
import threading
import signal
from pathlib import Path

try:
    import websockets
except ImportError:
    print("\n[!] Module 'websockets' manquant.")
    print("    Installe-le avec : pip install websockets\n")
    sys.exit(1)

# ── Configuration ──────────────────────────────────────────────────────────────
DEFAULT_PORT    = 8080
MAX_FILE_SIZE   = 10 * 1024 * 1024  # 10 MB
HEARTBEAT_SECS  = 15
CHUNK_SIZE      = 32 * 1024         # 32 KB par chunk binaire

# ── État global ────────────────────────────────────────────────────────────────
g_peers     = []          # liste des clients connectés
g_my_name   = "Anonyme"
g_server_url= ""
g_target_ip = ""          # IP de la cible actuelle
g_ws        = None        # websocket actif
g_loop      = None        # event loop asyncio

# Réception de fichier en cours
g_recv_file = {
    "name": None,
    "handle": None,
    "size": 0,
    "received": 0,
    "chunks": []  # buffer de chunks binaires
}

# ══════════════════════════════════════════════════════════════════════════════
#  UTILITAIRES
# ══════════════════════════════════════════════════════════════════════════════

def color(code, text):
    """ANSI colors — désactivé sur Windows si pas de support"""
    if os.name == 'nt':
        try:
            import ctypes
            ctypes.windll.kernel32.SetConsoleMode(
                ctypes.windll.kernel32.GetStdHandle(-11), 7)
        except Exception:
            return text
    return f"\033[{code}m{text}\033[0m"

def print_peers():
    print()
    if not g_peers:
        print(color("33", "  Aucun autre client connecté."))
        return
    print(color("36", f"  ┌────┬{'─'*20}┬{'─'*22}┐"))
    print(color("36", f"  │ #  │ {'Pseudo':<18} │ {'IP':<20} │"))
    print(color("36", f"  ├────┼{'─'*20}┼{'─'*22}┤"))
    for i, p in enumerate(g_peers):
        marker = " ◀" if p["ip"] == g_target_ip else ""
        print(color("36", f"  │ {i+1:<2} │ {p['name']:<18} │ {p['ip']:<20} │{marker}"))
    print(color("36", f"  └────┴{'─'*20}┴{'─'*22}┘"))
    print()

def print_menu():
    target_str = f"{g_target_ip}" if g_target_ip else color("33", "aucune")
    print(f"\n{'═'*44}")
    print(f"  {color('96', g_my_name)}  │  {g_server_url}")
    print(f"  Cible : {color('92', target_str)}")
    print(f"{'═'*44}")
    print("  1. Envoyer un message")
    print("  2. Envoyer un fichier")
    print("  3. Voir les clients connectés")
    print("  4. Choisir une cible dans la liste")
    print("  5. Saisir une IP manuellement")
    print("  0. Quitter")
    print("> ", end="", flush=True)

# ══════════════════════════════════════════════════════════════════════════════
#  ENVOI DE MESSAGES
# ══════════════════════════════════════════════════════════════════════════════

async def send_json(ws, obj):
    await ws.send(json.dumps(obj))

async def send_chat(ws, to_ip, text):
    await send_json(ws, {
        "type": "chat",
        "to":   to_ip,
        "from": "",       # le serveur connaît notre IP
        "name": g_my_name,
        "text": text
    })

async def send_file(ws, to_ip, filepath):
    path = Path(filepath)
    if not path.exists():
        print(color("31", f"\n[!] Fichier introuvable : {filepath}"))
        return
    size = path.stat().st_size
    if size > MAX_FILE_SIZE:
        print(color("31", f"\n[!] Fichier trop grand ({size/1024/1024:.1f} MB, max 10 MB)"))
        return

    filename = path.name
    print(color("33", f"\n[→] Envoi de {filename} ({size/1024:.1f} KB) vers {to_ip}..."))

    # FILE_START
    await send_json(ws, {
        "type": "file_start",
        "to":   to_ip,
        "from": "",
        "name": filename,
        "size": size
    })

    # Chunks binaires : [2 octets big-endian = len(header_json)][header_json][data]
    sent = 0
    with open(filepath, "rb") as f:
        chunk_idx = 0
        while True:
            data = f.read(CHUNK_SIZE)
            if not data:
                break
            header = json.dumps({
                "type":  "file_data",
                "to":    to_ip,
                "from":  "",
                "name":  filename,
                "chunk": chunk_idx
            }).encode()
            hdr_len = struct.pack(">H", len(header))
            await ws.send(bytes(hdr_len) + header + data)
            sent += len(data)
            pct = sent * 100 // size
            print(f"\r  Progression : {pct}%  ", end="", flush=True)
            chunk_idx += 1
            await asyncio.sleep(0)  # yield

    # FILE_END
    await send_json(ws, {
        "type": "file_end",
        "to":   to_ip,
        "from": "",
        "name": filename,
        "size": size
    })
    print(color("92", f"\r[✓] {filename} envoyé ({size/1024:.1f} KB)          "))

# ══════════════════════════════════════════════════════════════════════════════
#  RÉCEPTION DES MESSAGES
# ══════════════════════════════════════════════════════════════════════════════

def handle_message(msg):
    """Appelé depuis le thread asyncio pour chaque message reçu"""
    global g_peers, g_recv_file

    if isinstance(msg, bytes):
        # Frame binaire : chunk de fichier
        if len(msg) < 2:
            return
        hdr_len = struct.unpack(">H", msg[:2])[0]
        if hdr_len + 2 > len(msg):
            return
        hdr = json.loads(msg[2:2 + hdr_len])
        data = msg[2 + hdr_len:]

        if hdr.get("type") == "file_data":
            if g_recv_file["handle"] is None:
                # Ouvre le fichier si pas encore fait
                fname = f"recu_{hdr.get('name', 'fichier')}"
                g_recv_file["handle"] = open(fname, "wb")
                g_recv_file["name"] = fname
            g_recv_file["handle"].write(data)
            g_recv_file["received"] += len(data)
        return

    # Frame texte : JSON
    try:
        obj = json.loads(msg)
    except json.JSONDecodeError:
        return

    t = obj.get("type")

    if t == "clients":
        g_peers = obj.get("list", [])

    elif t == "notify":
        event = obj.get("event")
        name  = obj.get("name", "?")
        ip    = obj.get("ip", "?")
        if event == "join":
            print(color("92", f"\n  [+] {name} ({ip}) a rejoint le réseau"))
            print("> ", end="", flush=True)
        elif event == "leave":
            print(color("91", f"\n  [-] {name} ({ip}) a quitté le réseau"))
            print("> ", end="", flush=True)
        # Rafraîchit la liste
        if g_ws and g_loop:
            asyncio.run_coroutine_threadsafe(
                send_json(g_ws, {"type": "list"}), g_loop)

    elif t == "chat":
        sender = obj.get("name", obj.get("from", "?"))
        text   = obj.get("text", "")
        print(color("96", f"\n  [{sender}] : {text}"))
        print("> ", end="", flush=True)

    elif t == "file_start":
        sender   = obj.get("name", "?")
        filename = obj.get("name", "fichier")
        size     = obj.get("size", 0)
        print(color("33", f"\n  [↓] Réception de {filename} ({size/1024:.1f} KB) de {obj.get('from','?')}..."))
        print("> ", end="", flush=True)
        g_recv_file["received"] = 0
        g_recv_file["size"]     = size

    elif t == "file_end":
        if g_recv_file["handle"]:
            g_recv_file["handle"].close()
            g_recv_file["handle"] = None
            fname = g_recv_file["name"]
            size  = g_recv_file["received"]
            print(color("92", f"\n  [✓] {fname} reçu ({size/1024:.1f} KB)"))
            print("> ", end="", flush=True)
        g_recv_file = {"name": None, "handle": None, "size": 0, "received": 0, "chunks": []}

    elif t == "pong":
        pass  # heartbeat ok

    elif t == "error":
        print(color("31", f"\n  [!] Erreur serveur : {obj.get('msg', '?')}"))

# ══════════════════════════════════════════════════════════════════════════════
#  BOUCLE ASYNCIO — connexion WebSocket
# ══════════════════════════════════════════════════════════════════════════════

async def ws_loop(url, name):
    global g_ws, g_loop
    g_loop = asyncio.get_event_loop()

    print(color("33", f"\n  Connexion à {url}..."))
    try:
        async with websockets.connect(
            url,
            ping_interval=None,     # on gère le heartbeat manuellement
            max_size=WS_BUF_SIZE if 'WS_BUF_SIZE' in dir() else 11*1024*1024,
            open_timeout=10
        ) as ws:
            g_ws = ws
            print(color("92", "  Connecté !\n"))

            # Enregistrement
            await send_json(ws, {"type": "register", "name": name})

            # Heartbeat en arrière-plan
            async def heartbeat():
                while True:
                    await asyncio.sleep(HEARTBEAT_SECS)
                    try:
                        await send_json(ws, {"type": "ping"})
                    except Exception:
                        break

            asyncio.create_task(heartbeat())

            # Écoute les messages
            async for msg in ws:
                handle_message(msg)

    except (websockets.exceptions.ConnectionClosed,
            websockets.exceptions.InvalidURI,
            OSError) as e:
        print(color("31", f"\n  [!] Connexion perdue : {e}"))
    finally:
        g_ws = None

# ══════════════════════════════════════════════════════════════════════════════
#  THREAD INTERFACE — boucle menu dans le thread principal
# ══════════════════════════════════════════════════════════════════════════════

def menu_loop():
    global g_target_ip

    # Attend que la connexion soit établie
    import time
    for _ in range(20):
        if g_ws is not None:
            break
        time.sleep(0.3)

    if g_ws is None:
        print(color("31", "\n[!] Impossible de se connecter au serveur."))
        os._exit(1)

    while True:
        print_menu()
        try:
            choix = input().strip()
        except (EOFError, KeyboardInterrupt):
            choix = "0"

        if choix == "1":
            if not g_target_ip:
                print(color("31", "  [!] Aucune cible. Utilise option 4 ou 5."))
                continue
            print("  Message : ", end="", flush=True)
            text = input().strip()
            if text and g_ws and g_loop:
                asyncio.run_coroutine_threadsafe(
                    send_chat(g_ws, g_target_ip, text), g_loop)

        elif choix == "2":
            if not g_target_ip:
                print(color("31", "  [!] Aucune cible. Utilise option 4 ou 5."))
                continue
            print("  Chemin du fichier : ", end="", flush=True)
            filepath = input().strip().strip('"')
            if filepath and g_ws and g_loop:
                asyncio.run_coroutine_threadsafe(
                    send_file(g_ws, g_target_ip, filepath), g_loop)

        elif choix == "3":
            print_peers()

        elif choix == "4":
            print_peers()
            if not g_peers:
                continue
            print("  Numéro : ", end="", flush=True)
            try:
                idx = int(input().strip()) - 1
                if 0 <= idx < len(g_peers):
                    g_target_ip = g_peers[idx]["ip"]
                    print(color("92", f"  [✓] Cible : {g_peers[idx]['name']} ({g_target_ip})"))
                else:
                    print(color("31", "  [!] Numéro invalide."))
            except ValueError:
                print(color("31", "  [!] Entrée invalide."))

        elif choix == "5":
            print("  IP : ", end="", flush=True)
            ip = input().strip()
            if ip:
                g_target_ip = ip
                print(color("92", f"  [✓] Cible : {g_target_ip}"))

        elif choix == "0":
            if g_ws and g_loop:
                asyncio.run_coroutine_threadsafe(
                    send_json(g_ws, {"type": "bye"}), g_loop)
            import time; time.sleep(0.3)
            print("  Au revoir !")
            os._exit(0)

# ══════════════════════════════════════════════════════════════════════════════
#  MAIN
# ══════════════════════════════════════════════════════════════════════════════

def main():
    global g_my_name, g_server_url

    # Parse les arguments
    if len(sys.argv) < 2:
        print(f"Usage : python3 {sys.argv[0]} <IP_ou_URL> [port]")
        print(f"  Ex  : python3 {sys.argv[0]} 192.168.1.42")
        print(f"        python3 {sys.argv[0]} wss://mon-serveur.ts.net")
        sys.exit(1)

    arg = sys.argv[1]

    # Construit l'URL WebSocket
    if arg.startswith("ws://") or arg.startswith("wss://"):
        # URL complète fournie (ex: wss://debian-stick.tail1234.ts.net)
        if not arg.endswith("/ws"):
            g_server_url = arg.rstrip("/") + "/ws"
        else:
            g_server_url = arg
    else:
        # IP ou hostname simple
        port = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_PORT
        g_server_url = f"ws://{arg}:{port}/ws"

    print(f"\n{'═'*44}")
    print(f"  CLIENT DE COMMUNICATION")
    print(f"  Serveur : {g_server_url}")
    print(f"{'═'*44}")
    print("  Ton pseudo : ", end="", flush=True)
    g_my_name = input().strip()
    if not g_my_name:
        g_my_name = "Anonyme"

    # Lance le menu dans un thread séparé
    t = threading.Thread(target=menu_loop, daemon=True)
    t.start()

    # Lance la boucle WebSocket dans le thread principal
    try:
        asyncio.run(ws_loop(g_server_url, g_my_name))
    except KeyboardInterrupt:
        pass

    print("\n  Déconnecté.")

if __name__ == "__main__":
    main()
