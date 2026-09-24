#!/bin/bash
# USB serial console on the same xHCI controller as the MSC root.
#
# Boots ICS-OS from CD (grub ISO) with the USB thumb drive as the root
# filesystem and a QEMU usb-serial gadget (FTDI 0403:6001; the same TX
# path as Pico CDC-ACM) on the same controller. Both attach orders must
# keep the MSC root and write USB_CDC_CONSOLE_OK to the chardev
# (host bulk OUT = gadget RX).
#
# PORT_ORDER=cdc_first   -> usb-serial added before usb-storage
# PORT_ORDER=msc_first   -> usb-storage added before usb-serial
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SOURCE_IMAGE="${1:-ics-os-usb.img}"
PORT_ORDER="${PORT_ORDER:-msc_first}"
TIMEOUT_SECONDS="${CDC_COEXIST_TIMEOUT_SECONDS:-90}"
CDC_HOTPLUG="${CDC_HOTPLUG:-0}"
QEMU_BIN="${QEMU_X64:-qemu-system-x86_64}"
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-$$"
ARTIFACT_DIR="${CDC_COEXIST_ARTIFACT_DIR:-/tmp/icsos-tests/xhci-cdc-$PORT_ORDER-$RUN_ID}"
WORK_DIR="$ARTIFACT_DIR/work"
RAW_IMAGE="$WORK_DIR/ics-os-usb.img"
ISO_ROOT="$WORK_DIR/iso-root"
BOOT_ISO="$WORK_DIR/boot.iso"
SERIAL_LOG="$ARTIFACT_DIR/serial.log"
CDC_FILE="$ARTIFACT_DIR/cdc-out.txt"
QMP_SOCKET="$WORK_DIR/qmp.sock"
PAYLOAD_SOURCE="$WORK_DIR/persist-source.txt"
AUTOEXEC="$WORK_DIR/autoexec.bat"
QEMU_PID=""
ROOT_OK=0
CDC_OK=0
VER_OK=0

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
        echo "cdc-console: missing required tool: $tool" >&2
        exit 2
    fi
done
if [ ! -f "$SOURCE_IMAGE" ]; then
    echo "cdc-console: image not found: $SOURCE_IMAGE" >&2
    exit 2
fi
if [ ! -f kernel/Kernel64.bin ]; then
    echo "cdc-console: kernel/Kernel64.bin is missing; run make vmdex" >&2
    exit 2
fi
if [ ! -f apps/cp.exe ]; then
    echo "cdc-console: apps/cp.exe is missing; run make apps" >&2
    exit 2
fi

mkdir -p "$WORK_DIR" "$ISO_ROOT/boot/grub"
touch "$SERIAL_LOG"
touch "$CDC_FILE"
cp "$SOURCE_IMAGE" "$RAW_IMAGE"
cp kernel/Kernel64.bin "$ISO_ROOT/vmdex"
printf '%s\n' 'set timeout=0' \
    "menuentry \"ics\" { multiboot2 /vmdex; boot }" \
    > "$ISO_ROOT/boot/grub/grub.cfg"
grub-mkrescue -o "$BOOT_ISO" "$ISO_ROOT" >/dev/null 2>&1

printf 'ics-os QEMU xhci-cdc-console %s\n' "$PORT_ORDER" > "$PAYLOAD_SOURCE"
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

if [ "$PORT_ORDER" = "cdc_first" ]; then
    USB_DEV_ARGS=(-device usb-serial,id=cdcdev,chardev=c0,bus=xhci.0
                  -device usb-storage,bus=xhci.0,drive=stick)
elif [ "$PORT_ORDER" = "msc_first" ]; then
    USB_DEV_ARGS=(-device usb-storage,bus=xhci.0,drive=stick
                  -device usb-serial,id=cdcdev,chardev=c0,bus=xhci.0)
else
    echo "cdc-console: PORT_ORDER must be cdc_first or msc_first" >&2
    exit 2
fi

"$QEMU_BIN" -machine q35 -smp 1 -nographic -no-reboot -m 128M \
    -chardev file,id=c0,path="$CDC_FILE" \
    -cdrom "$BOOT_ISO" -boot d \
    -drive if=none,id=stick,format=raw,file="$RAW_IMAGE" \
    -qmp unix:"$QMP_SOCKET",server=on,wait=off \
    -device qemu-xhci,id=xhci \
    "${USB_DEV_ARGS[@]}" \
    < /dev/null > "$SERIAL_LOG" 2>&1 &
QEMU_PID=$!

DEADLINE=$((SECONDS + TIMEOUT_SECONDS))
STATE=timeout
while [ $SECONDS -lt $DEADLINE ]; do
    if grep -a -q 'Root mount \[OK\]' "$SERIAL_LOG" 2>/dev/null && \
       grep -a -q 'USB_CDC_CONSOLE_OK' "$CDC_FILE" 2>/dev/null && \
       grep -a -q 'ICSOS_VER ' "$CDC_FILE" 2>/dev/null; then
        STATE=ok; break
    fi
    if grep -a -q 'no USB mass-storage device\|no xHCI mass-storage device\|Root mount \[FAIL\]\|panic\|General Protection fault\|Page fault' "$SERIAL_LOG" 2>/dev/null; then
        STATE=failed; break
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        STATE=exited; break
    fi
    sleep 1
done

if [ "$STATE" = ok ] && [ "$CDC_HOTPLUG" = 1 ]; then
    python3 - "$QMP_SOCKET" <<'PY'
import json, socket, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1]); f = s.makefile('rwb', buffering=0)
json.loads(f.readline())
def response():
    while True:
        msg = json.loads(f.readline())
        if "event" not in msg:
            if "error" in msg:
                raise SystemExit("QMP error: %s" % msg["error"])
            return msg
f.write(b'{"execute":"qmp_capabilities"}\n'); response()
f.write(b'{"execute":"device_del","arguments":{"id":"cdcdev"}}\n'); response()
time.sleep(3)
f.write(b'{"execute":"device_add","arguments":{"driver":"usb-serial","id":"cdcdev","chardev":"c0","bus":"xhci.0"}}\n'); response()
f.close(); s.close()
PY
    HOTPLUG_DEADLINE=$((SECONDS + 30))
    STATE=hotplug-timeout
    while [ $SECONDS -lt $HOTPLUG_DEADLINE ]; do
        CDC_COUNT=$(grep -a -c 'USB_CDC_CONSOLE_OK' "$CDC_FILE" 2>/dev/null || true)
        if grep -a -q 'XHCI_CDC_HOTPLUG_DISCONNECT' "$SERIAL_LOG" 2>/dev/null && \
           grep -a -q 'XHCI_CDC_HOTPLUG_REBIND_OK' "$SERIAL_LOG" 2>/dev/null && \
           [ "$CDC_COUNT" -ge 2 ]; then
            STATE=hotplug-ok; break
        fi
        sleep 1
    done
fi

kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""

grep -a -q 'Root mount \[OK\]' "$SERIAL_LOG" 2>/dev/null && ROOT_OK=1
grep -a -q 'USB_CDC_CONSOLE_OK' "$CDC_FILE" 2>/dev/null && CDC_OK=1
grep -a -q 'ICSOS_VER ' "$CDC_FILE" 2>/dev/null && VER_OK=1
SERIAL_CDC=0
grep -a -q 'USB_CDC_CONSOLE_OK' "$SERIAL_LOG" 2>/dev/null && SERIAL_CDC=1

echo "=============================================="
echo "  USB CDC-ACM console + MSC root"
echo "=============================================="
echo "PORT_ORDER     = $PORT_ORDER"
echo "STATE          = $STATE"
echo "ROOT_MOUNT_OK  = $ROOT_OK"
echo "CDC_CHARDEV_OK = $CDC_OK"
echo "ICSOS_VER_OK   = $VER_OK"
echo "SERIAL_CDC_OK  = $SERIAL_CDC"
echo "CDC_FILE_BYTES = $(wc -c < "$CDC_FILE" 2>/dev/null || echo 0)"
echo "--- usb/xhci serial lines ---"
grep -a -iE 'usb:|xhci:|Root mount|CDC|mass-storage|no BBB|panic|fault' "$SERIAL_LOG" 2>/dev/null | tail -50 || true
echo "--- cdc chardev (head) ---"
head -c 512 "$CDC_FILE" 2>/dev/null | tr -cd '\11\12\15\40-\176' || true
echo
echo "=============================================="
echo "Artifacts: $ARTIFACT_DIR"

if [ "$ROOT_OK" = 1 ] && [ "$CDC_OK" = 1 ] && [ "$VER_OK" = 1 ] && \
   { [ "$CDC_HOTPLUG" != 1 ] || [ "$STATE" = hotplug-ok ]; }; then
    echo "PASS: MSC root intact and CDC console wrote USB_CDC_CONSOLE_OK + ICSOS_VER ($PORT_ORDER)"
    exit 0
fi
echo "FAIL: need Root mount [OK], USB_CDC_CONSOLE_OK, and ICSOS_VER on the usb-serial chardev ($PORT_ORDER)"
exit 1
