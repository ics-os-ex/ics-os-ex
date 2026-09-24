#!/usr/bin/env python3
"""Host helpers for ICS-OS netbench: TCP sink, TCP source, UDP echo."""
import socket
import struct
import sys
import threading
import time

DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 300.0
TCP_BYTES = 2 * 1024 * 1024


def tcp_sink(port=7791):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(8)
    srv.settimeout(1.0)
    deadline = time.time() + DURATION
    while time.time() < deadline:
        try:
            conn, _ = srv.accept()
        except socket.timeout:
            continue
        try:
            conn.settimeout(60.0)
            got = 0
            t0 = time.time()
            while True:
                data = conn.recv(65536)
                if not data:
                    break
                got += len(data)
                if len(data) <= 64:
                    conn.sendall(data)
            dt = max(time.time() - t0, 1e-6)
            if got >= 64 * 1024:
                kbps = int((got * 8) / (dt * 1000.0))
                print(
                    f"HOST_SINK bytes={got} ms={int(dt*1000)} kbps={kbps}",
                    flush=True,
                )
        except OSError:
            pass
        finally:
            try:
                conn.close()
            except OSError:
                pass
    srv.close()


def tcp_source(port=7792):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(4)
    srv.settimeout(1.0)
    payload = b"\x5a" * 1024
    deadline = time.time() + DURATION
    while time.time() < deadline:
        try:
            conn, _ = srv.accept()
        except socket.timeout:
            continue
        try:
            conn.settimeout(60.0)
            sent = 0
            t0 = time.time()
            while sent < TCP_BYTES:
                n = conn.send(payload[: min(1024, TCP_BYTES - sent)])
                if n <= 0:
                    break
                sent += n
            dt = max(time.time() - t0, 1e-6)
            kbps = int((sent * 8) / (dt * 1000.0))
            print(
                f"HOST_SOURCE bytes={sent} ms={int(dt*1000)} kbps={kbps}",
                flush=True,
            )
        except OSError:
            pass
        finally:
            try:
                conn.close()
            except OSError:
                pass
    srv.close()


def udp_echo(port=7793):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", port))
    sock.settimeout(1.0)
    deadline = time.time() + DURATION
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


def main():
    threads = [
        threading.Thread(target=tcp_sink, daemon=True),
        threading.Thread(target=tcp_source, daemon=True),
        threading.Thread(target=udp_echo, daemon=True),
    ]
    for t in threads:
        t.start()
    # Give listeners time to bind before QEMU starts.
    time.sleep(0.2)
    print("NETBENCH_HOST_READY", flush=True)
    deadline = time.time() + DURATION
    while time.time() < deadline:
        time.sleep(0.5)


if __name__ == "__main__":
    main()
