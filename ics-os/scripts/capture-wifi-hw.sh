#!/usr/bin/env bash
# Capture N150 (or any target) PCI Wi-Fi / network hardware via the Pico
# CDC bridge. Saves a transcript for driver work.
#
# Usage:
#   ./scripts/capture-wifi-hw.sh [PICO_IP] [OUTDIR]
# With no PICO_IP, discover the bridge from its validated /health response.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PICO_IP="${1:-}"
if [ -z "$PICO_IP" ]; then
  IFS= read -r PICO_IP < <("$ROOT/scripts/discover-pico.py")
fi
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUTDIR="${2:-$ROOT/wifi-hw-capture-$STAMP}"
mkdir -p "$OUTDIR"

echo "capture-wifi-hw: Pico=$PICO_IP out=$OUTDIR"

curl -fsS --max-time 8 "http://$PICO_IP/health" | tee "$OUTDIR/health.txt"
echo
curl -fsS --max-time 15 "http://$PICO_IP/status" | tee "$OUTDIR/status.txt" || {
  echo "capture-wifi-hw: STATUS failed (CDC down?). Power-cycle laptop/Pico." >&2
  exit 1
}
echo

# Snapshot log before injecting commands (noise baseline).
curl -fsS --max-time 15 "http://$PICO_IP/log" >"$OUTDIR/log-before.txt" || true

echo "capture-wifi-hw: injecting pciwifi..."
curl -fsS --max-time 10 -H 'Content-Type: text/plain' --data 'pciwifi' \
  "http://$PICO_IP/cmd" >/dev/null
sleep 2

echo "capture-wifi-hw: injecting pci (full bus dump)..."
curl -fsS --max-time 10 -H 'Content-Type: text/plain' --data 'pci' \
  "http://$PICO_IP/cmd" >/dev/null
sleep 3

curl -fsS --max-time 20 "http://$PICO_IP/log" | tee "$OUTDIR/log-after.txt" >/dev/null

# Extract marked sections when present.
awk '/WIFI_HW_BEGIN/{p=1} p{print} /WIFI_HW_END/{p=0}' \
  "$OUTDIR/log-after.txt" >"$OUTDIR/wifi-hw.txt" || true
awk '/PCI_HW_BEGIN/{p=1} p{print} /PCI_HW_END/{p=0}' \
  "$OUTDIR/log-after.txt" >"$OUTDIR/pci-hw.txt" || true

if ! grep -q 'WIFI_HW_BEGIN' "$OUTDIR/log-after.txt"; then
  echo "capture-wifi-hw: no WIFI_HW_BEGIN in log — kernel may be older than" >&2
  echo "  the pciwifi command, or CMD did not reach the console." >&2
  echo "  Full log: $OUTDIR/log-after.txt" >&2
  exit 2
fi

echo "capture-wifi-hw: wrote:"
ls -la "$OUTDIR"
echo "----- wifi-hw.txt -----"
cat "$OUTDIR/wifi-hw.txt"
