# ICS-OS Testing and Quality-Assurance Modernization Plan

Status: architecture and implementation plan
Scope: kernel, SDK, user applications, build tools, boot media, emulated devices,
self-hosting, CI, release qualification, and physical hardware

Related designs:

- [Concurrent VFS, device, and async-I/O plan](io-subsystem-modernization-plan.md)
- [Device-driver subsystem architecture](device-driver-subsystem-architecture.md)
- [GCC self-host certification](gcc-selfhost.md)
- [SMP debugging hardness and architecture](smp-debugging-hardness.md)

## 1. Executive assessment

ICS-OS has a valuable end-to-end test foundation, but not yet a complete OS
quality system. The repository can build bootable Multiboot2 media, launch it in
QEMU, observe the serial console, and validate substantial vertical slices: SMP
startup, ELF execution, POSIX descriptors, asynchronous block I/O, spawning,
GNU build tools, GCC, kernel rebuild, kexec, and parts of the self-host chain.
These are unusually useful integration and acceptance tests for an
instructional kernel.

The principal problem is concentration. Most validation occurs at the most
expensive layer: a complete guest boots and shell recipes search an unstructured
serial log for success strings. There is no general host-unit or in-kernel unit
framework, no machine-readable test protocol, no automated repository CI, no
coverage or sanitizer lane, no systematic fault injection, little multi-CPU I/O
stress, and no physical-hardware qualification system. Build warnings are
suppressed, some application build failures are ignored, many QEMU exit statuses
are deliberately discarded, and fixed `/tmp/icsos-*` paths make parallel runs
unsafe.

The present system should therefore be described as a **broad QEMU smoke and
functional test suite**, not as a mature unit-testing or production QA
framework. Its end-to-end tests should be retained and placed at the top of a
new test pyramid rather than replaced.

The USB image has disposable VirtualBox BIOS and UEFI gates plus QEMU UHCI and
q35 xHCI controller gates. They require FAT32 root mount, guest
create/write/`fsync`, synchronous SCSI cache flush, VM termination, and
byte-for-byte host readback. The QEMU gates boot from a separate CD and assert
the selected HCD plus `usb0p0` root selection. An xHCI no-device lane checks
bounded failure, no accidental USB registration, and continued console startup.
A `test-usb-uefi` lane boots the thumbdrive image itself under OVMF from a USB
mass-storage device (the realistic removable-media path, not the ISO/IDE path)
and asserts UEFI firmware handoff (`BdsDxe` loading `BOOTX64.EFI`), `usb0p0` root
selection, `Root mount [OK]`, and AP scheduling with no GPF.
The MSI-X lane requires delivery on a dynamically allocated device vector and an
interrupt-enabled waiter woken by an xHCI IRQ; the forced-poll lane disables
MSI-X and preserves the same
mount, flush, and durable host-readback contract. The MSI-X recovery lane also
requires exact vector release/reclaim across three successful controller
recoveries and rejects any handler-drain timeout.
The vector-reservation lane holds the first device-domain vector under a
separate owner and requires xHCI to skip it through initial setup and three
recovery allocations.
Host IRQ tests verify exact xAPIC MSI address/data composition, the largest
encodable destination ID, and rejection of vectors outside the selected domain
or destination IDs that require x2APIC/remapping support.
xHCI controller registers, rings, DMA ownership, recovery flags, and IRQ
resources have explicit HCD owners. PCI discovery creates up to eight records;
dedicated assembly routes dispatch MSI-X by HCD and unbind after handler drain.
The two-controller gate leaves HCD 0 empty and verifies clean fallback, HCD 1
selection, MSI-X, and durable storage. Concurrent active controllers and
per-controller USB frontend isolation remain untested and unsupported.
The high-BAR lane relocates the controller BAR above 4 GiB. The recovery lane
injects repeat transfer timeouts and failed recovery initialization, then requires fail-closed
device state, successful restoration, sector equality, and durable host readback.
The BOT stall lane induces real QEMU Stall Error completions with malformed
CBWs, requires BOT reset plus endpoint reset/dequeue recovery, and verifies one
bounded escalation to full-controller recovery when the selective retry times out.
The QMP disconnect lane removes storage during an active bulk transfer and
requires bounded cancellation, fail-closed later I/O, no reset attempt, no
offline-device registration, and continued console startup.
The mounted-disconnect lane runs after FAT root mount, dirties one cache page,
and requires parent/partition cache invalidation, explicit dirty-page loss
reporting, and rejection of a stale cached reread.
The mounted-remount lane rejects recovery while a process workdir is below the
mount, preserves the namespace on that rejection, then remounts the verified
replacement generation and resolves a file through the rebuilt namespace.
The reconnect lane recreates the block backend, attaches replacement storage,
allows a different root port, repeats full controller and BOT enumeration,
checks unchanged geometry, and requires sector equality after raw I/O resumes.
Its mismatch variant requires a different-capacity replacement to remain
offline after enumeration.
Modern xHCI hardware still requires physical qualification.

The first incremental host-unit foothold now exists: `make test-io-unit` emits
TAP 13 for cache/device lifecycle, DMA, IRQ, media identity, generic IOMMU
domain decisions, and bounded ACPI DMAR parsing. `test-posixio` runs twice under
distinct PIDs on two vCPUs and verifies VFS create-failure cleanup plus io_uring
close with DMA in flight; `test-virtio` also uses two vCPUs. This remains a
small increment, not the general runner, CI, coverage, or fault framework.

The same host target now also checks the idempotent live-to-quiescing transition
and device reference/quiesce/retire decisions,
cache rejection after a device-generation change,
and completion-before-wait ordering. The virtio focused gate deterministically
holds completion harvesting, resets with an in-flight descriptor, requires the
request to fail, and verifies a subsequent DMA readback before emitting
`VIRTIO_RESET_RECOVERY_OK`. This is the first reusable device fault/recovery
hook, but it is not yet a general fault-injection framework.

io_uring CQ and close waits now exercise scheduler-backed hashed completion
queues, and process teardown retires address-space-owned virtio callbacks before
page-table release. Current two-vCPU gates validate normal and reset paths, but
deterministic foreign-CR3 read completion, inherited-ring owner exit,
completed-but-undrained cancellation, and tick-counter rollover remain required
regressions before this lifetime contract is considered fully covered.

`test-spawn` now includes a deterministic descriptor-lifetime regression: the
parent opens a FAT file, spawns a child, closes its descriptor immediately, and
requires the child to write and sync through its inherited reference before the
parent verifies the result. This covers ownership transfer and final-release
ordering in the real guest ABI. Close racing an already-active VFS syscall and
multi-vCPU repeated clone/close/exit stress remain Phase 4 requirements.

### 1.1 Overall maturity rating

| Capability | Current rating | Evidence and consequence |
|---|---:|---|
| Reproducible developer environment | Partial | Ubuntu container and dependency script exist, but packages and emulator behavior are not pinned to immutable versions. |
| Compile and link validation | Partial | Canonical builds work, but the kernel unity TU is still built with `-w`; a `warncheck` ratchet now strict-gates the hardened TUs (`gpt.c`, `klog.c`) with `-Werror=incompatible-pointer-types` as a prerequisite of `obj` (153 legacy sites remain), and several app builds are non-fatal. |
| Host-native unit tests | Emerging | `test-io-unit` has 98 TAP cases covering cache/device lifecycle, completion retention, USB media identity, coherent/streaming/SG/bounce DMA ownership, IRQ domains, generic IOMMU requester/mapping/fault policy, and valid/malformed DMAR structures; `test-partition-unit` adds 15 TAP cases for the partition layer (IEEE CRC-32 vectors/chunking, ATA LBA28/LBA48 capacity decode); `test-klog-unit` adds 19 TAP cases for the kernel-log ring (init/empty, append, wrap-around eviction, text truncation, clear); `test-fatchain-unit` adds 10 TAP cases for the bounded FAT cluster-chain walk (valid step, EOC, corrupt next pointer, loop detection, degenerate maxent); `test-fbconsole-unit` adds 35 TAP cases for GOP/VBE pitch (packed and padded 1920x1200x32 accepted; undersize pitch rejected), late WC GOP map policy (below 4 GiB identity and above 4 GiB high-window both allowed; wrap/missing tag refused), 2MiB PDE PAT-WC encoding, 80x25 grid origin, and per-axis zoom (1920x1080 is 3x2); `test-xhcipolicy-unit` adds 15 TAP cases for xHCI MSI-X vs poll (no COM1 and x2APIC refuse MSI-X), port CCS bits, and control TD flags (Setup Chain is RsvdZ; IN Setup TRT=3); `test-consolemux-unit` adds 12 TAP cases for tmux status-line NUL termination (no stack garbage after `console(0)`) and scrollback view mapping; `test-cdcacm-unit` adds 16 TAP cases for CDC-ACM config parsing (QEMU usb-serial ACM, Pico IAD, QEMU FTDI vendor bulk serial, MSC/HID/truncated rejection); `test-usbdbg-unit` adds 16 TAP cases for the CDC debug RPC parser (KEYS hex, SCREEN dump, PPM size, KEXEC size); `test-ttycanon-unit` adds 8 TAP cases for canonical tty `read()` remainder (1-byte reads must not drop the rest of the line); `test-pciscan-unit` adds 5 TAP cases for PCI slot function-count (empty 0xFFFF slots probe nothing); no general runner, fixture API, translating hardware-IOMMU backend, or coverage gate exists yet. |
| In-kernel unit tests | Missing | No suite/case registration or assertion runner; the common kernel `assert()` is a no-op. |
| Guest API selftests | Emerging | Several user programs test real APIs, but each invents marker strings and orchestration. |
| QEMU functional tests | Good foundation | Twenty-five `test-*` targets cover important vertical slices. |
| SMP/concurrency validation | Weak/emerging | Boot, scheduler, POSIX I/O, and virtio smoke use two CPUs; broader contention, lifecycle, spawn, and tool matrices remain. |
| Fault and recovery tests | Emerging | Virtio reset plus xHCI timeout, BOT stall, endpoint/controller reset, QMP in-flight removal, direct-device re-enumeration, repeated automatic hotplug, late attachment, replacement-identity rejection, and host-level coherent-allocation unwind cover narrow paths; no integrated allocation, IRQ, DMA, storage-error, or crash injection framework exists. |
| Fuzzing and malformed-input tests | Missing | No persistent corpus, coverage-guided fuzzing, or parser harness was found. |
| Dynamic analysis | Missing | No sanitizer, race detector, kernel memory checker, or systematic lock validator lane. |
| Coverage | Missing | No source, branch, syscall, device-state, or requirement coverage is reported. |
| Structured results | Emerging | The first host I/O unit emits TAP; guest/system success is still primarily inferred with `grep`, without KTAP/JUnit/JSON archives. |
| Automated CI | Missing | No repository workflow is present. |
| Hardware qualification | Missing | No board inventory, test agent, results service, or compatibility matrix is defined. The Pico 2 W Wi-Fi HTTP API is the first physical-debug agent surface (`GET /v1`); it is not yet a results service. |
| Release and supply-chain assurance | Missing | No formal release gate, reproducibility check, SBOM, signed provenance, or artifact attestation is defined. |

## 2. Current framework inventory

### 2.1 Existing orchestration

The top-level Makefile is the test controller. Recipes generally:

1. build the kernel and selected applications;
2. assemble a GRUB rescue ISO and optional block image under `/tmp`;
3. start `qemu-system-x86_64`, usually headless;
4. redirect serial and emulator output to a log;
5. search for required and forbidden marker strings; and
6. print a final target-level PASS message.

The serial console is the correct baseline oracle for an early kernel. It works
before filesystems, networking, or a full user environment and remains useful
when the kernel is damaged. The weakness is not serial transport; it is the
absence of a structured protocol and a runner that understands test lifecycle,
timeouts, crashes, skips, and artifacts.

### 2.2 Existing target groups

| Group | Current targets | What they prove | Principal limitation |
|---|---|---|---|
| Boot and scheduler | `test-boot`, `test-smp`, `test-klog`, `test-usb-uefi`, `test-usb-uefi-gpt`, `test-ide-thumbdrive`, `test-vbox-uefi-gpt`, `test-vbox-uefi-gpt-bios`, `test-bochs-uefi-gpt` | GRUB/Multiboot2 boot, root mount, AP startup, basic work stealing, no observed GPF; `test-klog` adds the build banner plus `version`/`uname`/`dmesg` console-observability gate (timestamped kernel-log records); `test-usb-uefi` boots the USB thumbdrive under OVMF from a USB mass-storage device and asserts UEFI firmware handoff, `usb0p0` root selection, and AP scheduling; `test-usb-uefi-gpt` boots the GPT ESP Etcher image (`ics-os-uefi.img`) under OVMF on q35 xHCI and asserts `BOOTX64.EFI` handoff, `GPT_DETECT usb0`, USB root, and GOP; `test-vbox-uefi-gpt` / `test-vbox-uefi-gpt-bios` boot that image in VirtualBox EFI and BIOS; `test-bochs-uefi-gpt` boots it under Bochs BIOS; `test-ide-thumbdrive` boots the thumbdrive image attached as an IDE/PATA disk and asserts the MBR parses (partition registered) and the FAT root mounts — regression for the signed-`char` MBR-magic comparison that rejected a valid `0x55 0xAA` signature | Small fixed scenario; no topology, hotplug, preemption, lock, or long-run matrix |
| Process and ABI | `test-exec`, `test-spawn`, `test-vim`, `test-dup` | ELF64 load/execute (`hello`, `spawn`, and the FEAT_TINY vim TUI via non-interactive `vim --version`), `posix_spawn()`, `waitpid()`, writable work disk, and `dup(2)` (`0xC5`) runtime semantics | Smoke cases only; vim gate checks the version banner and clean exit, not editor behavior; spawn test does not assert decoded child status or abnormal exits; `test-dup` proves `dup()` allocates a distinct slot, that a dup'd tty fd is closable, and that a dup'd file fd shares the open description (write-through-duplicate visible after `fsync()`) |
| Fork | `test-fork`, `test-fork-matrix` | COW return ABI and isolation, one-owner fast path, immutable text, injected COW OOM, inherited fd, wait/exit semantics, and ten-child delayed reap on 1/2/4/8 vCPUs | Active CPL0 user/syscall stacks are eager-copied; no shootdown-loss, DMA-pin, unmap-race, multithread rejection, or in-flight io_uring rejection case yet |
| Storage and async I/O | `test-iobench`, `test-virtio`, `test-posixio` | CD/page-cache behavior, virtio-blk DMA/MSI-X, selected POSIX and io_uring operations | Mostly one vCPU; no saturation, cancellation, reset, ENOSPC, corruption, or durability matrix |
| Networking | `test-net`, `test-net-unit` | virtio-net MSI-X attach, static SLIRP config, ARP + ICMP echo to `10.0.2.2`, UDP echo client to host `:7777` (guest also serves UDP/7); host TAP for checksum/ARP/ICMP/UDP | No TCP/sockets/DHCP; single interface; no flood/reset stress |
| USB devices | `test-usb-storage`, `test-usb-storage-xhci`, `test-usb-storage-xhci-multi-controller`, `test-usb-storage-xhci-sg`, `test-usb-storage-xhci-bounce`, `test-usb-storage-xhci-msix`, `test-usb-storage-xhci-poll`, `test-usb-storage-xhci-high-bar`, `test-usb-storage-xhci-recovery`, `test-usb-storage-xhci-stall-recovery`, `test-usb-storage-xhci-disconnect`, `test-usb-storage-xhci-mounted-disconnect`, `test-usb-storage-xhci-mounted-reconnect`, `test-usb-storage-xhci-mounted-remount`, `test-usb-storage-xhci-hotplug`, `test-usb-storage-xhci-hotplug-identity-mismatch`, `test-usb-storage-xhci-late-attach`, `test-usb-storage-xhci-reconnect`, `test-usb-storage-xhci-reconnect-mismatch`, `test-usb-storage-xhci-reconnect-identity-mismatch`, `test-usb-storage-xhci-no-device`, `test-usb-cdc-console`; `test-usb-cdc-pico` (physical Pico `usb-host`, SKIP if unplugged); `test-usb`, `test-usb-amd64` launch helpers | UHCI/q35 xHCI enumeration, two-controller PCI/HCD discovery and secondary-HCD selection, bounded scatter/gather bulk TDs, bidirectional bounce DMA, USB-root persistence, MSI-X delivery and IRQ-assisted waiting, polling fallback, high BAR, timeout/controller recovery, BOT/endpoint stall recovery, in-flight disconnect cancellation, mounted-cache invalidation and dirty-loss reporting, replacement-generation isolation from stale mounted callbacks, controlled quiescent non-root remount with busy-workdir rejection, standard FAT/exFAT/ext4/ISO9660 volume identity comparison, direct-device re-enumeration with sector equality, two automatic remove/add cycles, late first attachment, changed-identity/geometry rejection, failed recovery initialization state, no-device failure, and CDC-ACM console coexistence (`USB_CDC_CONSOLE_OK` plus `ICSOS_VER` on the usb-serial chardev in both attach orders; Pico `/health` `pico=`/`kernel=` when the gadget is present) | The frontend owns only one active HCD plus one optional CDC console slot; no hubs, concurrent controller/device topology, translated-I/O/IOMMU DMA, automatic namespace remount, root replacement, sustained contention, or physical-hardware lane |
| User toolchain | `test-bintools`, `test-buildtools`, `test-make` | In-OS assembler, archiver, linker, utilities, GNU Make, and generated program execution | Expensive whole-guest tests with ad hoc probes and marker contracts |
| GCC path | `test-cc1`, `test-gcc`, `test-gccdriver`, `test-dist`, `test-gcc-kbuild`, `test-kbuild` | Frontend, driver, toolchain, kernel generation, kexec, and capability checks; `test-dist` boots a BIOS+UEFI FAT thumb-drive distribution and drives cc1/as/ld through `gccdrv` against a long-named ldscript, then execs the result (`GCC_DRIVER_OK`/`DIST_GCC_OK`/`GCC_DRV_RUN_OK`) — regression for FAT LFN padding that corrupted VFS node names | KVM/host-CPU dependence for most jobs; long feedback cycle |
| Strict closure | `test-selfhost-cert` | Intended GCC rebuild, rebuilt Make, provenance, kernel rebuild, kexec, and capability loop | Eight-hour timeout, one vCPU, hard-coded object count, and strict closure not yet a consistently green release gate |
| Optional TinyCC | `test-selfhost`, `test-tccboot`, `test-tcc-kbuild`, `test-tcc-fullhost`, `test-fullhost` aliasing GCC path | Bootstrap experiments and compatibility | Must remain non-blocking for the supported GCC policy |
| Aggregate | `test-integration` | Runs boot, SMP, and exec | Its name overstates scope: it excludes storage, POSIX, devices, tools, GCC, and self-hosting |
| Host unit tests | `test-io-unit`, `test-partition-unit`, `test-gpt-unit`, `test-klog-unit`, `test-fatchain-unit`, `test-smpclaim-unit`, `test-fbconsole-unit`, `test-bridge-console-unit`, `test-kbdleds-unit`, `test-lapicx2-unit`, `test-xhcipolicy-unit`, `test-consolemux-unit`, `test-cdcacm-unit`, `test-usbdbg-unit`, `test-ttycanon-unit`, `test-pciscan-unit`, `test-net-unit` | Pure-logic verification off-target: I/O cache/device lifecycle and DMAR, IEEE CRC-32 and ATA LBA28/LBA48 decode, GPT detection/parsing/CRC, kernel-log ring semantics (init/append/wrap/truncate/clear), the bounded fail-closed FAT cluster-chain walk, GOP/VBE pitch acceptance (packed and padded Intel GOP rows), the ESP32 debug-bridge LCD console line buffer (commit/truncate/scroll/dirty tracking; compiles `extras/ics-esp32-serial-bridge/src/bridge_console.c` so the tested logic is the firmware logic), CDC debug RPC plus ICSOS_VER bind/STATUS stamps (`tests/usb_debug_unit.c`), Internet checksum / ARP / ICMP echo transform (`tests/net_*_unit.c`), and the SMP claim / `irq_kstack_dest` / leftover-current predicates (process kstack top only from the user stack; leftover USER on `pagedir1` keeps the USER crit token and must skip `vfs_busy`; leftover USER A on USER B is leftover-mm only; ready-ring `next`/`before` must be a kernel/kheap PCB (`sched_link_ok`); unclaimed ACCESS_SYS on idle BSS must repair; reserved idle slots are 64KiB (cert 248137 overflowed 32KiB); torn 64-bit slot detector — original causes of cert `PF64 rip=0x100000001000`, leftover `task_mgr` `UD64` into `cpus[]`, cert 248132 `CRIT-NONOWNER self=0x1`, cert 248133 `GPF64` in `scheduler`, and cert 248137 `IDLE-STACK-OVERFLOW`) | Host-only; no in-kernel or guest execution, no coverage gate |

### 2.3 Strengths to preserve

1. **Real boot path.** Tests use GRUB Multiboot2 instead of bypassing firmware and
   boot integration with QEMU's direct kernel loader.
2. **Headless failure evidence.** Serial logs survive many kernel failures and
   can be archived by any CI service.
3. **Vertical capability tests.** The GCC-to-kexec path validates interactions
   across process, VFS, VM, compiler, assembler, linker, loader, scheduler, and
   boot code that isolated tests cannot prove.
4. **Positive and negative markers.** Several targets require pass markers and
   reject explicit fail or fault markers.
5. **Useful runtime realism.** Tests use real FAT, ISO9660, virtio-blk, MSI-X,
   DMA, io_uring, and child-process paths rather than replacing every dependency
   with a mock.
6. **Canonical compiler policy.** GCC is clearly separated from optional TinyCC
   experiments; this distinction can become a formal CI policy.
7. **Debug symbols.** The kernel build retains a separate symbol file, which can
   support automated crash symbolization.

## 3. Verified deficiencies and risks

### 3.1 P0: there is no effective kernel assertion policy

The shared kernel assertion header expands `assert(x)` to `((void)0)`. The
allocator also contains locally disabled assertion configurations. This means an
invalid invariant can continue into silent corruption in both normal and debug
builds. A no-op assertion may be valid for a deliberately minimized release
profile, but it is not acceptable as the only profile.

Required correction:

- introduce `ICS_ASSERT()`, `ICS_BUG()`, `ICS_WARN_ON()`, and compile-time
  assertion primitives with one documented policy;
- make assertions active in debug, unit, stress, sanitizer, and release-
  qualification kernels;
- print expression, source location, CPU, current task, register/trap context,
  and a stable event ID before halting or entering the panic policy;
- permit explicitly measured assertion removal only in a production profile;
- make corruption-related invariants non-disableable where continuing is unsafe;
- test assertion and panic paths themselves.

### 3.2 P0: build failures and diagnostics can be hidden

The kernel and application build profiles include `-w`, suppressing all compiler
warnings. The aggregate `apps` recipe prefixes selected hello, POSIX I/O, spawn,
shell, and binutils-test builds and installs with `-`, so Make continues after
those commands fail. This can permit a stale binary to be tested or a requested
artifact to be absent for a reason obscured by later staging errors.

Required correction:

1. Stop ignoring required application failures. Mark genuinely optional programs
   explicitly and report `SKIP` with a reason.
2. Remove `-w` through a warning-debt ratchet, not a single disruptive change.
3. Capture a baseline by warning class and directory.
4. Require no new warnings in changed code first.
5. Progressively enable at least `-Wall -Wextra`, conversion/prototype checks
   suitable for the codebase, and `-Werror` on new QA-framework code.
6. Maintain separate canonical, debug, QA, and release profiles.
7. Add dependency files so header changes reliably rebuild affected objects.

### 3.3 P0: the runner does not distinguish exit causes

Many automated QEMU recipes use Make's ignore-error prefix on `timeout`. This is
understandable because a successful guest may reboot or QEMU may be terminated
by the harness, but it collapses materially different outcomes:

- expected guest-controlled completion;
- expected harness termination after a complete result stream;
- test timeout;
- QEMU command-line or device configuration error;
- QEMU crash;
- host resource failure; and
- guest hang after printing an early pass-looking substring.

The subsequent `grep` checks recover some correctness, but not a trustworthy
execution state. Implement a runner that records emulator return status and
signal, timeout status, last guest heartbeat, final structured plan, panic
state, and required artifacts. Add a test-only QEMU completion device, such as
an x86 debug-exit I/O port, while retaining serial KTAP as the human-readable
record. A target passes only if the test plan is complete and the termination
reason is allowed by that test's metadata.

### 3.4 P0: no automated CI protects the main branch

No repository CI workflow is defined. Passing commands documented for a local
machine are not equivalent to required branch checks. The project therefore has
no centrally enforced clean-build, test, artifact, review, or release policy.

The first CI implementation must use an unprivileged hosted or ephemeral runner
and QEMU TCG for untrusted pull requests. KVM, broad container privileges, and
physical-lab access must be restricted to trusted branches and isolated
short-lived workers. Pull-request code must never execute on a persistent
privileged host with secrets or access to production infrastructure.

### 3.5 P1: there is no unit-test layer

A QEMU boot for every check creates slow feedback and poor fault localization.
Pure or mostly pure kernel logic should be tested as host-linked C code:

- intrusive lists, queues, bitmaps, ID allocation, ring arithmetic;
- path normalization and lookup helpers;
- ELF, Multiboot2, PCI capability, FAT, ISO9660, and descriptor parsing;
- scheduler selection and priority calculations;
- block-range and scatter/gather validation;
- io_uring SQ/CQ validation and wraparound;
- timeout and state-machine decisions;
- device matching and lifecycle transition tables; and
- string, formatting, and SDK compatibility routines.

Target-dependent code still needs in-kernel unit tests with controlled kernel
fixtures. Neither class replaces integration tests.

### 3.6 P1: results are strings rather than test records

Marker strings do not encode suite hierarchy, case duration, skip reason,
expected failure, parameter set, random seed, CPU, or failure location. They
also make duplicate or stale output hard to reason about.

Adopt KTAP version 1 for kernel suites and TAP version 13 for guest user-space
selftests. The host runner should convert both to JUnit XML and preserve the raw
stream. Diagnostic text must be TAP comments. Stable test IDs, rather than prose
strings, should be the compatibility contract.

Required states are `PASS`, `FAIL`, `SKIP`, `XFAIL`, `XPASS`, `ERROR`, `TIMEOUT`,
and `CRASH`. `SKIP` is valid only with a machine-readable environmental reason;
it is not a substitute for an unimplemented assertion.

### 3.7 P1: tests are not isolated or parallel-safe

Recipes share fixed `/tmp/icsos-*` directories, ISO names, disks, sockets, and
logs. Concurrent targets can delete or overwrite one another's assets. Failed
runs can leave ambiguous state. Build and test outputs are also mixed with
source-tree outputs.

Every invocation must receive a unique run directory, for example:

`out/test/<run-id>/<suite>/<case>/`

It should contain a manifest, exact command line, environment allowlist, source
revision, tool versions, kernel and app hashes, ISO/disk hashes, serial log,
QEMU log, KTAP/TAP, JUnit, crash dump, symbolized stack, timing data, and final
verdict. Cleanup should be automatic on success and configurable on failure.

### 3.8 P1: the SMP claim is under-tested

The scheduler, POSIX I/O, and virtio focused gates exercise two CPUs, but spawn,
toolchain, and broader filesystem scenarios still use one, and current I/O cases
do not force every ownership race. A one-vCPU pass cannot establish lock
correctness, memory ordering, per-CPU ownership, completion routing, concurrent
close, migration, or teardown safety.

`make test-smp-matrix` now provides the initial 1/2/4/8-vCPU matrix. It verifies
the exact online count, root mount, AP scheduling enablement, absence of GPF, and
an aggregate mask proving that one atomically pinned worker executed on every AP.
The four-vCPU lane is the default `test-smp` and integration configuration. The
1-vCPU lane remains important because it detects accidental SMP assumptions.

This is still a focused topology gate, not broad SMP qualification. Add repeated
long-run contention, randomized scheduling, 2/4/8-vCPU I/O and filesystem
matrices, sparse APIC-ID/MADT fixtures, hotplug, lock-order validation, and
forced timer/IPI loss or delay before making production-scale SMP claims.

### 3.9 P1: no systematic fault, robustness, or recovery qualification

The driver and I/O designs require lifecycle and recovery semantics that normal
success-path smoke tests cannot validate. A common fault-injection core should
support deterministic `fail_nth`, probability, seed, scope, count, and event-log
controls for:

- kernel/page/DMA allocation failure;
- short I/O, ENOSPC, read-only media, corruption, and I/O error;
- delayed, reordered, duplicated, and lost completions;
- masked, spurious, shared, and storming interrupts;
- device reset, queue reset, hot removal, and surprise removal;
- timeout at every state transition;
- worker creation and process-spawn failure;
- malformed descriptors and device-reported lengths; and
- cancellation during close, exit, unbind, and module unload.

Every injected run must print its seed and injection schedule. CI must retain a
one-command reproducer.

### 3.10 P1: no malformed-input or fuzzing program

Kernel parsers and syscall surfaces process attacker-controlled data. Begin
with host fuzz targets for ELF headers/program headers, filesystem directory and
allocation metadata, Multiboot2 tags, PCI/USB descriptors, paths, format
strings, io_uring entries, and syscall argument validation. Seed corpora with
valid minimal objects plus every historical regression.

Later add a guest syscall-program executor modeled on coverage-guided kernel
fuzzers. Full syzkaller integration requires an ICS-OS executor, syscall
descriptions, crash recognition, VM control, reset strategy, and useful kernel
coverage; it is a later phase, not the first fuzzing step.

### 3.11 P1: no dynamic instrumentation or coverage lane

Canonical GCC 4.7.4 remains the self-host compiler and must not be displaced by
QA tooling. Linux-style generic KASAN and KCOV require newer compilers than this
canonical compiler. Use separate, explicitly non-canonical QA configurations:

- host-native unit/fuzz tests under current GCC and Clang with ASan, UBSan, and
  where appropriate TSan;
- a newer GCC/Clang freestanding compile lane to expose warnings and undefined
  behavior without changing the supported build contract;
- allocator redzones, poisoning, quarantine, guard pages, and ownership checks
  implemented inside ICS-OS for debug kernels;
- stack canaries and checked object/range helpers where ABI-compatible;
- a lightweight kernel coverage transport based on stable PCs/IDs, then a
  KCOV-like per-task mode for fuzzing; and
- a lock-order/deadlock validator before claiming production SMP safety.

Do not report host-unit line coverage as kernel integration coverage. Report
coverage independently by layer and subsystem.

### 3.12 P2: test metadata and aggregate names are incomplete

`test-integration` currently means only boot, SMP, and exec. The asserted USB
storage targets are phony and automated, but the older USB boot targets remain
manual launch helpers. KVM requirements, estimated durations, CPU/memory/storage
needs, destructive behavior, and test ownership are encoded only in recipes or
comments.

Move test descriptions to a manifest consumed by a runner. Keep Make targets as
stable developer entry points. Define honest suites such as `test-pr`,
`test-functional`, `test-io`, `test-toolchain`, `test-nightly`, and
`test-release`; retain `test-integration` as a compatibility alias with a clear
printed expansion.

### 3.13 P2: brittle acceptance checks and stale documentation

The strict GCC target requires an exact count of 349 objects. An exact count is
useful as provenance evidence only if derived from a versioned expected manifest.
A hard-coded scalar fails when upstream configuration legitimately changes and
cannot identify missing or unexpected members. Generate and compare a sorted
manifest of source path, object path, component archive, compiler identity, and
hash.

The binutils test still comments that the kernel cannot propagate child exit
status, while current process code records and returns it. Stale test rationale
reduces trust and should fail documentation review where it affects the oracle.

## 4. Target test architecture

### 4.1 Test pyramid

```text
                 Physical hardware and release certification
                   Long soak, recovery, compatibility, self-host
              QEMU system integration and performance regression
          Guest user-space API, ABI, syscall, and command selftests
          In-kernel component, lifecycle, fault, and concurrency tests
       Host-native unit, property, parser, sanitizer, and fuzz tests
  Compile/link/static checks, generated manifests, ABI and reproducibility checks
```

The lower layers run most frequently and locate faults quickly. Upper layers
validate emergent behavior and real integration. A feature is not complete when
it has only one layer unless its design makes the other layers inapplicable and
the test manifest records that decision.

### 4.2 Common freestanding test core

Implement a small `kernel/test/` core that has no hosted-runtime dependency and
can execute selected suites in either a host adapter or the kernel.

Proposed concepts:

- `struct ics_test_case`: stable ID, function, flags, timeout, tags, parameters;
- `struct ics_test_suite`: name, init/exit, case table, required capabilities;
- expectations that record failure and continue the case;
- assertions that abort only the current case after cleanup;
- typed equality, range, string, memory, null, error, and predicate helpers;
- explicit `skip`, `xfail`, and failure-reason APIs;
- per-case cleanup stack so an assertion does not leak resources;
- deterministic PRNG with printed/replayable seed;
- parameterized cases, repeat count, shuffle mode, and fail-fast mode;
- fixture setup/teardown at suite and case scope;
- test-only fake clock and controlled scheduler/yield hooks;
- watchdog timeout and heartbeat support; and
- KTAP output through a transport-independent writer.

Tests must be linker-discoverable or generated into a registry without relying
on C constructors. The release linker must be able to discard the test sections.
Test code must never be reachable from unprivileged production interfaces.

### 4.3 Host adapter

The host adapter supplies allocation, logging, fake MMIO, fake DMA maps, fake
IRQs, and deterministic clocks. Production modules should expose pure logic and
narrow platform-operation tables rather than compile tests by including `.c`
files. Mocks are appropriate at hardware boundaries; over-mocking VFS or process
interactions would merely reproduce the implementation and should be avoided.

Host tests are ordinary executables and must support debugger invocation,
ASan/UBSan, fuzz entry points, gcov/LLVM coverage, filtering, and one-case
execution.

### 4.4 In-kernel runner

A test kernel accepts boot arguments such as suite/tag filter, seed, repeat,
shuffle, CPU mask, fault profile, and panic policy. It runs early-boot suites
before the scheduler where necessary and normal suites in a dedicated test task
after core initialization. Destructive tests run in a disposable VM and may
request reset after their result has been flushed.

The runner emits KTAP over serial and mirrors structured events to a memory ring
buffer. On panic it flushes the ring, stack/register state, current test ID,
seed, and recent scheduler/IRQ/device events.

### 4.5 Guest selftests

Create a single selftest runner and small focused executables under a coherent
tree rather than adding more console commands for each probe. Selftests should:

- use the public SDK and ABI;
- return meaningful status through `waitpid()`;
- emit TAP;
- isolate writable state per case;
- test success and failure semantics;
- support case filtering and timeout;
- identify capability-based skips; and
- avoid diagnostic probes in the normal pass stream unless they are assertions.

Existing `posixio`, `spawntest`, and `bintest` behaviors can migrate incrementally
without deleting their current Make targets.

### 4.6 System-test runner

Use a Python 3 runner because robust process supervision, manifests, JUnit XML,
parallel isolation, QMP, and artifact management are cumbersome in Make recipes.
Keep build logic in Make and call the runner from stable Make targets.

The runner should:

1. validate dependencies and emulator capabilities before the timed case;
2. allocate a unique scratch directory;
3. build or select immutable assets and record hashes;
4. generate ISO/disk contents from a declarative manifest;
5. pin machine, CPU, memory, device, and accelerator configuration;
6. launch QEMU in its own process group with serial and QMP channels separated;
7. stream and parse KTAP/TAP while preserving raw bytes;
8. monitor heartbeat, panic, watchdog, emulator exit, and host timeout;
9. request a diagnostic dump over QMP before killing a hung VM;
10. symbolize kernel addresses against the exact symbol file;
11. write JUnit and a JSON result manifest; and
12. clean assets on success or retain them according to policy.

Make recipes should no longer duplicate GRUB, QEMU, timeout, log, and grep logic.

## 5. CI standard

### 5.1 Required lanes

| Trigger | Maximum intent | Required work |
|---|---|---|
| Every commit / local pre-push | Minutes | Format/diff checks, generated-file checks, canonical incremental build, changed host units |
| Pull request, untrusted | About 15–25 minutes | Clean canonical build, warning ratchet, host units with ASan/UBSan, parser fuzz smoke, TCG boot/SMP/exec, selected guest selftests, artifact upload on failure |
| Pull request, trusted opt-in | About 30–45 minutes | KVM functional I/O/device matrix and changed-subsystem integration |
| Main-branch merge | Under 60 minutes | All functional suites, 1/2/4-vCPU matrix, virtio/POSIX/spawn/build tools, reproducibility sample |
| Nightly | Several hours | 1/2/4/8-vCPU stress, fault matrices, shuffle/repeat, sanitizer suites, fuzz corpus, filesystem/device combinations, performance tracking |
| Weekly | Long-running | GCC kernel rebuild/kexec, broader toolchain tests, long soak, larger fuzz budget, emulator-version compatibility |
| Release candidate | As long as required | Strict GCC closure, generated-kernel capability matrix, reproducible release build, all required virtual and physical platforms, security/signing/provenance gates |
| Post-release | Continuous | Hardware canaries, fuzzing, trend analysis, regression bisection, supported-version monitoring |

Fast lanes should use change-based selection only as an optimization. A complete
scheduled lane must detect incorrect dependency mapping.

### 5.2 Matrix dimensions

Track these dimensions explicitly instead of embedding them in command strings:

- accelerator: TCG and KVM;
- CPU count: 1, 2, 4, 8, and selected larger systems;
- stable virtual CPU model plus trusted `host` compatibility lane;
- pinned QEMU baseline plus a current-QEMU canary;
- memory pressure: minimum supported, normal, and large toolchain workloads;
- machine/chipset and interrupt mode;
- storage: ISO9660, RAM disk, FAT, virtio-blk, empty/full/error media;
- device transport and queue count;
- canonical GCC, newer GCC warning lane, and Clang QA lane;
- debug/assertion, optimized, sanitizer, coverage, and release profiles; and
- clean build versus incremental rebuild.

A matrix combination may skip only through declared capability predicates. The
CI summary must show skipped required coverage as a release blocker.

### 5.3 Branch protection and review

Required checks for merge should include clean build, warning ratchet, host
units, TCG smoke, and changed-subsystem tests. Protect CI definitions,
build/release scripts, boot code, memory management, scheduler/context switch,
syscall ABI, crypto/signing, and driver core with code-owner review. Require a
test or documented test-impact rationale for behavioral changes.

### 5.4 CI security

- Default workflow token permissions to read-only.
- Pin third-party workflow actions by full commit digest and audit updates.
- Never use privileged pull-request triggers to check out untrusted code.
- Do not expose secrets to untrusted test jobs.
- Use isolated, ephemeral workers for KVM and hardware jobs.
- Do not mount a host container socket or use the current broadly privileged
  development Compose profile in a pull-request lane.
- Separate artifact build from signing; signing identities must not be available
  to user-defined build steps.
- Prefer short-lived identity federation over stored cloud credentials.
- Record and verify artifact provenance and checksums.

## 6. Static, dynamic, and security quality gates

### 6.1 Compile and static gates

Introduce gates in this order:

1. syntax and link success for all supported configurations;
2. warning baseline with no-new-warning enforcement;
3. header self-containment and generated dependency correctness;
4. stack-usage reports and frame-size ceilings for IRQ/exception paths;
5. object-size, section, relocation, executable-stack, and W^X policy checks;
6. ABI layout and exported-symbol manifests;
7. source static analyzers under a newer compiler lane;
8. custom checks for pointer truncation, CPU-pointer-as-DMA, MMIO access,
   unchecked user pointers, and forbidden floating point/red-zone use; and
9. documentation/test-manifest consistency checks.

Warnings are evidence, not style noise. Suppressions require a narrow scope,
rationale, owner, and expiry or upstream compatibility reason.

**Current state (2026-09-03).** The kernel is still compiled with `-w` for the
legacy `kernel32.c` unity TU: a strict trial compile
(`-Werror=incompatible-pointer-types`) surfaces **153** pre-existing
`incompatible-pointer-types` errors, concentrated in
`hardware/chips/irqhandlers.c` (IRQ-handler signatures) and
`hardware/vga/dexvga.c`, with `filesystem/fat12.c` alone carrying 24. Rather
than block the build on that backlog, `kernel/Makefile` now runs a **warning
ratchet**: a `warncheck` target strict-synthesizes the new/hardened TUs
(`partition/gpt.c`, `console/klog.c`) with `-Werror=incompatible-pointer-types`
and is a prerequisite of `obj`, so the gate only tightens. The 153 legacy
sites are the next ratchet batch; as each TU is cleaned it is added as a
recipe line in `warncheck`, and once all are clean `-w` is dropped and the flag
becomes global.

Three implementation gotchas (all verified by running the gate, 2026-09-03):
- The gate must strip `-w` before adding the error flag: use
  `$(filter-out -w,$(CFLAGS))`, not `$(CFLAGS: -w=)` (the substitution
  reference silently leaves a bare `-w` word in place, making `-Werror` a
  no-op).
- Each check is a separate *simple* recipe line (no shell metacharacters): the
  in-OS kbuild make (`contrib/gnumake/job-icsos.patch`) direct-execs simple
  lines via `posix_spawn` and has no `/bin/sh` for a `for`-loop recipe.
- `warncheck` is a no-op when `INOS=1` (passed by the in-OS
  `gmake_kbuild_run`): the in-OS gcc driver cannot run `-fsyntax-only` (it
  always expects assembly output) and the in-OS kbuild is a build-capability
  cert, so the strict gate is enforced by the host build.

### 6.2 Memory and concurrency diagnostics

Build native ICS-OS diagnostics rather than waiting for complete sanitizer
support:

- allocator redzones, poison patterns, double-free/invalid-free detection;
- allocation tags, owner/task/CPU, high-water marks, and leak snapshots;
- guard pages around critical stacks and selected large objects;
- stack watermark and overflow checks for task, IRQ, and AP stacks;
- reference-count underflow/overflow checks;
- lock owner, lock hold time, IRQ-context rules, and lock-order graph;
- IRQ disable-duration and scheduler-latency histograms;
- DMA map/unmap ownership and device-generation validation;
- request state-transition validation; and
- use-after-unbind protection with object generations and quarantine.

These diagnostics must emit stable event IDs consumable by tests and production
monitoring. Debugging and observability are part of the feature, not test-only
printf statements.

### 6.3 Security testing

Security qualification includes:

- malformed binaries, filesystem images, descriptors, packets, and firmware;
- syscall pointer/length/alignment/overflow and privilege validation;
- W^X/NX, user/kernel address separation, executable-stack, and mapping-policy
  tests;
- race and time-of-check/time-of-use tests around VFS, process, and devices;
- resource exhaustion and quota behavior;
- secure failure of module/driver load and artifact verification;
- dependency and toolchain vulnerability tracking; and
- regression cases for every security defect.

Adopt the NIST Secure Software Development Framework as process guidance and
SLSA-style provenance levels for released boot media and kernel artifacts.

## 7. Hardware and performance qualification

### 7.1 Hardware lab

QEMU cannot prove behavior of real IOMMUs, interrupt routing, firmware, USB
controllers, power states, cache coherency, timing, or device quirks. Define a
hardware inventory with immutable machine IDs, firmware versions, CPU/topology,
RAM, chipset, IOMMU, controllers, devices, wiring, and remote power/serial
access.

A lab agent should accept signed test bundles, boot by removable media or PXE,
capture serial independently of the target, power-cycle hung machines, upload
structured results, and erase writable media between trust domains. Initial
coverage should include at least two materially different x86-64 platforms and
representative storage/USB devices. Future ARM64 systems consume the same result
protocol and capability model.

### 7.2 Performance tests

Measure distributions, not one-off timestamps:

- boot and AP-online latency;
- scheduler wake, migration, and context-switch latency;
- interrupt and deferred-work latency;
- syscall and VFS operation cost;
- sequential/random block throughput, IOPS, tail latency, and CPU cost;
- queue-depth and vCPU scaling;
- page-cache hit/miss/writeback behavior;
- process spawn/exec/wait cost;
- toolchain build time and peak memory; and
- recovery/reset downtime.

Record warm-up, sample count, host load, accelerator, CPU model, QEMU version,
frequency policy, seed, and confidence interval. Start in observe-only mode;
make regressions gating only after variance is understood. Correctness always
overrides throughput.

## 8. Test authoring and defect policy

### 8.1 Definition of done

A kernel or SDK change is complete only when:

1. requirements and externally observable behavior are identified;
2. the lowest practical test layer covers normal, boundary, and error behavior;
3. affected integration and SMP scenarios pass;
4. fault, teardown, and recovery paths are covered where applicable;
5. warnings and diagnostics do not regress;
6. structured results and failure artifacts are produced;
7. documentation and test metadata are updated; and
8. no required test is silently skipped.

Every fixed defect receives a regression test that fails for the original cause,
not merely a success marker for the repaired end state.

### 8.2 Flaky tests

A retry must never silently convert failure to success. Store each attempt and
classify a test as flaky when identical inputs produce both outcomes. Quarantine
requires an issue, owner, reproducer evidence, reason, expiry, and continued
nightly execution. Product races are defects even if retry lowers their
frequency. Remove quarantine only after repeated stress evidence.

### 8.3 Test IDs and traceability

Use stable IDs such as `vfs.path.normalize.dotdot-root` or
`virtio-blk.reset.inflight-read`. Metadata should include subsystem, owner,
requirements, level, speed, capabilities, destructive flag, timeout, estimated
resources, supported architectures, fault profile, and source references.
Dashboards should report requirement, code, test, platform, and historical
failure coverage separately.

## 9. Prioritized implementation roadmap

### Phase 0 — truth-preserving harness fixes

- Add all test and launch targets to `.PHONY` or rename manual launch targets.
- Make required app builds fatal and classify optional components explicitly.
- Rename or document the narrow `test-integration` aggregate.
- Introduce unique artifact directories and a test-run manifest.
- Supervise QEMU exit/timeout reasons rather than ignoring status.
- Correct stale test comments and replace scalar GCC object count with a
  versioned manifest.
- Capture tool versions and exact QEMU command lines.

Exit criteria: two tests can run concurrently without collision; a missing
binary, QEMU configuration error, guest timeout, panic, and assertion failure
produce distinct verdicts and retained artifacts.

### Phase 1 — unit core and debug invariants

- Implement runtime assertion/panic policy.
- Add freestanding suite/case/expect/assert/fixture/cleanup APIs and KTAP.
- Add host and in-kernel adapters.
- Create first unit suites for ring arithmetic, paths, ELF validation, block
  ranges, PCI capabilities, lists/queues, and device matching.
- Add ASan/UBSan host runs and source coverage.

Exit criteria: at least fifty focused cases run in seconds on the host, selected
cases run in the kernel, deliberately failing tests are correctly reported, and
the QA kernel stops on invariant corruption with symbolized evidence.

### Phase 2 — structured guest and QEMU runner

- Build TAP guest selftest runner.
- Build Python QEMU/QMP supervisor and JUnit converter.
- Migrate boot, SMP, exec, POSIX I/O, virtio, and spawn first.
- Add test-only deterministic completion channel and watchdog heartbeat.
- Publish failure artifacts in CI.

Exit criteria: the existing primary smoke capabilities pass through the new
runner without grep-only verdicts; local Make entry points remain available.

### Phase 3 — baseline CI and warning ratchet

- Add unprivileged TCG pull-request CI.
- Add trusted isolated KVM jobs.
- Establish warning baseline and no-new-warning gate.
- Add clean/incremental/reproducibility checks.
- Protect required checks and CI definitions.

Exit criteria: no main-branch merge can bypass clean build, host units, TCG
boot/SMP/exec, or changed-subsystem tests.

### Phase 4 — SMP, fault, and lifecycle qualification

- Add 1/2/4/8-vCPU contention suites.
- Implement common fault injection.
- Add delayed completion, saturation, cancellation, reset, ENOSPC, OOM,
  hot-unplug, close/exit, redirty/writeback, and repeated bind/unbind tests.
- Add lock-order, allocator, stack, DMA, and request-state diagnostics.

Exit criteria: the VFS/I/O and driver architecture acceptance scenarios run
repeatedly with deterministic seeds and no leak, hang, stale callback, or
post-unbind access.

### Phase 5 — fuzzing and security QA

- Add host parser fuzzers and corpus management.
- Add guest syscall sequence executor and kernel coverage transport.
- Add malformed image/device/input suites.
- Add vulnerability, W^X, ABI, and artifact-policy checks.

Exit criteria: scheduled fuzzing preserves corpora and minimized reproducers,
and every crash is automatically deduplicated, symbolized, and assigned a stable
signature.

### Phase 6 — performance, hardware, and release assurance

- Deploy hardware-lab agents and compatibility dashboard.
- Establish performance distributions and regression policy.
- Make strict GCC closure a consistently green release gate.
- Run the rebuilt kernel across the required CPU/device matrix, not only the
  self-host workload's one-vCPU configuration.
- Add reproducible build, SBOM, signed provenance, and release attestation.

Exit criteria: a release candidate has traceable source-to-artifact provenance,
all required virtual and physical platforms pass, and performance/security
exceptions are explicitly approved and time-bounded.

## 10. Initial backlog

The first implementation increment should be intentionally small:

1. `test-run` script with unique artifacts, QEMU status classification, raw
   serial, JSON manifest, and JUnit result;
2. strict build prerequisites for `hello`, `posixio`, `spawntest`, and
   `bintest`;
3. active QA assertions and panic metadata;
4. KTAP core plus host adapter;
5. unit suites for io_uring ring fullness/wrap, VFS path normalization, ELF64
   header/range validation, and intrusive-list operations;
6. migration of `test-boot` and `test-smp` to the runner;
7. unprivileged TCG pull-request workflow;
8. 2/4-vCPU variants of POSIX I/O and virtio tests;
9. deterministic allocation-failure and delayed-virtio-completion injection;
10. warning inventory and no-new-warning policy.

This increment produces fast feedback and trustworthy verdicts before expanding
CI breadth. It must not wait for the complete driver or I/O redesign.

## 11. Quality metrics

Track trends without turning metrics into targets that encourage superficial
coverage:

- required suite pass/skip/error rate by platform;
- median and tail test duration;
- flaky rate and quarantine age;
- warning count by class and subsystem;
- host unit line/branch coverage by subsystem;
- kernel test-point and syscall/state coverage;
- fuzz executions, corpus growth, unique crashes, and reproducer latency;
- SMP/fault seeds executed;
- mean time to diagnose and repair regressions;
- escaped defect count and missing-regression-test count;
- hardware platform pass age;
- reproducible artifact match rate; and
- release requirement coverage.

A dashboard should link every failure to source revision, configuration,
artifacts, symbolized crash, seed, and one-command reproducer.

## 12. Recommended policy decisions

1. **Keep GCC canonical.** Newer GCC and Clang are QA instruments, not a change
   to self-host certification.
2. **Keep QEMU vertical tests.** Refactor their harness; do not trade away the
   strongest current evidence for mocks.
3. **Make TCG the untrusted PR baseline.** KVM is a trusted acceleration lane,
   not a prerequisite for basic correctness.
4. **Use KTAP/TAP as the guest contract.** JUnit and JSON are generated host-side
   for CI and analytics.
5. **Treat assertions and observability as production architecture.** A critical
   system must fail diagnosably and expose ownership/state evidence.
6. **Require SMP and failure-path evidence before performance claims.** A fast
   one-vCPU happy path is not a production concurrency result.
7. **Separate launch helpers from automated tests.** A target without assertions
   is not a test.
8. **Make skips visible and governed.** Missing capability is not a silent pass.
9. **Never certify from a single success marker.** Require complete plans,
   allowed termination, forbidden crash events, and artifact consistency.
10. **Gate releases on provenance and hardware.** Emulation-only and locally
    built artifacts are insufficient for critical deployment claims.

## 13. Reference practices

The recommendations adapt established practices rather than copying another
kernel's framework wholesale:

- Linux KUnit: white-box in-kernel suites, assertions, fixtures, QEMU execution,
  and KTAP output.
- Linux kselftest: user-space kernel API tests, explicit skips, subsystem
  selection, per-test timeout, installation, and TAP output.
- Linux KASAN/KCOV and fault injection: memory error detection,
  coverage-guided fuzzing support, and deterministic subsystem failures.
- Zephyr Ztest/Twister/Ztress: host and target tests, fixtures, parameterization,
  skips, repeat/shuffle, platform metadata, and multi-context stress.
- QEMU testing: separate unit/functional/I/O/fuzz tiers, speed groups, isolated
  scratch assets, explicit flaky-test handling, sanitizer/coverage builds, and
  QMP-assisted VM control.
- KernelCI: centralized build/boot/test results across virtual and physical
  platforms.
- NIST SP 800-218 SSDF: secure development practices integrated across the
  lifecycle.
- SLSA: increasing build provenance and build-platform integrity for released
  artifacts.
- GitHub Actions secure-use guidance: least privilege, immutable action pins,
  protected secrets, and isolation of untrusted code from self-hosted runners.

Primary references:

- <https://docs.kernel.org/dev-tools/kunit/index.html>
- <https://docs.kernel.org/dev-tools/kselftest.html>
- <https://docs.kernel.org/dev-tools/kasan.html>
- <https://docs.kernel.org/dev-tools/kcov.html>
- <https://docs.kernel.org/fault-injection/index.html>
- <https://docs.zephyrproject.org/latest/develop/test/ztest.html>
- <https://kernelci.org/docs/>
- <https://www.qemu.org/docs/master/devel/testing/main.html>
- <https://www.qemu.org/docs/master/devel/testing/functional.html>
- <https://github.com/google/syzkaller/blob/master/docs/setup.md>
- <https://doi.org/10.6028/NIST.SP.800-218>
- <https://slsa.dev/spec/v1.2/>
- <https://docs.github.com/en/actions/reference/security/secure-use>
