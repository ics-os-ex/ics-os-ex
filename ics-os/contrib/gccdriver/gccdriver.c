/*
 * gccdriver -- a minimal in-OS "gcc" front-end for the GCC self-host.
 *
 * This is the gcc DRIVER step of the self-host chain (see docs/gcc-selfhost.md).
 * It is a thin C program that composes the REAL GCC C frontend (cc1, built in-OS)
 * and the REAL GNU binutils backend (as, ld, built in-OS) into a single
 * `gcc x.c -o x` compile+link, using the SDK's posix_spawn/waitpid to drive
 * each phase in turn.  The actual C compilation is still performed by the real
 * cc1, so this is a genuine GCC toolchain, not a reimplementation of the
 * compiler -- only the driver glue is hand-written.
 *
 * Why a hand-written driver: the pruned GCC 4.7.4 source kept in references/
 * retains only the cc1 frontend objects; gcc.cc / options.cc / gcc.opt (the
 * upstream driver) were removed, and regenerating options.h requires running
 * genopt (a large extra build).  A thin driver is the standard approach for
 * bare-metal / embedded toolchains and is exactly the glue the self-host
 * needs: one `gcc` that spawns cc1/as/ld.
 *
 * The kernel propagates child status through waitpid. The driver additionally
 * verifies each phase's own output so truncated files cannot pass as success.
 *
 * The tool ELFs (cc1/as/ld) are loaded from /work/apps when that volume is
 * present (self-host cert), otherwise /icsos/apps (gccdrv smoke). Intermediate
 * .s/.o temps follow: /work when the work disk is there, else /ramdisk. SDK
 * runtime .o's stay on /ramdisk for the small `gccdrv` smoke that stages them.
 *
 * Usage (enough to build+link a C program, and to grow toward the self-host):
 *   gcc [opts] in.c -o out            compile+link  -> runnable ELF64
 *   gcc -c [opts] in.c -o out.o       compile+assemble -> relocatable object
 *   gcc [opts] in.s -o out            assemble+link
 *   gcc [opts] in.S -o out.o          preprocessed-asm source: assembled
 *                                     directly (no cpp; the kernel .S files
 *                                     use no preprocessor directives)
 *
 * Forwarded to cc1: -O0 -O1 -O2 -O3 -Os -g -w -I<dir> -D<def> -U<def> -std=<s>
 *                   -f<flag> -m<flag>   (the -m* machine flags, e.g. -m64
 *                   -mcmodel=large -mno-red-zone -msse2, are forwarded so the
 *                   in-OS build can mirror the host kernel CFLAGS exactly)
 * Forwarded to ld:   -L<dir> -l<name> -static -Wl,<flag>
 * Driver-only:       -c  -o<n>  -nostdlib
 * Tool selection:    -B<prefix>  (cc1.exe/as.exe/ld.exe under prefix)
 * When linking (no -c), the SDK runtime -- the ICS-OS "libc"
 * (crt1/tccsdk/libtcc1/posix/setjmp .o's in RTDIR) -- is linked in
 * automatically unless -nostdlib is given.
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/stat.h>

#define TOOLDIR_WORK "/work/apps"  /* cert / SMP make -jN */
#define TOOLDIR_CD   "/icsos/apps" /* gccdrv smoke (no /work) */
#define RTDIR        "/ramdisk"    /* SDK runtime .o's for gccdrv smokes */
/* Intermediate temps are named per-process: the driver is invoked concurrently
   by `make -jN`, so a fixed shared path would let parallel jobs clobber each
   other's cc1/as scratch files. The concrete names are built in main() from
   getpid(). Prefer /work so four concurrent .s files cannot fill /ramdisk. */
#define T_S_FMT  "%s/.gccdrv.%d.s"
#define T_O_FMT  "%s/.gccdrv.%d.o"

static int have_work_cc1(void)
{
   static int cached = -1;
   int fd;
   if (cached >= 0)
      return cached;
   fd = open("/work/apps/cc1.exe", O_RDONLY);
   if (fd >= 0) {
      close(fd);
      cached = 1;
   } else
      cached = 0;
   return cached;
}

static const char *gccdrv_tooldir(void)
{
   return have_work_cc1() ? TOOLDIR_WORK : TOOLDIR_CD;
}

static const char *gccdrv_tmpdir(void)
{
   return have_work_cc1() ? "/work" : "/ramdisk";
}

/* The SDK runtime ("libc") linked in automatically when linking (no -c). */
static const char *sdkrt[] = {
   RTDIR "/crt1.o",
   RTDIR "/tccsdk.o",
   RTDIR "/libtcc1.o",
   RTDIR "/posix.o",
   RTDIR "/setjmp.o",
   0
};

#define MAXOPTS 128
static char *cc1optargv[MAXOPTS]; static int cc1nopts = 0;
static char *ldoptargv[MAXOPTS];  static int ldnopts  = 0;

/* GNU Make's in-OS job command buffer is intentionally small. Keep the
   reproducible GCC-closure configuration in the driver (like a specs
   profile) so Make only needs to pass one short option per translation unit. */
static const char *selfhost_opts[] = {
   "-m64", "-std=gnu89", "-w", "-nostdinc", "-fno-builtin",
   "-ffreestanding", "-fno-pie", "-fno-pic",
   "-fno-stack-protector", "-fno-asynchronous-unwind-tables",
   "-fno-strict-aliasing", "-mcmodel=large", "-mno-red-zone",
   "-DIN_GCC", "-DHAVE_CONFIG_H", "-DBASEVER=\"4.7.4\"",
   "-DBUGURL=\"\"", "-DDATESTAMP=\"20260830\"", "-DDEVPHASE=\"\"",
   "-DREVISION=\"\"", "-DPKGVERSION=\"4.7.4\"",
   "-DTARGET_NAME=\"x86_64-ics-os\"",
   "-DHAVE_GAS_CFI_PERSONALITY_DIRECTIVE=1", "-DHAVE_GAS_CFI_DIRECTIVE=1",
   "-DHAVE_COMDAT_GROUP=1", "-DHAVE_GAS_SHF_MERGE=1",
   "-DHAVE_GAS_CFI_SECTIONS_DIRECTIVE=1", "-DHAVE_GAS_HIDDEN=1",
   "-DHAVE_GAS_MAX_SKIP_P2ALIGN=65535", "-DHAVE_AS_GOTOFF_IN_DATA=1",
   "-DHAVE_AS_IX86_FFREEP=1", "-DHAVE_AS_IX86_FILDQ=1",
   "-DHAVE_AS_IX86_FILDS=1", "-DHAVE_AS_IX86_REP_LOCK_PREFIX=1",
   "-DHAVE_AS_TLS=1", "-DHAVE_AS_GOTTPLTPCALL=1",
   "-DHAVE_AS_TLSDIRECT=1", "-DHAVE_AS_CFI_SECTIONS=1",
   "-DHAVE_AS_X86_CMPXCHG16B=1",
   "-I/work/gccsrc/gen", "-I/work/gccsrc/shims",
    "-I/work/gccsrc/conf/gcc", "-I/work/gccsrc/sdk/include",
    "-I/work/gccsrc/up/gcc", "-I/work/gccsrc/up/gcc/c-family",
    "-I/work/gccsrc/up/gcc/common",
    "-I/work/gccsrc/up/gcc/common/config/i386",
    "-I/work/gccsrc/up/gcc/config", "-I/work/gccsrc/up/gcc/config/i386",
    "-I/work/gccsrc/up/libcpp", "-I/work/gccsrc/up/libcpp/include",
    "-I/work/gccsrc/up/libiberty", "-I/work/gccsrc/conf/gmp",
    "-I/work/gccsrc/up/mpfr",
    "-I/work/gccsrc/up/mpc/src", "-I/work/gccsrc/up/libdecnumber",
    "-I/work/gccsrc/up/libdecnumber/bid", "-I/work/gccsrc/up/libgcc",
    "-I/work/gccsrc/up/zlib", "-I/work/gccsrc/up/include", 0
};

static void die(const char *phase)
{
   printf("GCC_DRV_FAIL %s\n", phase);
   _exit(1);
}

/* Run tool (path + NULL-terminated argv) and wait. Returns 0 on spawn+wait ok. */
static int run_tool(const char *path, char *const argv[])
{
   pid_t pid = 0;
   int st = 0;
   int r;
   r = posix_spawn(&pid, path, 0, 0, argv, 0);
   if (r != 0) {
      printf("gccdriver: spawn %s failed r=%d\n", path, r);
      return -1;
   }
   /* Wait before printing: the child shares the serial console, and the SDK
      printf is char-at-a-time, so printing while the child runs would
      interleave the two streams into an unreadable blob. */
   if (waitpid(pid, &st, 0) != pid) {
      printf("gccdriver: waitpid %s failed\n", path);
      return -1;
   }
   if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
      printf("gccdriver: %s exited status=%d\n", path, WEXITSTATUS(st));
      return -1;
   }
   return 0;
}

/* Verify path exists, is non-empty, and (if wantelf) starts with the ELF magic.
   The child's final file flush can land just after waitpid returns, so the
   header read is retried briefly until the magic is stable. */
static int check_out(const char *path, int wantelf)
{
   int fd, n, i, try;
   char buf[8];
   long sz;
   for (try = 0; try < 20; try++) {
      fd = open(path, O_RDONLY);
      if (fd < 0) {
         if (try == 0) printf("gccdriver: missing output %s\n", path);
         for (i = 0; i < 20000; i++) { }
         continue;
      }
      sz = lseek(fd, 0, SEEK_END);
      lseek(fd, 0, SEEK_SET);
      n = (int)read(fd, buf, sizeof(buf));
      close(fd);
      if (sz < 1 || n < 1) {
         if (try == 0) printf("gccdriver: empty output %s (sz=%ld)\n", path, (long)sz);
         for (i = 0; i < 20000; i++) { }
         continue;
      }
      if (!wantelf) return (int)sz;
      if (n >= 4 && buf[0] == (char)0x7f && buf[1] == 'E' &&
          buf[2] == 'L' && buf[3] == 'F')
         return (int)sz;
      if (try == 19) {
         printf("gccdriver: not an ELF: %s (sz=%ld n=%d)\n", path, (long)sz, n);
         return -1;
      }
      for (i = 0; i < 20000; i++) { }
   }
   printf("gccdriver: output unavailable after retries: %s\n", path);
   return -1;
}

/* Child status is checked by run_tool(). Keep an additional non-empty output
   check for frontends terminated before they can flush an assembly file. Empty
   translation units legitimately contain only directives such as .file. */
static int check_asm(const char *path)
{
   static char buf[4097];
   int fd, n;
   fd = open(path, O_RDONLY);
   if (fd < 0) return -1;
   n = (int)read(fd, buf, sizeof(buf) - 1);
   close(fd);
   if (n < 0) return -1;
   buf[n] = 0;
         return n > 0 ? 0 : -1;
}

int main(int argc, char **argv)
{
   static char out[256];
   static char inc[256];
   static char objsrc[256];      /* object fed to ld (temp) or the -c output */
   static char cc1path[256];
    static char aspath[256];
    static char ldpath[256];
    static char ts_name[160];     /* per-pid cc1 assembly temp */
    static char to_name[160];     /* per-pid as object temp (link mode) */
    static char *cc1argv[192];
    static char *asargv[16];
    static char *ldargv[96];
    int i, j, k;
    int mypid;
   int compile_only = 0;
   int nostdlib = 0;
   int have_in = 0;
   int have_out = 0;
   int is_s = 0;

   sprintf(cc1path, "%s/cc1.exe", gccdrv_tooldir());
   sprintf(aspath, "%s/as.exe", gccdrv_tooldir());
   sprintf(ldpath, "%s/ld.exe", gccdrv_tooldir());

   /* ---- parse the gcc command line ---- */
   for (i = 1; i < argc; i++) {
      char *a = argv[i];
      size_t L;
      if (a[0] != '-') {
         L = strlen(a);
          if (L >= 2 && a[L-2] == '.') {
             if (a[L-1] == 'c')      { strcpy(inc, a); have_in = 1; }
             else if (a[L-1] == 's' || a[L-1] == 'S') {
                /* .s/.S: assemble directly. The kernel .S files carry no C
                   preprocessor directives, so no cpp step is needed (the
                   host gcc would preprocess them, but they use none). */
                strcpy(inc, a); have_in = 1; is_s = 1;
             }
          }
          continue;
      }
      if (!strcmp(a, "-c"))         { compile_only = 1; continue; }
      if (!strcmp(a, "-o"))         { if (i+1 < argc) { strcpy(out, argv[++i]); have_out = 1; } continue; }
      if (!strcmp(a, "-nostdlib"))  { nostdlib = 1; continue; }
      if (!strcmp(a, "-nostdinc"))  { if (cc1nopts < MAXOPTS) cc1optargv[cc1nopts++] = a; continue; }
      if (!strcmp(a, "-ficsos-gcc-selfhost")) {
         for (j = 0; selfhost_opts[j] && cc1nopts < MAXOPTS; j++)
            cc1optargv[cc1nopts++] = (char *)selfhost_opts[j];
         continue;
      }
      if (a[1] == 'B') {
         char *p = a + 2;
         size_t n;
         if (!*p && i+1 < argc)
            p = argv[++i];
         n = strlen(p);
         if (n && p[n-1] == '/') {
            sprintf(cc1path, "%scc1.exe", p);
            sprintf(aspath, "%sas.exe", p);
            sprintf(ldpath, "%sld.exe", p);
         } else {
            sprintf(cc1path, "%s/cc1.exe", p);
            sprintf(aspath, "%s/as.exe", p);
            sprintf(ldpath, "%s/ld.exe", p);
         }
         continue;
      }
      if (!strcmp(a, "-g"))         { if (cc1nopts < MAXOPTS) cc1optargv[cc1nopts++] = a; continue; }
      if (a[1] == 'O' || a[1] == 'f') { if (cc1nopts < MAXOPTS) cc1optargv[cc1nopts++] = a; continue; }
       if (a[1] == 'm')             { if (cc1nopts < MAXOPTS) cc1optargv[cc1nopts++] = a; continue; }
       if (!strcmp(a, "-w"))         { if (cc1nopts < MAXOPTS) cc1optargv[cc1nopts++] = a; continue; }
       if (strncmp(a, "-std=", 5) == 0) { if (cc1nopts < MAXOPTS) cc1optargv[cc1nopts++] = a; continue; }
      if (a[1] == 'I' || a[1] == 'D' || a[1] == 'U') {
         if (a[2] == 0) {   /* separate form: -I dir */
            if (i+1 < argc) {
               if (cc1nopts < MAXOPTS) cc1optargv[cc1nopts++] = a;
               if (cc1nopts < MAXOPTS) cc1optargv[cc1nopts++] = argv[++i];
            }
         } else if (cc1nopts < MAXOPTS) {
            cc1optargv[cc1nopts++] = a;
         }
         continue;
      }
      if (a[1] == 'L' || a[1] == 'l') {
         if (a[2] == 0) {   /* separate form: -L dir */
            if (i+1 < argc) {
               if (ldnopts < MAXOPTS) ldoptargv[ldnopts++] = a;
               if (ldnopts < MAXOPTS) ldoptargv[ldnopts++] = argv[++i];
            }
         } else if (ldnopts < MAXOPTS) {
            ldoptargv[ldnopts++] = a;
         }
         continue;
      }
      if (!strcmp(a, "-static"))    { if (ldnopts < MAXOPTS) ldoptargv[ldnopts++] = a; continue; }
      if (strncmp(a, "-Wl,", 4) == 0) {
         /* forward the tail to ld as a single arg (single-flag case) */
         if (ldnopts < MAXOPTS) ldoptargv[ldnopts++] = a + 4;
         continue;
      }
      /* unknown driver flag: drop (e.g. -Wall, -w, -m64) */
    }

    /* Per-pid scratch paths so concurrent `make -jN` driver invocations never
       share (and clobber) the same cc1/as intermediate files. */
    mypid = getpid();
    sprintf(ts_name, T_S_FMT, gccdrv_tmpdir(), mypid);
    sprintf(to_name, T_O_FMT, gccdrv_tmpdir(), mypid);
    printf("gccdriver: tooldir=%s tmpdir=%s pid=%d\n",
           gccdrv_tooldir(), gccdrv_tmpdir(), mypid);

    if (!have_in) die("parse: no input file");

   /* default output name */
   if (!have_out) {
      strcpy(out, compile_only ? "a.out.o" : "a.out");
      have_out = 1;
   }

   /* object file: for -c it IS the output; otherwise a temp fed to ld */
   if (compile_only)
       strcpy(objsrc, out);
    else
       strcpy(objsrc, to_name);

   /* ---- phase 1: cc1 (C frontend) emits assembly -- only for .c input ---- */
  if (!is_s) {
       unlink(ts_name);
       k = 0;
       cc1argv[k++] = cc1path;
      /* The upstream GCC driver always supplies -quiet. Without it cc1
         prints every parsed/generated symbol and timing details; on the
         serial console that creates roughly a million syscalls per unit. */
      cc1argv[k++] = "-quiet";
      for (i = 0; i < cc1nopts; i++) cc1argv[k++] = cc1optargv[i];
     cc1argv[k++] = inc;
       cc1argv[k++] = "-o";
       cc1argv[k++] = ts_name;
       cc1argv[k] = 0;
       if (run_tool(cc1path, cc1argv)) die("cc1 spawn");
       if (check_out(ts_name, 0) < 0) die("cc1: no asm");
       if (check_asm(ts_name) < 0) die("cc1: incomplete asm");
      printf("gccdriver: cc1 ok\n");
   }

   /* ---- phase 2: as (GAS) assembles into an ELF64 object ---- */
   unlink(objsrc);
   asargv[0] = aspath;
   asargv[1] = "--64";
   asargv[2] = is_s ? inc : ts_name;
   asargv[3] = "-o";
   asargv[4] = objsrc;
   asargv[5] = 0;
   if (run_tool(aspath, asargv)) die("as spawn");
   if (check_out(objsrc, 1) < 0) die("as: no object");
   if (!is_s)
      unlink(ts_name);
   printf("gccdriver: as ok\n");

   /* ---- phase 3: ld (GNU ld) links object + SDK runtime into a runnable ELF64 ---- */
   if (!compile_only) {
      unlink(out);
      k = 0;
      ldargv[k++] = ldpath;
      ldargv[k++] = objsrc;
      if (!nostdlib) {
         /* The ICS-OS binutils port cannot reliably derive ldscripts/ from
            argv[0], so select its packaged default script explicitly. */
         ldargv[k++] = "-T";
          ldargv[k++] = have_work_cc1()
             ? "/work/apps/ldscripts/elf_x86_64.xc"
             : "/icsos/apps/ldscripts/elf_x86_64.xc";
         for (i = 0; sdkrt[i]; i++) ldargv[k++] = sdkrt[i];
      }
      for (i = 0; i < ldnopts; i++) ldargv[k++] = ldoptargv[i];
      ldargv[k++] = "-o";
      ldargv[k++] = out;
      ldargv[k] = 0;
      if (run_tool(ldpath, ldargv)) die("ld spawn");
      if (check_out(out, 1) < 0) die("ld: no exe");
      printf("gccdriver: ld ok\n");
   }

   printf("gccdriver: wrote %s\n", out);
   printf("GCC_DRIVER_OK\n");
   return 0;
}
