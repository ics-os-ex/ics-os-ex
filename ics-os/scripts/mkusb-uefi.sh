#!/bin/bash
# Build a GPT + EFI System Partition USB disk image for 64-bit UEFI firmware
# (Intel N150-class laptops) and Balena Etcher. Raw .img is a full-disk image
# with 512-byte sectors, protective MBR, one FAT32 ESP, and
# EFI/BOOT/BOOTX64.EFI. No root required: mtools + python3 GPT writer.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

IMG="${ICSOS_UEFI_IMG:-ics-os-uefi.img}"
SIZE_MB="${ICSOS_USB_SIZE_MB:-128}"
PART_START=2048
STAGE_DIR="tmp-usb"
GRUB_EFI_DIR="${GRUB_EFI_DIR:-/usr/lib/grub/x86_64-efi}"
GRUB_PC_DIR="${GRUB_PC_DIR:-/usr/lib/grub/i386-pc}"
DISK_GUID="${ICSOS_UEFI_DISK_GUID:-4943534f-5545-4649-4e31-353000000001}"
PART_GUID="${ICSOS_UEFI_PART_GUID:-4943534f-4553-5030-0000-000000000001}"

if [ ! -f vmdex ]; then
    echo "vmdex not found. Run 'make' first." >&2
    exit 1
fi

if [ ! -d "$GRUB_EFI_DIR" ] || ! command -v grub-mkimage >/dev/null; then
    echo "GRUB x86_64-efi not found (install grub-efi-amd64-bin)." >&2
    exit 1
fi

if [ "$SIZE_MB" -lt 64 ]; then
    echo "ICSOS_USB_SIZE_MB must be at least 64 (FAT32 ESP)." >&2
    exit 1
fi

rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR/boot/grub" "$STAGE_DIR/EFI/BOOT"

if [ -f /usr/share/grub/ascii.pf2 ]; then
    cp /usr/share/grub/ascii.pf2 "$STAGE_DIR/boot/grub/ascii.pf2"
fi

cat > "$STAGE_DIR/boot/grub/grub.cfg" << 'EOF'
# N150-class UEFI firmware has no COM0. Program GOP through gfxterm and keep
# that linear framebuffer for Multiboot2 (gfxpayload=keep). Do not probe
# serial: `serial port 'com0' isn't found` is expected and looks like a hang.
insmod font
if loadfont $prefix/ascii.pf2 ; then
    insmod all_video
    insmod efi_gop
    insmod gfxterm
    set gfxmode=1920x1200x32,1920x1200,2560x1600x32,1920x1080x32,auto
    if terminal_output gfxterm ; then
        set gfxpayload=keep
    fi
fi
terminal_input console
set timeout=2
set default=0

menuentry 'ICS Operating System (UEFI USB)' {
    search --file --set=root /vmdex
    set gfxpayload=keep
    echo 'Loading ICS-OS (multiboot2)...'
    multiboot2 /vmdex
    boot
}
EOF

cp "$STAGE_DIR/boot/grub/grub.cfg" "$STAGE_DIR/grub.cfg"

cp -r tmp/* "$STAGE_DIR/" 2>/dev/null || true
if [ ! -f "$STAGE_DIR/vmdex" ]; then
    echo "prep_image contents missing; recreate with the Makefile usb-uefi target." >&2
    exit 1
fi

# Removable-media path that 64-bit UEFI (including N150) looks up first.
# video + all_video + efi_gop so GRUB can hand a GOP framebuffer to Multiboot2.
grub-mkimage -O x86_64-efi -o "$STAGE_DIR/EFI/BOOT/BOOTX64.EFI" \
    -p /boot/grub \
    fat iso9660 part_gpt part_msdos multiboot multiboot2 gzio serial terminal \
    video all_video efi_gop gfxterm font normal configfile ls search echo linux boot

rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB" status=none

python3 - "$IMG" "$SIZE_MB" "$DISK_GUID" "$PART_GUID" "$PART_START" << 'PY'
import sys, struct, zlib, pathlib

img_path = sys.argv[1]
size_mb = int(sys.argv[2])
disk_guid_canonical = sys.argv[3]
part_guid_canonical = sys.argv[4]
p0_first = int(sys.argv[5])

# EFI System Partition, on-disk mixed-endian (gpt.c type table).
ESP_TYPE_OD = bytes([0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
                     0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B])

def canon_to_od(canon):
    b = bytes.fromhex(canon.replace("-", ""))
    return b[0:4][::-1] + b[4:6][::-1] + b[6:8][::-1] + b[8:16]

disk_guid_od = canon_to_od(disk_guid_canonical)
part_guid_od = canon_to_od(part_guid_canonical)

total = size_mb * 2048
last_lba = total - 1
num_entries = 128
entry_size = 128
array_bytes = num_entries * entry_size
array_sec = (array_bytes + 511) // 512
first_usable = 2 + array_sec
backup_array_lba = last_lba - array_sec
last_usable = backup_array_lba - 1
entry_lba = 2
p0_last = last_usable
if p0_first < first_usable or p0_last <= p0_first:
    raise SystemExit("ESP geometry does not fit in the GPT usable range")

def mk_entry(type_od, uniq_od, first, last, attrs, name):
    e = bytearray(entry_size)
    e[0:16] = type_od
    e[16:32] = uniq_od
    struct.pack_into("<Q", e, 32, first)
    struct.pack_into("<Q", e, 40, last)
    struct.pack_into("<Q", e, 48, attrs)
    e[56:128] = (name.encode("utf-16-le") + b"\x00\x00")[:72].ljust(72, b"\x00")
    return bytes(e)

entries = [
    mk_entry(ESP_TYPE_OD, part_guid_od, p0_first, p0_last, 0, "EFI System"),
]
while len(entries) < num_entries:
    entries.append(bytes(entry_size))
entry_array = b"".join(entries)
array_crc = zlib.crc32(entry_array) & 0xffffffff

def mk_header(my_lba, alt_lba, elba):
    h = bytearray(512)
    h[0:8] = b"EFI PART"
    struct.pack_into("<I", h, 8, 0x00010000)
    struct.pack_into("<I", h, 12, 92)
    struct.pack_into("<Q", h, 24, my_lba)
    struct.pack_into("<Q", h, 32, alt_lba)
    struct.pack_into("<Q", h, 40, first_usable)
    struct.pack_into("<Q", h, 48, last_usable)
    h[56:72] = disk_guid_od
    struct.pack_into("<Q", h, 72, elba)
    struct.pack_into("<I", h, 80, num_entries)
    struct.pack_into("<I", h, 84, entry_size)
    struct.pack_into("<I", h, 88, array_crc)
    struct.pack_into("<I", h, 16, zlib.crc32(bytes(h[0:92])) & 0xffffffff)
    return bytes(h)

img = bytearray(size_mb * 1024 * 1024)
mbr = bytearray(512)
pe = bytearray(16)
pe[4] = 0xEE
struct.pack_into("<I", pe, 8, 1)
struct.pack_into("<I", pe, 12, last_lba)
mbr[446:462] = bytes(pe)
mbr[510], mbr[511] = 0x55, 0xAA
img[0:512] = bytes(mbr)
img[512:1024] = mk_header(1, last_lba, entry_lba)
img[entry_lba * 512:entry_lba * 512 + array_bytes] = entry_array
img[backup_array_lba * 512:backup_array_lba * 512 + array_bytes] = entry_array
img[last_lba * 512:last_lba * 512 + 512] = mk_header(last_lba, 1, backup_array_lba)
pathlib.Path(img_path).write_bytes(bytes(img))
print("GPT ESP: LBA %d..%d first_usable=%d last_usable=%d"
      % (p0_first, p0_last, first_usable, last_usable))
PY

OFFSET=$((PART_START * 512))
mformat -i "${IMG}@@${OFFSET}" -v ICSOS -F ::
mcopy -i "${IMG}@@${OFFSET}" -s "$STAGE_DIR"/* ::

# BIOS/Bochs: embed i386-pc GRUB in the protective MBR + GPT gap (LBA 34..2047).
# ESP stays GPT slot 0 so the kernel still mounts usb0p0 / hdp0p0 as FAT.
if [ -f "$GRUB_PC_DIR/boot.img" ] && [ -f "$GRUB_PC_DIR/diskboot.img" ]; then
    CORE_IMG=$(mktemp)
    trap 'rm -f "$CORE_IMG"' EXIT
    grub-mkimage -O i386-pc -p '(hd0,gpt1)/boot/grub' -o "$CORE_IMG" \
        biosdisk part_gpt part_msdos fat multiboot multiboot2 gzio serial \
        terminal video all_video vbe gfxterm font configfile normal ls search echo boot
    python3 - "$IMG" "$GRUB_PC_DIR/boot.img" "$CORE_IMG" << 'PY'
import sys, pathlib
img_path, boot_path, core_path = sys.argv[1], sys.argv[2], sys.argv[3]
img = bytearray(pathlib.Path(img_path).read_bytes())
boot = pathlib.Path(boot_path).read_bytes()
core = bytearray(pathlib.Path(core_path).read_bytes())
if len(boot) < 512 or len(core) < 512:
    raise SystemExit("GRUB BIOS images are truncated")
core_lba = 34
gap_end = 2048 * 512
if core_lba * 512 + len(core) > gap_end:
    raise SystemExit("GRUB core.img does not fit in the GPT gap")
rest = len(core) - 512
rest_sectors = (rest + 511) // 512
# diskboot blocklist starts at the sector AFTER the first core sector.
core[500:508] = (core_lba + 1).to_bytes(8, "little")
core[508:510] = rest_sectors.to_bytes(2, "little")
img[0:440] = boot[0:440]
# boot.img default kernel LBA is 1 (GPT header). Point it at the core.
img[0x5c:0x64] = core_lba.to_bytes(8, "little")
img[core_lba * 512:core_lba * 512 + len(core)] = core
pathlib.Path(img_path).write_bytes(img)
print("embedded GRUB i386-pc (%d bytes) at LBA %d" % (len(core), core_lba))
PY
else
    echo "warning: grub-pc-bin missing; BIOS/Bochs boot will be unavailable"
fi

chmod 666 "$IMG" 2>/dev/null || true
echo "Created $IMG (${SIZE_MB} MiB GPT + FAT32 ESP, BIOS+UEFI)."
echo "Flash with Balena Etcher (select the .img or the .img.zip), or:"
echo "  sudo dd if=$IMG of=/dev/sdX bs=4M status=progress conv=fsync"
