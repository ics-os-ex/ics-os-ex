#!/usr/bin/env python3
"""Minimal UDP echo server for ICS-OS make test-net."""
import socket
import sys

port = int(sys.argv[1]) if len(sys.argv) > 1 else 7777
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("0.0.0.0", port))
sock.settimeout(1.0)
deadline_rounds = 180
for _ in range(deadline_rounds):
    try:
        data, addr = sock.recvfrom(2048)
    except socket.timeout:
        continue
    try:
        sock.sendto(data, addr)
    except OSError:
        pass
sock.close()
