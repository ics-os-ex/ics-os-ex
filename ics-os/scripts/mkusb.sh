#!/bin/bash
# Build a BIOS+UEFI bootable USB disk image that uses FAT as the root filesystem.
# Does not require root: uses mtools + a GRUB i386-pc embedder.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

IMG="${ICSOS_USB_IMG:-ics-os-usb.img}"
SIZE_MB="${ICSOS_USB_SIZE_MB:-64}"
PART_START=2048
STAGE_DIR="tmp-usb"
GRUB_PC_DIR="${GRUB_PC_DIR:-/usr/lib/grub/i386-pc}"
GRUB_EFI_DIR="${GRUB_EFI_DIR:-/usr/lib/grub/x86_64-efi}"

if [ ! -f vmdex ]; then
    echo "vmdex not found. Run 'make' first." >&2
    exit 1
fi

if [ ! -f "$GRUB_PC_DIR/boot.img" ] || [ ! -f "$GRUB_PC_DIR/diskboot.img" ]; then
    echo "GRUB i386-pc images not found in $GRUB_PC_DIR (install grub-pc-bin)." >&2
    exit 1
fi

rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR/boot/grub" "$STAGE_DIR/EFI/BOOT"

cat > "$STAGE_DIR/boot/grub/grub.cfg" << 'EOF'
# Real laptops often expose no COM0; fall back to the video console so the
# menu and OS still get a terminal when the serial probe fails.
terminal_input console
terminal_output console
if serial --unit=0 --speed=115200; then
    terminal_input serial console
    terminal_output serial console
fi
set timeout=0
set default=0

menuentry 'ICS Operating System (USB)' {
    search --file --set=root /vmdex
    echo 'Loading ICS-OS (multiboot2)...'
    multiboot2 /vmdex
    boot
}
EOF

cp "$STAGE_DIR/boot/grub/grub.cfg" "$STAGE_DIR/grub.cfg"

# Stage the same root filesystem layout used by the floppy/livecd targets.
cp -r tmp/* "$STAGE_DIR/" 2>/dev/null || true
if [ ! -f "$STAGE_DIR/vmdex" ]; then
    echo "prep_image contents missing; recreate with the Makefile usb target." >&2
    exit 1
fi

# UEFI GRUB image (64-bit firmware can still chain a 32-bit multiboot kernel).
if [ -d "$GRUB_EFI_DIR" ] && command -v grub-mkimage >/dev/null; then
    # video + all_video (efifb/GOP) so the multiboot2 loader can hand the
    # kernel a framebuffer info tag under UEFI.
    grub-mkimage -O x86_64-efi -o "$STAGE_DIR/EFI/BOOT/BOOTX64.EFI" \
        -p /boot/grub \
        fat iso9660 part_msdos part_gpt multiboot multiboot2 gzio serial terminal \
        video all_video normal configfile ls search echo linux boot || \
        echo "warning: could not build BOOTX64.EFI (UEFI boot will be unavailable)"
fi

# Create the disk image and a single FAT partition.
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB" status=none
printf 'label: dos\nlabel-id: 0x4943534f\n\nstart=%s, type=e, bootable\n' "$PART_START" | \
    sfdisk "$IMG" >/dev/null

OFFSET=$((PART_START * 512))
mformat -i "${IMG}@@${OFFSET}" -v ICSOS -F ::

# Copy staged files. mcopy -s copies directories.
mcopy -i "${IMG}@@${OFFSET}" -s "$STAGE_DIR"/* ::

# Build and embed GRUB for BIOS (i386-pc) in the gap before the first partition.
CORE_IMG=$(mktemp)
trap 'rm -f "$CORE_IMG"' EXIT

# video + all_video (VBE) so the multiboot2 loader hands the kernel a
# framebuffer info tag; +3.6 KiB, well inside the 1 MiB MBR gap.
grub-mkimage -O i386-pc -p '(hd0,msdos1)/boot/grub' -o "$CORE_IMG" \
    biosdisk part_msdos fat multiboot multiboot2 gzio serial terminal \
    video all_video configfile normal ls search echo boot

python3 - "$IMG" "$GRUB_PC_DIR/boot.img" "$CORE_IMG" << 'PY'
import sys, math, pathlib
img_path, boot_path, core_path = sys.argv[1], sys.argv[2], sys.argv[3]
img = bytearray(pathlib.Path(img_path).read_bytes())
boot = pathlib.Path(boot_path).read_bytes()
core = bytearray(pathlib.Path(core_path).read_bytes())

# boot.img kernel sector at 0x5c already defaults to LBA 1.
if len(boot) < 512:
    raise SystemExit("boot.img is truncated")

# Patch diskboot blocklist: start LBA 2, length = remaining core sectors.
if len(core) < 512:
    raise SystemExit("core.img is truncated")
rest = len(core) - 512
rest_sectors = (rest + 511) // 512
# last 12 bytes of the first sector: uint64 start, uint16 len, uint16 seg
core[500:508] = (2).to_bytes(8, "little")
core[508:510] = rest_sectors.to_bytes(2, "little")

# Keep the partition table (bytes 446-509) and 0x55AA.
img[0:440] = boot[0:440]
img[510:512] = b"\x55\xaa"

core_off = 512  # LBA 1
if core_off + len(core) > 2048 * 512:
    raise SystemExit("GRUB core.img does not fit in the MBR gap")
img[core_off:core_off + len(core)] = core
pathlib.Path(img_path).write_bytes(img)
print("embedded GRUB i386-pc (%d bytes) at LBA 1" % len(core))
PY

chmod 666 "$IMG" 2>/dev/null || true
echo "Created $IMG (${SIZE_MB} MiB). Write it to a thumb drive with:"
echo "  sudo dd if=$IMG of=/dev/sdX bs=4M status=progress conv=fsync"
