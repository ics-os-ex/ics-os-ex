#!/usr/bin/env python3
"""Concurrent TCP echo server for ICS-OS make test-net / netstress."""
import socket
import sys
import threading
import time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 7778
duration = float(sys.argv[2]) if len(sys.argv) > 2 else 300.0


def handle(conn):
    try:
        conn.settimeout(5.0)
        data = conn.recv(2048)
        if data:
            conn.sendall(data)
    except OSError:
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass


srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", port))
srv.listen(64)
srv.settimeout(1.0)
deadline = time.time() + duration
while time.time() < deadline:
    try:
        conn, _addr = srv.accept()
    except socket.timeout:
        continue
    threading.Thread(target=handle, args=(conn,), daemon=True).start()
srv.close()
