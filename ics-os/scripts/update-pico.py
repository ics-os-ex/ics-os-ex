#!/usr/bin/env python3
"""Atomically update the ICS-OS Pico bridge firmware over the lab LAN."""
import argparse
import hashlib
import pathlib
import subprocess
import time
import urllib.request


def discover(script):
    output = subprocess.check_output([str(script)], text=True)
    return output.splitlines()[0].strip()


def main():
    root = pathlib.Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument("firmware", nargs="?", default=str(
        root.parent / "extras" / "pico2w-serial-bridge" / "main.py"))
    parser.add_argument("--ip")
    args = parser.parse_args()
    data = pathlib.Path(args.firmware).read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    discover_script = root / "scripts" / "discover-pico.py"
    ip = args.ip or discover(discover_script)
    request = urllib.request.Request(
        "http://%s/update" % ip, data=data, method="POST",
        headers={"X-SHA256": digest, "Content-Type": "application/octet-stream"})
    with urllib.request.urlopen(request, timeout=45) as response:
        reply = response.read().decode("utf-8", "replace").strip()
    if digest not in reply:
        raise SystemExit("Pico did not confirm the uploaded SHA-256")
    print(reply)
    current = None
    for _ in range(6):
        time.sleep(5)
        try:
            current = discover(discover_script)
            break
        except subprocess.CalledProcessError:
            pass
    if current is None:
        raise SystemExit("update installed, but Pico was not rediscovered after 30s")
    print("Pico restarted and discovered at %s" % current)


if __name__ == "__main__":
    main()
