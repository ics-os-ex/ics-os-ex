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

static int sched_runnable_here(PCB386 *ptr) {
   int me = smp_cpu_id();
   if (ptr->waiting)
      return 0;
   if (ptr->status & PS_ATTB_BLOCKED)
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

   if (!lastprocess)
      lastprocess = sched_phead;
   if (!lastprocess)
      return lastprocess;

   /* ready_lock is held with interrupts disabled: scheduler() runs from
      the timer IRQ (IF=0) and from voluntary taskswitch/waitpid paths
      (IF=1).  Without stopints(), a timer IRQ inside the ready-list walk
      re-enters scheduler() and spins forever on the held ready_lock. */
   { DWORD fl; storeflags(&fl); stopints();
   spin_lock(&ready_lock);

   start = lastprocess->next;
   ptr = start;
   best = 0;
   best_prio = -1;

   /* Pass 1: find maximum priority among runnable tasks; tick down sleepers. */
   do {
      if ((ptr->status&PS_ATTB_BLOCKED) &&
          deadline_expired(ticks,ptr->wait_deadline)) {
         ptr->wait_deadline=0;
         ptr->status&=~PS_ATTB_BLOCKED;
      }
      if (ptr->waiting) {
         ptr->waiting--;
      } else if (sched_runnable_here(ptr)) {
         if ((int)ptr->priority > best_prio) {
            best = ptr;
            best_prio = (int)ptr->priority;
         }
      }
      ptr = ptr->next;
   } while (ptr != start);

   /* Pass 2: among that priority, pick the next after lastprocess (RR). */
    if (best) {
       ptr = lastprocess->next;
       do {
          if (sched_runnable_here(ptr)
              && (int)ptr->priority == best_prio) {
             best = ptr;
             break;
          }
          ptr = ptr->next;
       } while (ptr != lastprocess->next);
       /* Claim before unlock so another CPU cannot pick the same task. */
       best->on_cpu = me;
    }

    spin_unlock(&ready_lock);
    restoreflags(fl); }

    if (best && best != lastprocess) {
       static volatile unsigned long schedsel_user_count = 0;
       if (best->accesslevel == ACCESS_USER && schedsel_user_count < 96) {
          char sb[128];
          schedsel_user_count++;
          sprintf(sb, "SCHEDSEL cpu=%d n=%lu last=%s best=%s prio=%d on=%d\n",
                  me, (unsigned long)schedsel_user_count,
                  lastprocess ? lastprocess->name : "?",
                  best->name, (int)best->priority, (int)best->on_cpu);
          serial_puts(sb);
       }
    }
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
	 
   //no processes in memory yet?
   if (sched_phead==0){
      sched_phead = process;
      //fill up phead's connections
      sched_phead->next = sched_phead;
      sched_phead->before = sched_phead;
   }else{
      //Use insert at head method
      temp = sched_phead->next;
      //fill up phead's connections
      sched_phead->next = process;

      //fill up process's connections
      process->next = temp;
      process->before = sched_phead;

   //fill up temp's connections
   temp->before = process;
   };
   spin_unlock(&ready_lock);
   restoreflags(fl); }
   smp_reschedule_others();
};

//removes a process with the specified pid from a doubly-linked list process queue
int sched_dequeue(PCB386 *ptr){
   { DWORD fl; storeflags(&fl); stopints();
   spin_lock(&ready_lock);
   ptr->before->next=ptr->next;
   ptr->next->before=ptr->before;
   spin_unlock(&ready_lock);
   restoreflags(fl); }
   return 1;
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

   if (head_ptr) {
      do{
         if (ptr->processid == pid) {
            retval = ptr;
            break;
         };
         ptr = ptr ->next;
       } while (ptr != head_ptr);
   }
    
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
    
   ptr = head_ptr;
    
   storeflags(&cpuflags);
   stopints();

   if (head_ptr) {
      do{
         if (process_buf!=0 && items!=0 ){ 
            if (i < items)    
               memcpy(&process_buf[i], ptr, size_per_item < sizeof(PCB386) ?
                                               size_per_item : sizeof(PCB386) );
            else
               break;
         };
         ptr = ptr ->next; i++;                                            
      } while (ptr != head_ptr);
   }
    
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
