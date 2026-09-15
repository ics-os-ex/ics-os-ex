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
Networking milestones A–C plus Berkeley sockets: `make test-net-unit` and
`make test-net` (virtio-net SLIRP ICMP + UDP + TCP echo + `netecho.exe`
sockets → `NET_SOCK_OK`).

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
`serial_puts()` instead of parsing concurrent `printf()` prose. User SDK
`printf`/`vprintf` must stay bounded (`sdk/tccsdk.c`): a 1024-byte stack
`vsprintf` smashes RIP and shows as `GPF64 err=0` (noncanonical `ret`, not
a segment-index fault). COM1 output from kernel C uses register-only UART
TX (`uart_com1_putc`) in registers only. IRQ/syscall C runs on a per-process
kheap stack (`irq_kstack_enter`); RSP is switched with RDTSCP before that C
runs. That switch is stateless: `IRQ_KSTACK_ENTER` stays when the interrupted
RSP is already on `[kstack_base, kstack_top)` or on `MEM_CPUIRQ`; it switches
to the process kstack only from the user stack
(`MEM_USER_STACK_GUARD`..`MEM_USER_STACK`). A claimed user whose RSP is some
other kernel stack (publish-before-switch, leftover `current`) stays on
that stack — resetting `kstack_top` from there smashes the saved syscall
`iretq` frame (`PF64 rip=0x100000001000` on `gcc.exe`). Each wrapper reads
its own `PUSH_ALL` frame from `%r13`, and `IRQ_KSTACK_LEAVE` restores `%r13`.
A nesting counter or a PCB-held restore pointer makes a nested `#PF`/`#GP`
`iretq` from the wrong frame (`GPF64 rip=0x8 cs=0x206`). `PCB.on_cpu` is the
single-owner claim for a PCB and every claim must be a CAS
(`scheduler`/`ps_switchto`/`self_exit_current` do not share a lock); never
publish `current_process` for an unclaimed task, or two CPUs end up sharing one
IRQ kstack. `irq_kstack_enter` prints `KSTACK-FOREIGN` when that happens, and
`make test-stress-user-smp` is the gate. The last-resort successor in
`self_exit_current` must be this CPU's own idle task: `&sPCB` is a single global
PCB with one stack, so two CPUs falling back to it resume one context on one
stack. Any shared-stack bug shows up as a 64-bit stack slot whose high half
holds another CPU's 32-bit `smp_cpu_id()` result — a return address read back as
`0x1_xxxxxxxx`, usually faulting in the serial path with `rdi = &uart1`.
Leftover ACCESS_SYS on idle BSS (`task_mgr` advertised while RSP is below
4MiB) is the same class: timer C tears RSI/R14/RBP and `held_crit_n`.
`leftover_current_should_repair` drops that advertisement; unclaimed kernel
on a kheap stack is still release-before-switch. ACCESS_SYS `#PF` that is
not a restored identity page must halt — ignore-and-return retried a torn
RIP into `cpus[]` (`UD64`). Leftover USER advertised on `pagedir1` must
keep that USER for crit tokens (`current_mm_process` must not retarget
to idle pid 0). Leftover idle on a user CR3 / user stack / CPUIRQ / kheap
RSP, and leftover USER on `pagedir1`, must not enter or leave `vfs_busy`
(`CRIT-NONOWNER busy=0x12 self=0x1`). Do not dest leftover USER A onto
CPUIRQ while HW CR3 is USER B — that breaks the fork 77-storm.
`irq_iretq_guard` must not `exc_recover` a leftover USER when the
frame is on `MEM_CPUIRQ` or the reserved idle stack with a bad RIP
(`rip=0` there killed `make` during cert 248135). A leftover timer
that is FOREIGN, leftover idle on a user/CPUIRQ stack, or leftover
USER `crit_wait` on a bad stack/CR3 must abandon the advertisement
and `context_load` a local task without saving the leftover RSP
(cert 248136 leftover `make` spun on `io_devlock` while `disk_mgr`
stayed claimed on CPU 0). Do not skip-schedule a leftover waiter.
Do not dest ACCESS_SYS on a reserved idle stack onto `MEM_CPUIRQ`
(leftover user-stack dests already use that stack; a second dest
GPF'd `fork_child_return` in the 77-storm). Cert 248137 overflowed
32KiB idle under nested timer C; reserved idle slots are 64KiB.
Leftover advertised idle still on a user CR3 must skip.
Leftover idle `#UD` while HW CR3 is a user AS retargets that owner
(cert 248139 `UD64 rip=0xac10000`). Do not reject ACCESS_SYS `iretq`
into the user ELF window — that is the fork 77-storm.

A task spinning in `sync_entercrit` must never outrank the lock's owner. User
processes get `priority = 1` and kernel threads keep `0`, so comparing raw
priority let a spinning user process win every `scheduler()` pass while the
BSP-pinned holder starved — the `make -j4` self-host deadlock.
`sched_eff_prio` ranks a `crit_wait` task at the floor, and the cooperative
early return in `schedule_from_timer` skips it too. That makes `crit_wait`
exactness load-bearing: `sync_entercrit` does the CAS, clears `crit_wait`, sets
`var->wait` and calls `sync_track_hold` in one interrupts-off region, because a
holder still flagged as a waiter is demoted while owning a hot lock. To debug a
lock hang, read the `CRITHANG hop=N ... wants=...` wait-for chain and the
`CRITCYCLE` line, which distinguishes a lock-order inversion from starvation;
resolve crit addresses with `nm -n kernel/Kernel64.sym`. `make
test-fatwrite-coop` reproduces the closure's scheduling regime without kexec.

Experience from the user-on-AP / GCC self-host campaign — why the
faulting RIP is almost never the bug, which failures are ordinary SMP,
and which are architecture (same-privilege IRQs, per-process kstacks,
`current`-sampled crit tokens) — is in
`ics-os/docs/smp-debugging-hardness.md`. Prefer a new gate that fails
the original cause over another cert loop.

Do not load the kernel PML4 while RSP
still points at a user-private page. `getphys64` must walk the full CR2 through `KDIRECT`. Sparse APIC IDs,
MADT discovery, NUMA, and CPU hotplug are not implemented and must not be
claimed. Local APIC read/write uses x2APIC MSRs when `IA32_APIC_BASE.EXTD`
is set (UEFI on the N150); MMIO at `0xFEE00000` hangs in that mode.
`lapic_present()` is true for MMIO or x2APIC. Without COM1, `smp_start_aps`
is skipped (APIC ids are not 1..7) and `smp_rdtscp_available` does not
run CPUID. `make test-lapicx2-unit` checks the MSR numbers.

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
`tests/fat_chain_unit.c` (`make test-fatchain-unit`). Every FAT data path
(`fat_openfileEX`, directory load, write, grow) must take `fat_lock_volume`
for that device: `fatcache[]` is a shared buffer, and `fat_wait_io()` may
taskswitch. Unlocked readers racing a writer truncated in-OS `.s` files
(`make test-fatwrite`). `vfs_directwrite` uses `vfs_units_covering` (ceil)
not `size/unit+1`. The 4 KiB page cache `pc_lookup` must find a line even
when `pc_claim` placed it outside the 8-slot hash probe (`kernel/iomgr/blkcache.c`).

Kernel builds are versioned and the kernel log is inspectable. `kernel/Makefile`
generates `kernel/build_info.h` (release id, git short hash, dirty flag, UTC
timestamp) at build time and `dex_init` prints a release banner at boot. CDC
bind also prints an unframed `ICSOS_VER release=... build=... ts=... compiled=...`
line (`compiled` is `__DATE__`/`__TIME__`) so the Pico `/log` and `/health`
(`kernel=`) still show the identity when STATUS RPC times out. Console
commands `version`, `uname [-a|-r|-m|-v]`, and `dmesg [-c|-n <lvl>|-l <lvl>]`
expose the build identity and a fixed-record kernel log (every `printf`/console
character is captured, gated live by a console-max level). Use `make test-klog`
for the boot gate and `make test-klog-unit` for the ring unit test.

User programs are ELF64 (`MEM_USER_ELF_BASE` 4 MiB). Console `execp` and
`posix_spawn` stream PT_LOAD pages from VFS (`elf64_stream_load`); typing
`hello` retries `hello.exe`. Leftover 32-bit PE/ELF (`ed.exe`, `nasm.exe`,
`vgademo.exe`) are rejected with an explicit message instead of
`unidentified executable format`. Use `make test-exec`.

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

An xHCI HCD keeps up to two per-port slots (`usbdevs[0]` MSC root,
`usbdevs[1]` CDC-ACM console). The port scan walks every CCS port. MSC
always claims device 0; a later walk claims a remaining CDC-ACM port as
device 1 without releasing the stick. Control TDs leave Setup Stage Chain
clear (RsvdZ) and invert the first TRB cycle until Data/Status are posted.
Console TX is a lock-free ring drained by `usb_cdc_pump` (hotplug thread
and bind flush); IRQ/fault paths must not issue USB. CDC bulk IN stays posted across empty polls and across MSC/OUT waits.
Completions that arrive on the shared event ring while waiting for MSC
are stashed (`xhci_stash_cdc_in_event`) instead of dropped or Stop-EP'd.
Look for `USB_CDC_CONSOLE_OK` then `ICSOS_VER` then `USB_CDC_RX` on the gadget. Framed
debug RPC (`usb_debug.h`: KEYS, CMD, STATUS, SCREEN, FB, DMESG, REBOOT,
KEXEC) plus unframed keystrokes go into the foreground tty.
`GET /health` on the Pico reports `pico=` firmware and `kernel=` scraped
from the bind stamp (no RPC), so a frozen `/log` still identifies both
ends. STATUS also includes `release=`/`build=`/`ts=` when RPC works.
After a successful `KEXEC`, the target rewrites `/icsos/vmdex` (else
`/vmdex`) on the USB ESP before `kexec_reboot`, so cold boot matches the
live image without re-etching. Host helper: `scripts/remote-kexec.sh`
(see `docs/intel-n150-usb-readiness.md` and the Pico bridge README).
`make test-cdcacm-unit`, `make test-usbdbg-unit`,
`make test-xhcipolicy-unit`, `make test-ttycanon-unit`,
`make test-usb-cdc-console` (both attach orders), and
`make test-usb-cdc-pico` (real Pico via QEMU `usb-host`; SKIP if unplugged)
are the gates.
Canonical `read()` keeps unread line bytes (`tty_canon.h`); a 1-byte
userland `read` used to drop the rest of `ls`. DDL output applies ONLCR
so `\n` returns to column 0.

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
timer interrupts. Polling is forced when COM1 is absent or the LAPIC is in
x2APIC (N150): MSI-X still uses the xAPIC address `0xFEE00000`, which hung that
PCH. Polling alone also remains active during interrupt-disabled boot
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
 renderer is switched to the framebuffer (RGB 16/24/32 bpp; pitch may be padded
 past `width*(bpp/8)`, as Intel GOP does on 1920x1200 N150 panels; glyph zoom is
 per-axis so 1920x1200 is 3x3 and fills the panel, while 1920x1080 is 3x2
 with letterbox top/bottom), and a
 self-test prints `FBCONSOLE_PASS`. A machine with no COM1 skips GOP MMIO
 until `fbconsole_late_init()` (after LAPIC/scheduler, identity
 write-combining via PAT PA4, live-render on). Early GOP and VGA CRTC
 `0x3D4` / `0xB8000` rebooted the N150. QEMU has COM1, maps GOP in
 `fbconsole_deferred_init()`, and keeps live-blit off after the self-test.
 Without a framebuffer tag and with COM1 present, the legacy VGA text driver
 remains active. Serial stays the headless oracle either way.
 
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
 geometry only (no GOP MMIO — early writes triple-faulted the N150 before
 the IDT), and `fbconsole_deferred_init()` maps it after `mem_init()`.
 QEMU marks a low framebuffer uncacheable so the pixel selftest can
 read back. A machine with no COM1 skips that early map (N150 rebooted
 when the console or fault path painted GOP) and keeps a RAM DDL shadow
 instead of `0xB8000`. After stage 16 (scheduler), `fbconsole_late_init()`
 identity-maps GOP write-combining below 4 GiB, or maps a high GOP
 through `KFB_BASE` (`boot_pdpt_high[5]`, 2MiB WC pages, 64 MiB cap),
 black-fills the panel, blits the 80x25 shadow (per-axis zoom; 1920x1080
 letterboxes top/bottom), prints `FB WxH pitch=… zoom=XxY`, and turns
 live-render on.
 Fault LEDs are both off so they do not look like stage 3 (Caps+Num);
 the fault path does not fill GOP without COM1. `getcpuid` takes the leaf in `rdi`
 by value (SysV) and zeros `%ecx` before `cpuid`; treating the leaf as
 a pointer wrote CPUID results to addresses 0, 1 and `0x80000002`.
 Without COM1, `hardware_getcpuinfo` skips CPUID (N150 hung in leaf
 0/1 / `0x80000000`). `init_kbd` flush is bounded; PS/2 mouse
 programming is skipped (USB RAX / no aux).
 
 GRUB only emits the info tag when its video subsystem is present. The EFI
 image and the BIOS `CORE_IMG` embedded in the thumbdrive MBR gap are built
 in `scripts/mkusb.sh`; both module lists must keep `video all_video`.
 `scripts/mkusb-uefi.sh` builds the GPT ESP image (`make usb-etcher`) with
 `EFI/BOOT/BOOTX64.EFI`, `efi_gop`, `gfxterm`, a baked-in ASCII font, and
 `gfxpayload=keep` so N150 firmware hands a linear GOP framebuffer to
 Multiboot2. That image also stages the dist toolchain (`gcc`/`cc1`,
 `as`/`ld`/`ar`/`objcopy`, `make`, `tcc`, SDK `.o` files, ldscripts). Do not probe GRUB `serial` on that image: laptops without COM0
 print `serial port 'com0' isn't found` and then look hung.
 `make test-fbconsole-unit` is the host TAP for packed vs padded pitch
 and per-axis zoom (1920x1080 is 3x2).
 `kbd_boot_leds()` programs i8042 Caps/Num/Scroll as a 3-bit boot-stage
 breadcrumb (`make test-kbdleds-unit`). Stage 0 is Caps (kernel C);
 `fbdbg_stage` updates the LEDs even when GOP is unmapped. A missing
 8042 latches dead and cannot stall boot. USB-only HID keyboards will
 not light until an HID driver exists; Intel laptop internals are
 usually i8042.
 `make test-boot`, `make test-usb-uefi`, `make test-usb-uefi-gpt`, and
 `make test-ide-thumbdrive` all assert `FBCONSOLE_PASS` and cover the GRUB
 paths (BIOS VBE, UEFI GOP on MBR FAT, UEFI GOP on GPT ESP, embedded
 i386-pc core).

 The tmux-style multiplexer (`console/foreground.c`, `console_mux.h`)
 paints a blue status bar on row 24. The painter stops at NUL; a previous
 80-byte read showed stack garbage after `3:console(0)`. Each DDL keeps
 128 scrolled-off rows. `C-b [` or shell `PgUp` opens copy-mode (vim's
 alternate screen still gets `PgUp`). Mux letter folding accepts Caps Lock
 and Ctrl forms of `c/n/p/...` (`fg_mux_letter`). Extra `C-b c` windows use
 the kernel prompt (no auto `sh.exe`) so a wedged USB-root exec cannot leave
 a blank tty. Boot starts the console and waits for `CONSOLE_READY` before
 the xHCI hotplug/CDC pump thread. `make test-consolemux-unit` covers
 status truncation, hist/view mapping, and mux letters. USB CDC-ACM console
 (Pico gadget) is `make test-cdcacm-unit` plus `make test-usb-cdc-console`.

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
|`hardware/`    |Sources for hardware device drivers (ATA PIO, UHCI, virtio-blk, virtio-net, …)|
|`net/`         |Minimal IPv4 stack (pbuf, netif, Ethernet, ARP, ICMP, UDP, TCP) plus `sock.c` Berkeley sockets (`FD_SOCK`)|
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
