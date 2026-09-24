#!/bin/bash
# Pass a physical Pico 2 W MicroPython CDC-ACM gadget (2e8a:0005) into QEMU
# q35 xHCI next to the MSC root. This is the N150 IN/RPC path: emulated
# usb-serial (FTDI) only proves host bulk OUT. The Pico stays on Wi-Fi;
# the host curls its HTTP API the same way as the laptop.
#
# SKIP (exit 0) if no Pico is plugged into this machine.
#
# PICO_IP                 default 192.168.0.174
# CDC_PICO_TIMEOUT_SECONDS default 120
# CDC_PICO_POLL=1         add xhci-poll-test (N150-like; default on)
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SOURCE_IMAGE="${1:-ics-os-usb.img}"
TIMEOUT_SECONDS="${CDC_PICO_TIMEOUT_SECONDS:-120}"
QEMU_BIN="${QEMU_X64:-qemu-system-x86_64}"
PICO_VID="${PICO_VID:-2e8a}"
PICO_PID="${PICO_PID:-0005}"
PICO_IP="${PICO_IP:-192.168.0.174}"
POLL_CMDLINE="xhci-poll-test"
if [ "${CDC_PICO_POLL:-1}" = "0" ]; then
    POLL_CMDLINE=""
fi
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-$$"
ARTIFACT_DIR="${CDC_PICO_ARTIFACT_DIR:-/tmp/icsos-tests/xhci-cdc-pico-$RUN_ID}"
WORK_DIR="$ARTIFACT_DIR/work"
RAW_IMAGE="$WORK_DIR/ics-os-usb.img"
ISO_ROOT="$WORK_DIR/iso-root"
BOOT_ISO="$WORK_DIR/boot.iso"
SERIAL_LOG="$ARTIFACT_DIR/serial.log"
HTTP_LOG="$ARTIFACT_DIR/pico-http.log"
QEMU_PID=""
ROOT_OK=0
CDC_OK=0
RX_OK=0
STATUS_OK=0
PICO_VER_OK=0
KVER_OK=0
STATUS_VER_OK=0
FINAL_HEALTH=""

cleanup() {
    if [ -n "$QEMU_PID" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

pico_present() {
    lsusb -d "${PICO_VID}:${PICO_PID}" >/dev/null 2>&1
}

if ! pico_present; then
    echo "SKIP: plug Pico 2 W (${PICO_VID}:${PICO_PID}) into this host for CDC IN/RPC"
    exit 0
fi

for tool in "$QEMU_BIN" grub-mkrescue curl lsusb python3 sfdisk; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "cdc-pico: missing required tool: $tool" >&2
        exit 2
    fi
done
if [ ! -f "$SOURCE_IMAGE" ]; then
    echo "cdc-pico: image not found: $SOURCE_IMAGE" >&2
    exit 2
fi
if [ ! -f kernel/Kernel64.bin ]; then
    echo "cdc-pico: kernel/Kernel64.bin is missing; run make vmdex" >&2
    exit 2
fi

mkdir -p "$WORK_DIR" "$ISO_ROOT/boot/grub" "$ARTIFACT_DIR"
touch "$SERIAL_LOG" "$HTTP_LOG"
cp "$SOURCE_IMAGE" "$RAW_IMAGE"
cp kernel/Kernel64.bin "$ISO_ROOT/vmdex"
printf '%s\n' 'set timeout=0' \
    "menuentry \"ics\" { multiboot2 /vmdex ${POLL_CMDLINE}; boot }" \
    > "$ISO_ROOT/boot/grub/grub.cfg"
grub-mkrescue -o "$BOOT_ISO" "$ISO_ROOT" >/dev/null 2>&1

"$QEMU_BIN" -machine q35 -smp 1 -nographic -no-reboot -m 128M \
    -cdrom "$BOOT_ISO" -boot d \
    -drive if=none,id=stick,format=raw,file="$RAW_IMAGE" \
    -device qemu-xhci,id=xhci \
    -device usb-storage,bus=xhci.0,drive=stick \
    -device usb-host,vendorid=0x${PICO_VID},productid=0x${PICO_PID},bus=xhci.0 \
    < /dev/null > "$SERIAL_LOG" 2>&1 &
QEMU_PID=$!

pico_get() {
    curl -sS -m 8 "$1" 2>/dev/null || true
}

DEADLINE=$((SECONDS + TIMEOUT_SECONDS))
STATE=timeout
while [ $SECONDS -lt $DEADLINE ]; do
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        STATE=exited
        break
    fi
    LOG="$(pico_get "http://${PICO_IP}/log")"
    STATUS="$(pico_get "http://${PICO_IP}/status")"
    {
        echo "---- $(date -u +%H:%M:%S) ----"
        echo "$LOG" | tail -c 400
        echo
        echo "STATUS: $STATUS"
    } >> "$HTTP_LOG"
    HEALTH="$(pico_get "http://${PICO_IP}/health")"
    echo "$LOG" | grep -aq 'USB_CDC_CONSOLE_OK' && CDC_OK=1
    echo "$LOG" | grep -aq 'USB_CDC_RX' && RX_OK=1
    echo "$LOG" | grep -aq 'ICSOS_VER ' && KVER_OK=1
    echo "$STATUS" | grep -aq 'cdc=1' && STATUS_OK=1
    echo "$STATUS" | grep -aq 'release=' && STATUS_VER_OK=1
    echo "$HEALTH" | grep -aq '^pico=' && PICO_VER_OK=1
    grep -a -q 'Root mount \[OK\]' "$SERIAL_LOG" 2>/dev/null && ROOT_OK=1
    if [ "$ROOT_OK" = 1 ] && [ "$CDC_OK" = 1 ] && [ "$RX_OK" = 1 ] && \
       [ "$STATUS_OK" = 1 ] && [ "$KVER_OK" = 1 ] && [ "$PICO_VER_OK" = 1 ] && \
       [ "$STATUS_VER_OK" = 1 ]; then
        STATE=ok
        break
    fi
    if grep -a -q 'failed to open host usb device\|no USB mass-storage device\|panic\|General Protection fault\|Page fault' "$SERIAL_LOG" 2>/dev/null; then
        STATE=failed
        break
    fi
    sleep 2
done

kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""

grep -a -q 'Root mount \[OK\]' "$SERIAL_LOG" 2>/dev/null && ROOT_OK=1
FINAL_LOG="$(pico_get "http://${PICO_IP}/log")"
echo "$FINAL_LOG" | grep -aq 'USB_CDC_CONSOLE_OK' && CDC_OK=1
echo "$FINAL_LOG" | grep -aq 'USB_CDC_RX' && RX_OK=1
echo "$FINAL_LOG" | grep -aq 'ICSOS_VER ' && KVER_OK=1
FINAL_STATUS="$(pico_get "http://${PICO_IP}/status")"
echo "$FINAL_STATUS" | grep -aq 'cdc=1' && STATUS_OK=1
echo "$FINAL_STATUS" | grep -aq 'release=' && STATUS_VER_OK=1
FINAL_HEALTH="$(pico_get "http://${PICO_IP}/health")"
echo "$FINAL_HEALTH" | grep -aq '^pico=' && PICO_VER_OK=1
echo "$FINAL_HEALTH" | grep -aq '^kernel=ICSOS_VER ' && KVER_OK=1

echo "=============================================="
echo "  USB CDC Pico gadget (QEMU usb-host)"
echo "=============================================="
echo "PICO_IP        = $PICO_IP"
echo "STATE          = $STATE"
echo "ROOT_MOUNT_OK  = $ROOT_OK"
echo "CDC_OK         = $CDC_OK"
echo "USB_CDC_RX     = $RX_OK"
echo "STATUS_CDC     = $STATUS_OK"
echo "PICO_VER       = $PICO_VER_OK"
echo "ICSOS_VER      = $KVER_OK"
echo "STATUS_RELEASE = $STATUS_VER_OK"
echo "--- pico /health ---"
echo "$FINAL_HEALTH" | tr -cd '\11\12\15\40-\176\n' | head -c 400
echo
echo "--- usb/xhci serial lines ---"
grep -a -iE 'usb:|xhci:|Root mount|CDC|USB_CDC|failed to open|panic|fault' "$SERIAL_LOG" 2>/dev/null | tail -60 || true
echo "--- pico /log (tail) ---"
echo "$FINAL_LOG" | tr -cd '\11\12\15\40-\176\n' | tail -c 1200
echo
echo "--- pico /status ---"
echo "$FINAL_STATUS" | tr -cd '\11\12\15\40-\176\n' | head -c 800
echo
echo "=============================================="
echo "Artifacts: $ARTIFACT_DIR"

if [ "$ROOT_OK" = 1 ] && [ "$CDC_OK" = 1 ] && [ "$RX_OK" = 1 ] && \
   [ "$STATUS_OK" = 1 ] && [ "$KVER_OK" = 1 ] && [ "$PICO_VER_OK" = 1 ] && \
   [ "$STATUS_VER_OK" = 1 ]; then
    echo "PASS: QEMU xHCI MSC root + Pico CDC IN/RPC (USB_CDC_RX, STATUS, ICSOS_VER)"
    exit 0
fi
echo "FAIL: need Root mount [OK], USB_CDC_CONSOLE_OK, ICSOS_VER, USB_CDC_RX, pico=, and STATUS cdc=1/release="
exit 1
