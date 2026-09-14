#!/bin/bash
# Boot the GPT ESP thumbdrive image under Bochs BIOS and require serial
# root-mount markers. Bochs is a BIOS emulator; the image carries i386-pc
# GRUB in the GPT gap so the same Etcher image works here and on UEFI.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SOURCE_IMAGE="${1:-ics-os-uefi.img}"
TIMEOUT_SECONDS="${BOCHS_TIMEOUT_SECONDS:-120}"
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-$$"
ARTIFACT_DIR="${BOCHS_ARTIFACT_DIR:-/tmp/icsos-tests/bochs-usb-$RUN_ID}"
WORK_DIR="$ARTIFACT_DIR/work"
RAW_IMAGE="$WORK_DIR/ics-os-uefi.img"
SERIAL_LOG="$ARTIFACT_DIR/serial.log"
BOCHS_LOG="$ARTIFACT_DIR/bochs.log"
BOCHSRC="$WORK_DIR/bochsrc"
AUTOEXEC="$WORK_DIR/autoexec.bat"

cleanup()
{
    if [ -n "${BOCHS_PID:-}" ]; then
        kill "$BOCHS_PID" >/dev/null 2>&1 || true
        wait "$BOCHS_PID" >/dev/null 2>&1 || true
    fi
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

for bindir in /tmp/bochs-install/bin "$HOME/.local/bin"; do
    if [ -x "$bindir/bochs" ]; then
        PATH="$bindir:$PATH"
        break
    fi
done

for tool in bochs mcopy mmd sfdisk python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "test-bochs-usb-image: missing required tool: $tool" >&2
        exit 1
    fi
done
if [ ! -f "$SOURCE_IMAGE" ]; then
    echo "test-bochs-usb-image: image not found: $SOURCE_IMAGE" >&2
    exit 1
fi

BIOS_ROM="${BOCHS_BIOS:-}"
VGA_ROM="${BOCHS_VGA_BIOS:-}"
if [ -z "$BIOS_ROM" ]; then
    BIOS_ROM="$(find /tmp/bochs-install/share/bochs /usr/share/bochs /usr/local/share/bochs \
        -name 'BIOS-bochs-latest' 2>/dev/null | head -1 || true)"
fi
if [ -z "$VGA_ROM" ]; then
    VGA_ROM="$(find /tmp/bochs-install/share/bochs /usr/share/bochs /usr/local/share/bochs /usr/share/vgabios \
        \( -name 'VGABIOS-lgpl-latest' -o -name 'vgabios.bin' \) 2>/dev/null | head -1 || true)"
fi
if [ -z "$BIOS_ROM" ] || [ ! -f "$BIOS_ROM" ]; then
    echo "test-bochs-usb-image: Bochs BIOS ROM not found (install bochsbios)" >&2
    exit 1
fi
if [ -z "$VGA_ROM" ] || [ ! -f "$VGA_ROM" ]; then
    echo "test-bochs-usb-image: VGA BIOS ROM not found (install vgabios/bochsbios)" >&2
    exit 1
fi

mkdir -p "$WORK_DIR"
cp "$SOURCE_IMAGE" "$RAW_IMAGE"
cat > "$AUTOEXEC" <<'EOF'
@echo off
echo BOK
echo BOK
echo BOK
EOF

PART_START="$(sfdisk -J "$RAW_IMAGE" | python3 -c \
    'import json,sys; print(json.load(sys.stdin)["partitiontable"]["partitions"][0]["start"])')"
OFFSET=$((PART_START * 512))
mcopy -o -i "${RAW_IMAGE}@@${OFFSET}" "$AUTOEXEC" ::autoexec.bat

SECTORS="$(python3 -c "import os; print(os.path.getsize('$RAW_IMAGE') // 512)")"
# Bochs wants CHS; 16 heads * 63 spt matches the 128 MiB image (260 cyl).
HEADS=16
SPT=63
CYLS=$((SECTORS / (HEADS * SPT)))
if [ "$CYLS" -lt 1 ]; then
    echo "test-bochs-usb-image: image too small for CHS geometry" >&2
    exit 1
fi

DISPLAY_LIB=nogui
cat > "$BOCHSRC" <<EOF
romimage: file=$BIOS_ROM
vgaromimage: file=$VGA_ROM
megs: 512
cpu: model=core2_penryn_t9600, count=1, ips=50000000, reset_on_triple_fault=1
ata0: enabled=1, ioaddr1=0x1f0, ioaddr2=0x3f0, irq=14
ata0-master: type=disk, path=$RAW_IMAGE, mode=flat, cylinders=$CYLS, heads=$HEADS, spt=$SPT
boot: disk
com1: enabled=1, mode=file, dev=$SERIAL_LOG
log: $BOCHS_LOG
panic: action=fatal
error: action=report
info: action=ignore
debug: action=ignore
mouse: enabled=0
display_library: $DISPLAY_LIB
EOF

: > "$SERIAL_LOG"
# -q skips the interactive start menu.
bochs -q -f "$BOCHSRC" </dev/null >/dev/null 2>"$ARTIFACT_DIR/bochs.stderr" &
BOCHS_PID=$!

if ! timeout "$TIMEOUT_SECONDS" grep -a -m1 -q \
    'Root mount \[OK\]' < <(tail -n +1 -F "$SERIAL_LOG" 2>/dev/null); then
    echo "test-bochs-usb-image: guest marker timed out" >&2
    tail -n 80 "$SERIAL_LOG" >&2 || true
    tail -n 40 "$BOCHS_LOG" >&2 || true
    exit 1
fi

grep -a -q 'serial console ready' "$SERIAL_LOG"
grep -a -q 'Root mount \[OK\]' "$SERIAL_LOG"
grep -a -q 'GPT_DETECT hdp0' "$SERIAL_LOG"
! grep -a -q 'General Protection fault\|Page fault\|Double fault' "$SERIAL_LOG"

echo "test-bochs-usb-image PASS"
echo "Artifacts: $ARTIFACT_DIR"
