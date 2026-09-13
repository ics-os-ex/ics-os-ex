#ifndef ICSOS_IRQ_KSTACK_H
#define ICSOS_IRQ_KSTACK_H

#include "../memory/memlayout.h"

/*
 * IRQ_KSTACK_ENTER destination.  irqwrap.S implements the same tests;
 * keep the immediates in sync (MEM_USER_STACK_GUARD / MEM_USER_STACK).
 *
 * STAY:     already on this process kstack, or on some other kernel
 *           stack (publish-before-switch / leftover current).  Do not
 *           move RSP.  Resetting kstack_top from a kernel RSP smashes
 *           the saved syscall iretq frame (cert PF64 rip=0x100000001000).
 *           Yanking mid-C onto MEM_CPUIRQ shifts the iretq (UD64 rip=0x217).
 * PROCESS:  first entry from the user stack of a claimed (or leftover
 *           unclaimed-but-executing) user; switch to kstack_top.
 * CPU:      FOREIGN user (another CPU holds on_cpu) still on the user
 *           stack.  Use MEM_CPUIRQ; never the other CPU's process kstack.
 */
#define IRQ_KSTACK_STAY     0
#define IRQ_KSTACK_PROCESS  1
#define IRQ_KSTACK_CPU      2

static inline int irq_kstack_dest(unsigned long rsp,
                                  unsigned long kbase,
                                  unsigned long ktop,
                                  int claimed)
{
   if (kbase && ktop && rsp >= kbase && rsp < ktop)
      return IRQ_KSTACK_STAY;
   if (rsp >= MEM_USER_STACK_GUARD && rsp < MEM_USER_STACK)
      return claimed ? IRQ_KSTACK_PROCESS : IRQ_KSTACK_CPU;
   /* Leftover current often interrupts the idle BSS stack.  Staying there
      overflows 8KiB; resetting process kstack_top from there smashes a
      yielded syscall (cert PF64 / task_mgr halt).  Park those on MEM_CPUIRQ.
      A kheap RSP is publish-before-switch or another process kstack: stay. */
   if (rsp < MEM_KERNEL_LIMIT)
      return IRQ_KSTACK_CPU;
   return IRQ_KSTACK_STAY;
}

/*
 * Leftover current: this CPU still advertises a USER PCB it is not
 * executing.  IRQ C, crit tokens, and coop schedule_from_timer then treat
 * that PCB as running (cert smash / make.exe killed / ACCESS_SYS halt).
 *
 * Repair (drop THIS CPU's advertisement only; never steal on_cpu):
 *   - leftover FOREIGN current (on_cpu is another CPU): KSTACK-SHARED
 *     pid=40 on cert, two slots advertising one cc1,
 *   - unclaimed USER (on_cpu < 0) whose CR3 has already left.
 * Do not repair:
 *   - mid ps_switchto / context_load (publish-before-stack-switch),
 *   - a live claim (on_cpu == me): dest is published before CR3/RSP
 *     switch; stealing it GPF'd in ps_switchto (cert rip=0x13c0bc),
 *   - released user still on its CR3 (release-before-switch).
 * A false claim (on_cpu == me while executing idle) is dropped only
 * by smp_cpu_idle, not from IRQ C.
 */
static inline int leftover_current_should_repair(int on_cpu, int me,
                                                 int access_user,
                                                 unsigned long pcb_cr3,
                                                 unsigned long hw_cr3,
                                                 int mid_switch)
{
   unsigned long pcb = pcb_cr3 & ~0xFFFUL;
   unsigned long hw = hw_cr3 & ~0xFFFUL;

   if (mid_switch || !access_user)
      return 0;
   if (on_cpu == me)
      return 0;
   /* FOREIGN leftover: drop the advertisement only after CR3 has left.
      Setting current=idle while still on that user CR3/stack left the
      next IRQ on the user stack (ACCESS_SYS stay) and GPF'd in idle
      (cert rip=0x12b943 frsp=0x3fffac10 proc=cpu_idle). */
   if (pcb && pcb == hw)
      return 0;
   return 1;
}

#endif
