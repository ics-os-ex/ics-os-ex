# ICS-OS x86-64 long mode, software scheduling, and SMP

## Overview

The kernel now boots in **x86-64 long mode** (Multiboot2), uses **software context switching** instead of hardware TSS task gates, and brings up additional cores via the **LAPIC** (QEMU `-smp N`).

## Boot path

1. GRUB loads `vmdex` with `multiboot2` (see `scripts/mkusb.sh` / `grub-mkrescue` ISO).
2. 32-bit trampoline in `startup/startup.S` builds a 4-level identity map (0–4 GiB, 2 MiB pages), enables PAE + LME + paging, and jumps to 64-bit code.
3. LAPIC is **left enabled** (no longer disabled at boot).
4. `main()` parses Multiboot2 tags (or Multiboot1 if present).

QEMU cannot `-kernel` an ELF64 Multiboot image. Prefer the ISO smoke tests:

```
cd ics-os
make
make test-usb-amd64   # grub-mkrescue + Multiboot2
make test-smp         # qemu -smp 4 by default
make test-smp-matrix  # qemu -smp 1/2/4/8
```

USB images still build with `make usb` (multiboot2 in grub.cfg); ISO boot is the more reliable QEMU path today.

## Context switch

- PCB embeds `cpu_context` + `fpu_state` (`cpu/context.h`).
- `ps_switchto()` uses `context_switch` / `context_load` (`cpu/context.S`) with `fxsave`/`fxrstor`.
- Timer IRQ uses an **interrupt gate** to `timerwrapper` → `time_handler` → `schedule_from_timer`.

## Scheduler

Default scheduler is **priority round-robin** (`process/scheduler.c`):

- Higher `PCB.priority` wins.
- Equal priority: next runnable after the previous process.
- Ready-queue walks are protected with a spinlock (SMP-ready).

## SMP

- `cpu/lapic.c` — map LAPIC, EOI, timer, INIT/SIPI IPIs.
- `cpu/smp.c` + `cpu/ap_trampoline.S` — AP trampoline at `0x8000`, per-CPU stacks.
- Up to `MAX_CPUS=8` xAPIC CPUs are supported. AP startup separates slot claim
  from fully initialized online publication; the BSP waits for the latter,
  retries one SIPI when needed, and never exposes a partial per-CPU record.
- xAPIC ICR writes wait for delivery-idle before and after each IPI. This avoids
  overwriting an in-progress reschedule or startup command at larger CPU counts.
- `current_process` is per-CPU (`smp_this_cpu()->current`).
- APs come up **parked** until `smp_enable_scheduling()` after a successful root mount, then load the **kernel GDT**, arm a **LAPIC timer on vector 0x41**, and participate in scheduling.
- Ready-queue tasks are claimed via `on_cpu`; `cpu_affinity` pins console/`fg_mgr`/user processes to the BSP. APs run migratable kthreads (see `ap_work` smoke).
- `createkthread_on_cpu()` installs affinity before ready-queue publication;
  setting affinity after `createkthread()` is unsafe once AP scheduling is live.
- The ready-queue walk skips foreign idle threads and wrong-affinity tasks.
- The context-switch reentrancy guard is sized by `MAX_CPUS`, not a fixed
  topology size; context-load and voluntary-switch guards are per CPU. FPU
  save/restore uses per-CPU aligned scratch storage so concurrent switches never
  share the `fxsave` staging buffer. The SMP smoke creates one pinned worker per AP and publishes
  a BSP-generated aggregate execution mask. COM1 writes are protected by an
  IRQ-safe SMP lock; test assertions use whole-record `SMP_RESULT` messages so
  concurrent console output cannot corrupt acceptance markers.
- QEMU defaults to four CPUs for `make test-smp`; override with
  `SMP_CPUS=1..8`. `make test-smp-matrix` validates 1/2/4/8 CPUs.

Current bare-metal discovery still assumes contiguous legacy xAPIC IDs starting
at one. ACPI MADT parsing, sparse APIC IDs, x2APIC, NUMA topology, CPU hotplug,
and more than eight CPUs remain future architecture work; QEMU's validated
contiguous topology must not be presented as that broader hardware support.

## Boot root

- Multiboot EAX/EBX are saved at `0x9000` **before** early serial I/O (which clobbers `%al`).
- Multiboot2 BIOS boot-device tag is packed into Multiboot1 layout; BIOS CD (`0xE0+`) or drive 0 → `cds0`.
- Floppy driver is skipped unless booting from `fd0`.
- **ATA PIO helpers** (`repinword` / etc. in `asmlib.S`) were fixed for SysV AMD64 (port was wrongly taken from the segment arg).
- **ISO9660** `convertname` now respects `ident_length` (names are not NUL-terminated).
- QEMU Multiboot2 ISO boots reach `Root mount [OK]` via `cdfs` on `cds0` (`make test-smp` also asserts this).
- Free-page pool expanded (~120 MiB usable under 128 MiB QEMU).
- APs unpark with kernel GDT + LAPIC timer; work-steal is proven by the
  `SMP_RESULT work-steal=ok cpus=N mask=M` aggregate AP execution record.
- `test-exec` runs real ELF64 CRT/`hello.exe` and expects `Hello World` + `EXEC_TEST_PASS`.

## Userland / TinyCC

- Host apps build as **ELF64** (`sdk/app.mk` uses `-m64`).
- In-OS **x86_64 TinyCC** (`apps/tcc.exe`) can compile and run programs on the
  long-mode kernel. Smoke test: `make test-selfhost` (Multiboot2 ISO, `-smp 1`).
- Selfhost stages `tcc` + sources onto `/ramdisk` first so compiles do not
  depend on ATAPI mid-run (CD reads during large ELF64 user processes remain
  flaky). Asserts `SELFHOST_TEST_PASS` after compiling/running `min.c` and
  `hello.c` (tinyio/tinycrt).
- Kernel `lmodeproc` lives at `0x10000000` so it does not collide with TCC's
  default ELF `.data` window at `0x600000`.
- **ISO9660**: directory sector padding (`size==0`) advances to the next
  sector instead of ending the directory early (multi-sector dirs like
  `/src/tcc` previously hid later files).
- Full `tccboot` (rebuild TinyCC with itself): `make test-tccboot`
  (Multiboot2 ISO, KVM) **PASS**. Sources + SDK + `tcc.exe` are packed as one
  ustar (`tccsrc.tar`) and extracted onto `/ramdisk`. TinyCC is then
  compiled **per file** (`-DONE_SOURCE=0`) and linked to `tccnew.exe`,
  which compiles and runs `min.c`. Static EXEs must `fill_got()` for
  `R_X86_64_JUMP_SLOT` (otherwise `call foo@plt` jumps to rip=0).
  `make test-kbuild` compiles the kernel with in-OS GCC/binutils and kexecs it.
  `make test-tcc-kbuild` is the optional TinyCC kernel experiment;
  `make test-tcc-fullhost` first rebuilds TinyCC as `tccnew.exe`.
- ISO9660 directory records skip sector padding so multi-sector dirs
  (e.g. `/src/tcc`) list all files.

## TTY / userland console

The interactive shell is moving out of the kernel (`console_execute` in
`kernel/console/console.c`) onto a POSIX-style tty + userland `sh.exe`.

Kernel keeps: character queues, canonical line discipline (`\b`, `\r`),
VGA (DEX DDL) and serial backends, Ctrl-C → SIGINT to the foreground pgrp,
and a fallback kernel prompt if `/icsos/apps/sh.exe` is missing.

Userland: `contrib/sh/sh.exe` reads fd 0 / writes fd 1. Unknown commands
call `sys_kcmd` so existing builtins still work while they are ported.

Command classification (`console_execute`):

- **shell builtin:** `echo`, `cd`, `set`, `exit`, `help`, `!!`, `pwd`
- **user utility (target):** `ls`/`dir`, `cat`/`type`, `cp`/`copy`, `mkdir`,
  `ps`/`procs`, `rm`/`del`
- **privileged (stay kernel syscalls):** `mount`, `umount`, `reboot`,
  `kbuild` (GCC), `tcckbuild` (optional), `loadmod`, `selfhost`, `tccboot`

F12 still switches virtual consoles; each VT has its own tty. COM1 is
`ttyS0` (serial backend) for headless tests.

**tmux-style window keys** (prefix `Ctrl-B`, same as tmux; F2/F11/F12 still work):

| After `C-b` | Action |
|-------------|--------|
| `c` | new console |
| `n` / `p` | next / previous (wraps) |
| `l` | last window |
| `0`–`9` | select window |
| `w` | window list (fg manager) |
| `x` | kill current window (not the last one) |
| `?` | help on the status line |
| `C-b` | send a literal Ctrl-B to the tty |

A blue status line on row 24 shows `[*n:name …]` while the prefix is armed.

Ring-3: user CS is recorded as `USER_CODE` (64-bit DPL=3 GDT). Software
context switch still uses kernel CS until TSS.rsp0 + `iretq` is wired;
`int 0x30` already has DPL=3.

## Block I/O (P0–P2)

`dex32_requestIO` runs the device transfer in the caller under a
**per-device blk-mq lock**, not `IOrequest_busy`. Task switching stays enabled
across ATA PIO. `IOrequest.lba` is 64-bit. `disk_mgr` sleeps (`sleep(1)` +
`hlt`) between flush passes; `iomgr_request_flush()` clears its `waiting`
flag. `make test-iobench` maps `/icsos/apps/tcc.exe` from the CD.

**P2 page cache:** 512 × 4KiB write-back pages indexed
by `(device, byte_offset >> 12)`. CD 2048-byte sectors occupy two per page
(the old `cd*` skip is gone). Misses merge into aligned 4KiB device reads.
`bio_submit_sync()` is the internal submit path. Dirty pages flush from
`disk_mgr` / `fclose`. Ramdisk still uses its own `getcache`/`putcache`.

**P3 POSIX / uring:** Per-process fd table (`FD_MAX` 64). `sys_open` / `sys_close` /
`sys_read` / `sys_write` / `sys_lseek` / `sys_preadv` / `sys_pwritev` /
`sys_fsync` wrap `file_PCB`. DEX `fopen`/`fread` stay as compat. Ring VA is
`params.sq_off.user_addr` (identity map, no mmap). Ramdisk SQEs complete
inline. `/dev/vblk` READ/WRITE/FSYNC SQEs are submitted to virtio-blk. The
MSI-X handler harvests used descriptors and marks slots complete; a later
process-context harvest publishes asynchronous callbacks into the CQ. Successful
read copyback is restricted to the submitting address space; process teardown
resets and retires callbacks before releasing its page tables. `io_uring_enter`
uses scheduler-backed hashed completion wait queues and periodically harvests in
submitter context while waiting for `min_complete`. A global bottom-half worker
must not copy through user virtual addresses until DMA pin/kernel-map support
exists.
`make test-posixio` greps `POSIXIO_PASS`, `URING_PASS`, and
`URING_VBLK_PASS` while running two processes on two vCPUs. VFS and block fd
lookups acquire typed transient references before releasing `fd_lock`; close
atomically detaches first, and final release waits for active users. Spawned
processes increment descriptor-owner references and skip reserved slots rather
than shallow-copying the fd table. Shared offsets and buffered VFS state use a
per-open-description serialization gate. This is focused SMP regression
coverage, not an exhaustive memory-ordering or lifetime proof. The required
concurrency, teardown, and completion contracts are specified in
`docs/io-subsystem-modernization-plan.md`.

**POSIX process creation / waitpid:** Syscalls `0xB1` (`waitpid`), `0xB2`
(`posix_spawn` / non-waiting ELF load), `0xB3` (`execve` same-pid replace), and
`0x90` (copy-on-write `fork`). `user_execp` (`0x5B`) still waits. The x86-64
fork path bypasses the legacy dispatcher, COW-shares ordinary writable private
PML4 leaves, eagerly copies the active CPL0 user/syscall stack, and publishes a
fresh PCB only after resource cloning succeeds. ELF text remains read-only. It
rejects multithreaded callers and in-flight io_uring.
`make test-spawn` greps `SPAWN_PASS`, `FD_INHERIT_PASS`, and `WORK_DISK_PASS`;
the inheritance case closes the parent's VFS fd before the child writes through
its retained open description. COW page-table changes use synchronous CR3-
targeted TLB invalidation on IPI vector `0xFB`; remote targets require matching
CR3 and authoritative `on_cpu` ownership. `CR0.WP` is enabled on every CPU.
`make test-fork-matrix` validates COW faults, OOM recovery, immutable text, and
ten-child delayed reaping on 1/2/4/8 vCPUs. User processes remain BSP-pinned, so
the remote shootdown path is infrastructure for later process migration.

**virtio-blk** (`hardware/virtio/virtio_blk.c`) is the VM production path:
modern virtio-pci caps, one request queue with a 3-descriptor slot pool,
one dynamically allocated MSI-X device vector, 512-byte LBAs, registered as block device `vblk` and
as `/dev/vblk`. Completions harvest the used ring in the IRQ (hlt wait, not
pause-spin). ATA PIO remains the bare-metal fallback. PCI MMIO BARs are mapped
through `mmio_map()`: low 4 GiB BARs use the identity-map UC path, while BARs
above 4 GiB use the bounded `KMMIO_BASE` kernel window with PCD|PWT and SMP
TLB shootdown.
`make test-virtio` greps the allocated-vector marker, `VIRTIO_BLK_OK`, and
`VIRTIO_IRQ_OK`.
If sector 0 looks like FAT, the kernel mounts `vblk` at `/work` and prints
`work: mounted` (skipped on a zeroed disk).

## Memory map

Identity-mapped low 4GiB. **Source of truth:** `kernel/memory/memlayout.h`.
The page allocator skips that reserved-range table; the linker
`ASSERT`s `bssEnd <= 0x3F0000` (a 64KiB guard under the 4MiB user-ELF base;
the former 256KiB "frame stack" was replaced by the global frame pool) so the
kernel cannot grow into TinyCC's ELF window. `KMMIO_BASE` reserves a 2 MiB kernel-only MMIO window for
device BARs above 4 GiB. Kernel stacks are `.bss` arrays. Kernel heap is a
closed 32MiB interval; `sbrk` must not `mempop`. Add new regions to
the header first.

Next: GCC 4.7.4 self-host (`docs/gcc-selfhost.md`) / ring-3 / user processes
on any CPU. TinyCC kbuild is deferred.

## Key files

| Area | Path |
|------|------|
| Long-mode entry | `kernel/startup/startup.S` |
| SysV asm helpers | `kernel/startup/asmlib.S` |
| IRQ wrappers | `kernel/irqwrap.S` |
| Context switch | `kernel/cpu/context.S` |
| LAPIC / SMP | `kernel/cpu/lapic.c`, `smp.c`, `ap_trampoline.S` |
| TTY | `kernel/console/tty.c` |
| Userland shell | `contrib/sh/sh.c` |
| Block I/O | `kernel/iomgr/iosched.c`, `blkcache.c` |
| POSIX fds / io_uring / spawn | `kernel/vfs/posixfd.c` |
| virtio-blk | `kernel/hardware/virtio/virtio_blk.c` |
| Concurrent I/O modernization plan | `docs/io-subsystem-modernization-plan.md` |
| GCC self-host plan | `docs/gcc-selfhost.md` |
| Memory map | `kernel/memory/memlayout.h` |
