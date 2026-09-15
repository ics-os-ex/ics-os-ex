#!/usr/bin/env python3
"""GET / from a guest HTTP server via QEMU user-mode hostfwd."""
import socket
import sys
import time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 18080
req = b"GET / HTTP/1.0\r\nHost: ics-os\r\n\r\n"
want = b"ICS-OS HTTP OK"
deadline = time.time() + 60.0
while time.time() < deadline:
    try:
        sock = socket.create_connection(("127.0.0.1", port), timeout=1.0)
    except OSError:
        time.sleep(0.4)
        continue
    try:
        sock.settimeout(3.0)
        sock.sendall(req)
        data = b""
        while True:
            chunk = sock.recv(512)
            if not chunk:
                break
            data += chunk
            if want in data:
                sock.close()
                sys.exit(0)
    except OSError:
        pass
    finally:
        try:
            sock.close()
        except OSError:
            pass
    time.sleep(0.4)
sys.exit(1)
