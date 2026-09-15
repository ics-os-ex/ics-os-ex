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
- `smp_cpu_id()` reads **IA32_TSC_AUX** via `RDTSCP` (published at BSP `smp_init`
  and as soon as an AP claims its slot). It does **not** walk LAPIC MMIO as the
  primary source: a user private PML4 can make `0xFEE00000` unreadable, after
  which a LAPIC-only id would return 0 and the AP would enter/leave crits as
  the BSP process. A LAPIC id match is only a fallback if TSC_AUX is unset.
- APs come up **parked** until `smp_enable_scheduling()` after a successful root mount, then load the **kernel GDT**, arm a **LAPIC timer on vector 0x41**, and participate in scheduling.
- Ready-queue tasks are claimed via `on_cpu`. Console/`fg_mgr` stay BSP-pinned.
   User processes are BSP-pinned unless `user_procs_smp` is set (`user-smp` or
  `selfhost-stage1-parallel`). APs enable SSE (`CR4.OSFXSR|OSXMMEXCPT`,
  `CR0.EM` clear) before `fxrstor`. UART `serial_puts` identifies the CPU with
  `smp_cpu_id()` (never LAPIC MMIO under a user CR3). COM1 TX is GPR-only
  (`uart_com1_putc`); do not reload a `uart_dev*` from the user stack after
  `inportb`. Same-privilege IRQs move kernel C onto a per-process kheap stack
  before running any C. `IRQ_KSTACK_ENTER` switches RSP with RDTSCP/TSC_AUX (no
  C prologue on the interrupted stack); it does not execute `rdtscp` unless
  `smp_have_rdtscp==1` (TCG qemu64 may #UD), and it refuses to use a kstack
  whose task is not claimed by this CPU (`PCB.on_cpu`).
  Nesting is **stateless**: stay when RSP is already on `[kstack_base,
  kstack_top)` or `MEM_CPUIRQ`; switch to the process kstack only from the
  user stack (`MEM_USER_STACK_GUARD`..`MEM_USER_STACK`). A claimed user whose
  interrupted RSP is some other kernel stack stays there so
  `kstack_top` is not reset over a saved syscall `iretq` frame. Each wrapper
  reads its own `PUSH_ALL` frame from `%r13`, and `IRQ_KSTACK_LEAVE` is just
  `cli; movq %r13, %rsp`.
  `%r13` is callee-saved and is preserved by `context_switch`/`context_load`, so
  it survives a reschedule mid-syscall. Do not reintroduce a nesting counter or
  a PCB-held restore pointer: with those, a nested `#PF`/`#GP` read its error
  code at the outermost frame's offset and `iretq`'d from the syscall's user
  frame after `addq $8` for an error code that frame never had (`GPF64 rip=0x8
  cs=0x206`, i.e. CS landing in the RIP slot).
  Do not load the kernel PML4 while RSP still points at a user-private page.
  `fork` COW uses `PCB.irq_user_rsp` (recorded by the outermost entry).
- **One PCB, one CPU.** `PCB.on_cpu` is the claim. Every claim must be a CAS:
  `scheduler()`, `ps_switchto()`, and `self_exit_current()` claim without a
  common lock, so a plain store or a test-then-assign lets two CPUs run one
  PCB — and therefore share one IRQ kstack, with each CPU's locals overwriting
  the other's frames. Never publish `current_process` for a task this CPU has
  not claimed; `ps_switchto()` publishes only after the claim is re-verified
  with interrupts off. `irq_kstack_enter()` prints `KSTACK-FOREIGN cpu=/owner=`
  when this invariant breaks — that line is the fastest way to identify the
  class of "random" kernel corruption on APs. `make test-stress-user-smp` is
  the gate. The last-resort successor in `self_exit_current()` must be this
  CPU's own idle task, force-claimed. `&sPCB` is a *single global* `PCB386`
  with one `ctx.rsp`, so two CPUs falling back to it resume one context on one
  stack. The signature of any shared-stack bug is a 64-bit stack slot whose
  high half holds another CPU's 32-bit `smp_cpu_id()` result: a return address
  read back as `0x1_xxxxxxxx`, usually faulting in the serial path with
  `rdi = &uart1`.
- **Fork's child `iretq` frame is this syscall's `%r13`, not `irq_user_rsp`.**
  `fork_child_return` does `POP_ALL; iretq` over the interrupted user PUSH_ALL
  (15 GPRs + RIP/CS/RFLAGS; same-privilege, no SS:RSP). The wrapper must pass
  `%r13`; trusting a stale `irq_user_rsp` (a prior timer frame) is a 16-byte
  shift that shows up as `UD64 rip=0x207` (RFLAGS popped as RIP).
  `userpd_clone_cow` private-copies both pages the 144-byte frame occupies.
  `IRQ_KSTACK_ENTER` needs RDTSCP (TCG: `-cpu qemu64,+rdtscp`) or kernel C
  runs on the user stack and spills `KDIRECT()` pointers into the child's
  frame. `make test-fork` (`FORK_STRADDLE_PASS`) is the gate.
- **A spinning lock waiter must never outrank the lock's owner.** User
  processes get `priority = 1` (`process.c`) while kernel threads keep `0`, so
  comparing raw priority let a spinning user process win every `scheduler()`
  pass forever while the BSP-pinned holder (`disk_mgr`) sat runnable in the
  ready queue — `no-yield` climbing with `critspins` in the tens of millions.
  `sched_eff_prio()` ranks a `crit_wait` task at the floor; the cooperative
  early return in `schedule_from_timer()` also skips a `crit_wait` task. This
  is why `crit_wait` must be exact: `sync_entercrit()` performs the CAS,
  `crit_wait = 0`, `var->wait = 1` and `sync_track_hold()` in one
  interrupts-off region, because a holder still flagged as a waiter gets
  demoted to the floor while owning a hot lock.
- **Debugging a lock hang.** `CRITHANG hop=N crit=... owner=... wants=...`
  prints the whole wait-for chain (`sync_report_owner()`), and `CRITCYCLE`
  fires when the chain closes — that is what separates a lock-order inversion
  from CPU starvation. Resolve crit addresses with
  `nm -n kernel/Kernel64.sym`. `make test-fatwrite-coop` reproduces the
  closure's scheduling regime without a stage-1 kexec kernel.
  `getphys64()` walks page tables through `KDIRECT` and must not truncate
  CR2 to 32 bits. A kernel-text `#PF` must not kill the current user
  process or `while(1)` on nested `serial_puts`. Cooperative stage-1
  timers do not preempt a running user tool, but idle CPUs still steal ready
  work. A user `#PF` kills that process and resumes the parent; it must not
  `while(1)` the CPU. A `#PF` whose RIP is still in the kernel image is not
  treated as a user kill.
- `IPI_RESCHEDULE` (0xFC) may `taskswitch()` only when the interrupted task is
  idle or `ACCESS_SYS`. A user process is not preempted from that wrapper:
  software `context_switch` from the IPI frame enables IF before `iretq` and
  GPFs (`reschedwrapper`, selector error). Spawned user work is picked up by
  idle CPUs or by the spawner's voluntary `waitpid`/`taskswitch`. Timer
  preemption of user tools stays off for `selfhost-stage1-parallel`.
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

Why these bugs take days and which of them are design rather than "SMP
is hard" is collected in [smp-debugging-hardness.md](smp-debugging-hardness.md).
The short version: same-privilege user IRQs, a per-process kstack whose
owner is a CAS convention, and crit tokens sampled from `current` at
both enter and leave turn every claim race into a `#GP` in `vsprintf`
or a stuck `vfs_busy`. A single switch primitive and CPU-local IRQ
stacks delete most of that class.

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
| `[` or `PgUp` | copy-mode scrollback (also bare `PgUp` on the shell) |
| `c` / `C` / `Ctrl-C` | new console (Caps Lock OK) |
| `n` / `p` | next / previous (wraps) |
| `l` | last window |
| `0`–`9` | select window |
| `w` | window list (fg manager) |
| `x` | kill current window (not the last one) |
| `?` | help on the status line |
| `C-b` | send a literal Ctrl-B to the tty |

A blue status line on row 24 shows `[n *id:name …]`. It is a C string: the
painter stops at NUL (it used to read 80 bytes of stack after `console(0)`).
In copy mode the bar is `[n] COPY off/hist` with Up/Dn/PgUp/PgDn; `q` or Esc
leaves. Each console keeps 128 scrolled-off rows.

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
`work: mounted` (skipped on a zeroed disk). All FAT read and write paths on
that volume take `fat_lock_volume` so the shared FAT cache is not walked while
another CPU `loadfat()`s into it. `pc_lookup` must find a page-cache line even
if `pc_claim` stored it outside the 8-slot hash probe (`make test-fatwrite`).

**virtio-net** (`hardware/virtio/virtio_net.c`) plus the in-tree stack under
`kernel/net/` (Ethernet/ARP/IPv4/ICMP) is milestone-A networking: modern
virtio-pci, RX+TX queues, MSI-X, static SLIRP addressing (`10.0.2.15` /
`10.0.2.2`). Boot selftest ARP-resolves the gateway and ICMP-pings it.
`IRQ_IRETQ_KTEXT_END` must stay above `textEnd` when the stack grows.
`make test-net` greps `VIRTIO_NET_OK`, `NETIF_UP`, `VIRTIO_NET_IRQ_OK`,
`NET_PING_OK`, `NET_UDP_OK`, and `NET_TCP_OK` (host echo on UDP `:7777` and
TCP `:7778`). Host TAP: `make test-net-unit`. Guest also echoes UDP/TCP on
port 7. No Berkeley sockets/DHCP yet.

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
| virtio-net / IPv4 | `kernel/hardware/virtio/virtio_net.c`, `kernel/net/` |
| Concurrent I/O modernization plan | `docs/io-subsystem-modernization-plan.md` |
| GCC self-host plan | `docs/gcc-selfhost.md` |
| Memory map | `kernel/memory/memlayout.h` |
