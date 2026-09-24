#!/usr/bin/env python3
"""Exercise ICS-OS telnetd via QEMU user-mode hostfwd."""
import socket
import sys
import time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 10023
want = b"ICS-TELNET-OK"
deadline = time.time() + 90.0

while time.time() < deadline:
    try:
        sock = socket.create_connection(("127.0.0.1", port), timeout=2.0)
    except OSError:
        time.sleep(0.5)
        continue
    try:
        sock.settimeout(8.0)
        data = b""
        t0 = time.time()
        while time.time() - t0 < 5.0:
            try:
                chunk = sock.recv(256)
            except socket.timeout:
                break
            if not chunk:
                break
            data += chunk
            if b"telnet$" in data or b"telnetd" in data or b"ICS-OS" in data:
                break
        sock.sendall(b"echo ICS-TELNET-OK\r\n")
        t0 = time.time()
        while time.time() - t0 < 8.0:
            try:
                chunk = sock.recv(512)
            except socket.timeout:
                break
            if not chunk:
                break
            data += chunk
            if want in data:
                sock.sendall(b"exit\r\n")
                time.sleep(0.3)
                sock.close()
                sys.exit(0)
        sock.sendall(b"exit\r\n")
    except OSError:
        pass
    finally:
        try:
            sock.close()
        except OSError:
            pass
    time.sleep(0.5)

sys.exit(1)
