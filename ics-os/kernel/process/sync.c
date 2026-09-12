/*
  Name: sync.c
  Copyright: 
  Author: Joseph Emmanuel DL Dayo
  Date: 18/01/04 06:27
  Description: Provides process synchornization functions
*/

#include "sync.h"
#include "../cpu/smp.h"
#include "../memory/memlayout.h"
#include "process.h"

extern void taskswitch(void);
extern void serial_puts(const char *s);
extern int sprintf(char *str, const char *fmt, ...);

sync_sharedvar elf_map_crit;

/* Crit objects live in kernel BSS or the kernel heap (file_PCB.io_busy).
   A user-stack pointer here is saved-context / fd-table corruption; walking
   it as sync_sharedvar (old DIAGLOCK) wedges the timer path. */
static int sync_var_ok(const sync_sharedvar *var)
{
    uintptr p = (uintptr)var;
    if (p >= MEM_KERNEL_LOAD && p < MEM_KERNEL_LIMIT)
        return 1;
    if (p >= MEM_KHEAP_BASE && p < MEM_KHEAP_END)
        return 1;
    return 0;
}

static void sync_bad_var(const char *op, const sync_sharedvar *var)
{
    char b[96];
    sprintf(b, "SYNCBAD %s crit=0x%lx\n", op, (unsigned long)(uintptr)var);
    serial_puts(b);
}

static int sync_owner_token(void)
{
    /* A process is scheduled on at most one CPU at a time, so the owner token
       must identify the process only. Including the CPU made a crit released
       after the owning process migrated to another CPU look like a
       non-owner release and left the crit locked forever. */
    return (int)((getprocessid() & 0x007FFFFF) + 1);
}

static void sync_track_hold(sync_sharedvar *var)
{
    PCB386 *p = current_process;
    int i;

    if (!p)
       return;
    for (i = 0; i < p->held_crit_n; i++)
        if (p->held_crits[i] == var)
           return;
     if (p->held_crit_n < 16)
        p->held_crits[p->held_crit_n++] = var;
     else {
        /* DECISIVE probe: the 16-slot cap is being hit and this crit is
           SILENTLY DROPPED from tracking.  A dropped crit is never released by
           sync_release_process_crits / sync_untrack_hold, so its busy field is
           orphaned forever -> the io_devlock deadlock.  Report it (once per
           overflow event, throttled) so we can confirm the root cause. */
        static volatile int overflow_reported = 0;
        if (!overflow_reported) {
           overflow_reported = 1;
           printf("TRACKOVERFLOW pid=%d crit=0x%lx held_crit_n=%d (17th crit DROPPED -> orphaned)\n",
                  (int)p->processid, (unsigned long)(uintptr)var, p->held_crit_n);
        }
     }
}

static void sync_untrack_hold(sync_sharedvar *var)
{
    PCB386 *p = current_process;
    int i;

    if (!p)
       return;
    for (i = 0; i < p->held_crit_n; i++) {
       if (p->held_crits[i] == var) {
          p->held_crits[i] = p->held_crits[--p->held_crit_n];
          return;
       }
    }
}

void sync_release_process_crits(void *pcb, int owner)
{
    PCB386 *p = (PCB386 *)pcb;
    int i;

    if (!p)
        return;
    /* A dying process's crits are released from two places at once: the
       external killer (dex32_killprocess, victim already off-CPU) and the
       victim's own self-exit path. Both race on the same held_crits array.
       Without this latch the second pass can iterate a half-torn array and
       either double-free a crit or tear it out from under a process that is
       still mid-critical-section, leaking devmgr_busy / io_devlock and
       deadlocking every later acquire. The first releaser wins; the rest are
       no-ops. */
    if (__sync_lock_test_and_set(&p->crits_freed, 1))
        return;
    for (i = 0; i < p->held_crit_n; i++) {
       sync_sharedvar *v = p->held_crits[i];
       if (v && __sync_val_compare_and_swap(&v->busy, 0, 0) == owner) {
          v->wait = 0;
          __sync_lock_release(&v->busy);
       }
    }
    p->held_crit_n = 0;
}


//perform busy waiting
void sync_justwait(sync_sharedvar *var){
   int owner=sync_owner_token();
   int held;
   if (!sync_var_ok(var))
      return;
   do {
      held=__sync_val_compare_and_swap(&var->busy,0,0);
      if (held && held!=owner)
         __asm__ __volatile__("pause");
   } while (held && held!=owner);
};

//attempt to enter the critical section
void sync_entercrit(sync_sharedvar *var){
    int owner=sync_owner_token();
    unsigned long spins=0;
    if (!sync_var_ok(var)) {
       sync_bad_var("enter", var);
       return;
    }
    /* If we already hold other crits we are in a NESTED acquire.  Yielding here
       (taskswitch) would deschedule us with the outer crits still held in their
       busy fields, so any inner-crit owner that later needs one of those outer
       crits spins forever while we sit in the run queue: a classic io_devlock /
       devmgr_busy deadlock under SMP.  Nested acquires must therefore pure-spin
       (no yield).  That is safe because the timer IRQ still preempts the spin,
       so the inner-crit owner is not starved; we simply cannot sleep holding an
       outer lock. */
    int nested = (current_process && current_process->held_crit_n > 0);

    if (__sync_val_compare_and_swap(&var->busy,0,0)==owner) {
         __sync_add_and_fetch(&var->wait, 1);
         return;
     }

     /* We are about to WAIT for a crit we do not hold.  Mark it so the
        no-preempt guard (scheduler.c) does not pin us to the CPU: a nested
        spinner still holds its OUTER crits (held_crit_n>0), and if the guard
        pinned us, the lock's real owner -- which is runnable in the ready
        queue -- would be starved forever (the SMP io_devlock deadlock).  A
        holder doing real work under a crit never sets crit_wait and is still
        pinned, so mid-critical-section preemption protection is preserved. */
     if (current_process)
         current_process->crit_wait = 1;

     while (!__sync_bool_compare_and_swap(&var->busy,0,owner)) {
          __asm__ __volatile__("pause");
          ++spins;
          /* A pure spin can starve the lock owner on SMP: every CPU ends up
             occupied by waiters while the owner sits in the run queue.  Yield
             periodically so the scheduler can reschedule the owner.  Only do
             this when we hold no other crits (see `nested` above). */
          if (!nested && (spins & 0xFFUL) == 0)
              taskswitch();
    }
    /* Acquired.  We are no longer WAITING, so the no-preempt guard may pin us
        again while we do work under the crit.  Clear before track_hold so the
        guard sees (held_crit_n>0 && crit_wait==0) == a real holder. */
     if (current_process)
         current_process->crit_wait = 0;
     var->wait=1;
     sync_track_hold(var);
    };

unsigned long sync_entercrit_irqsave(sync_sharedvar *var){
    unsigned long flags=0;
    int owner=sync_owner_token();
    int held;
    unsigned long spins=0;

    if (!sync_var_ok(var)) {
       sync_bad_var("enterirq", var);
       __asm__ __volatile__("pushfq; popq %0; cli"
                            : "=r"(flags) : : "memory");
       return flags;
    }

    for (;;) {
       __asm__ __volatile__("pushfq; popq %0; cli"
                            : "=r"(flags) : : "memory");
       held=__sync_val_compare_and_swap(&var->busy,0,0);
       if (held==owner) {
          __sync_add_and_fetch(&var->wait, 1);
          return flags;
       }
       if (!held && __sync_bool_compare_and_swap(&var->busy,0,owner)) {
           var->wait=1;
           sync_track_hold(var);
           return flags;
        }
       __asm__ __volatile__("pause");
        ++spins;
    }
};

//leave the critical section
void sync_leavecrit(sync_sharedvar *var){
   int owner=sync_owner_token();

   if (!sync_var_ok(var)) {
        sync_bad_var("leave", var);
        return;
   }

   if (__sync_val_compare_and_swap(&var->busy,0,0)!=owner) {
        printf("sync: warning critical section released by non-owner! crit=0x%lx busy=0x%x self=0x%x wait=%d\n",
               (unsigned long)(uintptr)var,
               __sync_val_compare_and_swap(&var->busy,0,0),
               owner,
               (int)var->wait);
        printf("SYNCLEAVE rip0=0x%lx rip1=0x%lx rip2=0x%lx rip3=0x%lx rip4=0x%lx rip5=0x%lx\n",
               (unsigned long)(char *)__builtin_return_address(0),
               (unsigned long)(char *)__builtin_return_address(1),
               (unsigned long)(char *)__builtin_return_address(2),
               (unsigned long)(char *)__builtin_return_address(3),
               (unsigned long)(char *)__builtin_return_address(4),
               (unsigned long)(char *)__builtin_return_address(5));
        return;
    }
   if (__sync_sub_and_fetch(&var->wait, 1) == 0) {
       __sync_lock_release(&var->busy);
       sync_untrack_hold(var);
    }
};

   void sync_leavecrit_irqrestore(sync_sharedvar *var,unsigned long flags){
      sync_leavecrit(var);
      if (flags & (1UL << 9))
      __asm__ __volatile__("sti" : : : "memory");
   };

