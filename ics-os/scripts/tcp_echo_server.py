#!/usr/bin/env python3
"""Minimal TCP echo server for ICS-OS make test-net."""
import socket
import sys

port = int(sys.argv[1]) if len(sys.argv) > 1 else 7778
srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", port))
srv.listen(1)
srv.settimeout(1.0)
rounds = 90
for _ in range(rounds):
    try:
        conn, _addr = srv.accept()
    except socket.timeout:
        continue
    try:
        conn.settimeout(2.0)
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
srv.close()
