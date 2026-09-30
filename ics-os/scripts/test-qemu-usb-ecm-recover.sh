#!/bin/bash
# Phase 3: q35 xHCI CDC-ECM NIC survives a controller recovery.
#
# The primary USB MSC (usb0) is the boot/root image and a QEMU CDC-ECM gadget
# (usb-net, SLIRP user-net) is the NIC. The kernel self-test (cmdline
# usb-ecm-recover-test) proves the NIC works (DHCP + ping), forces a full xHCI
# controller reset via usb_xhci_recover(), then asserts BOTH the MSC root and
# the ECM NIC come back and the NIC is functional again (DHCP + ping post-reset).
# Regression: without the usb_xhci_bind_ecm() call in the recovery/reconnect
# paths, the NIC stays down after the reset (ECM_RECOVER_FAIL nic-not-rebound).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SOURCE_IMAGE="${1:-ics-os-usb.img}"
QEMU_BIN="${QEMU_X64:-qemu-system-x86_64}"
TIMEOUT_SECONDS="${QEMU_USB_ECM_RECOVER_TIMEOUT_SECONDS:-180}"
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-$$"
ARTIFACT_DIR="${QEMU_USB_ECM_RECOVER_ARTIFACT_DIR:-/tmp/icsos-tests/usb-ecm-recover-$RUN_ID}"
WORK_DIR="$ARTIFACT_DIR/work"
RAW_IMAGE="$WORK_DIR/usb0.img"      # primary / boot (usb0)
ISO_ROOT="$WORK_DIR/iso-root"
BOOT_ISO="$WORK_DIR/boot.iso"
SERIAL_LOG="$ARTIFACT_DIR/serial.log"
AUTOEXEC="$WORK_DIR/autoexec.bat"
QEMU_PID=""

cleanup()
{
    if [ -n "$QEMU_PID" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

for tool in "$QEMU_BIN" grub-mkrescue python3 sfdisk mcopy; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "test-qemu-usb-ecm-recover: missing required tool: $tool" >&2
        exit 1
    fi
done
if [ ! -f "$SOURCE_IMAGE" ]; then
    echo "test-qemu-usb-ecm-recover: image not found: $SOURCE_IMAGE" >&2
    exit 1
fi
if [ ! -f kernel/Kernel64.bin ]; then
    echo "test-qemu-usb-ecm-recover: kernel/Kernel64.bin is missing; run make vmdex" >&2
    exit 1
fi

mkdir -p "$WORK_DIR" "$ISO_ROOT/boot/grub"
touch "$SERIAL_LOG"

# Primary: the boot/root image (has the OS + a partitioned FAT volume).
cp "$SOURCE_IMAGE" "$RAW_IMAGE"

cp kernel/Kernel64.bin "$ISO_ROOT/vmdex"
printf '%s\n' 'set timeout=0' \
    "menuentry \"ics\" { multiboot2 /vmdex usb-ecm-recover-test; boot }" \
    > "$ISO_ROOT/boot/grub/grub.cfg"
grub-mkrescue -o "$BOOT_ISO" "$ISO_ROOT" >/dev/null 2>&1

# Deterministic autoexec so the run is reproducible (the self-test runs during
# usb_init, before the root autoexec; this just lets the boot complete cleanly).
printf '%s\n' '@echo off' 'echo USB_ECM_RECOVER_AUTOEXEC_DONE' > "$AUTOEXEC"
PART_START="$(sfdisk -J "$RAW_IMAGE" | python3 -c \
    'import json,sys; print(json.load(sys.stdin)["partitiontable"]["partitions"][0]["start"])')"
OFFSET=$((PART_START * 512))
mcopy -o -i "${RAW_IMAGE}@@${OFFSET}" "$AUTOEXEC" ::autoexec.bat

# SLIRP user-net provides DHCP and an ICMP-echo endpoint at 10.0.2.2, so the
# pre/post-recovery DHCP + ping need no host-side probes.
"$QEMU_BIN" -machine q35 -smp 2 -nographic -no-reboot -m 128M \
    -cdrom "$BOOT_ISO" -boot d \
    -drive if=none,id=stick0,format=raw,file="$RAW_IMAGE" \
    -device qemu-xhci,id=xhci \
    -device usb-storage,id=dev0,bus=xhci.0,drive=stick0 \
    -netdev user,id=n0 \
    -device usb-net,id=usbnet,bus=xhci.0,netdev=n0 \
    < /dev/null > "$SERIAL_LOG" 2>&1 &
QEMU_PID=$!

if ! timeout "$TIMEOUT_SECONDS" grep -a -m1 -q 'ECM_RECOVER_OK\|ECM_RECOVER_FAIL' \
    < <(tail -n +1 -F "$SERIAL_LOG" 2>/dev/null); then
    echo "test-qemu-usb-ecm-recover: self-test marker timed out" >&2
    tail -n 120 "$SERIAL_LOG" >&2 || true
    exit 1
fi

kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""

grep -a -q 'USB_ECM_NIC_OK' "$SERIAL_LOG"
grep -a -q 'ECM_RECOVER_PRE_OK' "$SERIAL_LOG"
grep -a -q 'ECM_RECOVER_REBIND_OK' "$SERIAL_LOG"
grep -a -x -q $'ECM_RECOVER_OK\r' "$SERIAL_LOG"
! grep -a -q 'ECM_RECOVER_FAIL' "$SERIAL_LOG"
! grep -a -q 'General Protection fault\|Page fault\|Double fault\|Divide by zero' "$SERIAL_LOG"

echo "test-qemu-usb-ecm-recover PASS"
echo "Artifacts: $ARTIFACT_DIR"
