#!/usr/bin/env python3
"""Connect to a guest TCP listener via QEMU user-mode hostfwd."""
import socket
import sys
import time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 17779
payload = b"ICS-LISN!"
deadline = time.time() + 55.0
while time.time() < deadline:
    try:
        sock = socket.create_connection(("127.0.0.1", port), timeout=1.0)
    except OSError:
        time.sleep(0.4)
        continue
    try:
        sock.settimeout(2.0)
        sock.sendall(payload)
        data = sock.recv(64)
        if data == payload:
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
