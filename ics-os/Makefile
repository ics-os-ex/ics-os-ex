# ics-os Makefile
#
# written by Joseph Anthony C. Hermocilla
#

vmdex:
	make -C kernel/

# Build and install contributed applications (netdiag etc.)
.PHONY: contrib-apps
contrib-apps:
	$(MAKE) -C contrib/netdiag install
	$(MAKE) -C contrib/dmesg install

.PHONY: clean
clean:
	rm -f vmdex
	rm -f ics-os-livecd.iso
	rm -fr tmp/*
	make -C kernel/ clean

update_kernel_only: vmdex
	sudo rm -fr mnt
	sudo mkdir mnt
	sudo mount ics-os-floppy.img mnt -tmsdos -oloop
	sudo cp -r vmdex mnt
	sudo umount mnt
	sudo chmod 666 ics-os-floppy.img

prep_image: vmdex
	rm -fr tmp
	mkdir tmp
	cp -r vmdex tmp
	scripts/gen-help.sh
	cp  base/* tmp
	mkdir -p tmp/apps
	mkdir -p tmp/tcc1
	mkdir -p tmp/lib1
	cp apps/* tmp/apps/
	cp sdk/* tmp/tcc1/
	cp lib/* tmp/lib1/

floppy: contrib-apps prep_image
	cp grub.img ics-os-floppy.img                #copy an image with grub
	-sudo umount mnt 2>/dev/null || true         #unmount if already mounted
	sudo rm -fr mnt
	sudo mkdir mnt
	sudo mount ics-os-floppy.img mnt -tmsdos -oloop
	sudo rm -f mnt/apps/*.exe mnt/*.bat mnt/*.hlp mnt/*.txt mnt/license mnt/vmdex 2>/dev/null || true
	sudo rm -rf mnt/tcc1 mnt/lib1 2>/dev/null || true
	sudo mkdir -p mnt/apps mnt/tcc1 mnt/lib1
	sudo cp tmp/apps/* mnt/apps/ 2>/dev/null || true
	sudo cp tmp/tcc1/* mnt/tcc1/ 2>/dev/null || true  
	sudo cp tmp/lib1/* mnt/lib1/ 2>/dev/null || true
	sudo cp tmp/* mnt/ 2>/dev/null || true
	sudo umount mnt
	sudo chmod 666 ics-os-floppy.img
	rm -fr tmp/

livecd: prep_image
	mkdir -p tmp/boot/grub
	cp boot/grub/grub.cfg tmp/boot/grub/.
	grub-mkrescue -o ics-os-livecd.iso tmp/
	rm -fr tmp/

boot-floppy:
	qemu-system-i386 -netdev user,id=net0 -device rtl8139,netdev=net0 -audiodev none,id=none -machine pcspk-audiodev=none -drive file=ics-os-floppy.img,format=raw,if=floppy -boot a -m 64M 

boot-floppy-gdb:
	qemu-system-i386 -netdev user,id=net0 -device rtl8139,netdev=net0 -audiodev none,id=none -machine pcspk-audiodev=none -drive file=ics-os-floppy.img,format=raw,if=floppy -boot a -m 64M -s -S

boot-floppy-vnc:
	qemu-system-i386 -netdev user,id=net0 -device rtl8139,netdev=net0 -audiodev none,id=none -machine pcspk-audiodev=none -drive file=ics-os-floppy.img,format=raw,if=floppy -boot a -m 64M -vnc :1 

boot-debug:
	cd kernel ;	qemu-system-i386 -s -S -kernel Kernel32.bin -fda ../ics-os-floppy.img -m 64M -append "root (hdp0p0)" 


boot-livecd:
	qemu-system-i386 -cdrom ics-os-livecd.iso -boot d -m 64M

# Add convenience targets for modern development
boot-floppy-modern:
	qemu-system-i386 -netdev user,id=net0 -device rtl8139,netdev=net0 -audiodev none,id=none -drive file=ics-os-floppy.img,format=raw,if=floppy -boot a -m 256M -enable-kvm

# Test target for AMD64 compatibility (32-bit OS on 64-bit host)
boot-floppy-amd64:
	qemu-system-x86_64 -netdev user,id=net0 -device rtl8139,netdev=net0 -audiodev none,id=none -drive file=ics-os-floppy.img,format=raw,if=floppy -boot a -m 256M -cpu qemu32

# UEFI and USB Boot Support
uefi-iso: vmdex
	./scripts/build-uefi.sh iso

usb-image: vmdex
	./scripts/build-uefi.sh usb

uefi-all: vmdex
	./scripts/build-uefi.sh all

# UEFI Testing Targets
boot-uefi-iso: uefi-iso
	qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd -cdrom ics-os-uefi.iso -m 256M -netdev user,id=net0 -device rtl8139,netdev=net0

boot-uefi-iso-fallback: uefi-iso
	qemu-system-x86_64 -cdrom ics-os-uefi.iso -m 256M -netdev user,id=net0 -device rtl8139,netdev=net0

boot-usb: usb-image
	qemu-system-x86_64 -drive format=raw,file=ics-os-usb.img -m 256M -netdev user,id=net0 -device rtl8139,netdev=net0

boot-usb-uefi: usb-image
	qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd -drive format=raw,file=ics-os-usb.img -m 256M -netdev user,id=net0 -device rtl8139,netdev=net0

# Clean UEFI build artifacts
clean-uefi:
	rm -f ics-os-uefi.iso ics-os-usb.img
	./scripts/build-uefi.sh clean

# USB Testing Targets with Virtual USB Devices
boot-usb-test: usb-thumb.img
	qemu-system-i386 -netdev user,id=net0 -device rtl8139,netdev=net0 -audiodev none,id=none -drive file=ics-os-floppy.img,format=raw,if=floppy -boot a -m 256M -device piix3-usb-uhci,id=uhci -device usb-storage,bus=uhci.0,drive=usbstick -drive if=none,id=usbstick,file=usb-thumb.img,format=raw

boot-usb-test-modern: usb-thumb.img
	qemu-system-i386 -netdev user,id=net0 -device rtl8139,netdev=net0 -audiodev none,id=none -drive file=ics-os-floppy.img,format=raw,if=floppy -boot a -m 256M -enable-kvm -device piix3-usb-uhci,id=uhci -device usb-storage,bus=uhci.0,drive=usbstick -drive if=none,id=usbstick,file=usb-thumb.img,format=raw

boot-usb-test-amd64: usb-thumb.img
	qemu-system-x86_64 -netdev user,id=net0 -device rtl8139,netdev=net0 -audiodev none,id=none -drive file=ics-os-floppy.img,format=raw,if=floppy -boot a -m 256M -cpu qemu32 -device piix3-usb-uhci,id=uhci -device usb-storage,bus=uhci.0,drive=usbstick -drive if=none,id=usbstick,file=usb-thumb.img,format=raw

# Create virtual USB thumb drive
usb-thumb.img:
	dd if=/dev/zero of=usb-thumb.img bs=1M count=64
	mkfs.fat -F 32 usb-thumb.img
	mkdir -p usb-mount
	sudo mount -o loop usb-thumb.img usb-mount
	echo "Hello from USB thumb drive!" | sudo tee usb-mount/readme.txt
	echo "ICS-OS USB Test File" | sudo tee usb-mount/test.txt
	sudo umount usb-mount
