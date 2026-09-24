#!/usr/bin/env python3
"""UDP echo server for ICS-OS make test-net / netstress."""
import socket
import sys
import time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 7777
duration = float(sys.argv[2]) if len(sys.argv) > 2 else 300.0

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("0.0.0.0", port))
sock.settimeout(1.0)
deadline = time.time() + duration
while time.time() < deadline:
    try:
        data, addr = sock.recvfrom(2048)
    except socket.timeout:
        continue
    try:
        sock.sendto(data, addr)
    except OSError:
        pass
sock.close()
