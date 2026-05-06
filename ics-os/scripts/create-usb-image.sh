#!/bin/sh
set -e

IMG="ics-os-usb.img"
SIZE_MB=${SIZE_MB:-64}

if [ ! -f vmdex ]; then
  echo "vmdex not found. Run 'make' first."
  exit 1
fi

if [ "$(id -u)" -ne 0 ]; then
  echo "This script requires root (for losetup/mount/grub-install)."
  exit 1
fi

rm -f "$IMG"

dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB"
parted -s "$IMG" mklabel msdos
parted -s "$IMG" mkpart primary fat32 1MiB 100%

LOOP=$(losetup --show -fP "$IMG")
mkfs.vfat -F 32 "${LOOP}p1"

MNT="./mnt"
mkdir -p "$MNT"
mount "${LOOP}p1" "$MNT"

# copy system files
cp vmdex "$MNT"/
cp kernel/Kernel32.bin "$MNT"/
cp -r base/* "$MNT"/
mkdir -p "$MNT/apps" "$MNT/tcc1" "$MNT/lib1"
cp apps/* "$MNT/apps/"
cp -r sdk/* "$MNT/tcc1/"
cp lib/* "$MNT/lib1/"

# install GRUB
mkdir -p "$MNT/boot/grub"
cp boot/grub/grub.cfg "$MNT/boot/grub/grub.cfg"

grub-install --target=i386-pc --boot-directory="$MNT/boot" --modules="part_msdos fat multiboot" "$LOOP"

sync
umount "$MNT"
losetup -d "$LOOP"

# fix ownership/permissions for non-root use
if [ -n "$SUDO_USER" ]; then
  chown "$SUDO_USER":"$SUDO_USER" "$IMG"
fi
chmod 664 "$IMG"

echo "Created $IMG ($SIZE_MB MB)"
