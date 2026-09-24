/*
  asloop.c - tight-loop reproduction of the non-deterministic in-OS `as` failure.

  Background: the strict GCC self-host cert (test-selfhost-cert) fails
  non-deterministically in the in-OS `as`: a valid .s on disk (the guest
  re-reads identical bytes, and host GNU `as` assembles it cleanly) is reported
  by the guest `as` as having errors at lines that are valid on disk, and the
  failing .s differs run to run. Four synthetic working-set shapes (see
  memcorrupt.c) did NOT reproduce it, so the trigger is specific to running the
  real `as` repeatedly.

  This driver seeds a single fixed valid .s on /work and runs the real in-OS
  `as` on it in a tight loop (N_ITER iterations). Each iteration is a fresh
  `as` process, so a failure at iteration N>1 means the kernel corrupted the
  `as` user address space (or its read path) cumulatively, NOT that the input
  line is bad. Iteration 1 is the baseline: if it fails, the .s is invalid and
  the loop is meaningless.

  Markers (serial):
    ASLOOP_BEGIN niter=<N>
    ASLOOP_SRC sz=<bytes>          (input present and non-trivial)
    ASLOOP_BASELINE_OK             (first run assembled a valid ELF)
    ASLOOP_OK iter=<N>             (periodic progress)
    ASLOOP_FAIL iter=<N> status=<s>  (reproduction: `as` exited non-zero)
    ASLOOP_FAIL obj iter=<N>        (reproduction: `as` exit 0 but no valid ELF)
    ASLOOP_PASS niter=<N>           (no failure in N runs)

  The `as` stderr (the real error text) is redirected to /work/ASLOOP.err for
  the failing iteration so it can be read back from the work image.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>

#define N_ITER_DEFAULT 400
#define ASM_BIN   "/icsos/apps/as.exe"
#define ASM_SRC   "/work/ASLOOP.s"
#define ASM_OBJ   "/work/ASLOOP.o"
#define ASM_ERR   "/work/ASLOOP.err"

/* Verify `path` starts with the ELF magic. The child's final flush can land
   just after waitpid returns, so retry briefly until the header is stable. */
static int verify_elf(const char *path)
{
   int fd, n, i, try;
   char buf[8];
   for (try = 0; try < 30; try++) {
      fd = open(path, O_RDONLY);
      if (fd < 0) { for (i = 0; i < 20000; i++) { } continue; }
      lseek(fd, 0, SEEK_SET);
      n = (int)read(fd, buf, sizeof(buf));
      close(fd);
      if (n >= 4 && buf[0] == (char)0x7f && buf[1] == 'E' &&
          buf[2] == 'L' && buf[3] == 'F')
         return 1;
      for (i = 0; i < 20000; i++) { }
   }
   return 0;
}

static int run_as(int iter)
{
   static char *argv[] = { (char *)ASM_BIN, (char *)"--64", (char *)ASM_SRC,
                           (char *)"-o", (char *)ASM_OBJ, 0 };
   pid_t pid = 0;
   int st = 0, r;
   (void)iter;
   r = posix_spawn(&pid, ASM_BIN, 0, 0, argv, 0);
   if (r != 0) {
      printf("ASLOOP_FAIL spawn iter=%d r=%d\n", iter, r);
      return -1;
   }
   r = waitpid(pid, &st, 0);
   if (r != (int)pid) {
      printf("ASLOOP_FAIL waitpid iter=%d r=%d\n", iter, r);
      return -1;
   }
   if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
      return WEXITSTATUS(st);
   }
   return 0;
}

int main(int argc, char **argv)
{
   int niter = N_ITER_DEFAULT, iter, fd, st;
   long sz;
   (void)argc;
   if (argc > 1) {
      long v = atol(argv[1]);
      if (v >= 1 && v <= 100000) niter = (int)v;
   }

   printf("ASLOOP_BEGIN niter=%d\n", niter);
   fflush(stdout);

   fd = open(ASM_SRC, O_RDONLY);
   if (fd < 0) { printf("ASLOOP_FAIL missing %s\n", ASM_SRC); return 1; }
   sz = lseek(fd, 0, SEEK_END);
   close(fd);
   printf("ASLOOP_SRC sz=%ld\n", sz);
   if (sz < 1000) { printf("ASLOOP_FAIL src too small sz=%ld\n", sz); return 1; }

   for (iter = 1; iter <= niter; iter++) {
      st = run_as(iter);
      if (st != 0) {
         /* Reproduction. Capture the `as` error text to the work disk: rerun
            once more with stderr pointed at ASM_ERR so the real diagnostic
            (not the serial-mangled stream) survives for readback. */
         int efd, saved_out, saved_err;
          char *eargv[] = { (char *)ASM_BIN, (char *)"--64", (char *)ASM_SRC,
                            (char *)"-o", (char *)ASM_OBJ, 0 };
          pid_t epid = 0;
          int est = 0;
          printf("ASLOOP_FAIL iter=%d status=%d\n", iter, st);
          /* The `as` emits its diagnostics via the SDK printf, which lands on
             stdout (fd 1), not stderr -- serial batching then mangles/drops the
             message text after "Error:". Capture BOTH fd 1 and fd 2 (same
             technique as gccdriver.c) so the real diagnostic survives readback. */
          efd = open(ASM_ERR, O_WRONLY | O_CREAT | O_TRUNC, 0666);
          if (efd >= 0) {
             saved_out = dup(1);
             saved_err = dup(2);
             dup2(efd, 1);
             dup2(efd, 2);
             close(efd);
             if (posix_spawn(&epid, ASM_BIN, 0, 0, eargv, 0) == 0) {
                (void)waitpid(epid, &est, 0);
             }
             if (saved_out >= 0) { dup2(saved_out, 1); close(saved_out); }
             if (saved_err >= 0) { dup2(saved_err, 2); close(saved_err); }
          }
         printf("ASLOOP_DONE fail_iter=%d status=%d rerun_status=%d\n",
                iter, st, est);
         return 1;
      }
      if (!verify_elf(ASM_OBJ)) {
         printf("ASLOOP_FAIL obj iter=%d\n", iter);
         return 1;
      }
      if (iter == 1) { printf("ASLOOP_BASELINE_OK\n"); fflush(stdout); }
      if (iter % 25 == 0) { printf("ASLOOP_OK iter=%d\n", iter); fflush(stdout); }
   }
   printf("ASLOOP_PASS niter=%d\n", niter);
   return 0;
}
