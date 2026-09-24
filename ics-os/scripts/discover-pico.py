#!/usr/bin/env python3
"""Locate the ICS-OS Pico bridge by validating its HTTP /health response."""
import argparse
import concurrent.futures
import ipaddress
import re
import subprocess
import urllib.request


def local_subnets():
    text = subprocess.check_output(
        ["ip", "-4", "route", "show", "scope", "link"], text=True
    )
    nets = []
    for line in text.splitlines():
        field = line.split()[0] if line.split() else ""
        if re.fullmatch(r"\d+\.\d+\.\d+\.\d+/\d+", field):
            net = ipaddress.ip_network(field, strict=False)
            if net.prefixlen >= 24:
                nets.append(net)
    return nets


def probe(ip, timeout):
    try:
        with urllib.request.urlopen("http://%s/health" % ip,
                                    timeout=timeout) as response:
            body = response.read(4096).decode("utf-8", "replace")
        if "pico=" in body and "mode=" in body and "ip=" in body:
            return str(ip), body
    except Exception:
        pass
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--subnet", action="append",
                        help="CIDR to scan; defaults to local IPv4 /24 networks")
    parser.add_argument("--timeout", type=float, default=0.6)
    parser.add_argument("--all", action="store_true", help="print every bridge")
    args = parser.parse_args()
    nets = ([ipaddress.ip_network(value, strict=False) for value in args.subnet]
            if args.subnet else local_subnets())
    hosts = [host for net in nets for host in net.hosts()]
    found = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=48) as pool:
        futures = [pool.submit(probe, host, args.timeout) for host in hosts]
        for future in concurrent.futures.as_completed(futures):
            result = future.result()
            if result:
                found.append(result)
                if not args.all:
                    break
    if not found:
        raise SystemExit("ICS-OS Pico bridge not found")
    for ip, body in sorted(found):
        print(ip)
        for line in body.splitlines():
            if line.startswith(("pico=", "kernel=", "mode=", "ip=")):
                print("  " + line)


if __name__ == "__main__":
    main()
