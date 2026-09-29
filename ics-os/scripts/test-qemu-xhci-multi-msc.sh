#!/bin/bash
# Phase 2: two simultaneous q35 xHCI USB MSC drives (usb0 + usb1).
# The primary (usb0) is the boot/root image; the secondary (usb1) is a fresh
# superfloppy FAT disk. The kernel self-test (cmdline xhci-multi-msc-test)
# proves both are bound as independent MSC drives and that a raw scratch write
# to the secondary persists; this script then reads that LBA back from the
# secondary image file to confirm the write went through the USB MSC.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SOURCE_IMAGE="${1:-ics-os-usb.img}"
QEMU_BIN="${QEMU_X64:-qemu-system-x86_64}"
TIMEOUT_SECONDS="${QEMU_XHCI_MULTI_TIMEOUT_SECONDS:-120}"
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-$$"
ARTIFACT_DIR="${QEMU_XHCI_MULTI_ARTIFACT_DIR:-/tmp/icsos-tests/xhci-multi-msc-$RUN_ID}"
WORK_DIR="$ARTIFACT_DIR/work"
RAW_IMAGE="$WORK_DIR/usb0.img"      # primary / boot (usb0)
SECOND_IMAGE="$WORK_DIR/usb1.img"   # secondary (usb1)
ISO_ROOT="$WORK_DIR/iso-root"
BOOT_ISO="$WORK_DIR/boot.iso"
SERIAL_LOG="$ARTIFACT_DIR/serial.log"
AUTOEXEC="$WORK_DIR/autoexec.bat"
QEMU_PID=""
# LBA the kernel self-test writes the signature to (matches
# USB_MULTI_MSC_TEST_BLOCK in kernel/hardware/usb/uhci.c).
WRITE_BLOCK=512

cleanup()
{
    if [ -n "$QEMU_PID" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

for tool in "$QEMU_BIN" grub-mkrescue python3 truncate mkfs.vfat sfdisk mcopy; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "test-qemu-xhci-multi-msc: missing required tool: $tool" >&2
        exit 1
    fi
done
if [ ! -f "$SOURCE_IMAGE" ]; then
    echo "test-qemu-xhci-multi-msc: image not found: $SOURCE_IMAGE" >&2
    exit 1
fi
if [ ! -f kernel/Kernel64.bin ]; then
    echo "test-qemu-xhci-multi-msc: kernel/Kernel64.bin is missing; run make vmdex" >&2
    exit 1
fi

mkdir -p "$WORK_DIR" "$ISO_ROOT/boot/grub"
touch "$SERIAL_LOG"

# Primary: the boot/root image (has the OS + a partitioned FAT volume).
cp "$SOURCE_IMAGE" "$RAW_IMAGE"

# Secondary: a fresh superfloppy FAT16 disk (no partition table) so it
# enumerates as a second MSC with a volume identity distinct from usb0.
truncate -s 32M "$SECOND_IMAGE"
mkfs.vfat -F 16 -n ICSOSSEC "$SECOND_IMAGE" >/dev/null 2>&1

cp kernel/Kernel64.bin "$ISO_ROOT/vmdex"
printf '%s\n' 'set timeout=0' \
    "menuentry \"ics\" { multiboot2 /vmdex xhci-multi-msc-test; boot }" \
    > "$ISO_ROOT/boot/grub/grub.cfg"
grub-mkrescue -o "$BOOT_ISO" "$ISO_ROOT" >/dev/null 2>&1

# Deterministic autoexec so the run is reproducible (no leftover dist script).
printf '%s\n' '@echo off' 'echo XHCI_MULTI_MSC_AUTOEXEC_DONE' > "$AUTOEXEC"
PART_START="$(sfdisk -J "$RAW_IMAGE" | python3 -c \
    'import json,sys; print(json.load(sys.stdin)["partitiontable"]["partitions"][0]["start"])')"
OFFSET=$((PART_START * 512))
mcopy -o -i "${RAW_IMAGE}@@${OFFSET}" "$AUTOEXEC" ::autoexec.bat

"$QEMU_BIN" -machine q35 -smp 2 -nographic -no-reboot -m 128M \
    -cdrom "$BOOT_ISO" -boot d \
    -drive if=none,id=stick0,format=raw,file="$RAW_IMAGE" \
    -drive if=none,id=stick1,format=raw,file="$SECOND_IMAGE" \
    -device qemu-xhci,id=xhci \
    -device usb-storage,id=dev0,bus=xhci.0,drive=stick0 \
    -device usb-storage,id=dev1,bus=xhci.0,drive=stick1 \
    < /dev/null > "$SERIAL_LOG" 2>&1 &
QEMU_PID=$!

# The self-test runs during usb_init (before root mount), so its marker is an
# early gate; the foreground-manager line confirms the boot completed.
if ! timeout "$TIMEOUT_SECONDS" grep -a -m1 -q 'XHCI_MULTI_MSC_OK' \
    < <(tail -n +1 -F "$SERIAL_LOG" 2>/dev/null); then
    echo "test-qemu-xhci-multi-msc: self-test marker timed out" >&2
    tail -n 100 "$SERIAL_LOG" >&2 || true
    exit 1
fi
if ! timeout 60 grep -a -m1 -q 'Running foreground manager thread' \
    < <(tail -n +1 -F "$SERIAL_LOG" 2>/dev/null); then
    echo "test-qemu-xhci-multi-msc: boot completion timed out" >&2
    tail -n 100 "$SERIAL_LOG" >&2 || true
    exit 1
fi

kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""

# Host-side persistence: the signature the kernel wrote to the secondary at
# WRITE_BLOCK must be present in the image file (proves the write went through
# the USB MSC to the raw image, not an in-memory bounce).
python3 - "$SECOND_IMAGE" "$WRITE_BLOCK" <<'PY'
import sys

path, block = sys.argv[1], int(sys.argv[2])
data = open(path, "rb").read()
off = block * 512
chunk = data[off:off + 512]
if len(chunk) != 512:
    raise SystemExit("secondary: short read at block %d" % block)
if chunk[0:4] != b"ICSM":
    raise SystemExit("secondary: signature mismatch: %r" % chunk[0:4])
for i in range(4, 512):
    if chunk[i] != 0xA5:
        raise SystemExit("secondary: fill byte mismatch at offset %d" % i)
print("host: secondary block %d pattern OK" % block)
PY

grep -a -q '^xhci: configured bulk endpoints' "$SERIAL_LOG"
test "$(grep -a -c '^xhci: configured bulk endpoints' "$SERIAL_LOG")" -ge 2
grep -a -q '^usb: registered block device usb0' "$SERIAL_LOG"
grep -a -q '^usb: registered block device usb1' "$SERIAL_LOG"
grep -a -q 'Root mount \[OK\]' "$SERIAL_LOG"
grep -a -x -q $'XHCI_MULTI_MSC_OK primary=usb0 secondary=usb1 block=512\r' "$SERIAL_LOG"
! grep -a -q 'XHCI_MULTI_MSC_FAIL' "$SERIAL_LOG"
! grep -a -q 'General Protection fault\|Page fault\|Double fault\|Divide by zero' "$SERIAL_LOG"

echo "test-qemu-xhci-multi-msc PASS"
echo "Artifacts: $ARTIFACT_DIR"
