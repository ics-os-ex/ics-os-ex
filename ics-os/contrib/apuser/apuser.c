/*
 * Parallel user processes on APs (kernel cmdline user-smp).
 *
 * The parent posix_spawns several workers that do SSE-ish float work
 * without yielding, matching seed cc1 after mmap.  APUSER_PASS requires
 * every child to exit 0.  The kernel USER_RUN records show which CPUs
 * actually ran them.
 */
#include <stdio.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>

#define NWORK 3
#define NITERS 4000000

static int worker(void)
{
   volatile float acc;
   int i;

   acc = 1.0f;
   for (i = 0; i < NITERS; i++)
      acc = acc * 1.000001f + 0.0001f;
   printf("APUSER_CHILD_OK acc=%d\n", (int)acc);
   return 0;
}

int main(int argc, char **argv)
{
   const char *self = "/icsos/apps/apuser.exe";
   char *cargv[] = { (char *)self, (char *)"work", 0 };
   pid_t pids[NWORK];
   int i, st, reaped;

   if (argc > 1 && argv[1][0] == 'w')
      return worker();
   if (argc > 1 && argv[1][0] == 'f') {
      volatile int *p = (int *)0x20000000UL;
      *p = 1;
      printf("APUSER_FAIL no page fault\n");
      return 1;
   }

   printf("apuser: spawning %d workers\n", NWORK);
   for (i = 0; i < NWORK; i++) {
      if (posix_spawn(&pids[i], cargv[0], 0, 0, cargv, 0) != 0) {
         printf("apuser: spawn failed errno=%d\n", errno);
         printf("APUSER_FAIL\n");
         return 1;
      }
   }
   reaped = 0;
   for (i = 0; i < NWORK; i++) {
      if (waitpid(pids[i], &st, 0) != pids[i] || st != 0) {
         printf("apuser: wait pid=%d st=%d errno=%d\n",
                (int)pids[i], st, errno);
         printf("APUSER_FAIL\n");
         return 1;
      }
      reaped++;
   }
   printf("apuser: reaped=%d\n", reaped);
   cargv[1] = (char *)"fault";
   {
      pid_t fp;
      if (posix_spawn(&fp, cargv[0], 0, 0, cargv, 0) != 0) {
         printf("apuser: fault spawn errno=%d\n", errno);
         printf("APUSER_FAIL\n");
         return 1;
      }
      if (waitpid(fp, &st, 0) != fp || st == 0) {
         printf("apuser: fault child st=%d errno=%d\n", st, errno);
         printf("APUSER_FAIL\n");
         return 1;
      }
   }
   printf("APUSER_PF_RECOVER_OK\n");
   printf("APUSER_PASS\n");
   return 0;
}
