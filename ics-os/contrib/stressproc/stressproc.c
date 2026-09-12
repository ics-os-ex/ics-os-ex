/*
 * stressproc.c - in-OS process spawn/exit/reap stress reproducer.
 *
 * Sequential posix_spawn/wait of hello.exe matches make's job server
 * spawning one compiler at a time from a single parent.  A short fork
 * overlap phase also runs two ELF stream-loads at once (make -jN).
 *
 * Prints STRESSPROC_PASS when it survives all rounds without a kernel fault.
 */
#include <stdio.h>
#include <string.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>

#define BURST           8
#define ROUNDS          40
#define OVERLAP_BURST   2
#define OVERLAP_ROUNDS  4

static int fail(const char *tag, const char *msg)
{
   printf("stressproc: FAIL %s %s errno=%d\n", tag, msg, errno);
   printf("STRESSPROC_FAIL\n");
   return 1;
}

static int spawn_hello(pid_t *out)
{
   const char *child = "/icsos/apps/hello.exe";
   char *cargv[] = { (char *)child, 0 };
   return posix_spawn(out, cargv[0], 0, 0, cargv, 0);
}

int main(int argc, char **argv)
{
   int round, i, spawned, reaped, st;
   pid_t pids[BURST];
   unsigned long long total = 0;

   (void)argc;
   (void)argv;
   printf("stressproc: begin overlap=%dx%d then seq rounds=%d burst=%d\n",
          OVERLAP_ROUNDS, OVERLAP_BURST, ROUNDS, BURST);

   for (round = 0; round < OVERLAP_ROUNDS; round++) {
      spawned = 0;
      reaped = 0;
      for (i = 0; i < OVERLAP_BURST; i++) {
         pid_t p = fork();
         if (p < 0)
            return fail("fork", "fork");
         if (p == 0) {
            pid_t c;
            int cst;
            if (spawn_hello(&c) != 0)
               _exit(1);
            if (waitpid(c, &cst, 0) != c)
               _exit(1);
            _exit(0);
         }
         pids[i] = p;
         spawned++;
      }
      for (i = 0; i < OVERLAP_BURST; i++) {
         if (waitpid(pids[i], &st, 0) != pids[i])
            return fail("wait", "overlap helper");
         if (st != 0)
            return fail("child", "overlap helper status");
         reaped++;
      }
      total += reaped;
      printf("stressproc: overlap round=%d spawned=%d reaped=%d\n",
             round, spawned, reaped);
   }

   for (round = 0; round < ROUNDS; round++) {
      spawned = 0;
      reaped = 0;
      for (i = 0; i < BURST; i++) {
         if (spawn_hello(&pids[i]) != 0)
            return fail("spawn", "posix_spawn");
         spawned++;
      }
      for (i = BURST - 1; i >= 0; i--) {
         if (waitpid(pids[i], &st, 0) != pids[i])
            return fail("wait", "waitpid");
         reaped++;
      }
      total += reaped;
      if ((round % 10) == 0)
         printf("stressproc: round=%d spawned=%d reaped=%d total=%llu\n",
                round, spawned, reaped, total);
   }
   printf("stressproc: done total=%llu\n", total);
   printf("STRESSPROC_PASS\n");
   return 0;
}
