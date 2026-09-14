#!/bin/bash
# Validate BIOS boot and persistent FAT-root writes through VirtualBox IDE.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SOURCE_IMAGE="${1:-ics-os-usb.img}"
TIMEOUT_SECONDS="${VBOX_TIMEOUT_SECONDS:-90}"
FIRMWARE="${VBOX_FIRMWARE:-bios}"
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-$$"
ARTIFACT_DIR="${VBOX_ARTIFACT_DIR:-/tmp/icsos-tests/vbox-usb-$RUN_ID}"
WORK_DIR="$ARTIFACT_DIR/work"
VM_NAME="icsos-vbox-usb-$RUN_ID"
RAW_IMAGE="$WORK_DIR/ics-os-usb.img"
VDI_IMAGE="$WORK_DIR/ics-os-usb.vdi"
ROUNDTRIP_IMAGE="$WORK_DIR/ics-os-usb-roundtrip.raw"
SERIAL_LOG="$ARTIFACT_DIR/serial.log"
PAYLOAD_SOURCE="$WORK_DIR/persist-source.txt"
PAYLOAD_RESULT="$ARTIFACT_DIR/persist-result.txt"
AUTOEXEC="$WORK_DIR/autoexec.bat"
VM_REGISTERED=0

if [ "$FIRMWARE" != bios ] && [ "$FIRMWARE" != efi ]; then
    echo "test-vbox-usb-image: VBOX_FIRMWARE must be bios or efi" >&2
    exit 1
fi

cleanup()
{
    if [ "$VM_REGISTERED" -eq 1 ]; then
        if VBoxManage list runningvms | grep -Fq "\"$VM_NAME\""; then
            VBoxManage controlvm "$VM_NAME" poweroff >/dev/null 2>&1 || true
        fi
        VBoxManage unregistervm "$VM_NAME" --delete >/dev/null 2>&1 || true
    fi
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

for tool in VBoxManage mcopy mmd mtype sfdisk python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "test-vbox-usb-image: missing required tool: $tool" >&2
        exit 1
    fi
done
if [ ! -f "$SOURCE_IMAGE" ]; then
    echo "test-vbox-usb-image: image not found: $SOURCE_IMAGE" >&2
    exit 1
fi
if [ ! -f apps/cp.exe ]; then
    echo "test-vbox-usb-image: apps/cp.exe is missing; run make apps" >&2
    exit 1
fi

mkdir -p "$WORK_DIR"
cp "$SOURCE_IMAGE" "$RAW_IMAGE"
printf 'ics-os VirtualBox persistent root write\n' > "$PAYLOAD_SOURCE"
cat > "$AUTOEXEC" <<'EOF'
@echo off
set PATH=/icsos/apps
cp-posix.exe /icsos/work/VBOXSRC.TXT /icsos/work/VBOXRES.TXT
echo VBOK
echo VBOK
echo VBOK
EOF

PART_START="$(sfdisk -J "$RAW_IMAGE" | python3 -c \
    'import json,sys; print(json.load(sys.stdin)["partitiontable"]["partitions"][0]["start"])')"
OFFSET=$((PART_START * 512))
mmd -i "${RAW_IMAGE}@@${OFFSET}" ::work
mcopy -o -i "${RAW_IMAGE}@@${OFFSET}" "$PAYLOAD_SOURCE" ::work/VBOXSRC.TXT
mcopy -o -i "${RAW_IMAGE}@@${OFFSET}" "$AUTOEXEC" ::autoexec.bat
mcopy -o -i "${RAW_IMAGE}@@${OFFSET}" apps/cp.exe ::apps/cp-posix.exe

VBoxManage convertfromraw "$RAW_IMAGE" "$VDI_IMAGE" --format VDI >/dev/null
VBoxManage createvm --name "$VM_NAME" --basefolder "$WORK_DIR" \
    --ostype Other_64 --register >/dev/null
VM_REGISTERED=1
VBoxManage modifyvm "$VM_NAME" --firmware "$FIRMWARE" --memory 256 --cpus 1 \
    --ioapic on --boot1 disk --boot2 none --boot3 none --boot4 none \
    --nic1 none --audio-enabled off --usb-ohci off --usb-ehci off \
    --usb-xhci off --uart1 0x3F8 4 --uart-mode1 file "$SERIAL_LOG" >/dev/null
VBoxManage storagectl "$VM_NAME" --name IDE --add ide --controller PIIX4 >/dev/null
VBoxManage storageattach "$VM_NAME" --storagectl IDE --port 0 --device 0 \
    --type hdd --medium "$VDI_IMAGE" >/dev/null

VBoxManage startvm "$VM_NAME" --type headless >/dev/null
if ! timeout "$TIMEOUT_SECONDS" grep -a -m1 -q \
    'VBOK' < <(tail -n +1 -F "$SERIAL_LOG" 2>/dev/null); then
    echo "test-vbox-usb-image: guest marker timed out" >&2
    VBoxManage controlvm "$VM_NAME" screenshotpng "$ARTIFACT_DIR/screen.png" \
        >/dev/null 2>&1 || true
    VBoxManage showvminfo "$VM_NAME" --log 0 > "$ARTIFACT_DIR/VBox.log" \
        2>/dev/null || true
    tail -n 80 "$SERIAL_LOG" >&2 || true
    exit 1
fi
VBoxManage controlvm "$VM_NAME" poweroff >/dev/null
VBoxManage storageattach "$VM_NAME" --storagectl IDE --port 0 --device 0 \
    --type hdd --medium none >/dev/null
VBoxManage clonemedium disk "$VDI_IMAGE" "$ROUNDTRIP_IMAGE" --format RAW >/dev/null

mcopy -i "${ROUNDTRIP_IMAGE}@@${OFFSET}" ::work/VBOXRES.TXT "$PAYLOAD_RESULT"
cmp "$PAYLOAD_SOURCE" "$PAYLOAD_RESULT"
grep -a -q 'Root mount \[OK\]' "$SERIAL_LOG"
if [ "$FIRMWARE" = bios ]; then
    grep -a -q 'boot_device_name=hdp0p0' "$SERIAL_LOG"
fi
if [ "${VBOX_EXPECT_GPT:-0}" = 1 ]; then
    grep -a -q 'GPT_DETECT hdp0' "$SERIAL_LOG"
fi
! grep -a -q 'General Protection fault\|Page fault\|Double fault' "$SERIAL_LOG"

echo "test-vbox-usb-image PASS firmware=$FIRMWARE"
echo "Artifacts: $ARTIFACT_DIR"