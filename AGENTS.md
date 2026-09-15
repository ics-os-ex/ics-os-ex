# AGENTS.md — ICS-OS

Guidance for humans and coding agents working in this tree.

## What this repo is

Instructional OS forked from DEX-OS. While being instructional this should not prevent this operating system from being
a state of the art, performant system and showcases the best practices in operating system design. This system should
be good enough to be operational and deployable in critical production settings.

 The active kernel path is **x86-64 long mode** (Multiboot2), with software context switching, LAPIC/SMP, ISO9660 CD root, and ELF64 user executables.

Primary code lives under `ics-os/`. Course labs are under `labs/`.

A Kernel developer guide live here, update when necessary:

wiki/Kernel-Developer's-Guide.md

## Build & test (start here)

```bash
cd ics-os
./scripts/install-deps.sh   # once
make -C kernel bzImage      # Kernel64.bin / vmdex
make -C contrib/hello install
make test-integration       # boot + SMP + exec (QEMU, serial)
```

Useful individual targets (from `ics-os/`):

| Target | Checks |
|--------|--------|
| `test-kbuild` | In-OS GCC → cc1 → GAS → GNU ld kernel build and kexec; not full self-host certification |
| `test-boot` | Multiboot2 ISO boots; `Root mount [OK]`; MB2 framebuffer console active (`FBCONSOLE_PASS`) |
| `test-smp` | `-smp 4` by default; every AP executes pinned work; no GPF |
| `test-smp-matrix` | `-smp 1/2/4/8`; boot, online count, root mount, per-AP scheduler mask |
| `test-exec` | `hello.exe` → `Hello World` + `EXEC_TEST_PASS` |
| `test-selfhost` | In-OS TinyCC compiles/runs `min.c` + `hello.c` |
| `test-tccboot` | In-OS TinyCC rebuilds itself (`tccnew.exe`) and compiles `min.c` |
| `test-tcc-kbuild` | Optional TinyCC kernel experiment; not a supported-path gate |
| `test-iobench` | CD sequential map; 4KiB page cache hits; `IOBENCH_PASS` + `IOBENCH_CACHE_OK` |
| `test-usb-storage` | UHCI USB root; guest write + SCSI cache sync + host readback |
| `test-usb-uefi` | OVMF UEFI boot of the USB thumbdrive image via USB mass-storage; USB root + AP scheduling; GOP framebuffer console (`FBCONSOLE_PASS`) |
| `test-usb-uefi-gpt` | OVMF q35 xHCI boot of the GPT ESP image (`ics-os-uefi.img`); firmware `BOOTX64.EFI`, `GPT_DETECT usb0`, USB root, AP scheduling, GOP (`FBCONSOLE_PASS`) |
| `test-vbox-uefi-gpt` | VirtualBox EFI boot of `ics-os-uefi.img` (IDE); `GPT_DETECT hdp0`, FAT root, persistent write/readback |
| `test-vbox-uefi-gpt-bios` | VirtualBox BIOS boot of the same GPT image (GRUB in the GPT gap) |
| `test-bochs-uefi-gpt` | Bochs BIOS boot of `ics-os-uefi.img`; `GPT_DETECT hdp0`, `Root mount [OK]` |
 | `test-ide-thumbdrive` | IDE/PATA thumbdrive image booted via GRUB; MBR parses (partition registered) + FAT root mount (signed-`char` MBR magic regression); VBE framebuffer console (`FBCONSOLE_PASS`) |
 | `test-dist` | BIOS+UEFI FAT thumb-drive image with the full in-OS GCC toolchain; `gccdrv` drives cc1/as/ld against a long-named ldscript and runs the resulting ELF (`GCC_DRIVER_OK` + `DIST_GCC_OK` + `GCC_DRV_RUN_OK`); regression for FAT long-name (LFN) padding that corrupted VFS node names |
 | `test-usb-storage-xhci` | q35 xHCI USB root; guest write + SCSI cache sync + host readback |
| `test-usb-storage-xhci-multi-controller` | Empty HCD 0 is skipped; HCD 1 delivers MSI-X and persists USB-root writes |
| `test-usb-storage-xhci-sg` | Chained multi-segment xHCI bulk TDs with MSI-X and persistent write |
| `test-usb-storage-xhci-bounce` | Bidirectional xHCI bounce DMA with MSI-X and persistent write |
| `test-usb-storage-xhci-vtd-discovery` | ACPI DMAR/DRHD discovery with QEMU VT-d and persistent xHCI write |
| `test-usb-storage-xhci-msix` | xHCI MSI-X vector delivery, IRQ-assisted wait, and persistent write |
| `test-usb-storage-xhci-msix-recovery` | MSI-X vector release/reclaim across controller recovery |
| `test-usb-storage-xhci-vector-reservation` | Reserved vector exclusion across xHCI MSI-X recovery |
| `test-usb-storage-xhci-poll` | Forced xHCI polling fallback with persistent write |
| `test-usb-storage-xhci-high-bar` | q35 xHCI persistence with its 64-bit BAR relocated above 4 GiB |
| `test-usb-storage-xhci-recovery` | q35 xHCI timeout/reset/re-enumeration, failed recovery initialization, and persistent-write recovery |
| `test-usb-storage-xhci-stall-recovery` | q35 BOT stall/reset and endpoint recovery with bounded controller-reset fallback |
| `test-usb-storage-xhci-disconnect` | QMP removal during active xHCI I/O; bounded cancellation and fail-closed storage |
| `test-usb-storage-xhci-mounted-disconnect` | Mounted USB root removal invalidates cache, quarantines device names, and rejects stale reads |
| `test-usb-storage-xhci-mounted-reconnect` | Replacement USB generation is published while stale mounted callbacks remain offline |
| `test-usb-storage-xhci-mounted-remount` | Same-media replacement rejects busy descendant workdirs, then remounts a quiescent non-root namespace |
| `test-usb-storage-xhci-hotplug` | Runtime monitor performs two automatic remove/add generation cycles |
| `test-usb-storage-xhci-hotplug-identity-mismatch` | Automatic same-size replacement with a changed volume identity remains latched offline |
| `test-usb-storage-xhci-late-attach` | Empty xHCI controller accepts and publishes its first device after boot |
| `test-usb-storage-xhci-reconnect` | QMP remove/add; full re-enumeration, geometry check, and sector equality |
| `test-usb-storage-xhci-reconnect-mismatch` | Re-enumerated replacement with different geometry remains offline |
| `test-usb-storage-xhci-reconnect-identity-mismatch` | Same-size replacement with a different FAT volume serial remains offline |
| `test-usb-storage-xhci-no-device` | q35 xHCI no-device path reaches console without registering USB storage |
| `test-usb-cdc-console` | q35 xHCI MSC root plus CDC-ACM console (`usb-serial`); both attach orders keep `Root mount [OK]` and write `USB_CDC_CONSOLE_OK` plus `ICSOS_VER` to the gadget chardev |
| `test-usb-cdc-pico` | Physical Pico 2 W (`2e8a:0005`) via QEMU `usb-host` on q35 xHCI; curls Pico Wi-Fi for `USB_CDC_CONSOLE_OK`, `ICSOS_VER`, `pico=`, `USB_CDC_RX`, and `STATUS cdc=1`/`release=`. SKIP if the gadget is not plugged into the host |
| `test-posixio` | POSIX fds + preadv/pwritev/fsync + io_uring; ramdisk `POSIXIO_PASS`/`URING_PASS`; virtio `/dev/vblk` `URING_VBLK_PASS` |
| `test-virtio` | QEMU virtio-blk DMA; MSI-X completions; `VIRTIO_BLK_OK` + `VIRTIO_IRQ_OK` |
| `test-net` | QEMU virtio-net + SLIRP; DHCP DORA+T1 renew+T2 rebind + ICMP + UDP/TCP echo + sockets (`netecho.exe`) + `httpd.exe` + `nc.exe`; `NET_DHCP_OK`/`NET_DHCP_RENEW_OK`/`NET_DHCP_REBIND_OK`/`NET_SOCK_*`/`NET_HTTPD_OK`/`NET_NC_OK` |
| `test-net-stress` | KVM `-smp 4` + `user-smp`; forked concurrent TCP/UDP socket clients (`netstress.exe`); `NETSTRESS_PASS` + `USER_RUN cpu=[1-7]`; no PF/GPF |
| `test-net-unit` | Host TAP for checksum / ARP / ICMP / UDP / TCP / sockaddr / DHCP helpers (`tests/net_*_unit.c`) |
| `test-ext4` | ext4 virtio-blk read/create/write; guest marker plus host `e2fsck`/`debugfs` validation of the post-test image |
| `test-spawn` | `posix_spawn` + `waitpid` of `hello.exe` (`SPAWN_PASS`); FAT `/work` on virtio (`WORK_DISK_PASS`) |
| `test-stress` | SMP=4 spawn/exit/reap + short fork+ELF overlap (`STRESSPROC_PASS`); no GPF/PF |
| `test-stress-user-smp` | Same churn with `user-smp` so children run on APs; regression gate for the one-PCB-one-CPU claim invariant (two CPUs sharing an IRQ kstack) and for lost `waitpid` statuses |
| `test-apuser` | `user-smp` cmdline; 3 SSE/red-zone workers on APs (`APUSER_PASS` + `USER_RUN cpu=[1-7]`); bad-pointer child is killed without halting (`APUSER_PF_RECOVER_OK`) |
| `test-fatwrite` | SMP=4 concurrent FAT16 `/work` writers+readers (`FATWR_PASS`); no truncated patterns |
| `test-fatwrite-coop` | Same writers under `coop-smp` (user processes on APs, no timer preemption of a running user tool — the self-host closure's scheduling regime); asserts no `CRITHANG`/`WATCHDOG`, so it fails on a lock hang rather than on a missing success marker |
| `test-fork` | COW fork ABI/isolation, fast path, text protection, OOM, inherited fd, exit/wait, and delayed reaping |
| `test-fork-matrix` | COW fork pressure gate on `-smp 1/2/4/8` |
| `test-make` | In-OS TinyCC builds GNU make 3.82 onto `/work`; `make -f t.mk` spawns `hello.exe` (`MAKE_PASS`) |
| `test-bintools` | In-OS GNU binutils: `as` assembles, `ar` archives, `ld` links a default-script ELF64, then it execs (`AS_PASS`/`AR_PASS`/`LD_PASS`/`BINTOOLS_PASS`) |
| `test-vim` | FEAT_TINY vim ELF64 TUI; non-interactive `vim --version` prints the real banner + exits cleanly |
| `test-dup` | Runtime `dup(2)` (`0xC5`) self-test: dup'd tty fd allocable+closable; dup'd file fd write read back through the original (`DUPT_PASS`) |
| `test-partition-unit` | Host-native TAP unit tests for partition-layer logic: IEEE CRC-32 vectors/chunking and ATA LBA28/LBA48 capacity decode (`tests/partition_unit.c`) |
| `test-fbconsole-unit` | Host-native TAP for GOP/VBE pitch, late map, PAT-WC, 80x25 origin, and per-axis zoom (`tests/fbconsole_geom_unit.c`) |
| `test-kbdleds-unit` | Host-native TAP for i8042 boot-stage Caps/Num/Scroll encoding (`tests/kbd_boot_leds_unit.c`) |
| `test-lapicx2-unit` | Host-native TAP for x2APIC MSR numbers (`tests/lapic_x2_unit.c`) |
| `test-xhcipolicy-unit` | Host-native TAP for xHCI MSI-X vs poll (no COM1 / x2APIC), CCS bits, and control TD flags (`tests/xhci_policy_unit.c`) |
| `test-consolemux-unit` | Host-native TAP for tmux status-line NUL/truncation, scrollback view mapping, and Caps/Ctrl mux letter folding (`tests/console_mux_unit.c`) |
| `test-scriptcomment-unit` | Host-native TAP for DOS `rem`/`#`/`'` script comment tokens (`tests/script_comment_unit.c`) |
| `test-cdcacm-unit` | Host-native TAP for CDC-ACM and QEMU FTDI vendor-serial config parsing (`tests/usb_cdc_acm_unit.c`) |
| `test-netchecksum-unit` | Host-native TAP for Internet checksum (`tests/net_checksum_unit.c`) |
| `test-netarp-unit` | Host-native TAP for ARP build/parse/cache (`tests/net_arp_unit.c`) |
| `test-neticmp-unit` | Host-native TAP for ICMP echo request→reply transform (`tests/net_icmp_unit.c`) |
| `test-netudp-unit` | Host-native TAP for UDP build/checksum (`tests/net_udp_unit.c`) |
| `test-nettcp-unit` | Host-native TAP for TCP build/checksum (`tests/net_tcp_unit.c`) |
| `test-netsock-unit` | Host-native TAP for htons/inet_addr sockaddr helpers (`tests/net_sock_unit.c`) |
| `test-netdhcp-unit` | Host-native TAP for DHCP discover/offer parse (`tests/net_dhcp_unit.c`) |
| `test-usbdbg-unit` | Host-native TAP for the CDC debug RPC line parser, KEYS hex, SCREEN dump, PPM size, and ICSOS_VER bind/STATUS stamps (`tests/usb_debug_unit.c`) |
| `test-ttycanon-unit` | Host-native TAP for canonical tty read remainder (`tests/tty_canon_unit.c`) |
| `test-pciscan-unit` | Host-native TAP for PCI slot function-count (empty slots skip fn 1-7; `tests/pci_scan_unit.c`) |
| `test-bridge-console-unit` | Host-native TAP for the ESP32 debug-bridge LCD line buffer (`tests/bridge_console_unit.c`) |
| `test-vfsgrow-unit` | Host-native TAP for VFS `vfs_units_covering` (exact cluster-size grow; `tests/vfs_grow_unit.c`) |
| `test-smpclaim-unit` | Host-native TAP for the one-PCB-one-CPU claim/publish protocol, crit nest tokens, and `irq_kstack_dest` (process kstack top only from the user stack; `tests/smp_claim_unit.c`) |
| `test-integration` | `test-boot` + `test-smp` + `test-exec` |

Do **not** use QEMU `-kernel` for the ELF64 image; boot via GRUB `multiboot2` (ISO/USB helpers in the Makefile).

## QA is part of every feature

Read `ics-os/docs/testing-and-qa-modernization-plan.md` before implementing a
feature or fixing a defect. QA modernization is not a separate future project:
implement the applicable improvements from that plan incrementally in the same
change as the production feature.

For every feature or behavioral change:

1. Define the observable behavior, failure semantics, concurrency assumptions,
	and test impact before implementation.
2. Add tests at the lowest practical layer: host-native unit tests for pure
	logic, in-kernel tests for target-dependent components, guest selftests for
	SDK/syscall behavior, and QEMU or hardware tests for integrated behavior.
3. Add normal, boundary, invalid-input, and failure-path cases. Changes involving
	shared state, IRQs, DMA, devices, VFS, scheduling, or asynchronous I/O also
	require multi-vCPU contention, timeout, cancellation, teardown, recovery, and
	deterministic fault-injection coverage where applicable.
4. Add a regression test for every defect. It must fail for the original cause,
	not merely look for a final success message.
5. Improve the shared QA infrastructure instead of creating another ad hoc test
	convention. New or migrated tests should use stable test IDs, explicit
	timeouts, isolated run artifacts, deterministic seeds, and KTAP/TAP-compatible
	results as those facilities become available.
6. Run the focused new tests and the relevant existing subsystem/integration
	tests. Report commands, configurations, and results; never claim PASS for a
	command that timed out, crashed, was skipped, or returned nonzero.
7. Update the QA plan, developer guide, test manifest, and observability tooling
	when the feature introduces a new test layer, fault model, invariant, hardware
	capability, or release requirement.

A feature is not complete when it has only an untested implementation, a manual
launch recipe, or a serial success marker without assertions. Do not defer
applicable tests, assertions, diagnostics, runner support, or fault hooks to an
unspecified follow-up. If missing infrastructure makes the complete test
impossible, implement the smallest reusable foundation now, document the precise
remaining gap and owner, and do not make the unsupported production-readiness
claim.

Preserve GCC as the canonical supported compiler. Newer GCC/Clang, sanitizers,
coverage, and host-native builds are additional QA lanes, not replacements for
the GCC self-host and Multiboot2/QEMU acceptance gates.

## Architecture notes that bite agents

- **SysV AMD64 ABI** in kernel C and IRQ wrappers (`irqwrap.S`). After `PUSH_ALL`, saved `rax` is at offset **112**, not `0` (that slot is `r15`).
- **DEX `int 0x30` ABI** still uses `rax/rbx/rcx/rdx/rsi/rdi` for syscall args; the wrapper maps them to SysV for `api_syscall`. Args are **pointer-width** (`api_arg_t`).
- **Identity map** covers low 4GiB; do not rebuild classic 2-level user PTs for that range on x86_64. PCI MMIO pages need PCD|PWT (`mmio_mark_uncacheable`); `dex32_restore_identity_map` reapplies those bits. User private PML4s keep only 0–4MiB and 32–128MiB in PD0; do not load the kernel CR3 while RSP still points at a user stack (those pages are not in `pagedir1`). IRQ/syscall wrappers copy the frame onto a per-process kheap stack (`irq_kstack_enter`) before C; `fork` must use `irq_user_rsp`, not the kstack copy. `getphys64` walks the full CR2 through `KDIRECT`.
- **Memory map** is `kernel/memory/memlayout.h`. Kernel image must stay below 4MiB (user ELF). Frame stack follows `bssEnd`. Do not invent a new fixed PA; add a reserved range so `mempop` skips it.
- **SMP**: APs load the kernel GDT (`ap_load_kernel_gdt`), use LAPIC timer vector **0x41**, claim tasks with `on_cpu`, and honor `cpu_affinity`. Console / `fg_mgr` / user processes are BSP-pinned today. Why SMP bugs take days, and which of them are design rather than "SMP is hard", is in `ics-os/docs/smp-debugging-hardness.md`.
- **Serial** is the headless oracle. Prefer `serial_puts` / putc mirroring for QEMU `-nographic` tests. COM1 TX is `uart_com1_putc` (port I/O in registers only); a C `uart_dev*` reload from the user stack #PF'd in `uart_putc_raw` during `make -j4`.
- **GCC is the supported kernel compiler.** `make test-kbuild` proves that host-seeded GCC/binutils executables running in ICS-OS can build and kexec the kernel. Do not call ICS-OS fully self-host capable until an in-OS-rebuilt GCC compiles the kernel and the generated kernel passes the post-kexec capability suite. x86_64 TinyCC remains optional.

## Coding conventions

- Match existing style: K&R-ish C, `DWORD`/`uintptr` mix, minimal new abstractions.
- Prefer small, targeted fixes over refactors. Do not reformat unrelated files.
- Kernel objects: freestanding (`-ffreestanding -fno-pic -mno-red-zone -mcmodel=large`).
- User apps: `sdk/app.mk` (`-m64`, link `crt1.c` + `tccsdk.c`).
- Document non-obvious long-mode/SMP behavior in `ics-os/docs/smp-longmode.md`.
- Treat tests, assertions, deterministic fault hooks, diagnostics, and structured
	result reporting as feature deliverables, following the QA policy above.

## Design considerations

- When designing system architectures, use state-of-the-art, best practices and/or industry standard mechanisms. Ensure the system is easy to use, stable and intuitive in its design. This system will eventually be used in datacenters and production grade use cases so take this into consideration.

- While Intel x86 32-bit and 64-bit amd64 is the focus at the moment, do consider design that does not impede support for other processors and systems like arm/arm64 that may be incorporated in the future.

- backwards compatibility from a user application prespective is not a concern at the moment, however if there are necessary kernel changes (e.g. system call enhancements or redesign)
that will affect applications, ensure the sample applications, SDK and tools that are provided in this repository are appropriately maintained to support these changes.

- When there are system related errors encountered during development - ensure the debugging, introspection and monitoring tools are sufficient and incorporate them as necessary. Maintainability, debugging and monitoring should be part of the feature set of the operating system.

## Research and Tools

For research on documentation, third party sources, state-of-the-art and industry standard approaches you may perform web search as available
in the current agent MCP functions as needed. You may also download any public, non-proprietary
documentation or sources for reference (e.g. hardware specs, standards). Place all of these artifacts in the /references folder for later reference and retrieval, but do not commit this as part of the repo.

When implementing a standard spec like multiboot, PCI-E, ACPI or the like do not guess, download the necessary references and base it on that. If there are existing stable opensource implementations of that you may also download that for reference.

Properly index these files as needed in a file called reference.md

## Development blog

Use development_blog.md as your diary of activities, what was done, difficulties faced and solutions to problems. Organize this by hour and date, place the latest entry at the top of the blog.

Make sure it contains the current problem and the activity currently being performed to solve it.

## Do not commit

- Build products: `*.o`, `Kernel64.bin`, `Kernel64.sym`, `vmdex`, `vmdex-raw`, ISO/USB images.
- Secrets or machine-local paths.

## Suggested next work

1. Qualify physical N150 xHCI USB root and writable `/icsos`. GOP console is live; parse MADT before re-enabling APs. Pico USB CDC-ACM gadget console is in (`test-usb-cdc-console`); UART bridges still need a 16550 COM1 header.
2. Complete strict GCC self-host certification (see `ics-os/docs/gcc-selfhost.md`): `test-kbuild` passes with a host-seeded compiler, but GCC must still rebuild itself in-OS and that rebuilt compiler must build the kernel before the loop is closed.
3. Richer io_uring (registered buffers, linked SQEs) if needed. Async virtio CQEs and `/dev/vblk` are in. `posix_spawn` / `waitpid` / `/work` are in (`make test-spawn`).
4. Allow user processes on any CPU and validate remote COW TLB shootdown during migration; harden `waitpid`/exit migration first.
5. Full ring-3 user mode (today user ELFs still enter with kernel CS).
