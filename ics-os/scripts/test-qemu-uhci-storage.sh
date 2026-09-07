#!/bin/bash
# Validate durable FAT-root writes through QEMU USB mass storage.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SOURCE_IMAGE="${1:-ics-os-usb.img}"
CONTROLLER="${QEMU_USB_HCD:-uhci}"
TIMEOUT_SECONDS="${QEMU_USB_STORAGE_TIMEOUT_SECONDS:-${QEMU_UHCI_TIMEOUT_SECONDS:-90}}"
QEMU_BIN="${QEMU_X64:-qemu-system-x86_64}"
QEMU_MACHINE="${QEMU_MACHINE:-}"
QEMU_KERNEL_CMDLINE="${QEMU_KERNEL_CMDLINE:-}"
QEMU_SMP="${QEMU_SMP:-1}"
QEMU_XHCI_SECONDARY_TEST="${QEMU_XHCI_SECONDARY_TEST:-0}"
QEMU_VTD_TEST="${QEMU_VTD_TEST:-0}"
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-$$"
ARTIFACT_DIR="${QEMU_USB_STORAGE_ARTIFACT_DIR:-/tmp/icsos-tests/$CONTROLLER-usb-$RUN_ID}"
WORK_DIR="$ARTIFACT_DIR/work"
RAW_IMAGE="$WORK_DIR/ics-os-usb.img"
ISO_ROOT="$WORK_DIR/iso-root"
BOOT_ISO="$WORK_DIR/boot.iso"
SERIAL_LOG="$ARTIFACT_DIR/serial.log"
PAYLOAD_SOURCE="$WORK_DIR/persist-source.txt"
PAYLOAD_RESULT="$ARTIFACT_DIR/persist-result.txt"
AUTOEXEC="$WORK_DIR/autoexec.bat"
QEMU_PID=""
CONTROLLER_ARGS=()
MACHINE_ARGS=()

if [ -n "$QEMU_MACHINE" ]; then
    MACHINE_ARGS=(-machine "$QEMU_MACHINE")
fi
if [ "$QEMU_VTD_TEST" = 1 ]; then
    MACHINE_ARGS+=(-device intel-iommu,intremap=off)
fi

case "$CONTROLLER" in
    uhci)
        CONTROLLER_ARGS=(-device piix3-usb-uhci,id=uhci
                         -device usb-storage,bus=uhci.0,drive=stick)
        ;;
    xhci)
        if [ "$QEMU_XHCI_SECONDARY_TEST" = 1 ]; then
            CONTROLLER_ARGS=(-device qemu-xhci,id=xhci
                             -device qemu-xhci,id=xhci-secondary
                             -device usb-storage,bus=xhci-secondary.0,drive=stick)
        else
            CONTROLLER_ARGS=(-device qemu-xhci,id=xhci
                             -device usb-storage,bus=xhci.0,drive=stick)
        fi
        ;;
    *)
        echo "test-qemu-usb-storage: QEMU_USB_HCD must be uhci or xhci" >&2
        exit 1
        ;;
esac

cleanup()
{
    if [ -n "$QEMU_PID" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

for tool in "$QEMU_BIN" grub-mkrescue mcopy mmd mdir mdel sfdisk python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "test-qemu-usb-storage: missing required tool: $tool" >&2
        exit 1
    fi
done
if [ ! -f "$SOURCE_IMAGE" ]; then
    echo "test-qemu-usb-storage: image not found: $SOURCE_IMAGE" >&2
    exit 1
fi
if [ ! -f kernel/Kernel64.bin ]; then
    echo "test-qemu-usb-storage: kernel/Kernel64.bin is missing; run make vmdex" >&2
    exit 1
fi
if [ ! -f apps/cp.exe ]; then
    echo "test-qemu-usb-storage: apps/cp.exe is missing; run make apps" >&2
    exit 1
fi

mkdir -p "$WORK_DIR" "$ISO_ROOT/boot/grub"
touch "$SERIAL_LOG"
cp "$SOURCE_IMAGE" "$RAW_IMAGE"
cp kernel/Kernel64.bin "$ISO_ROOT/vmdex"
printf '%s\n' 'set timeout=0' \
    "menuentry \"ics\" { multiboot2 /vmdex $QEMU_KERNEL_CMDLINE; boot }" \
    > "$ISO_ROOT/boot/grub/grub.cfg"
grub-mkrescue -o "$BOOT_ISO" "$ISO_ROOT" >/dev/null 2>&1

printf 'ics-os QEMU %s persistent root write\n' "$CONTROLLER" > "$PAYLOAD_SOURCE"
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

"$QEMU_BIN" "${MACHINE_ARGS[@]}" -smp "$QEMU_SMP" -nographic -no-reboot -m 128M \
    -cdrom "$BOOT_ISO" -boot d \
    -drive if=none,id=stick,format=raw,file="$RAW_IMAGE" \
    "${CONTROLLER_ARGS[@]}" \
    < /dev/null > "$SERIAL_LOG" 2>&1 &
QEMU_PID=$!

if ! timeout "$TIMEOUT_SECONDS" grep -a -m1 -q \
    'cp: copied /icsos/work/UHCISRC.TXT -> /icsos/work/UHCIRES.TXT' \
    < <(tail -n +1 -F "$SERIAL_LOG" 2>/dev/null); then
    echo "test-qemu-usb-storage: guest marker timed out hcd=$CONTROLLER" >&2
    tail -n 100 "$SERIAL_LOG" >&2 || true
    exit 1
fi
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""

mcopy -i "${RAW_IMAGE}@@${OFFSET}" ::work/UHCIRES.TXT "$PAYLOAD_RESULT"
cmp "$PAYLOAD_SOURCE" "$PAYLOAD_RESULT"
grep -a -q 'boot_device_name=cds0' "$SERIAL_LOG"
if [ "$CONTROLLER" = uhci ]; then
    grep -a -q 'usb: UHCI at PCI' "$SERIAL_LOG"
else
    grep -a -q 'usb: no UHCI controller found; probing xHCI' "$SERIAL_LOG"
    grep -a -q 'xhci: controller at PCI' "$SERIAL_LOG"
    grep -a -q 'xhci: configured bulk endpoints' "$SERIAL_LOG"
    if [ "$QEMU_XHCI_SECONDARY_TEST" = 1 ]; then
        grep -a -q '^xhci: discovered hcd=0 PCI ' "$SERIAL_LOG"
        grep -a -q '^xhci: discovered hcd=1 PCI ' "$SERIAL_LOG"
        grep -a -q '^xhci: discovered controllers=2' "$SERIAL_LOG"
        grep -a -q '^usb: probing xHCI hcd=0' "$SERIAL_LOG"
        grep -a -q '^usb: probing xHCI hcd=1' "$SERIAL_LOG"
        grep -a -q '^usb: selected xHCI hcd=1' "$SERIAL_LOG"
    else
        grep -a -q '^xhci: discovered controllers=1' "$SERIAL_LOG"
        grep -a -q '^usb: selected xHCI hcd=0' "$SERIAL_LOG"
    fi
    if [[ "$QEMU_KERNEL_CMDLINE" == *xhci-msix-test* ]]; then
        grep -a -x -q $'XHCI_MSIX_OK\r' "$SERIAL_LOG"
        ! grep -a -q 'XHCI_MSIX_FAIL' "$SERIAL_LOG"
        if [[ "$QEMU_KERNEL_CMDLINE" == *xhci-recovery-test* ]]; then
            test "$(grep -a -c '^xhci: MSI-X vector=[0-9][0-9]*' "$SERIAL_LOG")" -eq 4
            test "$(grep -a '^xhci: MSI-X vector=[0-9][0-9]*' "$SERIAL_LOG" |
                sort -u | wc -l)" -eq 1
            test "$(grep -a -c '^xhci: IRQ route=[0-9][0-9]* bound' "$SERIAL_LOG")" -eq 4
            test "$(grep -a -c '^xhci: IRQ route=[0-9][0-9]* unbound' "$SERIAL_LOG")" -eq 3
            test "$(grep -a '^xhci: IRQ route=[0-9][0-9]* bound' "$SERIAL_LOG" |
                sed 's/ bound$//' | sort -u | wc -l)" -eq 1
            ! grep -a -q 'xhci: no IRQ dispatch route available' "$SERIAL_LOG"
            ! grep -a -q 'xhci: MSI-X vector release timed out' "$SERIAL_LOG"
        fi
        if [[ "$QEMU_KERNEL_CMDLINE" == *xhci-vector-reservation-test* ]]; then
            grep -a -q '^xhci: test reserved platform vector=66' "$SERIAL_LOG"
            test "$(grep -a -c '^xhci: MSI-X vector=67' "$SERIAL_LOG")" -eq 4
            ! grep -a -q 'xhci: test platform vector reservation failed' "$SERIAL_LOG"
        fi
    fi
    if [[ "$QEMU_KERNEL_CMDLINE" == *xhci-poll-test* ]]; then
        grep -a -q 'xhci: test forcing polling fallback' "$SERIAL_LOG"
        grep -a -x -q $'XHCI_POLL_OK\r' "$SERIAL_LOG"
        ! grep -a -q 'XHCI_POLL_FAIL' "$SERIAL_LOG"
    fi
    if [[ "$QEMU_KERNEL_CMDLINE" == *xhci-sg-test* ]]; then
        grep -a -x -q $'XHCI_SG_OK\r' "$SERIAL_LOG"
    fi
    if [[ "$QEMU_KERNEL_CMDLINE" == *xhci-bounce-test* ]]; then
        grep -a -x -q $'XHCI_BOUNCE_OK\r' "$SERIAL_LOG"
    fi
    if [ "$QEMU_VTD_TEST" = 1 ]; then
        grep -a -q '^vtd: DMAR haw=[0-9][0-9]* flags=0x' "$SERIAL_LOG"
        grep -a -q '^vtd: DRHD segment=0 base=0x' "$SERIAL_LOG"
        grep -a -x -q $'VTD_DMAR_OK\r' "$SERIAL_LOG"
        ! grep -a -q 'vtd: invalid DMAR table' "$SERIAL_LOG"
    fi
    if [[ "$QEMU_KERNEL_CMDLINE" == *xhci-high-bar-test* ]]; then
        grep -a -q 'xhci: test BAR relocated above 4 GiB' "$SERIAL_LOG"
    fi
    if [[ "$QEMU_KERNEL_CMDLINE" == *xhci-recovery-test* ]]; then
        test "$(grep -a -c '^xhci: test dropping bulk doorbell' "$SERIAL_LOG")" -eq 3
        test "$(grep -a -c '^xhci: transfer timeout' "$SERIAL_LOG")" -eq 3
        test "$(grep -a -c '^xhci: controller recovery complete count=' "$SERIAL_LOG")" -eq 3
        grep -a -x -q $'xhci: controller recovery complete count=3\r' "$SERIAL_LOG"
        test "$(grep -a -c '^xhci: test forcing recovery initialization failure' "$SERIAL_LOG")" -eq 1
        test "$(grep -a -c '^xhci: controller recovery failed' "$SERIAL_LOG")" -eq 1
        grep -a -x -q $'XHCI_RECOVERY_INIT_FAILURE_OK\r' "$SERIAL_LOG"
        grep -a -x -q $'XHCI_RESET_RECOVERY_OK\r' "$SERIAL_LOG"
    elif [[ "$QEMU_KERNEL_CMDLINE" == *xhci-stall-recovery-test* ]]; then
        test "$(grep -a -c '^xhci: test sending invalid BOT CBW' "$SERIAL_LOG")" -eq 2
        test "$(grep -a -c '^xhci: transfer failed ep=4 cc=6' "$SERIAL_LOG")" -eq 2
        test "$(grep -a -c '^xhci: BOT stall recovery complete count=' "$SERIAL_LOG")" -eq 2
        test "$(grep -a -c '^xhci: controller recovery complete count=' "$SERIAL_LOG")" -eq 1
        test "$(grep -a -c '^xhci: test dropping bulk doorbell' "$SERIAL_LOG")" -eq 1
        test "$(grep -a -c '^xhci: transfer timeout' "$SERIAL_LOG")" -eq 1
        grep -a -x -q $'XHCI_BOT_SELECTIVE_RECOVERY_OK\r' "$SERIAL_LOG"
        grep -a -x -q $'XHCI_BOT_STALL_FALLBACK_OK\r' "$SERIAL_LOG"
        grep -a -x -q $'XHCI_BOT_STALL_RECOVERY_OK\r' "$SERIAL_LOG"
    else
        ! grep -a -q 'XHCI_RESET_RECOVERY_FAIL\|XHCI_BOT_STALL_RECOVERY_FAIL\|xhci: controller recovery failed' "$SERIAL_LOG"
    fi
fi
grep -a -q 'usb: registered usb0p0' "$SERIAL_LOG"
grep -a -q 'Root filesystem is the USB mass-storage device.' "$SERIAL_LOG"
grep -a -q 'Root mount \[OK\]' "$SERIAL_LOG"
grep -a -q 'usb: cache synchronized' "$SERIAL_LOG"
grep -a -q 'cp: copied /icsos/work/UHCISRC.TXT -> /icsos/work/UHCIRES.TXT' \
    "$SERIAL_LOG"
! grep -a -q 'General Protection fault\|Page fault\|Double fault\|Divide by zero' "$SERIAL_LOG"
! grep -a -q 'TLB shootdown timeout\|mmio: TLB shootdown failed' "$SERIAL_LOG"
! grep -a -q 'XHCI_RESET_RECOVERY_FAIL' "$SERIAL_LOG"
! grep -a -q 'XHCI_BOT_STALL_RECOVERY_FAIL' "$SERIAL_LOG"

echo "test-qemu-usb-storage PASS hcd=$CONTROLLER"
echo "Artifacts: $ARTIFACT_DIR"