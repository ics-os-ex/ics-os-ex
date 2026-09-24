#!/usr/bin/env bash
# Push Kernel64.bin to a live N150 (or QEMU+Pico) via the Pico CDC bridge
# HTTP API. The target stages the ELF, rewrites /icsos/vmdex (or /vmdex) on
# the USB ESP, ACKs, then kexecs — no thumb-drive reflash required.
#
# Usage:
#   ./scripts/remote-kexec.sh [PICO_IP] [KERNEL]
# With no PICO_IP, discovers the bridge; kernel defaults to Kernel64.bin.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PICO_IP="${1:-}"
if [ -z "$PICO_IP" ]; then
  IFS= read -r PICO_IP < <("$ROOT/scripts/discover-pico.py")
fi
KERNEL="${2:-$ROOT/kernel/Kernel64.bin}"

if [ ! -f "$KERNEL" ]; then
  echo "remote-kexec: missing $KERNEL (run: make -C kernel bzImage)" >&2
  exit 1
fi

SZ=$(wc -c <"$KERNEL" | tr -d ' ')
CRC32=$(python3 -c 'import sys,zlib; print(zlib.crc32(open(sys.argv[1], "rb").read()) & 0xffffffff)' "$KERNEL")
echo "remote-kexec: POST $KERNEL ($SZ bytes) -> http://$PICO_IP/kexec"
echo "remote-kexec: crc32=$CRC32"
echo "remote-kexec: health before:"
if ! curl -fsS --max-time 5 "http://$PICO_IP/health"; then
  echo
  echo "remote-kexec: Pico HTTP dead (often after a 0.4 /kexec OOM)." >&2
  echo "  Reset the Pico, flash extras/pico2w-serial-bridge/main.py (0.5+)," >&2
  echo "  then confirm: curl http://$PICO_IP/health  # expect pico=0.5-*" >&2
  exit 1
fi
echo
if ! curl -fsS --max-time 5 "http://$PICO_IP/health" | grep -q 'pico=0\.[7-9]'; then
  echo "remote-kexec: warning: need pico=0.7+ (ready/done kexec handshake)" >&2
fi

# Pico waits up to 180s for the KEXEC RPC ACK (stream + FAT rewrite).
# Requires Pico bridge firmware pico=0.6-* (paced stream; 0.5 could wedge).
curl -fsS --max-time 420 \
  -H 'Content-Type: application/octet-stream' \
  -H "X-CRC32: $CRC32" \
  --data-binary @"$KERNEL" \
  "http://$PICO_IP/kexec"
echo

echo "remote-kexec: waiting for new kernel STATUS..."
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
  sleep 5
  if curl -fsS --max-time 8 "http://$PICO_IP/health" 2>/dev/null; then
    echo
    if curl -fsS --max-time 15 "http://$PICO_IP/status" 2>/dev/null; then
      echo
      echo "remote-kexec: target responded after kexec"
      exit 0
    fi
  fi
  echo "remote-kexec: still waiting ($((i * 5))s)..."
done

echo "remote-kexec: timed out waiting for post-kexec health/status" >&2
exit 1
