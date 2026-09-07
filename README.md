![Alt Text](./ics-os.gif)

## About

Modern real-world operating systems are too complex to be taught to undergraduates and other instructional operating systems are not complete and usable and do not work on real hardware. By providing students with a _not so complex_ working operating system to play with, they will be able to appreciate and understand deeper the concepts underlying an operating system.

Thus, this project aims to develop a simple yet operational instructional operating system for teaching undergraduate operating systems courses. ICS-OS is a fork of <a href='http://sourceforge.net/projects/dex-os'>DEX-OS</a> by Joseph Dayo.

This tree (`ics-os-ex`) is an **x86-64 long-mode** instructional kernel. It boots via GRUB **Multiboot2**, uses **software context switching**, **LAPIC/SMP**, **ISO9660 CD root**, and **ELF64 user executables**. Host-seeded GCC/binutils executables can rebuild and kexec the kernel in-OS (`make test-kbuild`), but full self-host certification remains pending until GCC rebuilds itself in-OS and that rebuilt compiler closes the kernel-build loop. **x86_64 TinyCC** remains optional.

The historical DEX/ICS-OS 32-bit path is not the active kernel. See [ics-os/docs/smp-longmode.md](ics-os/docs/smp-longmode.md) for long-mode / SMP notes.

## Downloads

Latest floppy image: <a href='https://github.com/srg-ics-uplb/ics-os/raw/master/ics-os/ics-os-floppy.img'>ics-os-floppy.img</a>

Test the floppy image in qemu.
```
$ qemu-system-i386 -fda ics-os-floppy.img
```

## Build Environment

The kernel builds on modern 64-bit Ubuntu (tested on **Ubuntu 24.04** with gcc 13) using `gcc-multilib`. Docker is optional.

Install host packages:

```
$ ./ics-os/scripts/install-deps.sh
```

### Native build (recommended)

```
$ cd ics-os
$ make clean
$ make
$ make usb
```

Boot the USB image in QEMU on a 64-bit CPU, with the thumb drive as the root filesystem:

```
$ make boot-usb-amd64
```

To emulate a real UHCI USB stick (the kernel's USB driver mounts it as `/icsos`):

```
$ make boot-usb-storage
```

Headless smoke tests (serial console, no VGA window):

```
$ make test-boot          # Multiboot2 CD: serial + Root mount [OK]
$ make test-smp           # QEMU -smp 4, LAPIC AP bring-up + per-AP work
$ make test-smp-matrix    # QEMU -smp 1/2/4/8 topology matrix
$ make test-exec          # host-built hello.exe (ELF64 CRT)
$ make test-kbuild        # in-OS GCC builds ICS-OS and kexecs the result
$ make test-usb-storage   # QEMU UHCI USB root + durable FAT write/readback
$ make test-usb-storage-xhci # QEMU q35 xHCI USB root + durable readback
$ make test-usb-storage-xhci-msix # MSI-X delivery + IRQ-assisted waits
$ make test-usb-storage-xhci-msix-recovery # IRQ release/reclaim across reset
$ make test-usb-storage-xhci-vector-reservation # reserved-vector exclusion
$ make test-usb-storage-xhci-poll # forced polling fallback + durable readback
$ make test-usb-storage-xhci-high-bar # xHCI BAR above 4 GiB
$ make test-usb-storage-xhci-recovery # timeout/reset/re-enumeration recovery
$ make test-usb-storage-xhci-stall-recovery # BOT/endpoint stall recovery
$ make test-usb-storage-xhci-disconnect # QMP unplug during active I/O
$ make test-usb-storage-xhci-mounted-disconnect # cache invalidation + device quarantine
$ make test-usb-storage-xhci-mounted-reconnect # replacement generation isolation
$ make test-usb-storage-xhci-mounted-remount # controlled non-root namespace recovery
$ make test-usb-storage-xhci-hotplug # two automatic remove/add cycles
$ make test-usb-storage-xhci-hotplug-identity-mismatch # reject and latch changed media
$ make test-usb-storage-xhci-late-attach # first device attached after boot
$ make test-usb-storage-xhci-reconnect # QMP remove/add + raw I/O recovery
$ make test-usb-storage-xhci-reconnect-mismatch # reject changed geometry
$ make test-usb-storage-xhci-reconnect-identity-mismatch # reject changed volume ID
$ make test-usb-storage-xhci-no-device # clean xHCI no-device failure path
$ make test-vbox-usb-image      # BIOS USB image + persistent FAT write/readback
$ make test-vbox-usb-image-efi  # UEFI USB image + persistent FAT write/readback
$ make test-integration   # test-boot + test-smp + test-exec
$ make test-selfhost      # optional TinyCC: compile and run small C programs
$ make test-tccboot       # optional in-OS TinyCC bootstrap
# make test-tcc-kbuild    # optional TinyCC kernel experiment
# make test-iobench       # skipped until disk_mgr is stable on x86_64
```

Agent/contributor notes: see [AGENTS.md](AGENTS.md).

The kernel is **ELF64** and boots via GRUB `multiboot2`. QEMU `-kernel` cannot load this image — use an ISO or USB image.

Interactive VGA (GUI) boot for manual testing:

```
$ qemu-system-x86_64 -smp 2 -m 256M -display gtk \
    -cdrom /tmp/icsos-gui.iso -boot d
```

(`make boot-usb-amd64` also opens a QEMU window from `ics-os-usb.img`.)

Write `ics-os-usb.img` to a physical thumb drive (BIOS/CSM firmware can boot it as a disk; the kernel also has a UHCI USB mass-storage driver):

```
$ sudo dd if=ics-os-usb.img of=/dev/sdX bs=4M status=progress conv=fsync
```

For Intel N150 hardware, capacity sizing, safe flashing, emulator evidence, and
the remaining xHCI blockers, see [Intel N150 USB boot and working-storage readiness](ics-os/docs/intel-n150-usb-readiness.md).

For a firmware-bootable hybrid image that works like `dd` of a live USB (BIOS and UEFI):

```
$ make livecd
$ sudo dd if=ics-os-livecd.iso of=/dev/sdX bs=4M status=progress conv=fsync
$ make boot-livecd-amd64
```

The FAT USB image is mounted as `/icsos` (the root filesystem). The live CD uses ISO9660 (`cdfs`) instead.

### Floppy and live CD (legacy)

```
$ sudo make floppy
$ make boot-floppy
```

```
$ make livecd
$ make boot-livecd
```

### Using Docker to build

Docker is optional. The active kernel is **64-bit**; a 32-bit toolchain is only needed for leftover i386 bits. You need to install
[docker](https://docs.docker.com/engine/install/ubuntu/) and [docker-compose](https://docs.docker.com/compose/install/)
to build inside a container.

Run the following command to enter the build environment:

`$docker-compose run ics-os-build`

or if you are using the docker-compose plugin:

`$docker compose run ics-os-build`

You will be dropped to a shell where you can perform the build. The ics-os folder is mapped inside the container. Thus,
you can perform the edits outside the container(in another terminal) and the changes will be reflected inside the build environment.

```
#cd /home/ics-os
#make clean
#make
#make usb
#exit
```

See [Lab 01](https://github.com/srg-ics-uplb/ics-os/blob/master/labs/lab01/ICSOS_Lab01.pdf) for a more complete discussion of how
to set up the build environment.

## Current enhancements (ics-os-ex)

### x86-64 long mode and SMP

- Kernel builds as **ELF64** (`-m64`, `lscript64.ld`) and enters long mode from a Multiboot2 32-bit stub.
- **Software context switch** + `fxsave`/`fxrstor` (no hardware TSS task switching).
- **Priority round-robin** scheduler with a spinlock-protected ready queue.
- **LAPIC** init/timer/EOI and **AP bring-up** for up to eight xAPIC CPUs; APs unpark after root mount and each executes pinned scheduler work (`make test-smp-matrix` covers 1/2/4/8 vCPUs).
- User processes / console / `fg_mgr` are BSP-pinned today; APs run migratable kthreads.

### Userland, GCC, TinyCC, and self-hosting

- Host apps and in-OS binaries are **ELF64** (`sdk/app.mk`).
- Vendored **TinyCC 0.9.27** with an **x86_64 backend** (`contrib/tcc/x86_64-gen.c`, `x86_64-link.c`).
- SDK is LP64 (`sdk/include/`, `tccsdk.c`, `posix.c`, `crt1.c`); x86_64 `va_list` uses compiler builtins.
- **In-kernel FAT16 ramdisk** at `/ramdisk` for writable compile output (CD is read-only).
- `make test-selfhost` stages `tcc.exe` + sources onto `/ramdisk`, compiles `min.c` and `hello.c` inside the OS, and runs them (`SELFHOST_TEST_PASS`).
- `make test-kbuild` is the supported kernel-build path: host-seeded GCC → cc1 → GAS → GNU ld runs in-OS, builds the kernel, then kexec boots it. This is not by itself full self-host certification.
- Console commands: `kbuild` (GCC), `gkbuild` (GCC alias), `tcckbuild` (optional), `selfhost`, `exectest`, `tccboot`, `iobench`, `cc`.
- `make test-tccboot` rebuilds TinyCC inside ICS-OS. TinyCC kernel compilation is retained only as the optional `make test-tcc-kbuild` experiment.
- ISO9660 multi-sector directories skip sector padding (large dirs such as `/src/tcc` list all files).

### I/O performance

Sequential reads of large files (e.g. the ~210 KB `tcc.exe`) are much faster than the previous per-cluster wait path:

| Pass | ~210 KB `tcc.exe` map |
|------|------------------------|
| Cold | ~98 ms |
| Warm (block cache) | &lt;1 ms |

Techniques used:

- **FAT extent coalescing** — contiguous cluster runs become multi-sector IDE/USB requests (up to 64 sectors).
- **Generic block cache** (`kernel/iomgr/blkcache.c`, 512 KiB) shared via the I/O manager.
- **BPB caching** — avoid re-reading the boot sector on every open.
- **Eager file map** — `vfs_mapfile()` / `user_execp` `[mmap]` path for loading executables; POSIX `mmap` with an fd fills through the same reader.
- **USB MSC** — multi-block SCSI READ10/WRITE10 instead of one sector per command.
- **FAT32 create/alloc fixes** — correct free-cluster scan and directory slot lookup so in-OS compilers can write outputs.

### Other kernel / userland notes

- ELF user processes with larger heap/stack; kernel identity map retained in user page directories for syscalls.
- Headless-friendly exception dumps (serial) without keyboard pause.
- LFN support enabled for long source filenames on FAT.

See also `ics-os/contrib/tcc/README.icsos` and `ics-os/base/icsos.hlp`.

## Development and Support
This project is used at the <a href='http://www.ics.uplb.edu.ph'>Institute of Computer Science</a>, <a href='http://www.uplb.edu.ph'>University of the Philippines Los Banos</a> for <a href='http://ics.uplb.edu.ph/courses/ugrad/cmsc/125'>CMSC 125</a>. It is maintained by the <a href='https://srg-ics-uplb.github.io'>Systems Research Group</a>.

Get started by reading the <a href="https://github.com/srg-ics-uplb/ics-os/wiki/Kernel-Developer's-Guide">Kernel Developer's Guide</a>.

Don't forget to check the <a href="http://github.com/srg-ics-uplb/ics-os/wiki">Wiki</a>.

You can ask questions by submitting an issue.

## Citation

If you find his resource useful in your research or teaching, please cite our [paper](https://jachermocilla.org/publications/hermocilla-pitj2009-ics-os.pdf).

---

J. A. C. Hermocilla. Ics-os: A kernel programming approach to teaching operating system concepts. Philippine Information Technology Journal, 2(2):25--30, 2009.

---

You can also use the following bibtex entry.

```
@article{hermocilla-ics-os-pitj2009,
  author = {Hermocilla, J. A. C.},
  title = {ICS-OS: A Kernel Programming Approach to Teaching Operating System Concepts},
  journal = {Philippine Information Technology Journal},
  volume = {2},
  number = {2},
  year = {2009},
  issn = {2012-0761},
  pages = {25--30},
  publisher = {Philippine Society of Information Technology Educators and Computing Society of the Philippines },
  address = {Philippines},
  pdf = {https://jachermocilla.org/publications/hermocilla-pitj2009-ics-os.pdf}
}
```
