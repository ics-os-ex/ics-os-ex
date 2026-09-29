#!/bin/bash
# Build the ICS-OS distribution UEFI thumbdrive image for the Intel N150,
# validate it on the N150-equivalent firmware path (OVMF + q35 xHCI), and
# print flash instructions. This is the one-command path to a flashable
# distribution:
#
#   1. Builds the kernel (vmdex) + stages the in-OS GCC toolchain (make usb-etcher
#      -> prep-dist -> mkusb-uefi.sh): GPT + FAT32 ESP, UEFI BOOTX64.EFI +
#      BIOS/Bochs GRUB, GOP framebuffer console, gcc/cc1/as/ld/ar/objcopy/make/tcc
#      + SDK runtime objects + apps.
#   2. Zips the image for Balena Etcher.
#   3. Boots it under OVMF + q35 xHCI (the firmware the N150 actually uses) and
#      asserts the same markers as `make test-usb-uefi-gpt` -- so a broken image
#      is caught here, before it ever hits the physical machine.
#   4. Prints the image path, size, sha256, and dd/Etcher flash instructions.
#
# Usage:
#   scripts/mk-n150-dist.sh                 # build + validate + zip + report
#   scripts/mk-n150-dist.sh --no-verify     # build + zip, skip the OVMF boot
#   scripts/mk-n150-dist.sh --size 256      # 256 MiB image (default 128)
#
# Env:
#   OVMF_FD        OVMF firmware (default /usr/share/ovmf/OVMF.fd)
#   QEMU_X64       qemu binary (default qemu-system-x86_64)
#   ICSOS_USB_SIZE_MB  image size in MiB (default 128; --size wins)
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

DO_VERIFY=1
SIZE="${ICSOS_USB_SIZE_MB:-128}"
OVMF_FD="${OVMF_FD:-$( [ -f /usr/share/ovmf/OVMF.fd ] && echo /usr/share/ovmf/OVMF.fd || echo /usr/share/OVMF/OVMF.fd )}"
QEMU_BIN="${QEMU_X64:-qemu-system-x86_64}"
IMG="ics-os-uefi.img"
ZIP="ics-os-uefi.img.zip"

while [ $# -gt 0 ]; do
    case "$1" in
        --no-verify) DO_VERIFY=0 ;;
        --size)      shift; SIZE="${1:-128}" ;;
        --size=*)    SIZE="${1#--size=}" ;;
        -h|--help)   grep '^# ' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown arg: $1 (see --help)" >&2; exit 2 ;;
    esac
    shift
done

echo "=============================================================="
echo " ICS-OS distribution UEFI thumbdrive (Intel N150)"
echo "=============================================================="
echo " image     = $IMG"
echo " size      = ${SIZE} MiB"
echo " verify    = $([ "$DO_VERIFY" = 1 ] && echo "yes (OVMF + q35 xHCI)" || echo "no (--no-verify)")"
echo

# --- 1. Build + zip (kernel vmdex + prep-dist + mkusb-uefi.sh + Etcher zip) ---
echo "[1/3] Building image + Etcher zip (make usb-etcher)..."
if ! ICSOS_USB_SIZE_MB="$SIZE" make usb-etcher; then
    echo "ERROR: make usb-etcher failed" >&2
    exit 1
fi
[ -f "$IMG" ] || { echo "ERROR: $IMG was not produced" >&2; exit 1; }

# --- 2. Validate on the N150-equivalent firmware path (OVMF + q35 xHCI) ---
# VERIFY_EXIT stays 0 unless validation is actually run and fails, so a skipped
# validation (no OVMF / --no-verify) never fails an otherwise good build.
VERIFY_EXIT=0
if [ "$DO_VERIFY" = 1 ]; then
    if [ ! -f "$OVMF_FD" ]; then
        echo "[2/3] WARNING: OVMF firmware not found ($OVMF_FD); skipping validation."
        echo "        Install 'ovmf' and rerun with --verify to boot-test the image."
    else
        echo "[2/3] Validating: boot under OVMF + q35 xHCI (N150 firmware path)..."
        LOG=/tmp/icsos-n150-dist-verify.log
        : > "$LOG"
        # The dist autoexec drops to an interactive shell and never reboots, so we
        # must kill QEMU ourselves once boot is fully complete. The end-of-boot
        # marker is "kernel prompt" (the dist shell), printed only after root
        # mount + SMP + console are all up. NOTE: FBCONSOLE_PASS is printed EARLY
        # (framebuffer init, ~line 23), so it must NOT be used as the break
        # condition -- only an assertion. Under TCG (no KVM) the boot is
        # CPU-bound and variable, so allow a generous window before timeout.
        "$QEMU_BIN" -display none -serial stdio -no-reboot -m 512M -smp 2 \
            -machine q35 -bios "$OVMF_FD" \
            -drive if=none,id=stick,format=raw,file="$IMG" \
            -device qemu-xhci,id=xhci \
            -device usb-storage,bus=xhci.0,drive=stick \
            < /dev/null > "$LOG" 2>&1 &
        QPID=$!
        BOOT_DONE=0
        for _ in $(seq 1 180); do
            kill -0 "$QPID" 2>/dev/null || break
            grep -aq 'kernel prompt' "$LOG" 2>/dev/null && { BOOT_DONE=1; break; }
            grep -aq 'General Protection fault' "$LOG" 2>/dev/null && break
            sleep 1
        done
        kill "$QPID" 2>/dev/null || true
        wait "$QPID" 2>/dev/null || true
        [ "$BOOT_DONE" = 1 ] || echo "  (boot did not reach the shell within the window -- see log)"

        # Same assertions as `make test-usb-uefi-gpt`.
        ok=1
        { grep -aq 'BdsDxe: loading Boot0001' "$LOG" || grep -aq 'EFI/BOOT/BOOTX64.EFI' "$LOG" || grep -aq 'BdsDxe:' "$LOG"; } || { echo "  missing: OVMF BdsDxe boot"; ok=0; }
        grep -aq 'GPT_DETECT usb0' "$LOG"            || { echo "  missing: GPT_DETECT usb0"; ok=0; }
        grep -aq 'serial console ready' "$LOG"       || { echo "  missing: serial console ready"; ok=0; }
        grep -aq 'Root mount \[OK\]' "$LOG"          || { echo "  missing: Root mount [OK]"; ok=0; }
        grep -aq 'Root filesystem is the USB mass-storage device' "$LOG" || { echo "  missing: USB mass-storage root"; ok=0; }
        grep -aq 'AP scheduling enabled' "$LOG"      || { echo "  missing: AP scheduling enabled"; ok=0; }
        grep -aq 'FBCONSOLE_PASS' "$LOG"             || { echo "  missing: FBCONSOLE_PASS"; ok=0; }
        grep -aq 'General Protection fault' "$LOG"   && { echo "  fatal: General Protection fault"; ok=0; }
        if [ "$ok" = 1 ]; then
            echo "  validation PASS (OVMF q35 xHCI boot, GPT_DETECT, root mount, AP sched, framebuffer)"
        else
            echo "  validation FAILED -- serial log: $LOG" >&2
            VERIFY_EXIT=1
        fi
    fi
fi

# --- 3. Report ---
echo "[3/3] Reporting..."
echo "=============================================================="
if [ -f "$IMG" ]; then
    echo " image : $(pwd)/$IMG"
    echo " size  : $(du -h "$IMG" | cut -f1)  ($SIZE MiB GPT + FAT32 ESP, UEFI + BIOS)"
    sha256sum "$IMG" | awk '{print " sha256: " $1}'
fi
[ -f "$ZIP" ] && echo " etcher: $(pwd)/$ZIP"
echo "=============================================================="
echo " Flash to the N150 (replace /dev/sdX -- check with 'lsblk -o NAME,SIZE,MODEL' BEFORE):"
echo "   sudo dd if=$(pwd)/$IMG of=/dev/sdX bs=4M status=progress conv=fsync && sync"
echo " or select $ZIP in Balena Etcher."
echo
echo " On first boot the GOP console shows the distribution banner; 'help' lists"
echo " commands, 'sh' opens the POSIX shell. With a Pico CDC-ACM gadget plugged"
echo " in, the remote debug bridge attaches automatically."
echo "=============================================================="

if [ "${VERIFY_EXIT:-0}" = 1 ]; then
    echo "BUILD OK, validation FAILED (see above)." >&2
    exit 1
fi
echo "DONE: $IMG"
exit 0
