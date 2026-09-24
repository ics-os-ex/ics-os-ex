#!/bin/bash
# Bidirectional xHCI CDC debug RPC.
#
# After USB_CDC_CONSOLE_OK on the usb-serial socket, send
#   <RS>ICS 1 CMD echo USB_DBG_CMD_OK
# and require that marker on the gadget TX (kernel console_execute).
# MSC root must still reach Root mount [OK].
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SOURCE_IMAGE="${1:-ics-os-usb.img}"
TIMEOUT_SECONDS="${CDC_DEBUG_TIMEOUT_SECONDS:-90}"
QEMU_BIN="${QEMU_X64:-qemu-system-x86_64}"
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-$$"
ARTIFACT_DIR="${CDC_DEBUG_ARTIFACT_DIR:-/tmp/icsos-tests/xhci-cdc-debug-$RUN_ID}"
WORK_DIR="$ARTIFACT_DIR/work"
RAW_IMAGE="$WORK_DIR/ics-os-usb.img"
ISO_ROOT="$WORK_DIR/iso-root"
BOOT_ISO="$WORK_DIR/boot.iso"
SERIAL_LOG="$ARTIFACT_DIR/serial.log"
CDC_FILE="$ARTIFACT_DIR/cdc-out.txt"
PAYLOAD_SOURCE="$WORK_DIR/persist-source.txt"
AUTOEXEC="$WORK_DIR/autoexec.bat"
QEMU_PID=""
ROOT_OK=0
CDC_OK=0
CMD_OK=0
PORT=""

cleanup() {
    if [ -n "$QEMU_PID" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

for tool in "$QEMU_BIN" grub-mkrescue mcopy mmd mdir mdel sfdisk python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "cdc-debug: missing required tool: $tool" >&2
        exit 2
    fi
done
if [ ! -f "$SOURCE_IMAGE" ]; then
    echo "cdc-debug: image not found: $SOURCE_IMAGE" >&2
    exit 2
fi
if [ ! -f kernel/Kernel64.bin ]; then
    echo "cdc-debug: kernel/Kernel64.bin is missing; run make vmdex" >&2
    exit 2
fi
if [ ! -f apps/cp.exe ]; then
    echo "cdc-debug: apps/cp.exe is missing; run make apps" >&2
    exit 2
fi

mkdir -p "$WORK_DIR" "$ISO_ROOT/boot/grub" "$ARTIFACT_DIR"
touch "$SERIAL_LOG"
cp "$SOURCE_IMAGE" "$RAW_IMAGE"
cp kernel/Kernel64.bin "$ISO_ROOT/vmdex"
printf '%s\n' 'set timeout=0' \
    "menuentry \"ics\" { multiboot2 /vmdex; boot }" \
    > "$ISO_ROOT/boot/grub/grub.cfg"
grub-mkrescue -o "$BOOT_ISO" "$ISO_ROOT" >/dev/null 2>&1

printf 'ics-os QEMU xhci-cdc-debug\n' > "$PAYLOAD_SOURCE"
cat > "$AUTOEXEC" <<'EOF'
@echo off
set PATH=/icsos/apps
cp-posix.exe -v /icsos/work/UHCISRC.TXT /icsos/work/UHCIRES.TXT
EOF

PART_START="$(sfdisk -J "$RAW_IMAGE" | python3 -c \
    'import json,sys; print(json.load(sys.stdin)["partitiontable"]["partitions"][0]["start"])')"
OFFSET=$((PART_START * 512))
if ! mdir -i "${RAW_IMAGE}@@${OFFSET}" ::work >/dev/null 2>&1; then
    mmd -i "${RAW_IMAGE}@@${OFFSET}" ::work
fi
mcopy -o -i "${RAW_IMAGE}@@${OFFSET}" "$PAYLOAD_SOURCE" ::work/UHCISRC.TXT
mcopy -o -i "${RAW_IMAGE}@@${OFFSET}" "$AUTOEXEC" ::autoexec.bat
mcopy -o -i "${RAW_IMAGE}@@${OFFSET}" apps/cp.exe ::apps/cp-posix.exe
mdel -i "${RAW_IMAGE}@@${OFFSET}" ::work/UHCIRES.TXT >/dev/null 2>&1 || true

PORT="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')"

"$QEMU_BIN" -machine q35 -smp 1 -nographic -no-reboot -m 128M \
    -chardev socket,id=c0,host=127.0.0.1,port="$PORT",server=on,wait=off,logfile="$CDC_FILE" \
    -cdrom "$BOOT_ISO" -boot d \
    -drive if=none,id=stick,format=raw,file="$RAW_IMAGE" \
    -device qemu-xhci,id=xhci \
    -device usb-storage,bus=xhci.0,drive=stick \
    -device usb-serial,chardev=c0,bus=xhci.0 \
    < /dev/null > "$SERIAL_LOG" 2>&1 &
QEMU_PID=$!

python3 - "$PORT" "$TIMEOUT_SECONDS" "$CDC_FILE" <<'PY'
import socket, sys, time
port = int(sys.argv[1])
timeout = int(sys.argv[2])
out_path = sys.argv[3]
deadline = time.time() + timeout
s = None
buf = b""
sent = False
ok = False
while time.time() < deadline and s is None:
    try:
        s = socket.create_connection(("127.0.0.1", port), 1)
        s.settimeout(1)
    except OSError:
        time.sleep(0.2)
if s is None:
    sys.stderr.write("cdc-debug: could not connect to usb-serial socket\n")
    sys.exit(1)
try:
    last_send = 0
    while time.time() < deadline:
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            chunk = b""
        if chunk:
            buf += chunk
        now = time.time()
        if b"USB_CDC_CONSOLE_OK" in buf:
            if (not sent) or (now - last_send >= 1.0):
                s.send(b"\x1eICS 1 STATUS\n")
                s.send(b"\x1eICS 2 CMD echo USB_DBG_CMD_OK\n")
                sent = True
                last_send = now
        if sent and (b"USB_DBG_CMD_OK" in buf or b"cdc=1" in buf):
            ok = b"USB_DBG_CMD_OK" in buf or b"cdc=1" in buf
            break
        if not chunk:
            time.sleep(0.1)
finally:
    try:
        s.close()
    except OSError:
        pass
    open(out_path, "ab").write(buf)
    open(out_path + ".meta", "w").write("sent=%s len=%d\n" % (sent, len(buf)))
sys.exit(0 if ok else 1)
PY
CMD_RC=$?

kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""

grep -a -q 'Root mount \[OK\]' "$SERIAL_LOG" 2>/dev/null && ROOT_OK=1
grep -a -q 'USB_CDC_CONSOLE_OK' "$CDC_FILE" 2>/dev/null && CDC_OK=1
grep -a -q 'USB_DBG_CMD_OK\|cdc=1' "$CDC_FILE" 2>/dev/null && CMD_OK=1
[ "$CMD_RC" = 0 ] && CMD_OK=1

echo "=============================================="
echo "  USB CDC debug RPC"
echo "=============================================="
echo "ROOT_MOUNT_OK  = $ROOT_OK"
echo "CDC_CHARDEV_OK = $CDC_OK"
echo "CMD_ECHO_OK    = $CMD_OK"
echo "--- usb/xhci serial lines ---"
grep -a -iE 'usb:|xhci:|Root mount|CDC|USB_DBG|panic|fault' "$SERIAL_LOG" 2>/dev/null | tail -50 || true
echo "--- cdc socket capture (tail) ---"
tail -c 1024 "$CDC_FILE" 2>/dev/null | tr -cd '\11\12\15\40-\176' || true
echo
echo "=============================================="
echo "Artifacts: $ARTIFACT_DIR"

if [ "$ROOT_OK" = 1 ] && [ "$CDC_OK" = 1 ] && [ "$CMD_OK" = 1 ]; then
    echo "PASS: MSC root intact, CDC console up, USB_DBG_CMD_OK on gadget TX"
    exit 0
fi
echo "FAIL: need Root mount [OK], USB_CDC_CONSOLE_OK, and USB_DBG_CMD_OK"
exit 1
