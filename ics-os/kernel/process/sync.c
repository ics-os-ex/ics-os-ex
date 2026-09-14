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
#include "irq_kstack.h"

extern void taskswitch(void);
extern void serial_puts(const char *s);
extern int sprintf(char *str, const char *fmt, ...);

sync_sharedvar elf_map_crit;

/* Live "who is stuck on which crit" record, published by the acquire spin loop
   and read by the selfhost watchdog (stdlib/time.c).  Without it a deadlock
   only shows as a kernel RIP inside sync_entercrit, which does not identify
   the lock or its holder.  Per-CPU: the watchdog runs on the same CPU as the
   spinner. */
volatile unsigned long sync_wait_var[MAX_CPUS];
volatile int sync_wait_owner[MAX_CPUS];
volatile unsigned long sync_wait_spins[MAX_CPUS];

static void sync_wait_publish(const sync_sharedvar *var, int held)
{
    int me = smp_cpu_id();
    if (me < 0 || me >= MAX_CPUS)
        return;
    sync_wait_var[me] = (unsigned long)(uintptr)var;
    sync_wait_owner[me] = held;
    sync_wait_spins[me]++;
}

static void sync_wait_clear(void)
{
    int me = smp_cpu_id();
    if (me < 0 || me >= MAX_CPUS)
        return;
    sync_wait_var[me] = 0;
    sync_wait_owner[me] = 0;
    sync_wait_spins[me] = 0;
}

static int sync_var_ok(const sync_sharedvar *var);

/* Find the PCB holding a crit, given the owner token stored in var->busy.
   Walks the ready ring read-only: taking processmgr_busy from a spin loop would
   itself deadlock, and a hung owner is usually not `current` on any CPU. */
static PCB386 *sync_owner_pcb(int owner)
{
    PCB386 *head, *p;
    DWORD want;
    int hops = 0;

    if (owner <= 0)
        return 0;
    want = (DWORD)(owner - 1);
    head = sched_gethead();
    p = head;
    if (!head)
        return 0;
    do {
        /* Torn next (cert 248116 GPF64 err=0 in this walk).  Do not
           require pcb_ptr_ok: idle/sPCB live in BSS. */
        {
            long s = (long)(uintptr)p;
            if (((s << 16) >> 16) != s)
                return 0;
        }
        if ((p->processid & 0x007FFFFF) == want)
            return p;
        p = p->next;
    } while (p && p->before && p != head && ++hops < 512);
    return 0;
}

/* Print the whole wait-for chain rooted at a crit we have been spinning on for
   far too long: waiter -> crit -> owner -> the crit that owner wants -> ...
   A per-CPU watchdog snapshot cannot show this, because every owner past the
   first hop is descheduled and therefore not `current` anywhere.  If the chain
   returns to a process already seen, the locks form a cycle and no amount of
   scheduling can break it -- that is reported as CRITCYCLE, and it is the only
   evidence that distinguishes a lock-order inversion from CPU starvation.
   Throttled to one report per crit. */
static void sync_report_owner(sync_sharedvar *var, int owner)
{
    PCB386 *seen[8];
    sync_sharedvar *v = var;
    int held = owner;
    int n = 0, i;
    char b[256];

    if (owner <= 0 || var->diag_reported)
        return;
    var->diag_reported = 1;

    while (v && held > 0 && n < 8) {
        PCB386 *p = sync_owner_pcb(held);

        if (!p) {
            sprintf(b, "CRITHANG hop=%d crit=0x%lx owner_token=%d ORPHANED "
                       "(owner not in ready ring)\n",
                    n, (unsigned long)(uintptr)v, held);
            serial_puts(b);
            return;
        }
        sprintf(b, "CRITHANG hop=%d crit=0x%lx owner=%d '%s' status=0x%x "
                   "on_cpu=%d aff=%d waiting=%d held=%d critwait=%d "
                   "wants=0x%lx sc=%02x/%02x rip=0x%llx\n",
                n, (unsigned long)(uintptr)v, (int)p->processid, p->name,
                (unsigned)p->status, p->on_cpu, p->cpu_affinity,
                (int)p->waiting, pcb_held_n(p), p->crit_wait,
                (unsigned long)(uintptr)p->crit_wait_var,
                (unsigned)p->cursyscall[0], (unsigned)p->cursyscall[1],
                (unsigned long long)p->ctx.rip);
        serial_puts(b);

        for (i = 0; i < n; i++) {
            if (seen[i] == p) {
                sprintf(b, "CRITCYCLE crit=0x%lx closes at pid=%d '%s' "
                           "(lock-order inversion, not CPU starvation)\n",
                        (unsigned long)(uintptr)v, (int)p->processid, p->name);
                serial_puts(b);
                return;
            }
        }
        seen[n++] = p;

        /* Follow the chain only while this owner is itself blocked on a lock. */
        if (!p->crit_wait || !p->crit_wait_var)
            return;
        v = p->crit_wait_var;
        if (!sync_var_ok(v))
            return;
        held = __sync_val_compare_and_swap(&v->busy, 0, 0);
    }
}

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

#define CRIT_NEST_MAX 16

static PCB386 *crit_current(void)
{
    PCB386 *p = current_process;
    int me;

    /* NULL current is early boot / idle, not smash.  Use the idle PCB
       for nest without writing the slot (torn current must not become
       idle while still on a user CR3). */
    if (!p) {
        me = smp_cpu_id();
        if (me >= 0 && me < MAX_CPUS && cpus[me].idle)
            return (PCB386 *)cpus[me].idle;
        return 0;
    }
    if (!pcb_ptr_ok((unsigned long)(uintptr)p)) {
        static volatile unsigned long bad_n;
        if (++bad_n <= 8)
            serial_puts("CURRENT-BAD\n");
        return 0;
    }
    return current_mm_process();
}

static int nest_n_sanitize(PCB386 *p)
{
    int n;
    if (!p)
        return 0;
    n = p->crit_nest_n;
    if (n < 0 || n > CRIT_NEST_MAX) {
        p->crit_nest_n = 0;
        return 0;
    }
    return n;
}

static void crit_nest_push(sync_sharedvar *var, int tok)
{
    PCB386 *p = crit_current();
    int n;
    if (!p)
        return;
    n = nest_n_sanitize(p);
    if (n >= CRIT_NEST_MAX)
        return;
    p->crit_nest_var[n] = var;
    p->crit_nest_tok[n] = tok;
    p->crit_nest_n = n + 1;
}

static int crit_nest_pop_on(PCB386 *p, sync_sharedvar *var, int fallback)
{
    int n;
    if (!p)
        return fallback;
    n = nest_n_sanitize(p);
    if (n <= 0)
        return fallback;
    if (p->crit_nest_var[n - 1] != var)
        return fallback;
    p->crit_nest_n = n - 1;
    return p->crit_nest_tok[n - 1];
}

static int crit_nest_pop(sync_sharedvar *var, int fallback)
{
    PCB386 *p = crit_current();
    int busy;

    /* Leftover current (or an smp_cpu_id flip onto another cpu_local)
       can point p at the wrong PCB.  Nest was pushed on the lock owner.
       Cert ramdisk: busy=0x12 (disk_mgr) leave sampled kernel 0x1 and
       leaked kheap_crit (non-owner leave does not unlock). */
    if (var) {
        busy = __sync_val_compare_and_swap(&var->busy, 0, 0);
        if (busy > 0) {
            PCB386 *owner = sync_owner_pcb(busy);
            if (owner)
                p = owner;
        }
    }
    return crit_nest_pop_on(p, var, fallback);
}

int sync_cpu_holds(const sync_sharedvar *var)
{
    PCB386 *p = crit_current();
    int i;
    if (!var || !p)
        return 0;
    for (i = 0; i < nest_n_sanitize(p); i++)
        if (p->crit_nest_var[i] == var)
            return 1;
    return 0;
}

static int sync_owner_token(void)
{
    /* Token must name the MM PCB (crit_current), not leftover current.
       getprocessid() stays on current_process for fork/wait identity;
       routing it through current_mm_process broke test-fork.
       Cert 248122: leftover gcc41 on gcc30's CR3 entered vfs_busy with
       tok 0x2a pushed on gcc30, leave busy=0x1f self=0x2a, CRITCYCLE. */
    PCB386 *mm = current_mm_process();
    PCB386 *cur = current_process;
    DWORD leftover_pid = 0, mm_pid = 0;

    if (cur && pcb_ptr_ok((unsigned long)(uintptr)cur) &&
        cur->accesslevel == ACCESS_USER)
        leftover_pid = cur->processid;
    if (mm && pcb_ptr_ok((unsigned long)(uintptr)mm))
        mm_pid = mm->processid;
    else if (cur && pcb_ptr_ok((unsigned long)(uintptr)cur))
        mm_pid = cur->processid;
    return crit_token_for_pids((unsigned)leftover_pid, (unsigned)mm_pid);
}

int sync_leftover_vfs(void)
{
    int me = smp_cpu_id();
    PCB386 *p = current_process;
    PCB386 *idle = 0;
    unsigned long cr3, rsp;

    if (!p)
        return 0;
    if (me >= 0 && me < MAX_CPUS)
        idle = (PCB386 *)cpus[me].idle;
    if (!pcb_ptr_ok((unsigned long)(uintptr)p) && !(idle && p == idle))
        return 0;
    __asm__ __volatile__("movq %%cr3, %0" : "=r"(cr3));
    __asm__ __volatile__("movq %%rsp, %0" : "=r"(rsp));
    return leftover_vfs_must_skip(p->accesslevel == ACCESS_USER,
                                  (unsigned long)(uintptr)p->pagedirloc,
                                  cr3, rsp, idle && p == idle);
}

static int held_n_sanitize(PCB386 *p)
{
    int n;
    if (!p)
       return 0;
    n = p->held_crit_n;
    if (n < 0 || n > 16) {
       p->held_crit_n = 0;
       return 0;
    }
    return n;
}

static void sync_track_hold(sync_sharedvar *var)
{
    PCB386 *p = crit_current();
    int i, n;

    if (!p)
       return;
    n = held_n_sanitize(p);
    for (i = 0; i < n; i++)
        if (p->held_crits[i] == var)
           return;
     if (n < 16)
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

static void sync_untrack_hold_on(PCB386 *p, sync_sharedvar *var)
{
    int i;

    if (!p)
       return;
    for (i = 0; i < held_n_sanitize(p); i++) {
       if (p->held_crits[i] == var) {
          p->held_crits[i] = p->held_crits[--p->held_crit_n];
          return;
       }
    }
}

static void sync_untrack_hold(sync_sharedvar *var)
{
    PCB386 *p = crit_current();
    int busy;

    sync_untrack_hold_on(p, var);
    if (!var)
       return;
    /* busy is still the enter token when leave calls this before release. */
    busy = __sync_val_compare_and_swap(&var->busy, 0, 0);
    if (busy > 0) {
        PCB386 *owner = sync_owner_pcb(busy);
        if (owner && owner != p)
            sync_untrack_hold_on(owner, var);
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
    for (i = 0; i < held_n_sanitize(p); i++) {
       sync_sharedvar *v = p->held_crits[i];
       if (v && __sync_val_compare_and_swap(&v->busy, 0, 0) == owner) {
          v->wait = 0;
          __sync_lock_release(&v->busy);
       }
    }
    p->held_crit_n = 0;
    p->crit_nest_n = 0;
}


//perform busy waiting
void sync_justwait(sync_sharedvar *var){
   int owner=sync_owner_token();
   int held;
   unsigned long spins=0;
   PCB386 *p=current_process;
   if (!sync_var_ok(var))
      return;
   if (p) {
      p->crit_wait_var = var;
      p->crit_wait = 1;
   }
   do {
      held=__sync_val_compare_and_swap(&var->busy,0,0);
      if (!held || held==owner)
         break;
      __asm__ __volatile__("pause");
      ++spins;
      if ((spins & 0xFFUL) == 0)
         taskswitch();
   } while (1);
   if (p) {
      p->crit_wait = 0;
      p->crit_wait_var = 0;
   }
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
    int nested = (pcb_held_n(current_process) > 0);

    if (__sync_val_compare_and_swap(&var->busy,0,0)==owner) {
         __sync_add_and_fetch(&var->wait, 1);
         crit_nest_push(var, owner);
         return;
     }

     /* We are about to WAIT for a crit we do not hold.  Mark it so the
        no-preempt guard (scheduler.c) does not pin us to the CPU: a nested
        spinner still holds its OUTER crits (held_crit_n>0), and if the guard
        pinned us, the lock's real owner -- which is runnable in the ready
        queue -- would be starved forever (the SMP io_devlock deadlock).  A
        holder doing real work under a crit never sets crit_wait and is still
        pinned, so mid-critical-section preemption protection is preserved. */
     if (current_process) {
         current_process->crit_wait_var = var;
         current_process->crit_wait = 1;
     }

     /* Acquire, then publish "no longer waiting" and the hold, with interrupts
        off so the whole transition is atomic against preemption on this CPU.
        Doing it in separate steps leaves windows where our own state lies about
        us: after the CAS but before crit_wait is cleared we are a holder still
        flagged as a waiter, so the scheduler ranks us at the priority floor
        (sched_eff_prio) and can starve us while we own a hot lock -- every other
        CPU then spins on a crit whose owner is descheduled, which is exactly the
        "owner wants the crit it holds" CRITCYCLE seen under concurrent FAT
        writers.  Between clearing crit_wait and sync_track_hold() the reverse
        hole exists: held_crit_n is still 0, so the no-preempt guard does not
        recognise us as a holder either. */
     for (;;) {
          DWORD acq_flags;
          int held;

          storeflags(&acq_flags);
          stopints();
          if (__sync_bool_compare_and_swap(&var->busy,0,owner)) {
             sync_wait_clear();
             if (current_process) {
                current_process->crit_wait = 0;
                current_process->crit_wait_var = 0;
             }
             var->wait = 1;
             sync_track_hold(var);
             crit_nest_push(var, owner);
             restoreflags(acq_flags);
             return;
          }
          held = __sync_val_compare_and_swap(&var->busy,0,0);
          restoreflags(acq_flags);

          /* Re-check recursion on every pass, not just before the loop: if the
             busy field already carries our token we own this crit and must not
             spin for it, or we deadlock against ourselves forever (the CAS can
             never succeed). */
          if (held == owner) {
             __sync_add_and_fetch(&var->wait, 1);
             if (current_process) {
                current_process->crit_wait = 0;
                current_process->crit_wait_var = 0;
             }
             sync_wait_clear();
             crit_nest_push(var, owner);
             return;
          }

          __asm__ __volatile__("pause");
          ++spins;
          if ((spins & 0xFFUL) == 0)
              sync_wait_publish(var, __sync_val_compare_and_swap(&var->busy,0,0));
          if (spins == 20000000UL)
              sync_report_owner(var,
                                __sync_val_compare_and_swap(&var->busy,0,0));
          /* A pure spin can starve the lock owner on SMP: every CPU ends up
             occupied by waiters while the owner sits in the run queue.  Yield
             periodically so the scheduler can reschedule the owner.  Only do
             this when we hold no other crits (see `nested` above).
             A nested waiter still yields eventually: pinning this CPU forever
             deadlocks outright if the owner can only run here, which is
             strictly worse than briefly descheduling us with our outer crits
             held. */
          if ((spins & (nested ? 0xFFFFFUL : 0xFFUL)) == 0)
              taskswitch();
    }
}

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
          crit_nest_push(var, owner);
          return flags;
       }
       if (!held && __sync_bool_compare_and_swap(&var->busy,0,owner)) {
           var->wait=1;
           sync_track_hold(var);
           crit_nest_push(var, owner);
           return flags;
        }
       __asm__ __volatile__("pause");
        ++spins;
    }
};

//leave the critical section
void sync_leavecrit(sync_sharedvar *var){
   int sampled=sync_owner_token();
   int owner=crit_nest_pop(var, sampled);

   if (!sync_var_ok(var)) {
        sync_bad_var("leave", var);
        return;
   }

   if (__sync_val_compare_and_swap(&var->busy,0,0)!=owner) {
        /* Leftover current can hit this in a tight file_ok loop
           (cert 248112: 20k lines, qemu died mid-print). */
        static volatile unsigned long nonowner_n;
        if (++nonowner_n <= 8) {
           char line[192];
           sprintf(line,
                   "CRIT-NONOWNER crit=0x%lx busy=0x%x self=0x%x wait=%d "
                   "rip=0x%lx\n",
                   (unsigned long)(uintptr)var,
                   __sync_val_compare_and_swap(&var->busy,0,0),
                   owner,
                   (int)var->wait,
                   (unsigned long)(char *)__builtin_return_address(0));
           serial_puts(line);
        }
        return;
    }
   /* Untrack on the owner PCB while busy still names that owner.
      Releasing first made leftover-current untrack miss disk_mgr. */
   if (__sync_sub_and_fetch(&var->wait, 1) == 0) {
       sync_untrack_hold(var);
       __sync_lock_release(&var->busy);
    }
};

   void sync_leavecrit_irqrestore(sync_sharedvar *var,unsigned long flags){
      sync_leavecrit(var);
      if (flags & (1UL << 9))
      __asm__ __volatile__("sti" : : : "memory");
   };

