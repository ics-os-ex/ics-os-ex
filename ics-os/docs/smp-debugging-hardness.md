# Why SMP bugs in ICS-OS are hard — experience and architecture

Status: living notes from the x86-64 user-on-AP / GCC self-host campaign
(2026-09). Partial implementation of the design items below is in-tree
(GS `current`, reserved per-CPU IRQ stacks, crit nest tokens,
`ps_publish_current`, `make test-smpclaim-unit`). Certification is
**not** claimed. This is not a pass report.

Related: [smp-longmode.md](smp-longmode.md),
[testing-and-qa-modernization-plan.md](testing-and-qa-modernization-plan.md),
[wiki/Kernel-Developer's-Guide.md](../../wiki/Kernel-Developer's-Guide.md).

## Verdict

A large part of the pain is **ordinary SMP**: races that only appear under
`-smp 4` with real I/O, two valid interleavings that each look correct on
`-smp 1`, and faults that land far from the store that caused them.

A larger part is **architecture**. Several load-bearing invariants are
implicit, checked late, or implemented as conventions across C and asm.
When they break, the machine does not report "two CPUs ran one process";
it reports a `#GP` in `vsprintf`, a `UD64 rip=0x207`, a silent serial
hang, or a `vfs_busy` owner that is "waiting for the lock it holds."
Those are the same three or four bugs wearing different clothes.

The work is therefore both: you cannot skip the SMP experience, and you
should not keep paying for design choices that turn every claim race into
a week of post-mortem.

## What the campaign actually broke

These are the repeating classes, not a changelog. Each one failed a
cert or a gate more than once, often after a "fix" for a sibling.

| Class | What is actually wrong | How it presents |
|-------|------------------------|-----------------|
| **One PCB, two CPUs** | `on_cpu` claimed with a store, or `current` published without a claim, or `current` left stale after release | `KSTACK-FOREIGN` / `KSTACK-SHARED`; then `#GP`/`#UD` in someone else's frame; `on_cpu=127`; idle token on a `sys_read` leave |
| **Kernel C on a foreign stack** | Same-privilege IRQ keeps the interrupted RSP (user stack, or the *other* CPU's kstack) | `rsp=0x3fffe990` in `time_handler`; `KDIRECT()` spills into a fork `iretq` slot; guest goes silent |
| **Nested IRQ reuses the stack top** | FOREIGN/safe/idle stack switch always resets RSP to `*_top` | `GPF64 cs=0x220008` (stack address grafted onto CS) inside `vsprintf` while dumping the first fault |
| **Process kstack top from a kernel RSP** | Claimed user + `current==dest` but RSP still on prev's kheap stack (publish-before-switch or leftover claim) reset `kstack_top` | `PF64 rip=0x100000001000` / high-half grafted RIP on `gcc.exe` mid-`make -j4`; no `CTXBAD` because `ctx.rip` was fine — the syscall `iretq` slot at the top was overwritten. Yanking that kernel RSP onto `MEM_CPUIRQ` instead is also wrong (`UD64 rip=0x217` on `fatwr`). Stay. |
| **Stale interrupt frame** | Fork trusted `irq_user_rsp` (last timer) instead of this syscall's `%r13` | `UD64 rip=0x207` (RFLAGS popped as RIP); same family as `GPF64 rip=0x8 cs=0x206` |
| **Lock-order inversion** | `file_ok()` dropped `vfs_busy` before `fat_lock_volume()` | `CRITHANG` `vfs_busy` vs `fat_volume_busy[11]` under `-j4` |
| **Priority inversion / pin-the-waiter** | User `priority=1` beats `disk_mgr`; or a `crit_wait` holder is pinned | `no-yield` in the tens of thousands; owner runnable, never selected |
| **Token ≠ executing process** | `sync_owner_token()` reads `current_process` (and thus `smp_cpu_id()`) at enter *and* leave | Non-owner `leavecrit`; recursive `wait++` then a no-op leave; lock stuck with `wait=1` forever |
| **Cooperative scheduler + leftover claim** | `coop-smp` will not preempt a user tool; a leaked `on_cpu==me` makes the holder unrunnable on every other CPU | WATCHDOG on the waiter; `CRITHANG` owner `on_cpu=3` while CPU 3 runs someone else |
| **BSS / 4 MiB ceiling** | Per-CPU stacks live in kernel BSS; image must stay below the user ELF window | 16 KiB safe stacks fail the linker `ASSERT`; 8 KiB idle stacks overflow into the neighbour |
| **Oracle is the patient** | Serial `#GP` dumps call `sprintf` on the same smashed stack | The diagnostic is the second, fatal fault |

None of these are "QEMU is flaky." They are deterministic once the
interleaving exists. The hardness is finding the interleaving.

## Why the symptoms lie

### 1. The fault is not the bug

A shifted `iretq` frame always dies at `iretq`, with RIP taken from
RFLAGS or CS. A shared kstack dies in `vsprintf`, `uart_putc_raw`, or
`sched_runnable_here` — whichever runs next on the corrupted slot.
A stale `current` dies in `sync_leavecrit` as "released by non-owner"
with `self=0x7f0004` (an idle pid token). Agents and humans naturally
debug the faulting function. That is the wrong layer.

**Mitigation that already helps:** name the invariant at the *entry*
(`KSTACK-FOREIGN`, `KSTACK-SHARED`, `CRITHANG hop=`, `CRITCYCLE`,
`CTXCANARY`, `RLBAD`). A gate that only greps `GPF64` will pass a
hang and fail a crash, both from the same claim bug.

### 2. Time-to-fail is a function of the workload, not the race window

The fork `iretq` shift took ~30 minutes of `-j4` GCC before
`test-fork` reproduced it in ~12 seconds (`FORK_STRADDLE_PASS`).
The vfs/FAT inversion needed concurrent writers. The two-CPU-one-PCB
bug needed `user-smp` plus exit/wait. The cooperative hang needs
`coop-smp` specifically: the same code passes `test-fatwrite`
(preemptive) and fails `test-fatwrite-coop`.

If the only "test" is the cert, every hypothesis costs a host-stage
plus a guest that may die in 15 seconds *or* after an hour. That is
process, but it is also architecture: there was no host-unit or
in-kernel probe for "exactly one CPU has `current==p` and
`p->on_cpu==that CPU`."

### 3. Three clocks, one `current`

`IRQ_KSTACK_ENTER` identifies the CPU with `RDTSCP`/`TSC_AUX`.
`current_process` is `*ps_cur_slot()` → `cpus[smp_cpu_id()].current`.
`smp_cpu_id()` may fall back to LAPIC MMIO, which is unreadable under
a user private PML4, and then returns **0**. Crit tokens use
`getprocessid()` → that same `current`.

So a single C function can:

- enter a crit as pid 31 (correct TSC_AUX),
- take an IRQ whose wrapper sees CPU 2,
- leave the crit as the BSP's `current` or as idle,

without any of those paths being "wrong" in isolation. Publishing
`cpus[me].current` with a captured `me` is a patch for a design that
re-reads the CPU id on every `current_process` mention.

### 4. Diagnostics consume the resource they are diagnosing

GPF dumps, `KSTACK-*` `sprintf`s, and `CRITHANG` walkers run on the
IRQ stack of the CPU that noticed the problem. Nested timer or `#GP`
on a FOREIGN path that *always* resets RSP to `irq_safe_stack_top`
destroys the outer frame and then faults again (`cs=0x22xxxx`).
Idle stacks of 8 KiB were adjacent; overflow looked like "another CPU
is writing my frames." Enlarging stacks hits `bssEnd <= 0x3f0000`.

So the act of making the bug visible can change or hide it.

### 5. Fixes have negative space

Almost every successful patch created the next failure:

| Fix | Immediate win | Next failure |
|-----|---------------|--------------|
| Hold `vfs_busy` across FAT I/O | AB-BA deadlock gone | Holder pinned in `fat_wait_io`; starve `disk_mgr` |
| `fat_wait_io` sets `crit_wait` | Owner can be scheduled | (unrelated) FOREIGN path still wrong |
| FOREIGN → idle `kernel_stack` | Not on the user stack | Collided with idle frames; `GPF64 cs=0x1c0008` |
| FOREIGN → `irq_safe_stack` | Idle intact | Nested IRQ resets to top; smash `vsprintf` |
| Nested check on the safe stack | No top-reset | Two CPUs still share a PCB; coop hang |
| Refuse `context_load` if any CPU has `current==p` | No SHARED smash | Holder with stale advertisement never runs |
| `self_exit` store `on_cpu=me` removed | No steal | Parent not resumed; wait/exit paths more delicate |
| Skip nested `file_ok` acquire | One wait-inflation path gone | Leave still fires from `vfs_file_get` with an idle token |

That is the SMP experience: each invariant you tighten moves the
violation to the next implicit one. It is also a design smell. A
single "run this task" primitive should have made most of those
stores impossible.

## Inherent SMP vs architecture amplifiers

### Inherent (you will pay this on any kernel)

- Cache-coherent but not sequentially consistent *reasoning*: a CAS on
  `on_cpu` and a plain store to `cpus[j].current` are independent.
  x86 TSO helps; it does not make "I released, therefore they have
  updated `current`" true.
- Lock ranking under wait. A waiter that stays runnable at high
  priority starves the owner. This is textbook, and we hit it.
- I/O waits must drop or demote the pin. Holding `vfs_busy` across
  `fat_wait_io` is correct for lock *order* and fatal for lock *progress*
  unless `crit_wait` is set.
- Fork vs running children vs `waitpid` on another CPU. Exit must not
  `context_load` a live parent. That problem exists in Linux too
  (`try_to_wake_up`, not "switch to parent").
- Testing must include 1/2/4/8 and preemptive vs cooperative. A
  uniprocessor pass is not an SMP pass.

### Architecture amplifiers (we chose these)

**Same-privilege user processes.** User ELFs still enter with kernel CS.
There is no TSS RSP0. A timer or `int 0x30` does not switch stacks.
Every IRQ wrapper must notice "this RSP is not a kstack" *in asm,
before any C prologue*, using a CPU id that works under a user CR3.
That is why RDTSCP is load-bearing, why TCG `qemu64` without `+rdtscp`
spills `KDIRECT()` into fork frames, and why a FOREIGN miss runs
`time_handler` on `0x3fffe990`.

A ring-3 + IST/TSS design makes "kernel C on a user stack" a hardware
impossibility instead of a convention.

**Per-process IRQ kstack + software claim.** The kstack is a property
of the PCB, not of the CPU. The claim (`on_cpu`) and the advertisement
(`cpus[i].current`) are two words, updated in different functions, with
CAS in some paths and stores in others (`self_exit` used to store
`on_cpu = me`; scheduler used to store `best->on_cpu = me`). Two CPUs
that disagree share one stack. Linux's `rq->curr` and `task->on_cpu`
are the same idea, but they are updated in one scheduler core with
`rq` lock. We have three claim sites and no common lock.

**`current_process` is a function, not a register.** `%gs`-relative
`current` (or a dedicated per-CPU segment) would make "who am I" a
single load that cannot silently become the BSP. TSC_AUX is the right
*id* source; using it as a table index on every C access is how idle
tokens appear in `vfs_file_get`.

**One global `vfs_busy` and process-id tokens.** The crit is a busy
word plus a recursive `wait` count. Ownership is `pid+1` sampled from
`current` at enter and again at leave. Recursive enter increments
`wait` and does **not** re-track the hold. A stale `current` that
happens to name the holder inflates `wait`; the matching leave is a
non-owner no-op; the real owner decrements `wait` to 1 and never
releases. That is not an SMP textbook bug. It is a lock API that
trusts `current`.

**Cooperative user scheduling as the cert regime.**
`selfhost-stage1-parallel` / `coop-smp` does not preempt a running user
tool. That is a product choice (cc1 must not be timer-sliced off a
crit). It also means a leaked `on_cpu` or a waiter pinned on the only
legal CPU is a deadlock, not a latency blip. Preemptive `test-fatwrite`
does not catch it. The closure is the first time the regime is real.

**Kernel image under 4 MiB.** Per-CPU idle, AP, and safe stacks compete
with `.bss`. Overflow looks like cross-CPU corruption. The fix (bigger
stacks) fails the linker. Stacks should not be BSS arrays; they should
be allocated from a reserved range `mempop` already skips.

**DEX `int 0x30` plus software `ret` context switch.** Fork's child
must reconstruct a same-privilege `iretq` frame (3 words, not 5). The
wrapper and C have disagreed about *which* frame (`%r13` vs
`irq_user_rsp`) more than once. Hardware task gates would have been
worse; a single `irqentry`/`irqexit` pair with an explicit `pt_regs *`
would have been better.

**Serial as the only oracle.** Interleaved `printf` from four CPUs
already needed `SMP_RESULT` records. GPF dumps still interleave with
`KSTACK-*` on the same line. There is no per-CPU ring buffer that
survives a smash.

## Design improvements that would actually shrink the space

These are ordered by how many of the rows above they delete, not by
implementation size.

### 1. One "put this task on this CPU" primitive

Today: `scheduler` CAS, `ps_switchto` CAS or skip, `self_exit` CAS or
store, `context_switch` stores `-1`, `current` assigned separately.

Target: one function, interrupts off, that

1. asserts `task->on_cpu == me` (or CAS from `-1`),
2. sets `cpus[me].current = task` with the captured id,
3. issues a compiler+memory barrier,
4. switches, and only then releases `prev->on_cpu`.

Every other site calls it or does not exist. A store to `on_cpu` or
`cpus[i].current` outside that function is a build break (script or
sparse-style grep in CI). This is the single highest-leverage change.

### 2. Per-CPU kernel stack, not per-process

Keep the process kstack for syscall *depth* if you want, but **FOREIGN
user IRQ entry must land on a CPU stack** (IST or a dedicated per-CPU
stack chosen only from TSC_AUX). FOREIGN then cannot mean "run on the
other guy's kstack or user stack." Nested IRQs nest on the same CPU
stack with a depth check, which is the Intel SDM model.

Do **not** park ACCESS_SYS (idle, `disk_mgr`) on that CPU stack across
`context_switch`. The timer path saves their RIP/RSP at the current
top; the next IRQ reuses it and the saved RIP becomes a pointer into
`cpus[]` (`UD64 rip=cpus+0x21`). Kernel threads already have a private
stack — that is their continuation stack.

The 4 MiB BSS problem goes away if those stacks are reserved frames
above the kernel image, skipped by `mempop`.

### 3. `%gs`-relative `current` and CPU id

Publish `cpu_local` at `smp_init` / `ap_main` into `IA32_GS_BASE` (or
a GDT GS). `current` is one load. `smp_cpu_id()` is one load. Crit
tokens can be `(current->processid+1)` without a second RDTSCP that
might disagree. LAPIC fallback becomes debug-only.

### 4. Ring-3 user mode + TSS RSP0 / IST

This is already on the suggested-next-work list. It deletes the entire
"same-privilege IRQ on the user stack" class. It does *not* delete
claim races, but those become "wrong task scheduled" instead of
"kernel C ate the user red zone."

### 5. Real mutexes for VFS/FAT, not recursive busy-words

- Explicit lock ranking (`vfs` < `fat` < `pc_busy` < `io_devlock`)
  asserted on acquire.
- Owner is a `PCB *` written with the lock, not a pid token recomputed
  from `current` at leave.
- Recursion is either forbidden (preferred) or a per-lock depth on the
  *PCB that acquired*, not on `var->wait` interpreted as both nesting
  and waiter-count.
- `file_ok()` never acquires. Callers that already hold `vfs_busy` use
  `file_ok_locked`. That is already half-done; the remaining
  `fread`/`vfs_file_get` acquires are the leftover.

### 6. Sleeping locks + a single wait queue

`sync_entercrit` + `taskswitch` + `crit_wait` is a spin-then-yield
mutex. Under `coop-smp` a yield is the only way the owner runs. A
wait queue ("blocked on `vfs_busy`") makes the owner the obvious next
pick and makes "pin a waiter" impossible because waiters are not
runnable. This is standard; it also gives `CRITHANG` a real graph
instead of a sampled `crit_wait_var`.

### 7. Cheap, smash-resistant observability

- Per-CPU lock-free trace: `(tsc, cpu, event, pid, on_cpu, rsp)` in a
  reserved uncached line, dumped on panic *from the other CPUs' IST*.
- `KSTACK-*` / `CRITHANG` stay, but they must run on the CPU stack
  (item 2) with a reentrancy cap of 1 (already sketched with
  `gpf_busy[]`).
- A host-native or in-kernel TAP test: "after N random switches,
  `sum(cpus[i].current==p) <= 1` and equals 1 iff `p->on_cpu>=0`."
  That test should fail the original claim bugs without booting GCC.

### 8. Stop using the cert as a unit test

`test-fork`, `test-fatwrite`, `test-fatwrite-coop`,
`test-stress-user-smp`, `test-apuser` are the right layers. Keep
adding a gate that fails for the *original cause* (straddle depths,
FOREIGN count, CRITHANG, non-owner leave) instead of another 8-hour
cert loop. The cert remains the closure proof, not the debugger.

## What is "just the SMP experience"

- Writing the claim as a CAS, not a load/store.
- Not pinning waiters.
- Not dropping lock A before taking B if the other path takes B then A.
- Reproducing on `smp=1/2/4` and under both scheduler regimes.
- Not trusting a uniprocessor "it boots."
- Expecting the first symptom to be a lie.

Do not skip that. Do not romanticize it either. Once those habits exist,
the remaining cost is the amplifiers above. Paying it again on ARM64
later would be a choice.

## Open as of this writing

- `test-fatwrite-coop` still fails: `vfs_busy` non-owner leave (recently
  with an idle-task token from `vfs_file_get`) after `SDK_EXIT`.
  `test-fatwrite` (preemptive) passes. The claim/current split is not
  closed.
- Nested FOREIGN safe-stack check is in; 16 KiB stacks do not fit BSS.
- `GCC_SELF_CERT_PASS` is **not** claimed.

The next architectural step is item 1 (one switch primitive) plus
item 5 (stop sampling `current` at `leavecrit`), not another special
case in `IRQ_KSTACK_ENTER`.
