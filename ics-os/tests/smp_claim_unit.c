/*
 * Host TAP for the SMP claim / current-publish / crit-nest protocol
 * and the IRQ kstack destination predicate.
 * These are the original-cause checks for: two CPUs on one PCB, publishing
 * current without a claim, leavecrit sampling a stale current (idle
 * token on vfs_busy after SDK_EXIT), and resetting a process kstack top
 * from a kernel RSP (cert PF64 rip=0x100000001000 on gcc.exe).
 */
#include <stdio.h>
#include "kernel/process/irq_kstack.h"

static int g_ok = 1;
static int g_n = 0;

static void check(const char *name, int cond)
{
   g_n++;
   if (!cond) {
      printf("not ok %d - %s\n", g_n, name);
      g_ok = 0;
   } else {
      printf("ok %d - %s\n", g_n, name);
   }
}

static int cas_on_cpu(int *on_cpu, int from, int to)
{
   if (*on_cpu != from)
      return 0;
   *on_cpu = to;
   return 1;
}

static int publish_ok(int on_cpu, int me)
{
   return on_cpu == me;
}

int main(void)
{
   int on_cpu;
   int current[8];
   int nest_tok[4];
   int nest_n;
   int i;

   printf("TAP version 13\n");
   printf("1..14\n");

   on_cpu = -1;
   for (i = 0; i < 8; i++)
      current[i] = -1;

   check("first CAS from -1 wins", cas_on_cpu(&on_cpu, -1, 2));
   check("second CAS from -1 loses", !cas_on_cpu(&on_cpu, -1, 0));
   check("loser must not publish current", on_cpu == 2 && current[0] == -1);
   check("winner may publish", publish_ok(on_cpu, 2));
   current[2] = 31;
   /* Release: publish successor first, then drop the claim. */
   current[2] = 0;
   on_cpu = -1;
   check("after release no CPU advertises the task",
         current[0] != 31 && current[2] != 31);

   /* Stale advertisement must not block a CPU that already holds on_cpu. */
   on_cpu = 3;
   current[0] = 31;
   current[3] = 25;
   check("stale current on another CPU does not revoke an existing claim",
         publish_ok(on_cpu, 3));

   /* Crit nest lives on the PCB.  Leave uses the token pushed at enter,
      not a later current, and another process on the same CPU must not
      see the hold (fat_wait_io yield under coop). */
   nest_n = 0;
   nest_tok[nest_n++] = 32; /* pid 31 + 1, captured at enter */
   /* current becomes idle (token 0x7f0004) before leave */
   check("leave uses enter token not the idle current",
         nest_n == 1 && nest_tok[--nest_n] == 32 && nest_n == 0);

   check("store-steal of on_cpu is not a valid claim",
         !cas_on_cpu(&on_cpu, -1, 1) && on_cpu == 3);

   /* irqwrap must not reset process kstack_top except from the user stack.
      A claimed dest whose interrupted RSP is the previous process kheap
      stack is the publish-before-switch window that smashed gcc.exe. */
   check("already on process kstack stays",
         irq_kstack_dest(0x03a10000UL, 0x03a00000UL, 0x03a20000UL, 1)
         == IRQ_KSTACK_STAY);
   check("claimed user stack uses process kstack",
         irq_kstack_dest(0x3FFFE990UL, 0x03a00000UL, 0x03a20000UL, 1)
         == IRQ_KSTACK_PROCESS);
   check("FOREIGN user stack uses CPU stack",
         irq_kstack_dest(0x3FFFE990UL, 0x03a00000UL, 0x03a20000UL, 0)
         == IRQ_KSTACK_CPU);
   check("claimed kernel heap RSP must not reset process kstack top",
         irq_kstack_dest(0x03a15420UL, 0x02b00000UL, 0x02b20000UL, 1)
         == IRQ_KSTACK_STAY);
   check("user stack guard is the low bound",
         irq_kstack_dest(MEM_USER_STACK_GUARD, 0x02b00000UL, 0x02b20000UL, 1)
         == IRQ_KSTACK_PROCESS);
   check("claimed idle BSS stack uses CPU stack not process kstack top",
         irq_kstack_dest(0x2390f0UL, 0x02b00000UL, 0x02b20000UL, 1)
         == IRQ_KSTACK_CPU);

   return g_ok ? 0 : 1;
}
