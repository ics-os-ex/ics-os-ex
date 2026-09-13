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

#endif
