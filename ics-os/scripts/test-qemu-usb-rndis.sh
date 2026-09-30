#!/bin/bash
# q35 xHCI RNDIS USB NIC acceptance test.
# The primary USB MSC (usb0) is the boot/root image. A second device on the
# same xHCI controller is a QEMU usb-net gadget backed by SLIRP user-net.
# QEMU usb-net advertises RNDIS as config value 2 and CDC-ECM as config value
# 1; on the usb-rndis-test cmdline the kernel holds the ECM binder back and
# binds the gadget as a RNDIS NIC instead. The kernel self-test runs the full
# protocol suite over the RNDIS bulk endpoints and emits NET_* markers
# (NET_RNDIS_PASS on success), proving the RNDIS_PACKET_MSG framing and the
# encapsulated-control INIT/QUERY/SET path both work end to end.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SOURCE_IMAGE="${1:-ics-os-usb.img}"
QEMU_BIN="${QEMU_X64:-qemu-system-x86_64}"
TIMEOUT_SECONDS="${QEMU_USB_RNDIS_TIMEOUT_SECONDS:-180}"
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-$$"
ARTIFACT_DIR="${QEMU_USB_RNDIS_ARTIFACT_DIR:-/tmp/icsos-tests/usb-rndis-$RUN_ID}"
WORK_DIR="$ARTIFACT_DIR/work"
RAW_IMAGE="$WORK_DIR/usb0.img"      # primary / boot (usb0)
ISO_ROOT="$WORK_DIR/iso-root"
BOOT_ISO="$WORK_DIR/boot.iso"
SERIAL_LOG="$ARTIFACT_DIR/serial.log"
AUTOEXEC="$WORK_DIR/autoexec.bat"
QEMU_PID=""
UDP_PID=""; TCP_PID=""; DNS_PID=""

cleanup()
{
    if [ -n "$QEMU_PID" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    [ -n "$UDP_PID" ] && kill "$UDP_PID" 2>/dev/null || true
    [ -n "$TCP_PID" ] && kill "$TCP_PID" 2>/dev/null || true
    [ -n "$DNS_PID" ] && kill "$DNS_PID" 2>/dev/null || true
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

for tool in "$QEMU_BIN" grub-mkrescue python3 sfdisk mcopy; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "test-qemu-usb-rndis: missing required tool: $tool" >&2
        exit 1
    fi
done
if [ ! -f "$SOURCE_IMAGE" ]; then
    echo "test-qemu-usb-rndis: image not found: $SOURCE_IMAGE" >&2
    exit 1
fi
if [ ! -f kernel/Kernel64.bin ]; then
    echo "test-qemu-usb-rndis: kernel/Kernel64.bin is missing; run make vmdex" >&2
    exit 1
fi

mkdir -p "$WORK_DIR" "$ISO_ROOT/boot/grub"
touch "$SERIAL_LOG"

# Primary: the boot/root image (has the OS + a partitioned FAT volume).
cp "$SOURCE_IMAGE" "$RAW_IMAGE"

cp kernel/Kernel64.bin "$ISO_ROOT/vmdex"
printf '%s\n' 'set timeout=0' \
    "menuentry \"ics\" { multiboot2 /vmdex usb-rndis-test; boot }" \
    > "$ISO_ROOT/boot/grub/grub.cfg"
grub-mkrescue -o "$BOOT_ISO" "$ISO_ROOT" >/dev/null 2>&1

# Deterministic autoexec so the run is reproducible (the self-test runs during
# usb_init, before the root autoexec; this just lets the boot complete cleanly).
printf '%s\n' '@echo off' 'echo USB_RNDIS_AUTOEXEC_DONE' > "$AUTOEXEC"
PART_START="$(sfdisk -J "$RAW_IMAGE" | python3 -c \
    'import json,sys; print(json.load(sys.stdin)["partitiontable"]["partitions"][0]["start"])')"
OFFSET=$((PART_START * 512))
mcopy -o -i "${RAW_IMAGE}@@${OFFSET}" "$AUTOEXEC" ::autoexec.bat

# Host-side SLIRP probes: the guest talks to 10.0.2.2 (the SLIRP host), which
# forwards these connections to the local listeners below.
python3 scripts/udp_echo_server.py 7777 >/dev/null 2>&1 & UDP_PID=$!
python3 scripts/tcp_echo_server.py 7778 >/dev/null 2>&1 & TCP_PID=$!
python3 scripts/dns_stub.py 5353 >/dev/null 2>&1 & DNS_PID=$!
sleep 1

"$QEMU_BIN" -machine q35 -smp 1 -nographic -no-reboot -m 128M \
    -cdrom "$BOOT_ISO" -boot d \
    -drive if=none,id=stick0,format=raw,file="$RAW_IMAGE" \
    -device qemu-xhci,id=xhci \
    -device usb-storage,id=dev0,bus=xhci.0,drive=stick0 \
    -netdev user,id=n0 \
    -device usb-net,id=usbnet,bus=xhci.0,netdev=n0 \
    < /dev/null > "$SERIAL_LOG" 2>&1 &
QEMU_PID=$!

if ! timeout "$TIMEOUT_SECONDS" grep -a -m1 -q 'NET_RNDIS_PASS\|NET_RNDIS_FAIL' \
    < <(tail -n +1 -F "$SERIAL_LOG" 2>/dev/null); then
    echo "test-qemu-usb-rndis: self-test marker timed out" >&2
    tail -n 120 "$SERIAL_LOG" >&2 || true
    exit 1
fi

kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""

grep -a -q 'USB_RNDIS_NIC_OK' "$SERIAL_LOG"
grep -a -q 'NET_DHCP_OK' "$SERIAL_LOG"
grep -a -q 'NETIF_UP' "$SERIAL_LOG"
grep -a -q 'NET_PING_OK' "$SERIAL_LOG"
grep -a -q 'NET_UDP_OK' "$SERIAL_LOG"
grep -a -q 'NET_TCP_OK' "$SERIAL_LOG"
grep -a -q 'NET_TCP_REXMIT_OK' "$SERIAL_LOG"
grep -a -q 'NET_DNS_OK' "$SERIAL_LOG"
grep -a -q 'NET_SOFTNET_OK' "$SERIAL_LOG"
grep -a -x -q $'NET_RNDIS_PASS\r' "$SERIAL_LOG"
! grep -a -q 'NET_RNDIS_FAIL' "$SERIAL_LOG"
! grep -a -q 'NET_DHCP_FAIL\|NET_PING_FAIL\|NET_UDP_FAIL\|NET_TCP_FAIL\|NET_TCP_REXMIT_FAIL\|NET_DNS_FAIL' "$SERIAL_LOG"
! grep -a -q 'General Protection fault\|Page fault\|Double fault\|Divide by zero' "$SERIAL_LOG"

echo "test-qemu-usb-rndis PASS"
echo "Artifacts: $ARTIFACT_DIR"
