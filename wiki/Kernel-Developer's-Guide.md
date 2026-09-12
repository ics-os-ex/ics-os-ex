(Work in progress...)

# 1. Introduction
This guide is for software developers who would like to work on the source code of
ics-os. The suggested development platform is a linux box with the following tools installed.

  * make
  * gcc(v4.8)/tcc
  * GNU binutils (ld, strip)
  * mount
  * bochs/qemu
  * nasm
  * git

Instructions for installing the above packages differ from one linux distribution to another. Consult the documentation for the distribution that you use. (NOTE: This guide assumes a **64-bit Ubuntu 16.04** development machine).

For Ubuntu users, the following commands will install the required packages.
```
$sudo apt-get update
$sudo apt-get install build-essential nasm qemu-kvm tcc git gcc-multilib
```

The main target audience for this guide are students learning systems programming and OS kernel programming.

# 2. Obtaining the Source Code
```
$git clone https://github.com/srg-ics-uplb/ics-os.git
```

# 3. Building the Source Code
Building the source code for the kernel and the distribution disk is accomplished using ` make `. Make sure you perform steps 2-4 every time you make changes in the source code.

  1) Next, go inside the directory of the extracted source.
```
$cd ics-os/ics-os
```
  2) Remove binary files.
```
$make clean
```
  3) Build the kernel.
```
$make
```
  4) Create the distribution floppy image. Make sure that you have root privileges(use the `su` or `sudo` command).
```
$sudo make install
```
  5) Test the distribution floppy image. This does not require root privileges.
```
$qemu-system-i386 -fda ics-os-floppy.img -boot a
```

## 3.1 x86-64 self-host validation

The supported kernel compiler is GCC 4.7.4 with GNU binutils. From the
`ics-os/` directory, run `make test-kbuild`; ICS-OS runs host-seeded GCC,
cc1, GAS, and GNU ld executables in-OS, then kexecs the generated kernel. A
successful run reports `GKBUILD_TEST_PASS` and `KEXEC_BOOT_OK`. This does not
certify full self-hosting until GCC is rebuilt in-OS, the rebuilt compiler
builds the kernel, and the resulting kernel passes capability regressions.

TinyCC is an optional bootstrap compiler. `make test-selfhost` compiles small
programs, `make test-tccboot` rebuilds TinyCC, and `make test-tcc-kbuild` is an
experimental TinyCC kernel build. Failure of the TinyCC kernel experiment does
not invalidate the supported GCC self-host path.

## 3.2 VFS, devices, and asynchronous I/O

The first P0 slice now provides atomic process-context critical sections,
IRQ-safe virtio queue locking, generation-safe cache writeback, serialized VFS
lifetime transitions, per-volume FAT metadata locking, and drained io_uring
close. This is not yet the complete scalable production contract. Before changing
VFS, filesystem, device-manager, block-cache, virtio-blk, scheduler-wait, or
io_uring code, read `ics-os/docs/io-subsystem-modernization-plan.md`. It records
the verified lifetime and synchronization defects, target object model, lock
order, phased migration, and mandatory stress/fault tests.

In particular, do not use `sync_sharedvar` in IRQ context, do not
release a timed-out DMA descriptor until the device has completed it or the
queue has been reset, and do not publish shared ring indices without the
documented acquire/release ordering. Use `devmgr_getdevice_ref()` and
`devmgr_putdevice()` around operation-table callbacks; raw
`devmgr_getdevice()` is compatibility-only. Mounted VFS roots retain both
device references through unmount. IRQ-to-process notification must use
`completion_t`, not a raw `PCB386 *`; virtio reset retires every outstanding
chain before queue reuse. Completion waits use scheduler-backed hashed event
queues. Successful asynchronous read copyback must execute under the submitting
address space; process teardown retires owner requests before freeing page
tables. Do not move user-buffer callback drain to a generic worker until those
pages are pinned and kernel-mapped. Run `make test-io-unit`, `make
test-posixio`, and `make test-virtio`; the guest tests use two virtual CPUs and
remain focused regression/functional tests rather than exhaustive stress.

POSIX fd lookup must hold the process `fd_lock` until it has acquired a typed
reference on the VFS, block, or io_uring open description. `sys_close()` first
detaches the slot under that lock and then drops descriptor ownership; final
release waits for transient operations and async callbacks. Process creation
uses `posix_fd_clone()` and must never shallow-copy `fds[]` or inherit
`FD_RESERVED`. Shared file offsets and buffered VFS state are serialized per
open description. Direct legacy `FILE *` and POSIX descriptor references are
separate; SDK `fdopen()` tracks its originating descriptor and `fclose()` closes
that descriptor after flushing. fdopen-backed stdio operations route through
the POSIX descriptor syscalls, not raw VFS calls. Positioned block operations,
async submissions, and flush submission share the block description's I/O gate
so virtqueue submission order is preserved. `make test-spawn` verifies that a
child can use an inherited VFS descriptor after the parent closes its copy.
`sys_dup()` (syscall `0xC5`) clones a live descriptor under the same `fd_lock`:
 a tty source installs a new `FD_TTY` slot, a VFS source inherits through
 `vfs_file_inherit()`, and a block source through `fd_blk_inherit()`; the reserved
 slot is released if the typed reference cannot be taken. It backs SDK `dup()` and
 interactive editors (vim starts by `dup()`-ing the console tty). Closing a dup'd
 tty descriptor (`FD_TTY` at fd >= 3) releases only the slot; it must not return
 `EBADF` and must not destroy the shared tty. `make test-dup` verifies `dup()` at
 runtime: a dup'd tty fd is allocable and closable, and a dup'd file fd's write is
 visible reading back through the original after `fsync()` (buffered VFS writes do
 not update the node size until committed).

On x86-64, syscall `0x90` implements synchronous COW `fork()` directly from the
saved interrupt frame; do not route it through `pdispatch`. Use
`userpd_clone_cow()`, build a zeroed PCB, acquire resources transactionally,
and enqueue only after the child is complete. Ordinary writable ELF mappings
are shared read-only with frame references; ELF text stays read-only. The
active user and syscall stacks are eagerly copied because user code currently
runs at CPL0. COW faults serialize page-table changes, use a one-owner writable
fast path, and synchronously invalidate matching CR3s through IPI vector
`0xFB`. Fork rejects multithreaded callers, shared page directories, in-flight
io_uring, and parents whose retained child-status capacity is exhausted.
`vfork()` is not implemented. Run `make test-fork-matrix` for the
1/2/4/8-vCPU gate.

SMP supports up to eight contiguous legacy xAPIC CPUs. `make test-smp` defaults
to four CPUs, and `make test-smp-matrix` validates 1/2/4/8. AP startup state is
published only after per-CPU and idle-task initialization, and LAPIC ICR writes
must wait for delivery-idle. Use `createkthread_on_cpu()` when affinity is known:
setting affinity after ready-queue insertion races AP scheduling. Per-CPU arrays
must use `MAX_CPUS`, never a literal topology size. Context-load/voluntary guards
and FPU save/restore scratch storage must remain per CPU. The aggregate AP work mask is
the scheduler test oracle. Each AP worker also emits `SMP_RESULT cpuid cpu=N`
from `smp_cpu_id()` (TSC_AUX/RDTSCP, not LAPIC MMIO). `IPI_RESCHEDULE` must
not `taskswitch()` a user process (IRQ-frame software switch GPFs in
`reschedwrapper`); idle/kernel threads may still switch so work-steal stays
valid. COM1 output is protected by an IRQ-safe SMP lock;
machine-consumed tests must emit one atomic `SMP_RESULT` record with
`serial_puts()` instead of parsing concurrent `printf()` prose. Sparse APIC IDs,
MADT/x2APIC discovery, NUMA, and CPU hotplug are not implemented and must not be
claimed.

The FAT cluster-chain walk is bounded and fail-closed. `get_sector_fromcluster`
(`filesystem/fat12.c`) advances a file's clusters one step at a time and must not
trust the FAT table: each step is validated by `fat_chain_step()`
(`filesystem/fat_chain.h`, a pure host-testable function). Pass the highest
legal cluster **id** (`fat_cluster_count()+1`, because FAT numbers data
clusters from 2). A next pointer that is a valid FAT32
marker ends the chain; one above that id is a corrupt volume;
and a chain that has taken more steps than that id is a loop. In
both failure cases the walk stops and returns 0 (the read fails closed) instead
of spinning or reading out of bounds. Do not re-introduce an unbounded `while`
over `obtain_next_cluster()`; if you change the walk, add a case to
`tests/fat_chain_unit.c` (`make test-fatchain-unit`).

Kernel builds are versioned and the kernel log is inspectable. `kernel/Makefile`
generates `kernel/build_info.h` (release id, git short hash, dirty flag, UTC
timestamp) at build time and `dex_init` prints a release banner at boot. Console
commands `version`, `uname [-a|-r|-m|-v]`, and `dmesg [-c|-n <lvl>|-l <lvl>]`
expose the build identity and a fixed-record kernel log (every `printf`/console
character is captured, gated live by a console-max level). Use `make test-klog`
for the boot gate and `make test-klog-unit` for the ring unit test.

## 3.3 Device-driver architecture and lifecycle

Before adding a physical driver, changing PCI/USB discovery, introducing driver
modules, or implementing runtime hotplug/reset, read
`ics-os/docs/device-driver-subsystem-architecture.md`. It defines the target typed
device/bus/driver/class object model, managed resources, no-reboot lifecycle, IRQ
and DMA/IOMMU APIs, asynchronous queues, fault recovery, user-mode isolation,
power management, observability, class frameworks, staged rollout, and required
qualification tests.

New drivers must not register raw copied `devmgr_generic` operation tables. During
migration they should bind through the typed core or its explicit legacy adapter.
Device removal must first reject new operations, then drain/cancel requests,
synchronize IRQ/work/timer callbacks, stop and revoke DMA, release managed resources,
and wait for object/module references. Never cast a CPU pointer to a DMA address,
free a timed-out descriptor still owned by hardware, invoke driver callbacks under
a global registry lock, or force-unload active in-kernel driver text.

The current USB compatibility path uses `kernel/hardware/dma.h` to validate its
identity-mapped bus addresses against alignment, overflow, the 32-bit DMA mask,
and region ownership. xHCI derives all controller-programmed addresses from
these regions, dynamically allocates aligned and zeroed coherent controller
storage with transactional unwind, and uses ordering barriers rather than
`wbinvd`. Bulk and control data stages use direction-aware streaming mappings
whose lifetime ends on every completion, timeout, stall, and disconnect path.
When available, xHCI programs MSI-X table entry 0 for a dynamically allocated
device vector targeting the BSP, enables
interrupter 0, acknowledges it in a minimal hard-IRQ handler, and consumes event
TRBs in the waiting context. Sparse `hlt` wakeups retain polling between sleeps,
so a missing completion does not turn the existing spin timeout into millions of
timer interrupts. Polling alone remains active during interrupt-disabled boot
and when MSI-X setup is unavailable.
MSI-X drivers allocate vectors from the bounded device domain through
`hardware/irq_lifecycle.h`; owner-checked reservations exclude platform vectors,
and wrappers enter and exit the owner contract. Teardown must mask the device source before
`irq_vector_release()`, which blocks new entries and waits for active handlers
to drain.
The same domain composes validated xAPIC MSI address/data messages. Drivers must
not encode `0xFEE00000` directly; x2APIC and interrupt-remapped destinations are
not yet supported.
xHCI controller registers, rings, DMA regions, device/recovery state, and IRQ
resources are owned by `xhci_hcd`. New state belongs in that object, not in
another file-scope variable. Discovery and IRQ routing support up to eight HCDs,
and the singleton storage frontend selects one; concurrent active devices are
not yet supported.
The implementation relies on the bounded identity-mapped kernel heap. Streaming
DMA supports up to 32 scatter/gather segments with transactional map unwind, and
xHCI emits bounded chained bulk TDs. Device-scoped bounce mappings provide
non-identity caller/device buffers with directional copy and mask enforcement.
Translated IOVAs, non-coherent cache maintenance, and IOMMU isolation remain
unsupported. New drivers must not infer general DMA safety from this initial
contract.
`hardware/iommu.h` provides the initial backend-neutral identity, translated,
and blocked domain ownership model. Its translated mappings are control-plane
records only until a VT-d/AMD-IOMMU/SMMU backend programs and invalidates real
hardware tables; drivers must not submit those IOVAs yet.
`hardware/vtd.c` discovers ACPI DMAR, DRHD units, and requester scopes with
checksum and bounds validation. Discovery does not enable translation. Do not set
VT-d `GCMD.TE` until root/context/page tables, cache invalidation, rollback, and
fault handling are implemented and tested.

For USB storage changes, run `make test-usb-storage`. It boots from a separate
CD and requires the image attached through QEMU UHCI to enumerate as `usb0p0`,
mount as root, issue SCSI cache synchronization, and survive guest `fsync` plus
host byte readback. `make test-usb-storage-xhci` applies the same contract on
q35 xHCI. `make test-usb-storage-xhci-sg` forces multi-segment chained bulk TDs
through that same durable contract. `make test-usb-storage-xhci-bounce` forces
bidirectional bounce mappings. `make test-usb-storage-xhci-vtd-discovery` adds
QEMU Intel-IOMMU DMAR discovery while retaining identity DMA and durable storage.
`make test-usb-storage-xhci-high-bar` repeats it with the controller
BAR above 4 GiB. `make test-usb-storage-xhci-recovery` injects three transfer
timeouts and requires repeat controller reset/re-enumeration, fail-closed reset
failure, sector equality, and durable host readback.
`make test-usb-storage-xhci-msix` requires delivery on an allocated device vector and an
IRQ-assisted event wait. `make test-usb-storage-xhci-poll` forces MSI-X off and
requires the same durable storage behavior through polling fallback.
`make test-usb-storage-xhci-msix-recovery` additionally requires exact vector
release and reclaim across three successful controller recoveries.
`make test-usb-storage-xhci-vector-reservation` reserves the first device vector
under a separate owner and requires xHCI to skip it across those recoveries.
`make test-usb-storage-xhci-stall-recovery` induces BOT stalls and requires
BOT reset, clear-halt on both bulk endpoints, stalled-endpoint reset/dequeue
repair, successful command retry, and bounded controller-reset fallback.
`make test-usb-storage-xhci-disconnect` uses QMP removal during an active bulk
transfer and requires bounded cancellation, offline subsequent I/O, no reset
attempt, and continued console startup.
`make test-usb-storage-xhci-mounted-disconnect` runs after FAT root mount and
requires parent/partition cache invalidation, dirty-page loss reporting, and
failure of a cached reread after removal. It also requires the quiescing parent
and partition registrations to disappear from new device discovery while VFS
retains its pinned references for teardown.
`make test-usb-storage-xhci-mounted-reconnect` reattaches the image and requires
a fresh discoverable parent/partition generation to read successfully while the
old mounted partition callback stays offline.
`make test-usb-storage-xhci-mounted-remount` additionally requires a descendant
workdir to block remount without detaching the old namespace, then explicitly
remounts the quiescent non-root `/icsos` namespace on the verified replacement
generation and resolves `/icsos/vmdex`. It does not replace `vfs_root`, close
open files, relocate workdirs, or provide automatic namespace recovery.
`make test-usb-storage-xhci-hotplug` runs two automatic remove/add cycles through
the BSP-pinned polling monitor. The identity-mismatch variant requires a changed
FAT serial to be rejected once and remain latched offline until detach.
`make test-usb-storage-xhci-late-attach` boots an empty controller and requires
the first device attached after console startup to publish `usb0` and `usb0p0`.
The monitor publishes raw device generations only and never remounts VFS.
`make test-usb-storage-xhci-reconnect`
then re-adds storage, requires full controller/BOT re-enumeration with unchanged
geometry, and verifies sector equality through restored raw I/O. The mismatch
variant requires a different-capacity replacement to remain offline. The
identity-mismatch variant preserves geometry, changes only the FAT volume
serial, and requires rejection. Reconnect identity also recognizes exFAT volume
serials, ext4 UUIDs, and ISO9660 volume identifiers. The no-device target
requires bounded probe failure, no `usb0` registration, and continued console
operation.

`make test-usb-uefi` is the UEFI thumbdrive boot lane: it boots the thumbdrive
image itself under OVMF from a USB mass-storage device (the realistic
removable-media path, not the ISO/IDE path) and requires OVMF firmware handoff
(`BdsDxe` loading `EFI/BOOT/BOOTX64.EFI`), `usb0p0` root selection, `Root mount
[OK]`, and AP scheduling with no GPF. `make boot-usb-uefi` is the interactive
(windowed) variant of the same boot. Both require the OVMF firmware to be
installed (the Makefile auto-detects `/usr/share/ovmf/OVMF.fd`); the test fails
clearly if it is absent.

The first implementation target is a virtual bus/device and sample async driver,
followed by the IRQ and DMA foundations and a complete virtio-blk lifecycle. Do not
attempt all hardware classes in parallel before those correctness gates pass.

## 3.4 Testing and quality assurance

Before adding test targets, changing assertion behavior, introducing CI, or making
production-readiness claims, read
`ics-os/docs/testing-and-qa-modernization-plan.md`. The current suite has strong
Multiboot2/QEMU vertical coverage of boot, SMP, execution, I/O, build tools, GCC,
and kexec, but it is primarily a serial-marker functional suite rather than a
complete unit and QA framework.

New tests should use the lowest practical layer: host-native units for pure logic,
in-kernel KTAP suites for target-dependent components, guest TAP selftests for the
public SDK/syscall ABI, and supervised QEMU or physical-hardware tests for system
behavior. Every test needs a stable ID, explicit timeout and capability metadata,
structured PASS/FAIL/SKIP results, isolated artifacts, and deterministic replay
data for randomized or fault-injected runs. A launch command without assertions is
not an automated test.

Do not silently ignore required build or emulator failures. Do not infer success
from one marker without also proving plan completion, an allowed VM termination,
and absence of panic/crash events. Concurrency-sensitive VFS, driver, IRQ, DMA,
and io_uring changes require multi-vCPU contention plus teardown, timeout, reset,
and injected-failure cases. GCC remains the canonical supported compiler; newer
GCC/Clang sanitizer and analysis builds are separate QA configurations and do not
replace self-host certification.

## 3.5 ext4 driver and host validation

The ext4 driver lives in `ics-os/kernel/filesystem/ext4.{c,h}` and is exercised
by `make test-ext4`. The target boots a virtio-blk ext4 image, reads a seeded
file, creates a directory and file, writes through the VFS, requires the guest
test marker, and then validates the post-test image with host `e2fsck -fn`
and `debugfs`. A passing guest marker alone is not sufficient.

When changing ext4 allocation or metadata, preserve these on-disk invariants:

- Unused tail bytes in block and inode bitmaps must be `0xff` through the end
  of the bitmap block, including the final four bytes.
- Block and inode bitmap checksums are stored in the group descriptor, not in
  the bitmap block tail.
- With `metadata_csum`, the GDT block-bitmap checksum is CRC32C over the full
  block bitmap using the filesystem UUID seed; the inode-bitmap checksum is
  CRC32C over only the first `inodes_per_group / 8` bytes. Neither bitmap
  checksum includes a group-number prefix.
- The group descriptor checksum is CRC32C over the little-endian group number
  followed by the descriptor with `bg_checksum` zeroed; store the low 16 bits.
- Inode checksums use the inode number and generation seed and must cover the
  inode fields selected by `i_extra_isize`.
- Directory blocks with `metadata_csum` need the 12-byte directory tail entry.

The target writes `/tmp/icsos-ext4-e2fsck.log` and
 `/tmp/icsos-ext4-debugfs.log` for inspection. Update the guest test or host
 validation when the driver gains new allocation, journaling, multi-group, or
 filesystem-feature support.
 
 ## 3.6 Multiboot2 framebuffer console (fbconsole)
 
 The framebuffer console lives in `ics-os/kernel/hardware/vga/fbconsole.{c,h}`.
 When the bootloader provides a Multiboot2 framebuffer *info* tag (type 8),
 `main()` in `kernel32.c` hands it to `fbconsole_boot_init()`, the console
 renderer is switched to the framebuffer (1024x768x32 RGB), and a self-test
 prints `FBCONSOLE_PASS`. Without the tag the legacy VGA text driver remains
 active and no FBCONSOLE marker appears; serial stays the headless oracle
 either way.
 
 Keep these Multiboot2 v2.0 invariants in `kernel/startup/startup.S` — GRUB
 fails the whole boot with `error: unsupported tag: 0x8` if they break:
 
 - The framebuffer *header* tag is `type=5`, `flags=1` (optional), `size=20`
   (type, flags, size, width, height, depth), followed by the end tag
   (`type=0`, `size=8`).
 - Every tag in the header table must start at an 8-byte-aligned address.
   Padding between tags is *not* counted in the tag's `size` field, and the
   header `length` includes that padding.
 - Header tag types must never collide with *info* tag types (0x8 is the
   framebuffer info tag and is only valid in the Multiboot2 information
   structure). The bootloader may legally omit the info tag; a compliant
   kernel must still boot without it.
 
 The info tag is consumed in two phases: `fbconsole_boot_init()` records the
 geometry, and `fbconsole_deferred_init()` maps it. Framebuffers below 4 GiB
 are mapped immediately via `mmio_mark_uncacheable`; framebuffers at or above
 4 GiB (BIOS VBE on QEMU, e.g. `0xfd000000`) must wait until after memory
 initialization.
 
 GRUB only emits the info tag when its video subsystem is present. The EFI
 image and the BIOS `CORE_IMG` embedded in the thumbdrive MBR gap are built
 in `scripts/mkusb.sh`; both module lists must keep `video all_video`.
 `make test-boot`, `make test-usb-uefi`, and `make test-ide-thumbdrive` all
 assert `FBCONSOLE_PASS` and cover the three GRUB paths (BIOS VBE, UEFI GOP,
 embedded i386-pc core).
 
 # 4. Source Code Directory Structure
Top level directories.

| **Directory** | **Description** |
|:--------------|:----------------|
|`apps/`        |Executables of application programs |
|`apps-old/`    |Executables of old application programs|
|`base/`        |Contains files that will be on the root directory of the floppy distribution|
|`boot/`        |Contains files for grub|
|`contrib/`     |Sources for applications|
|`kernel/`      |Kernel sources directory|
|`lib/`         |Binaries of extension modules|
|`mnt/`         |Temporary folder for mounting the floppy image when creating the distribution|
|`sdk/`         |Libraries for application development|

Kernel source directories.

| **Directory** | **Description** |
|:--------------|:----------------|
|`console/`     |Kernel console (`makeboot`, `tccboot`, `spawntest`, …)|
|`devmgr/`      |Sources for the device and extension manager|
|`dexapi/`      |Sources for setting up the system call table|
|`docs/`        |Documentation files for kernel|
|`filesystem/`  |Sources for filesystem support (fat12, iso9660, and ext4)|
|`grub/`        |Files needed by grub|
|`hardware/`    |Sources for hardware device drivers (ATA PIO, UHCI, virtio-blk, …)|
|`iomgr/`       |I/O manager (bio, per-device blk-mq lock, 4KiB page cache)|
|`vfs/`         |VFS plus POSIX fd table, io_uring (`posixfd.c`), `waitpid`/`posix_spawn`/`execve`; `/dev/vblk` and optional FAT `/work`|
|`memory/`      |Memory management routines|
|`mnt/`         |Temporary mount directory|
|`module/`      |Implementation of supported executable file formats (PE, ELF)|
|`process/`     |Process management routines|
|`startup/`     |Contains startup routines after bootloader finishes(enables 32-bit protected mode|
|`stdlib/`      |Standard library routines|
|`vfs/`         |Virtual File System implementation|
|`vmm/`         |Virtual Memory Management implementation|


# 5. A Hello World Example
This section describes an example on how to modify ics-os, specifically the kernel by adding a `hello` command which displays a message. After extracting the sources, open the file `kernel/console/console.c` on a text editor from the top level directory of the extracted source. Locate the function
`int console_execute(const char *str)`. Find the code fragment before the START comment line in the code fragment below. Then insert the code fragment between the START and END comment lines on the location as shown below. Note that the code fragment between the START and END comment lines is not present on the original body of the function.

```
    //check if a pathcut command was executed
    if (u[command_length - 1] == ':') 
                {
                    char temp[512];
                    sprintf(temp,"cd %s",u);            
                    console_execute(temp); 
                }
                else
    /*----------------------START------------------*/
    if (strcmp(u,"hello")==0)
                {
                   printf("Hello World command!\n");
                }
		else
    /*-----------------------END------------------*/
    if (strcmp(u,"fgman")==0)
                {
                    fg_set_state(1);
                }

```

Perform steps 2-4 of Section 3 to build the source. You should see something similar to the figure below after typing `hello` on the command prompt and pressing enter.

![http://ics-os.googlecode.com/svn/trunk/ics-os/kernel/docs/figure01.png](http://ics-os.googlecode.com/svn/trunk/ics-os/kernel/docs/figure01.png)

# 6. Understanding the Kernel Makefile
In order to create the floppy distribution image of ics-os, it uses the `make` utility
to build the sources. For a detailed explanation of this utility, please read the <a href='http://www.gnu.org/software/make/manual/make.html'>GNU Make</a> manual. The primary input to `make` is a makefile. In ics-os, there are two makefiles, `Makefile` and `kernel/Makefile`. A simple makefile is composed of rules with the following syntax.
```
target ... : prerequisites ...
             command
             ...
             ...
```
Shown below are the contents of `kernel/Makefile` (see source code for updated version).
```
CC=gcc
CFLAGS=-w  -nostdlib -fno-builtin -ffreestanding -c
ASM=nasm
ASMFLAGS=-f elf

bzImage: all
	gzip -c -9 Kernel32.bin >  vmdex
	cp vmdex ..

all: obj Kernel32.bin

obj: scheduler.o fat.o iso9660.o devfs.o iomgr.o devmgr_error.o kernel32.o \
		startup.o asmlib.o irqwrap.o 
	strip --strip-debug *.o
		
kernel32.o: kernel32.c build.h
	$(CC) $(CFLAGS) -o kernel32.o kernel32.c 
	
scheduler.o:
	$(CC) $(CFLAGS) -o scheduler.o process/scheduler.c

fat.o:	
	$(CC) $(CFLAGS) -o fat.o filesystem/fat12.c
	
iso9660.o:
	$(CC) $(CFLAGS) -o iso9660.o filesystem/iso9660.c
	
devfs.o:
	$(CC) $(CFLAGS) -o devfs.o filesystem/devfs.c
	
iomgr.o:
	$(CC) $(CFLAGS) -o iomgr.o iomgr/iosched.c
	
devmgr_error.o:
	$(CC) $(CFLAGS) -o devmgr_error.o devmgr/devmgr_error.c

startup.o:
	$(ASM) $(ASMFLAGS) -o startup.o startup/startup.asm
	
asmlib.o:
	$(ASM) $(ASMFLAGS) -o asmlib.o startup/asmlib.asm 

irqwrap.o:
	$(ASM) $(ASMFLAGS) -o irqwrap.o irqwrap.asm
	
Kernel32.bin:
	ld -T lscript.txt -Map mapfile.txt

clean:
	rm -f *.o
	rm -f Kernel32.bin
	rm -f vmdex
```

When you run `make`, what happens is that the target `bzImage` is processed first because it is the very first target. The prerequisite for this target is also a target, `all`. Thus, the target `all` will be processed first before the commands for the target `bzImage` are executed. If you look at the target `all`, notice that the prerequisites are also targets and thus will be processed first. The processing is thus recursive. Majority of the commands for the targets invoke the C compiler and assembler defined as variables at the start of the makefile. For example the command for the `scheduler.o` target becomes `gcc -w -nostdlib -fno-builtin -ffreestanding -c -o scheduler.o process/scheduler.c` when executed.

Let us focus our attention on the `Kernel32.bin` target which is the target for creating the final kernel image. Unlike the other targets, the command for this target invokes the linker `ld`. Detailed information on the `ld` command is available <a href='http://sourceware.org/binutils/docs/ld/index.html'>here</a>. Basically, what a linker does is to combine several input files and archives into a single output file. When you compile a program, the final step is usually to invoke the linker. In the case of ics-os, it is composed of several object files (those targets ending in .o). The single kernel image file (Kernel32.bin) is created by invoking the `ld` command. The file `lscript.txt` is the linker script that describes how the output file is to be created. The contents of lscript.txt is shown below.
```
OUTPUT_FORMAT("elf32-i386")
ENTRY(startup)
SECTIONS {
  .text 0x00100000 :{
    *(.text)
  }
  textEnd = .;
  .data :{
    *(.data)
    *(.rodata)
  }
  dataEnd = .;
  .bss :{
    *(.common)
    *(.bss)
  }
  bssEnd = .;
}
INPUT(startup.o asmlib.o kernel32.o scheduler.o iomgr.o fat.o iso9660.o
      devfs.o irqwrap.o devmgr_error.o)
OUTPUT(Kernel32.bin)
```
The first line of the linker script specifies the type of executable to produce, elf32-i386. There are several executable file formats available but in the case of ics-os, we want to use ELF which is used in linux. The second line specifies `startup` as the entry point for the operating system to begin its execution. The entry point is a symbol (a label in assembly) to jump to. This symbol is defined in the file `kernel/startup/startup.asm`. `SECTIONS` specify the memory area where the instructions(.text) and data(.data) will be placed which in ics-os case is at memory location 0x00100000. The `INPUT` section specifies the input files which are the object files and the `OUTPUT` section specifies the output kernel image file. After linking, the linker generates a map file that summarizes how it created the output file. A portion of the generated mapfile(`mapfile.txt`) is shown below.
```
.text           0x0000000000100000    0x270d4
 *(.text)
 .text          0x0000000000100000      0x2d2 startup.o
                0x00000000001002ca                reset_gdtr
                0x0000000000100000                startup
 *fill*         0x00000000001002d2        0xe 00
 .text          0x00000000001002e0      0x5fe asmlib.o
                0x00000000001003f3                pci_writeconfigdword
                0x00000000001003a7                pci_writeconfigbyte
                0x00000000001005dc                refreshpages

```

The final kernel image `Kernel32.bin` is then gzipped into `vmdex` to conserve space. Control is transferred to this image after GRUB has loaded.



# 7. startup.asm
The file [`kernel/startup/startup.asm`](https://github.com/srg-ics-uplb/ics-os/blob/devel/ics-os/kernel/startup/startup.asm) enables the 32-bit protected mode of x86, enables the A20 line, and transfers control to the `main()` function in `kernel32.c`. [Here](http://www.brokenthorn.com/Resources/OSDev8.html) is a link to a more detailed discussion of protected mode.

# 8. kernel32.c
The file  [`kernel/kernel32.c`](https://github.com/srg-ics-uplb/ics-os/blob/devel/ics-os/kernel/kernel32.c) is the main entry point of the ics-os. The following steps are performed in `main()`

  1. Program IRQ lines for timer, keyboard, and floppy 
  1. Set up the interrupt descriptor table 
  1. Obtain boot device and memory information from GRUB
  1. Initialize memory subsystem 
  1. Sets the current process to the kernel process `_sPCB_`. This structure will be initialized at a later stage
  1. Setup context switch timer 
  1. Initialize bridge manager
  1. Initialize virtual console manager
  1. Initialize kernel virtual console for kernel messages

After the above operations, memory access should be saved. Control is transferred to the `dex32_startup()` function.

  1. Print CPU information
  1. Print available memory
  1. Initialize extension manager
  1. Initialize device manager
  1. Register memory manager and memory allocator
  1. Initialize `malloc()` provider
  1. Initialize ports
  1. Initialize kernel api
  1. Initialize process manager and start the task switcher

The task switcher calls the `dex_init()` function which is essentially the first "process" that is executed. It performs the following operations
  1. Initialize the keyboard
  1. Installs the floppy driver
  1. Initialize the ide driver
  1. Initialize the vga driver
  1. Initialize the I/O manager
  1. Initialize the virtual file system
  1. Initialize the task manager
  1. Initialize the Disk I/O manager
  1. Initialize null block device
  1. Initialize device filesystem driver
  1. Install the FAT12 filesystem driver
  1. Install the  ISO9660 filesystem driver
  1. Mount the floppy device
  1. Initialize module loader
  1. Run foreground manager thread
  1. Create a new instance of console
  1. Start the process dispatcher

# 8. User virtual memory (x86-64)

User processes have a private PML4.  Within the first GiB of VA:

- **sbrk / malloc** grow up from `0x0A000000` (`MEM_USER_HEAP`) toward `mmap_brk`.
- **Anonymous mmap** (`int 0x30` function `0xB6`) grows down from `MEM_USER_HEAP_LIMIT` (`0x3FD00000`).  `munmap` is `0xB7` and unmaps 4KiB private frames.
- The two regions must not meet.  This keeps GCC's zone collector pages (mmap) out of the malloc arena so GGC's 4KiB page-table lookup cannot collide with large `xmalloc` objects.

File-backed `mmap` remains SDK malloc-backed until a kernel file map exists.
