/*
  Name: sync.c
  Copyright: 
  Author: Joseph Emmanuel DL Dayo
  Date: 18/01/04 06:27
  Description: Provides process synchornization functions
*/

#include "sync.h"
#include "../cpu/smp.h"
#include "process.h"

extern void taskswitch(void);

sync_sharedvar elf_map_crit;

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
    for (i = 0; i < p->held_crit_n; i++) {
       sync_sharedvar *v = p->held_crits[i];
       if (v && __sync_val_compare_and_swap(&v->busy, 0, 0) == owner) {
          v->wait = 0;
          __sync_lock_release(&v->busy);
          printf("SYNCFREE pid=%d crit=0x%lx\n",
                 (int)p->processid, (unsigned long)(uintptr)v);
       }
    }
    p->held_crit_n = 0;
}


//perform busy waiting
void sync_justwait(sync_sharedvar *var){
   int owner=sync_owner_token();
   int held;
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

    if (__sync_val_compare_and_swap(&var->busy,0,0)==owner) {
       var->wait++;
       return;
    }

   while (!__sync_bool_compare_and_swap(&var->busy,0,owner)) {
         __asm__ __volatile__("pause");
         ++spins;
         /* A pure spin can starve the lock owner on SMP: every CPU ends up
            occupied by waiters while the owner sits in the run queue.  Yield
            periodically so the scheduler can reschedule the owner.  This is
            only safe here because the caller does not yet own the crit. */
         if ((spins & 0xFFFUL) == 0)
             taskswitch();
         if ((spins & 0xFFFFUL) == 0)
            printf("sync: spin crit=0x%lx owner=0x%x self=0x%x\n",
                   (unsigned long)(uintptr)var,
                   __sync_val_compare_and_swap(&var->busy,0,0),
                   owner);
    }
    var->wait=1;
    sync_track_hold(var);
};

unsigned long sync_entercrit_irqsave(sync_sharedvar *var){
    unsigned long flags;
    int owner=sync_owner_token();
    int held;
    unsigned long spins=0;

    for (;;) {
       __asm__ __volatile__("pushfq; popq %0; cli"
                            : "=r"(flags) : : "memory");
       held=__sync_val_compare_and_swap(&var->busy,0,0);
       if (held==owner) {
          var->wait++;
          return flags;
       }
       if (!held && __sync_bool_compare_and_swap(&var->busy,0,owner)) {
           var->wait=1;
           sync_track_hold(var);
           return flags;
        }
       __asm__ __volatile__("pause");
        ++spins;
       if ((spins & 0xFFFFUL) == 0)
          printf("syncirq: spin crit=0x%lx owner=0x%x self=0x%x\n",
                 (unsigned long)(uintptr)var,
                 __sync_val_compare_and_swap(&var->busy,0,0),
                 owner);
    }
};

//leave the critical section
void sync_leavecrit(sync_sharedvar *var){
   int owner=sync_owner_token();

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
   var->wait--;

    if (var->wait<0) 
       printf("sync: warning wrong number of enter-leave pairs detected!\n");

    if (var->wait==0) {
       __sync_lock_release(&var->busy);
       sync_untrack_hold(var);
    }
};

   void sync_leavecrit_irqrestore(sync_sharedvar *var,unsigned long flags){
      sync_leavecrit(var);
      if (flags & (1UL << 9))
      __asm__ __volatile__("sti" : : : "memory");
   };

