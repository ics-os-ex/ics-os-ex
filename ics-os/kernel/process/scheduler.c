/*
Name: Dex32 Default Priority Round-Robin Scheduler
Author: Joseph Emmanuel Dayo
Description: Priority-aware round-robin scheduler with a locked ready walk
             for SMP readiness.
*/

#include "../dextypes.h"
#include "process.h"
#include "scheduler.h"
#include "../devmgr/dex32_devmgr.h"
#include "../cpu/spinlock.h"
#include "../cpu/smp.h"
#include "irq_kstack.h"

extern void serial_puts(const char *s);
extern int sprintf(char *str, const char *fmt, ...);

PCB386 *sched_phead;
int ps_schedid;
devmgr_scheduler_extension ps_scheduler;
static spinlock_t ready_lock;
extern unsigned int ticks;

static int deadline_expired(DWORD now,DWORD deadline)
{
   return deadline && (int)(now-deadline)>=0;
}

void sched_block_process(PCB386 *process,DWORD deadline)
{
   spin_irq_flags_t flags=spin_lock_irqsave(&ready_lock);
   process->wait_deadline=deadline;
   process->status|=PS_ATTB_BLOCKED;
   spin_unlock_irqrestore(&ready_lock,flags);
}

void sched_wake_process(PCB386 *process)
{
   spin_irq_flags_t flags=spin_lock_irqsave(&ready_lock);
   process->wait_deadline=0;
   process->status&=~PS_ATTB_BLOCKED;
   spin_unlock_irqrestore(&ready_lock,flags);
}

/* Scheduling priority to compare tasks by.  A task spinning in sync_entercrit
   for a crit it does not hold performs no work, and the owner it waits for may
   be a lower-priority task that can only run on this CPU: user processes get
   priority 1 (see process.c) while kernel threads such as disk_mgr keep 0, so a
   spinning user process would win every pass 1 and the holder would never be
   selected.  That is a hard priority inversion -- it wedged the -j4 self-host
   bootstrap with mkdir.exe spinning on pc_busy held by the BSP-pinned disk_mgr.
   Ranking a waiter at the floor lets any other runnable task, including the
   holder, win; when the waiter is the only candidate it is still selected. */
static int sched_eff_prio(PCB386 *ptr) {
   if (ptr->crit_wait)
      return 0;
   return (int)ptr->priority;
}

/* A PCB the ready walk may safely dereference.  Nodes live in the kernel heap
   or are static kernel objects (sPCB, the per-CPU idle tasks), and their ring
   links must round-trip.  A node that fails this was freed while still linked,
   or the ring was torn by a dequeue that could not unlink cleanly; walking it
   dereferences recycled or unmapped memory and #GP'd inside this very function
   during fork churn.  Skipping the rest of the walk costs at most one tick of
   scheduling; RLBAD above still reports the corruption. */
static int sched_node_ok(PCB386 *p) {
   uintptr a = (uintptr)p;
   uintptr n, b;

   if (!sched_link_ok(a))
      return 0;
   n = (uintptr)p->next;
   b = (uintptr)p->before;
   if (!sched_link_ok(n) || !sched_link_ok(b))
      return 0;
   if (p->next->before != p || p->before->next != p)
      return 0;
   if (p->status & 0xFFFF0000)
      return 0;
   return 1;
}

static int sched_runnable_here(PCB386 *ptr) {
   int me = smp_cpu_id();
   if (ptr->waiting)
       return 0;
    if (ptr->status & PS_ATTB_BLOCKED)
       return 0;
    /* A self-exiting zombie is still in the ready list between the moment its
       CPU clears on_cpu and the moment ps_dequeue() removes it.  It must never
       be claimed in that window, or another CPU would context_load a PML4 the
       BSP is about to free (silent triple fault). */
    if (ptr->status & PS_ATTB_DYING)
       return 0;
    /* Claimed by another CPU */
      if (ptr->on_cpu >= 0 && ptr->on_cpu != me)
         return 0;
     if (ptr->cpu_affinity >= 0 && ptr->cpu_affinity != me)
       return 0;
   /* Only the owning CPU may select its idle thread. */
   if ((ptr->processid & 0xFFFF0000u) == 0xFFFF0000u
       && ptr != (PCB386 *)smp_this_cpu()->idle)
      return 0;
   return 1;
}

/* Priority round-robin: higher priority wins; equal priority is RR from last. */
PCB386 *scheduler(PCB386 *lastprocess){
   PCB386 *start, *ptr, *best;
   int best_prio;
   int me = smp_cpu_id();

   if (!lastprocess || !lastprocess->next || !lastprocess->before)
      lastprocess = sched_phead;
   if (!lastprocess || !lastprocess->next || !lastprocess->before)
      return lastprocess;

   /* A process that holds a crit (sync_entercrit) must not be descheduled
      while it does so: descheduling a lock holder (e.g. an IPI_RESCHEDULE
      waking a CPU whose current process is blocked in device I/O under
      io_devlock) strands the crit in the run queue, and every other CPU that
      needs that crit spins forever -- the SMP=4 self-host io_devlock
      deadlock.  Holders here are never in the ready walk for themselves, so
      returning lastprocess keeps the holder on its CPU until it leaves the
      crit.  (The timer path is already cooperative in selfhost mode; this
      also covers the voluntary/IPI switch paths.)

      Guard against returning a holder that is not actually runnable (a DYING
      zombie or a blocked process): in those cases fall through to the normal
      ready walk instead of pinning a dead process to the CPU. */
   /* crit_wait: lastprocess is itself SPINNING to acquire a crit (it holds
       other crits from the outer level, but is blocked on this one).  Pinning
       it here would starve the crit's actual owner (runnable in the ready
       queue) and deadlock the system -- the SMP io_devlock deadlock.  Do NOT
       pin a waiter; fall through to the ready walk so the owner gets a CPU.
       A holder doing real work under a crit (crit_wait==0) is still pinned, so
       mid-critical-section preemption protection is preserved. */
    if (pcb_held_n(lastprocess) > 0
        && !(lastprocess->status & (PS_ATTB_DYING | PS_ATTB_BLOCKED))
        && !lastprocess->waiting
        && !lastprocess->crit_wait)
       return lastprocess;

   /* ready_lock is held with interrupts disabled: scheduler() runs from
      the timer IRQ (IF=0) and from voluntary taskswitch/waitpid paths
      (IF=1).  Without stopints(), a timer IRQ inside the ready-list walk
      re-enters scheduler() and spins forever on the held ready_lock. */
   { DWORD fl; storeflags(&fl); stopints();
   spin_lock(&ready_lock);

   /* Re-validate lastprocess under the lock.  It is the caller's idea of the
      current task, checked above without ready_lock, so another CPU can have
      dequeued and freed it in between; the walk below would then read recycled
      kheap and fault (observed as RLBAD followed by a kernel #PF).  Require the
      ring links to round-trip and fall back to the head if they do not. */
   if (!sched_node_ok(lastprocess)) {
      PCB386 *head = sched_phead;
      if (head && sched_node_ok(head)) {
         lastprocess = head;
      } else {
         spin_unlock(&ready_lock);
         restoreflags(fl);
         return lastprocess;
      }
   }

   /* Bounded ready-list integrity validator (victim-vs-source probe for the
       first-switch wild-rip corruption). Runs under ready_lock + IF=0, so any
       corruption it finds was produced OUTSIDE the lock: either by a
       dequeue/free that did not hold ready_lock, or by a wild write into a
       PCB/heap. Reports the exact node + bad field once, then continues (the
       walk below will still fail safely via the GPF64 kernel-fault halt). */
    {
       static volatile unsigned long rl_bad = 0;
       if (rl_bad < 8) {
          PCB386 *n = lastprocess;
          int hops = 0;
          int bad = 0;
          /* A legal PCB lives in the kernel heap (>= 0x100000) or is the
             static sPCB. next/before must round-trip (n->next->before == n)
             and stay in the same world. status must not be wild. */
          while (hops < 256) {
             if (!sched_node_ok(n)) { bad = 1; break; }
             n = n->next;
             if (n == lastprocess) break;
             hops++;
          }
          if (bad) {
             rl_bad++;
             char lb[192];
             char nm[12];
             int k = 0;
             const char *s = lastprocess->name ? lastprocess->name : "?";
             while (s[k] && k < 11) { nm[k] = s[k]; k++; }
             nm[k] = 0;
             sprintf(lb, "RLBAD cpu=%d last=%s lastp=0x%llx next=0x%llx before=0x%llx status=0x%x hops=%d\n",
                     me, nm,
                     (unsigned long long)(uintptr)lastprocess,
                     (unsigned long long)(uintptr)lastprocess->next,
                     (unsigned long long)(uintptr)lastprocess->before,
                     (unsigned)lastprocess->status, hops);
             serial_puts(lb);
          }
       }
    }
    start = lastprocess->next;
    if (!start) {
       spin_unlock(&ready_lock);
       restoreflags(fl);
       return lastprocess;
    }
    ptr = start;
    best = 0;
    best_prio = -1;

   /* Pass 1: find maximum priority among runnable tasks; tick down sleepers. */
   {
      int hops = 0;
      do {
         PCB386 *nx;
         if (!sched_node_ok(ptr))
            break;
         if ((ptr->status&PS_ATTB_BLOCKED) &&
             deadline_expired(ticks,ptr->wait_deadline)) {
            ptr->wait_deadline=0;
            ptr->status&=~PS_ATTB_BLOCKED;
         }
         if (ptr->waiting) {
            ptr->waiting--;
         } else if (sched_runnable_here(ptr)) {
            if (sched_eff_prio(ptr) > best_prio) {
               best = ptr;
               best_prio = sched_eff_prio(ptr);
            }
         }
         nx = ptr->next;
         if (!nx || hops++ > 512)
            break;
         ptr = nx;
      } while (ptr != start);
   }

   /* Pass 2: among that priority, pick the next after lastprocess (RR). */
    if (best && lastprocess->next) {
       int hops = 0;
       ptr = lastprocess->next;
       do {
          PCB386 *nx;
          if (!sched_node_ok(ptr))
             break;
          if (sched_runnable_here(ptr)
              && sched_eff_prio(ptr) == best_prio) {
             best = ptr;
             break;
          }
          nx = ptr->next;
          if (!nx || hops++ > 512)
             break;
          ptr = nx;
       } while (ptr != lastprocess->next);
    }

    /* Claim before unlock so another CPU cannot pick the same task.  This must
       be a CAS, not a store: ps_switchto() and self_exit_current() claim and
       release on_cpu without ready_lock, so a plain store here could overwrite
       a claim another CPU had just won and put two CPUs on one PCB -- and thus
       on one IRQ kstack.  Losing the race just means idling this tick. */
    if (best && best->on_cpu != me
        && !__sync_bool_compare_and_swap(&best->on_cpu, -1, me))
       best = 0;

    spin_unlock(&ready_lock);
    restoreflags(fl); }

    return best ? best : lastprocess;
};



//This is called when the extension manager is ready to make the current
//scheduler active
int sched_attach(devmgr_generic *cur){
   devmgr_scheduler_extension *oldsched=(devmgr_scheduler_extension*)cur;
   //get the location of the PCB head 
   sched_phead = oldsched->ps_gethead();
   return 0;
};

PCB386 *sched_getcurrentprocess(){
   return current_process;
};

PCB386 *sched_gethead(){
   return sched_phead;
};

//adds a process to a circular doubly-linked list process queue
void sched_enqueue(PCB386 *process){
   PCB386 *temp;
   process->size = sizeof(PCB386);

   { DWORD fl; storeflags(&fl); stopints();
   spin_lock(&ready_lock);

   /* Already linked: a second insert would splice the ring and leave the
      old neighbors pointing at a node that is about to be overwritten. */
   if (sched_node_ok(process)) {
      spin_unlock(&ready_lock);
      restoreflags(fl);
      return;
   }

   if (sched_phead==0){
      sched_phead = process;
      sched_phead->next = sched_phead;
      sched_phead->before = sched_phead;
   }else{
      temp = sched_phead->next;
      sched_phead->next = process;
      process->next = temp;
      process->before = sched_phead;
      temp->before = process;
   };
   spin_unlock(&ready_lock);
   restoreflags(fl); }
   smp_reschedule_others();
};

int sched_dequeue(PCB386 *ptr){
   int was_queued = 0;
   { DWORD fl; storeflags(&fl); stopints();
   spin_lock(&ready_lock);
   if (sched_node_ok(ptr)) {
      was_queued = 1;
      if (ptr->next == ptr) {
         if (sched_phead == ptr)
            sched_phead = 0;
      } else {
         ptr->before->next = ptr->next;
         ptr->next->before = ptr->before;
         if (sched_phead == ptr)
            sched_phead = ptr->next;
      }
      ptr->next = 0;
      ptr->before = 0;
   } else if (ptr) {
      ptr->next = 0;
      ptr->before = 0;
   }
   spin_unlock(&ready_lock);
   restoreflags(fl); }
   return was_queued;
};

/*Unlike sched_listprocess, sched_findprocess should return the pointer
to the actual PCB structure it uses.  The DEX process manager may use this
to modify the actual PCB of the scheduler*/
PCB386 *sched_findprocess(int pid){
   PCB386 *retval = (PCB386*)-1;
   DWORD cpuflags;
   PCB386 *head_ptr = sched_phead, *ptr;
   ptr = head_ptr;

   storeflags(&cpuflags);
   stopints();
   spin_lock(&ready_lock);
   head_ptr = sched_phead;
   ptr = head_ptr;

   if (head_ptr && sched_node_ok(head_ptr)) {
      int hops = 0;
      do{
         if (!sched_node_ok(ptr))
            break;
         if (ptr->processid == pid) {
            retval = ptr;
            break;
         };
         ptr = ptr ->next;
         if (!ptr || hops++ > 512)
            break;
       } while (ptr != head_ptr);
   }

   spin_unlock(&ready_lock);
   restoreflags(cpuflags);
   return retval;
};


/*****************************************************************************
int sched_listprocess(PCB386 *process_buf,int items)
    process_buf = an array of PCB386 structures.
    items       = the maximum number of items process_buf can hold
    
return value: The total number of processes, if process_buf is NULL then
              the function simply returns the total number of processes.
              items must be non-zero 
-places the list of processes into a buffer*/
int sched_listprocess(PCB386 *process_buf, DWORD size_per_item, int items){
   DWORD cpuflags;
   int i = 0;
   PCB386 *head_ptr = sched_phead, *ptr;
    
   storeflags(&cpuflags);
   stopints();
   spin_lock(&ready_lock);
   head_ptr = sched_phead;
   ptr = head_ptr;

   if (head_ptr && sched_node_ok(head_ptr)) {
      do{
         if (!sched_node_ok(ptr))
            break;
         if (process_buf!=0 && items!=0 ){ 
            if (i < items)    
               memcpy(&process_buf[i], ptr, size_per_item < sizeof(PCB386) ?
                                               size_per_item : sizeof(PCB386) );
            else
               break;
         };
         ptr = ptr ->next; i++;
         if (!ptr || i > 512)
            break;
      } while (ptr != head_ptr);
   }

   spin_unlock(&ready_lock);
   restoreflags(cpuflags);
    
   return i;
};

//registers this scheduler extension to the device manager
void ps_scheduler_install(){
   spin_init(&ready_lock);

   //create a scheduler extension, fill up required information
   memset(&ps_scheduler,0,sizeof(devmgr_scheduler_extension));
   ps_scheduler.hdr.size = sizeof(devmgr_scheduler_extension);
   ps_scheduler.hdr.type = DEVMGR_SCHEDULER_EXTENSION;
   strcpy(ps_scheduler.hdr.name,"default_sched");
   strcpy(ps_scheduler.hdr.description,"Priority Round-Robin Scheduler");
   
   ps_scheduler.exthdr.attach  = sched_attach;
   ps_scheduler.ps_enqueue     = sched_enqueue;
   ps_scheduler.ps_dequeue     = sched_dequeue;
   ps_scheduler.scheduler      = scheduler;
   ps_scheduler.ps_gethead     = sched_gethead;
   ps_scheduler.ps_listprocess = sched_listprocess;
   ps_scheduler.ps_findprocess = sched_findprocess;

   //make interface available to the device manager
   ps_schedid = devmgr_register(&ps_scheduler);

   //update the current scheduler
   #ifdef DEBUG_STARTUP
      printf("Process manager: Installing default scheduler (Priority Round-Robin)\n");
   #endif
   
   extension_override(devmgr_getdevice(ps_schedid),0);   
};
