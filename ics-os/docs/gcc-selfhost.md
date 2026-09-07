# GCC self-host path (ICS-OS)

**Capstone goal:** ICS-OS self-hosts a GCC build. The way to discover every OS
gap that blocks it is to run the **real toolchain in-OS** and close what's missing.

Host Linux `gcc`/`make`/`ld`/`as`/`ar` cannot run here as-is (glibc + Linux
syscalls). So "host-compiled" means **rebuilt by the host toolchain against the
ICS-OS SDK** — the same pattern as the working `apps/make.exe`.

- Using the in-os gcc build, it should also be able to compile itself (gcc toolchain)
with no loss in functionality. The purely ics-os generated gcc build can then compile
the ics-os kernel sources in-os with no loss in functionality.

This closes the development loop. ics-os can exist independently and can be improved upon
and developed on indefinitely without a host OS.

## Approach (Phase 1: binutils)

```
host make → as, ld, ar (built vs SDK, shipped on /work) → in-OS: ar + as + ld + exec
   → (later) GCC C-only cross-build vs SDK, in-OS self-build → ICS-OS kernel
```

Phase 1 builds GNU binutils (`as`/`ld`/`ar`) with host gcc against
`sdk/include` + `tccsdk.c`/`posix.c` (same flags as `apps/make.exe`), then an
in-OS `test-bintools` runs: `ar rcs libx.a x.o`, `as prog.s -o prog.o`,
`ld -o prog.exe ...`, exec `prog.exe` and check output. Each missing POSIX
piece (getcwd, stat/access, unlink/rename, time, ...) is an OS gap to close.

Overlay: `contrib/binutils/` (config.h + Makefile, same pattern as
`contrib/gnumake/`). Sources under `references/binutils-2.X/` (not committed).

**Status:** make in-OS is green (`test-make`: in-OS TinyCC builds make, or the
host-gcc make runs a recipe — both paths PASS). `waitpid(-1)`/`WNOHANG`, `wait()`,
`posix_spawn`, `dirent`, FAT `/work`, virtio-blk are in. **binutils is green too**:
`test-bintools` runs `as`→`ar`→`ld` in-OS, links a default-script ELF64, and execs
it (`AS_PASS`/`AR_PASS`/`LD_PASS`/`LD_EXEC_PASS`/`BINTOOLS_PASS`).

**binutils (this round):** `contrib/binutils/` builds the curated **libiberty —
102/102 objects** — with host gcc against `sdk/include` + `tccsdk.c`/`posix.c`
(same flags as `apps/make.exe`). The curated **libbfd ELF x86-64 subset —
40/40 objects** (core + `elf.c`/`elflink.c` + `elf64.c`/`elf64-gen.c`/
`elf64-x86-64.c` + archive/link support) compiles clean against the SDK.
**Three tools now build:** `ar.exe` (ar frontend + libbfd + libiberty),
`as.exe` (GAS core 33 objects + opcodes i386 + libbfd + libiberty) and
`ld.exe` (ld core + the `elf_x86_64` emulation + libbfd + libiberty) all link
as statically-linked ELF64 x86-64 ICS-OS user executables (`int 0x30` ABI).
Key i386 finding: the cgen/itbl table backends (`itbl-ops.c`/`cgen.c`, MIPS
only, `HAVE_ITBL_CPU`) are NOT part of the i386 build — `as.c` compiles
`#define itbl_init()` (no-op) for non-itbl targets, and the target files
(`obj-elf.c`/`atof-ieee.c`/`tc-i386.c`) come from `gas/config/`. `DEFAULT_ARCH`
must be the string `"x86_64"`, not a BFD enum. The build
surfaced and closed a first batch of real SDK gaps (see "SDK gaps closed
(binutils)"):
`float.h`, `sys/param.h`, `sys/resource.h`, `malloc.h`, `strings.h`, C99
`intmax_t`/`uintmax_t`, `sigset_t`/signal-set APIs, `F_*`/`FD_CLOEXEC`,
`realpath`/`sysconf`/`getpagesize`/`pathconf`, extra `errno` codes — all
userspace-only (no new syscalls).

**Generated headers:** BFD's configure normally generates `bfd.h`, `bfdver.h`,
`bfd_stdint.h`, `targmatch.h`, `elf64-target.h`/`elf32-target.h`. These are
committed in `contrib/binutils/` (found first via `-I`) so no autotools run is
needed: `bfd.h` is the `bfd-in2.h` template with the 5 host-type placeholders
substituted for a 64-bit host (`file_ptr`=`long`, `BFD64`); `targmatch.h` is a
hand-written single-target table (ELF64 x86-64 + l1om/k1om, `&vec` pointer
entries); `elf64-target.h`/`elf32-target.h` are `sed s/NN/64|32/` of
`elfxx-target.h`.

**Diagnostic note:** the GPF64 handler no longer halts the VM on **user** faults —
it dumps RIP/CR2/regs then kills the child and resumes the parent (sets
`dex32_child_faulted`), mirroring the page-fault path. Kernel faults still halt.
This is how the TCC-linked make's startup GPF (`rip=0x8`, `call *%rax`, `rax=0`)
was captured.

## SDK gaps closed (binutils round)

Closed while compiling libiberty against the SDK (all in `sdk/`):

| Gap | Where | Notes |
|-----|-------|-------|
| `float.h` | new header | IEEE-754 float limits (floatformat.c) |
| `sys/param.h` | new header | `PATH_MAX`/`PAGE_SIZE`/`MIN`/`MAX`/`roundup` |
| `sys/resource.h` | new header | `struct rlimit`, `RLIMIT_*` |
| `malloc.h` | new header | legacy shim over `stdlib.h` (hashtab.c) |
| `intmax_t`/`uintmax_t` | `stdint.h` | C99; `strtoumax`/`PRIxMAX` |
| `sigset_t` + `sig*`/`raise` | `signal.h` + `posix.c` | SDK-side masks (no HW delivery) |
| `F_*`, `FD_CLOEXEC` | `fcntl.h` | `pex-unix.c`, libbfd file I/O |
| `realpath`/`sysconf`/`getpagesize`/`pathconf` | `unistd.h` + `posix.c` | libbfd `getpagesize`, ld `realpath` |
| `ENAMETOOLONG`/`ELOOP`/`EISDIR`/`ENOTEMPTY`/`EPIPE`/`ESRCH`/`EDEADLK` | `errno.h` | |

**`as` (GAS) round — additional gaps:**

| Item | Where | Notes |
|------|-------|-------|
| `ungetc` | `tccsdk.c` + `stdio.h` | opaque-fd `FILE`: file pushback = `fseek(ftell-1)`; `stdin` uses a 1-char slot |
| `localtime` | `posix.c` | was a fixed-date stub; now civil-from-days (Hinnant), UTC, real `tm_yday`/`tm_wday` |
| `strftime` | `posix.c` + `time.h` | subset for the `-L` listing timestamp |
| `mbstowcs` | `wchar.h` (new) | `static inline`; ICS-OS is single-byte/ASCII so byte length is correct |

`getrlimit`/`setrlimit` are implemented in-SDK (defaults unlimited,
`RLIMIT_NOFILE`=256) — no kernel backing yet. **Open kernel item:** the SDK
`fcntl` is a stub (returns 0); `F_GETFD`/`F_SETFD`/`F_GETFL`/`F_SETFL` need a
kernel side before libbfd file I/O is fully correct.

## ld (the linker) round

`ld.exe` builds from the 2.23 `ld/` core (`ldctor/ldemul/ldexp/ldfile/ldlang/
ldmain/ldmisc/ldver/ldwrite/lexsup/mri/ldcref/pe-dll/pep-dll/ldlex-wrapper/
ldgram/deffilep`) + the `elf_x86_64` emulation, linked against libbfd +
libiberty + the SDK runtime. Findings and the build's quirks:

- **Emulation is generated, so it is committed.** `ld/Makefile.in` produces
  `eelf_x86_64.c` from `emulparams/elf_x86_64.sh` + `emultempl/elf.em` +
  `scripttempl/elf.sc` via `genscripts.sh` (it inlines the `elf_x86_64`
  linker script as a C string). We ran that script once and commit
  `contrib/binutils/eelf_x86_64.c` (found via `-I$(CONFDIR)` before
  `$(SRC)/ld`). `ldemul-list.h` (the `EMULATION_LIST`) is a one-line
  `&ld_elf_x86_64_emulation` hand-written to match the `Makefile.in` rule.
  Both are found in `CONFDIR` before `$(SRC)/ld`, so `#include
  "ldemul-list.h"` and `#include "eelf_x86_64.c"` resolve to our committed
  copies.

- **Configure-injected defines.** Upstream passes `-DDEFAULT_EMULATION='"
  $(EMUL)"'`, `-DSCRIPTDIR=...`, `-DBINDIR=...`, `-DTOOLBINDIR=...` on the ld
  objects. We inject the same via `LDDEFS` (`DEFAULT_EMULATION` must equal the
  emulation's `.name`, `elf_x86_64`).

- **`#ifdef` vs `#if 0` traps (config.h).** `ld/sysdep.h` does
  `#ifdef HAVE_DLFCN_H → #include <dlfcn.h>` and the plugin code is gated by
  `#ifdef ENABLE_PLUGINS` — *definition* tests, not value tests. A plugins-off
  upstream build therefore leaves **both undefined**, so `config.h` must not
  `#define ENABLE_PLUGINS 0` / `#define HAVE_DLFCN_H 0` (a 0 definition still
  triggers `#ifdef` and pulls in `<dlfcn.h>`). They are intentionally undefined
  in our `config.h`.

- **`ldlex.c` compiles through `ldlex-wrapper.c`.** The checked-in 2.23
  `ldlex.c` (flex output) includes `bfd.h` with no prior `sysdep.h`/`config.h`,
  but `ldlex-wrapper.c` is just `#include "sysdep.h"` + `#include "ldlex.c"` —
  so it compiles `ldlex.c` with `config.h` loaded first. Upstream compiles
  *only* the wrapper (`CFILES` lists `ldlex-wrapper.c`, not `ldlex.c`); listing
  both gives "multiple definition of `yy*`/`lex_*`". We follow that.

- **`strpbrk`** was missing from the SDK (ld's `ldlang.c` uses it for option
  parsing). Added to `tccsdk.c` + declared in `sdk/include/string.h`
  (alongside `strspn`/`strcspn`).

- **No C++ demangler.** libiberty's `cp-demangle.c` is not built (no C++
  runtime). `demangle-stub.c` now provides `cplus_demangle` (→NULL), the
  `current_demangling_style` global, and `cplus_demangle_set_style` /
  `cplus_demangle_name_to_style` (ldlang.c / lexsup.c call these
  unconditionally for symbol display). `demangle.h` is self-contained, so the
  stub includes it for the exact enum.

- **The default script is the combreloc variant, not the bare `.x`.**
  `gldelf_x86_64_get_script()` (`eelf_x86_64.c`) picks the script by link
  flags: with the default `link_info.combreloc` it returns
  `ldscripts/elf_x86_64.xc` (the *-combreloc* variant), **not**
  `elf_x86_64.x`. So the build stages the whole `elf_x86_64.x*` family (13
  variants) next to `ld.exe` and into the `test-bintools` ISO (wildcard in both
  `contrib/binutils/Makefile` and the top-level ISO staging). Staging only the
  bare `.x` makes a plain `ld in.o -o out` fail with ENOENT on `.xc` — a
  confusing red herring because a *literal* `fopen` of the base name in the same
  process still succeeds.

**Host-run caveat:** `as.exe`/`ld.exe`/`ar.exe` are `int 0x30`-ABI ICS-OS user
executables (SDK runtime), so running them on host Linux segfaults at the first
syscall (expected — the `int 0x30` gate does not exist in Linux). Their
functional test is **in-OS** (the `test-bintools` target, now green):
`as mini.s -o mini.o`, `ar r test.a mini.o`, `ld mini.o -o mini.exe` (default
script), then exec `mini.exe` and check its output on serial.

## GCC is the supported kernel self-host compiler

> **Certification status:** `test-kbuild` currently starts from a host-built
> `cc1.exe`, GCC driver, GAS, and GNU ld. It proves those tools can run inside
> ICS-OS, generate a kernel, and boot it, but it does **not** yet prove compiler
> closure or post-kexec feature parity. Do not describe ICS-OS as fully
> self-host capable until an in-OS-built GCC rebuilds the kernel and the
> generated kernel passes the capability regression gate.

`test-kbuild` invokes the in-OS GCC kernel build. The older TinyCC kernel path
is retained only as `test-tcc-kbuild` / `test-tcc-fullhost`; it is not part of
the supported self-host completion criteria. Keep TinyCC for:

- `test-selfhost` / `test-tccboot` (bootstrap C)
- compiling GNU make and bootstrapping the C-only GCC 4.7.4 toolchain

## POSIX gaps (this round)

GCC/`make` do the usual spawn:

```c
pid = fork();
if (pid == 0) { execv(path, argv); _exit(127); }
waitpid(pid, &st, 0);
```

Historic DEX `user_execp` always **waits** (spawn+join). Console and
`tccboot` depend on that — do not change it.

| Piece | Status |
|-------|--------|
| `waitpid` | DEX syscall `0xB1` → `sys_waitpid`; SDK `sys/wait.h` |
| `wait` | SDK `int wait(int*)` → `waitpid(-1, status, 0)`; needed by GNU make's blocking job path (legacy DEX `0xC` spin-wait never yielded to the child) |
| `posix_spawn` | DEX syscall `0xB2` (`sys_spawn`): load ELF, return child pid, no wait |
| `execv` / `execvp` | DEX syscall `0xB3` (`sys_execve`): same-pid image replace (steal pid, switch) |
| `fork` | x86-64 COW fork is available for single-threaded private-PML4 processes without in-flight io_uring; active CPL0 user/syscall stacks are eagerly copied. GCC still uses `posix_spawn`; `vfork` is unavailable. |
| `user_execp` (`0x5B`) | Unchanged: still waits |

The ISO 9660 root is 8.3 unless Joliet is selected; Rock Ridge `NNM`
(absolute-path) records are now honoured on name lookup, so long names like
`ldscripts` / `elf_x86_64.xc` resolve even where the base 8.3 name is
truncated (this is what lets `ld` find its linker-script dir on the CD root).
`spawn.exe` is the packed test binary (`contrib/spawntest`, console command
`spawntest`).

Stock GCC `pexecute` can be wrapped with `posix_spawn` until fork is fixed.

## Disk plan

`/ramdisk` is 16 MiB FAT16 — too small for GCC/binutils trees. `vblk` is a
raw virtio-blk device.

After `virtio_blk_init` and FAT registration, the kernel probes sector 0.
If the BPB looks like FAT, it mounts at **`/work`** and prints `work: mounted`.
No vblk, or a zeroed/non-FAT image (as in `test-virtio` / `test-posixio`),
skips the mount so `test-boot` stays green.

Host side for `test-spawn`: `mkfs.vfat -F 16` a **512 MiB** image, attach
like the posixio virtio disk. 512 MiB is enough for spawn tests and a later
trimmed binutils + make tree. GCC 4.7 itself needs a bigger image in a
later round.

FAT 8.3 still bites some GCC names even with LFN on create; that is a later
FS issue.

## Rounds

1. **binutils** (`as`, `ld`, `ar`) — **done**: `test-bintools` builds, links,
   and execs an ELF64 in-OS.
2. **GCC 4.7.4** (`--enable-languages=c --disable-multilib`):
   - **cc1 (C frontend)** — **done**: `test-cc1` runs the host-built cc1 (the
     full GCC C frontend, ~18 MB, linked vs the SDK) in-OS to compile C →
     assembly.
   - **end-to-end toolchain** — **done**: `test-gcc` composes the *real* cc1
     (C→asm) + `as` (GAS, asm→ELF64 obj) + `ld` (GNU ld, obj + SDK runtime →
     runnable ELF64) in-OS, then the kernel **execs** the result. A C program
     is compiled, assembled, linked and **run** entirely on ICS-OS
     (`GCC_E2E_OK` + `GCC_E2E_RUN_OK`).
   - **gcc driver** — **done**: `test-gccdriver` runs the in-OS `gcc`
      front-end driver (option parsing, `pexecute`/`posix_spawn` of
      cc1/as/ld), so a single `gcc x.c -o x` compiles, assembles, links and
      the driver execs the result entirely on ICS-OS (`GCC_DRIVER_OK` +
      `GCC_DRV_OK` + `GCC_DRV_RUN_OK`).
3. **host-seeded in-OS kernel build** — **done**: `test-kbuild` uses the in-OS GCC driver,
   cc1, GAS, and GNU ld to compile/link ICS-OS, then kexecs the resulting
   kernel (`GKBUILD_TEST_PASS` + `KEXEC_BOOT_OK`).
4. **compiler closure** — **in progress**: `test-selfhost-cert` rebuilds
   ~441 objects (349 cc1 + libcpp/libiberty/libdec/zlib/runtime) inside ICS-OS
   via `contrib/gcc/Selfhost.mk`, uses the rebuilt cc1 to build a new `gcc.exe`,
   compiles `loop.o` with it, rebuilds make, then rebuilds + kexecs the kernel
   with provenance `in-os-rebuilt`; post-kexec must pass SMP, ELF exec, and
   GCC/binutils capability checks. Bounded baseline (2026-09-04, 12-min run):
   boot clean, 22/349 cc1 objects compiled with no blocker, ~31 s/object on the
   heavy `c-family` units, ~2.5–3 h full build. The closure is **serial and
   uniprocessor by design** (stage-1 leaves APs in reset for the kexec);
   `GKBUILD_TIME` phase marks report the compile-time profile, and
    `CERT_TIMEOUT` bounds diagnostic runs.

    **`as` heap OOM (fixed 2026-09-04):** the first full run died assembling
    `insn-attrtab.o` (`sbrk DENIED ... limit=0x3fd00000`). The SDK `malloc`
    did one `sbrk()` per fresh block and `dex32_sbrk` rounds each to a 4 KiB
    page, so ~250k GAS symbol/frag blocks each burned a whole page (~1 GiB)
    for a few-MB object, hitting the 1 GiB user-VA cap. New blocks are now
    carved from a shared page-aligned slab (≥ 64 KiB) in `sdk/tccsdk.c`, so
    small blocks share pages; `test-bintools`/`test-fork`/`test-exec` re-validated.

## Tests

```
cd ics-os
make -C kernel bzImage
make test-make           # MAKE_PASS (in-OS tcc → make.exe → hello.exe)
make test-bintools       # AS_PASS + AR_PASS + LD_PASS + BINTOOLS_PASS
make test-cc1            # CC1_TEST_PASS (in-OS cc1: C -> assembly)
make test-gcc            # GCC_E2E_OK + GCC_E2E_RUN_OK (cc1 -> as -> ld -> exec)
make test-gccdriver      # GCC_DRIVER_OK (in-OS gcc driver: x.c -> x -> exec)
make test-kbuild         # supported GCC kernel self-host + kexec
make test-selfhost-cert  # Round-4 compiler closure (~3 h; CERT_TIMEOUT to bound)
make test-tcc-kbuild     # optional TinyCC kernel experiment
make test-spawn          # SPAWN_PASS + WORK_DISK_PASS
make test-posixio        # still green (unformatted vblk → no /work)
make test-virtio
make test-boot
```
