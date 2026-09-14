#!/bin/bash
# Phase 0 oracle: characterize two-device coexistence on one xHCI controller.
#
# Boots ICS-OS from CD (grub ISO) with the USB thumb drive as the root
# filesystem, and a second USB device (QEMU usb-serial = CDC-ACM) on the
# SAME xHCI controller. The goal is to prove the exact failure/success mode
# when a non-MSC device shares the controller, in both port orders, BEFORE
# any kernel change.
#
# PORT_ORDER=cdc_first   -> usb-serial added before usb-storage
# PORT_ORDER=msc_first   -> usb-storage added before usb-serial
#
# Pass criterion (Phase 0, no kernel change yet):
#   msc_first must reach "Root mount [OK]" (presence of a second device must
#   not break the MSC root when MSC holds the first port).
#   cdc_first is characterized: we record whether the MSC root survives or is
#   lost (expected: lost, because xhci_init_hcd stops at the first connected
#   port and usb_parse_config rejects the CDC device).
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SOURCE_IMAGE="${1:-ics-os-usb.img}"
PORT_ORDER="${PORT_ORDER:-msc_first}"
TIMEOUT_SECONDS="${CDC_COEXIST_TIMEOUT_SECONDS:-90}"
QEMU_BIN="${QEMU_X64:-qemu-system-x86_64}"
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-$$"
ARTIFACT_DIR="${CDC_COEXIST_ARTIFACT_DIR:-/tmp/icsos-tests/xhci-cdc-$PORT_ORDER-$RUN_ID}"
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
CDC_PRESENT=0

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
        echo "cdc-coexist: missing required tool: $tool" >&2
        exit 2
    fi
done
if [ ! -f "$SOURCE_IMAGE" ]; then
    echo "cdc-coexist: image not found: $SOURCE_IMAGE" >&2
    exit 2
fi
if [ ! -f kernel/Kernel64.bin ]; then
    echo "cdc-coexist: kernel/Kernel64.bin is missing; run make vmdex" >&2
    exit 2
fi
if [ ! -f apps/cp.exe ]; then
    echo "cdc-coexist: apps/cp.exe is missing; run make apps" >&2
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

printf 'ics-os QEMU xhci-cdc-coexist %s\n' "$PORT_ORDER" > "$PAYLOAD_SOURCE"
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

# Device order on the xHCI controller decides which port each lands on.
if [ "$PORT_ORDER" = "cdc_first" ]; then
    USB_DEV_ARGS=(-device usb-serial,chardev=c0,bus=xhci.0
                  -device usb-storage,bus=xhci.0,drive=stick)
elif [ "$PORT_ORDER" = "msc_first" ]; then
    USB_DEV_ARGS=(-device usb-storage,bus=xhci.0,drive=stick
                  -device usb-serial,chardev=c0,bus=xhci.0)
else
    echo "cdc-coexist: PORT_ORDER must be cdc_first or msc_first" >&2
    exit 2
fi

"$QEMU_BIN" -machine q35 -smp 1 -nographic -no-reboot -m 128M \
    -chardev file,id=c0,path="$CDC_FILE" \
    -cdrom "$BOOT_ISO" -boot d \
    -drive if=none,id=stick,format=raw,file="$RAW_IMAGE" \
    -device qemu-xhci,id=xhci \
    "${USB_DEV_ARGS[@]}" \
    < /dev/null > "$SERIAL_LOG" 2>&1 &
QEMU_PID=$!

# Wait for a terminal state: either the root mounted (success path) or the
# boot clearly failed to find a usable root (failure path).
DEADLINE=$((SECONDS + TIMEOUT_SECONDS))
STATE=timeout
while [ $SECONDS -lt $DEADLINE ]; do
    if grep -a -q 'Root mount \[OK\]' "$SERIAL_LOG" 2>/dev/null; then
        STATE=mounted; break
    fi
    if grep -a -q 'no USB mass-storage device\|no xHCI mass-storage device\|no BBB mass-storage interface\|Root mount \[FAIL\]\|panic\|General Protection fault\|Page fault' "$SERIAL_LOG" 2>/dev/null; then
        STATE=failed; break
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        STATE=exited; break
    fi
    sleep 1
done

kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""

# Post-hoc assertions on the captured log.
grep -a -q 'xhci: controller at PCI' "$SERIAL_LOG" 2>/dev/null || true
grep -a -q 'usb: registered usb0p0' "$SERIAL_LOG" 2>/dev/null && CDC_PRESENT=0
grep -a -q 'Root mount \[OK\]' "$SERIAL_LOG" 2>/dev/null && ROOT_OK=1
grep -a -q 'no BBB mass-storage interface' "$SERIAL_LOG" 2>/dev/null && REJECT=1 || REJECT=0

echo "=============================================="
echo "  Phase 0 CDC/MSC coexistence oracle"
echo "=============================================="
echo "PORT_ORDER     = $PORT_ORDER"
echo "STATE          = $STATE"
echo "ROOT_MOUNT_OK  = $ROOT_OK"
echo "CDC_REJECTED   = $REJECT   (1 = xhci enumerated the CDC device, MSC filter rejected it)"
echo "CDC_FILE_BYTES = $(wc -c < "$CDC_FILE" 2>/dev/null || echo 0)"
echo "--- usb/xhci serial lines ---"
grep -a -iE 'usb:|xhci:|Root mount|mass-storage|no BBB|panic|fault' "$SERIAL_LOG" 2>/dev/null | tail -40 || true
echo "=============================================="
echo "Artifacts: $ARTIFACT_DIR"

# Phase 0 gate: msc_first must not regress the root.
if [ "$PORT_ORDER" = "msc_first" ]; then
    if [ "$ROOT_OK" = 1 ]; then
        echo "PASS: second device present, MSC root intact (msc_first)"
        exit 0
    else
        echo "FAIL: MSC root lost with a second device present (msc_first)"
        exit 1
    fi
else
    # cdc_first is a characterization: report the outcome, do not fail the
    # build (this is the known limitation the N=2 work will fix).
    if [ "$ROOT_OK" = 1 ]; then
        echo "CHAR: cdc_first still mounted MSC root (surprising; record it)"
    else
        echo "CHAR: cdc_first lost the MSC root (expected pre-fix behavior)"
    fi
    exit 0
fi
