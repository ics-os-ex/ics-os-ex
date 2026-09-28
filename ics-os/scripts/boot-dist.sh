#!/bin/bash
# Boot an ICS-OS distribution thumb-drive image in QEMU with a display window.
#
# Usage:
#   scripts/boot-dist.sh [IMAGE] [MODE] [extra QEMU args...]
#
# IMAGE:
#   Path to the thumb-drive image. Defaults to ics-os-dist.img, then
#   ics-os-uefi.img, in the ICS-OS source root. If a known image is missing
#   and AUTO_BUILD=1 (default), the script builds it with the matching make
#   target before launching QEMU.
#
# MODE:
#   auto   choose UEFI for GPT/ics-os-uefi images, BIOS otherwise (default)
#   bios   boot through SeaBIOS/GRUB i386-pc on an IDE drive
#   uefi   boot through OVMF q35 + xHCI
#
# Environment overrides:
#   QEMU=qemu-system-x86_64
#   SMP=2
#   MEM=1024M
#   DISPLAY_TYPE=gtk|sdl|cocoa|spice|vnc|none
#   VNC_DISPLAY=:1                used only when DISPLAY_TYPE=vnc
#   VGA=std                       guest VGA model
#   SERIAL=none|null|file|stdio   default: none for GUI display, stdio for DISPLAY_TYPE=none
#   SERIAL_LOG=/tmp/icsos-boot-dist-serial.log
#   MONITOR=none|socket           default: none
#   MONITOR_SOCK=/tmp/icsos-boot-dist-monitor.sock
#   AUTO_BUILD=1                  build a missing known image with make (default: 1)
#   ALLOW_REBOOT_AUTOEXEC=1       boot even if autoexec.bat contains 'reboot'
#   DRY_RUN=1                     print the QEMU command instead of running it
#
# Examples:
#   scripts/boot-dist.sh
#   scripts/boot-dist.sh ics-os-uefi.img uefi
#   scripts/boot-dist.sh ics-os-dist.img bios -vga std
#   DRY_RUN=1 scripts/boot-dist.sh ics-os-uefi.img uefi

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

usage() {
    sed -n '2,37p' "$0" | sed 's/^# \{0,1\}//'
}

case "${1:-}" in
    -h|--help)
        usage
        exit 0
        ;;
esac

IMG="${1:-}"
MODE="${2:-auto}"
AUTO_BUILD="${AUTO_BUILD:-1}"

image_make_target() {
    case "$1" in
        ics-os-dist.img)  echo dist ;;
        ics-os-usb.img)   echo usb ;;
        ics-os-uefi.img)  echo usb-uefi ;;
        *) return 1 ;;
    esac
}

ensure_image() {
    local img="$1"
    local target
    if [ -f "$img" ]; then
        return 0
    fi
    if [ "$AUTO_BUILD" != "1" ]; then
        echo "error: image not found: $img" >&2
        echo "Set AUTO_BUILD=1 to build it, or build it manually with make." >&2
        exit 1
    fi
    if ! target="$(image_make_target "$(basename "$img")")"; then
        echo "error: image not found and no known make target for: $img" >&2
        echo "Known buildable images: ics-os-dist.img, ics-os-usb.img, ics-os-uefi.img" >&2
        exit 1
    fi
    echo "image not found: $img"
    echo "building with: make $target"
    if [ "${DRY_RUN:-0}" = "1" ]; then
        echo "DRY_RUN: skipping make $target"
        return 0
    fi
    make "$target"
    if [ ! -f "$img" ]; then
        echo "error: make $target completed but $img was not produced." >&2
        exit 1
    fi
}

if [ -z "$IMG" ]; then
    if [ -f ics-os-dist.img ]; then
        IMG=ics-os-dist.img
    elif [ -f ics-os-uefi.img ]; then
        IMG=ics-os-uefi.img
    elif [ "$AUTO_BUILD" = "1" ]; then
        case "$MODE" in
            bios) IMG=ics-os-dist.img ;;
            *)    IMG=ics-os-uefi.img ;;
        esac
    else
        echo "error: no distribution image found." >&2
        echo "Build one first:" >&2
        echo "  make dist       # ics-os-dist.img" >&2
        echo "  make usb-uefi   # ics-os-uefi.img" >&2
        exit 1
    fi
fi

ensure_image "$IMG"

case "$MODE" in
    auto)
        gpt_magic="$(dd if="$IMG" bs=512 skip=1 count=1 2>/dev/null | head -c 8 || true)"
        case "$(basename "$IMG")" in
            *uefi*)
                MODE=uefi
                ;;
            *)
                if [ "$gpt_magic" = "EFI PART" ]; then
                    MODE=uefi
                else
                    MODE=bios
                fi
                ;;
        esac
        ;;
    bios|uefi)
        ;;
    *)
        echo "error: MODE must be auto, bios, or uefi (got: $MODE)" >&2
        exit 1
        ;;
esac

# A test harness that rewrote autoexec.bat in place (e.g. `colortest` +
# `reboot`) turns a manual boot into a reboot loop. Refuse such images.
check_autoexec() {
    local autoexec
    command -v mtype >/dev/null 2>&1 || return 0
    autoexec="$(mtype -i "$IMG@@$((2048 * 512))" ::autoexec.bat 2>/dev/null | tr -d '\r')" || return 0
    if printf '%s\n' "$autoexec" | grep -qix '[[:space:]]*reboot[[:space:]]*'; then
        echo "error: $IMG autoexec.bat ends in 'reboot' (left behind by a test run):" >&2
        printf '%s\n' "$autoexec" | sed 's/^/    /' >&2
        echo "Rebuild the image (make $(image_make_target "$(basename "$IMG")" || echo usb-uefi))" >&2
        echo "or set ALLOW_REBOOT_AUTOEXEC=1 to boot it anyway." >&2
        exit 1
    fi
}
if [ "${ALLOW_REBOOT_AUTOEXEC:-0}" != "1" ] && [ "${DRY_RUN:-0}" != "1" ]; then
    check_autoexec
fi

shift $(( $# >= 2 ? 2 : $# ))
EXTRA_ARGS=("$@")

QEMU="${QEMU:-qemu-system-x86_64}"
SMP="${SMP:-2}"
MEM="${MEM:-1024M}"
SERIAL_LOG="${SERIAL_LOG:-/tmp/icsos-boot-dist-serial.log}"
MONITOR="${MONITOR:-none}"
MONITOR_SOCK="${MONITOR_SOCK:-/tmp/icsos-boot-dist-monitor.sock}"
VGA="${VGA:-std}"

if ! command -v "$QEMU" >/dev/null 2>&1; then
    echo "error: QEMU not found: $QEMU" >&2
    echo "Install qemu-system-x86 or set QEMU=/path/to/qemu-system-x86_64." >&2
    exit 1
fi

KVM_ARGS=()
if [ -r /dev/kvm ]; then
    KVM_ARGS=(-enable-kvm -cpu host)
else
    KVM_ARGS=(-cpu "qemu64,+rdtscp")
fi

if [ -z "${DISPLAY_TYPE:-}" ]; then
    case "$(uname -s)" in
        Darwin) DISPLAY_TYPE=cocoa ;;
        *)      DISPLAY_TYPE=gtk ;;
    esac
fi

if [ -z "${SERIAL:-}" ]; then
    if [ "$DISPLAY_TYPE" = "none" ]; then
        SERIAL=stdio
    else
        SERIAL=none
    fi
fi

DISPLAY_ARGS=()
if [ "$DISPLAY_TYPE" = "none" ]; then
    DISPLAY_ARGS=(-display none)
elif [ "$DISPLAY_TYPE" = "vnc" ]; then
    DISPLAY_ARGS=(-display none -vnc "${VNC_DISPLAY:-:1}")
else
    DISPLAY_ARGS=(-display "$DISPLAY_TYPE")
fi

SERIAL_ARGS=()
case "$SERIAL" in
    none)
        SERIAL_ARGS=(-serial none)
        ;;
    null)
        SERIAL_ARGS=(-serial null)
        ;;
    file)
        SERIAL_ARGS=(-serial "file:$SERIAL_LOG")
        ;;
    stdio)
        SERIAL_ARGS=(-serial stdio)
        ;;
    *)
        echo "error: SERIAL must be none, null, file, or stdio (got: $SERIAL)" >&2
        exit 1
        ;;
esac

MONITOR_ARGS=()
case "$MONITOR" in
    none)
        MONITOR_ARGS=(-monitor none)
        ;;
    socket)
        MONITOR_ARGS=(-monitor "unix:$MONITOR_SOCK,server,nowait")
        ;;
    *)
        echo "error: MONITOR must be none or socket (got: $MONITOR)" >&2
        exit 1
        ;;
esac

CMD=("$QEMU")
CMD+=("${KVM_ARGS[@]}")
CMD+=("${DISPLAY_ARGS[@]}")
CMD+=(-vga "$VGA")
CMD+=(-m "$MEM" -smp "$SMP")
CMD+=("${SERIAL_ARGS[@]}")
CMD+=("${MONITOR_ARGS[@]}")

if [ "$MODE" = "uefi" ]; then
    OVMF_FD="${OVMF_FD:-}"
    if [ -z "$OVMF_FD" ]; then
        for candidate in \
            /usr/share/ovmf/OVMF.fd \
            /usr/share/OVMF/OVMF.fd \
            /usr/local/share/OVMF/OVMF.fd; do
            if [ -f "$candidate" ]; then
                OVMF_FD="$candidate"
                break
            fi
        done
    fi
    if [ -z "$OVMF_FD" ] || [ ! -f "$OVMF_FD" ]; then
        echo "error: OVMF firmware not found." >&2
        echo "Install OVMF (for example: sudo apt install ovmf) or set OVMF_FD=/path/to/OVMF.fd." >&2
        exit 1
    fi

    CMD+=(-machine q35 -bios "$OVMF_FD")
    CMD+=(-drive "if=none,id=stick,format=raw,file=$IMG")
    CMD+=(-device qemu-xhci,id=xhci)
    CMD+=(-device usb-storage,bus=xhci.0,drive=stick)
else
    CMD+=(-drive "file=$IMG,format=raw,if=ide" -boot c)
fi

CMD+=("${EXTRA_ARGS[@]}")

echo "ICS-OS manual boot"
echo "  image:   $IMG"
echo "  mode:    $MODE"
echo "  qemu:    $QEMU"
echo "  kvm:     $([ -r /dev/kvm ] && echo yes || echo no)"
echo "  smp:     $SMP"
echo "  mem:     $MEM"
echo "  display: $DISPLAY_TYPE"
echo "  serial:  $SERIAL"
if [ "$SERIAL" != "none" ] && [ "$DISPLAY_TYPE" != "none" ]; then
    echo "  note:    a serial device is attached; the kernel may keep the framebuffer console inactive"
fi
if [ "$MONITOR" = "socket" ]; then
    echo "  monitor: socket $MONITOR_SOCK"
fi

if [ "${DRY_RUN:-0}" = "1" ]; then
    printf '%q ' "${CMD[@]}"
    echo
    exit 0
fi

exec "${CMD[@]}"
