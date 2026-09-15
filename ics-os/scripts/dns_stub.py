#!/usr/bin/env python3
"""Minimal UDP DNS stub: answers A queries for icsos.test → 10.0.2.2."""
import socket
import struct
import sys

NAME = b"\x05icsos\x04test\x00"
ANSWER_IP = bytes((10, 0, 2, 2))


def handle(data):
    if len(data) < 12:
        return None
    tid = data[0:2]
    # Skip question; rebuild a fixed answer for icsos.test
    # Header: QR=1 AA=1 RD=1 RA=1, QD=1 AN=1
    hdr = tid + bytes((0x85, 0x80, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00))
    # Question echo: find end of QNAME
    i = 12
    while i < len(data) and data[i] != 0:
        i += 1 + data[i]
    if i >= len(data):
        return None
    i += 1  # root
    if i + 4 > len(data):
        return None
    question = data[12:i + 4]
    # Answer: pointer to name at 0x0c, type A, class IN, TTL 60, RDATA 4
    ans = bytes((0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C,
                 0x00, 0x04)) + ANSWER_IP
    return hdr + question + ans


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 5353
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", port))
    sock.settimeout(0.5)
    while True:
        try:
            data, addr = sock.recvfrom(512)
        except socket.timeout:
            continue
        except KeyboardInterrupt:
            break
        resp = handle(data)
        if resp:
            sock.sendto(resp, addr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
