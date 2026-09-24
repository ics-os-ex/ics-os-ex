/*
 *   Name: DEX32 Process Management Module
 *   Copyright: 
 *   Author: Joseph Emmanuel Dayo
 *   Date: 09/11/03 04:11
 *   Description: Provides functions for process management and task switching
 
    DEX educational extensible operating system 1.0 Beta
    Copyright (C) 2004  Joseph Emmanuel DL Dayo

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. 
 */

#include "process.h"
#include "scheduler.h"
#include "completion.h"
#include "irq_kstack.h"
#include "../cpu/context.h"
#include "../vfs/posixfd.h"

extern unsigned int ticks;

/* dexmem frame/PML4 post-mortem helpers (see dexmem.h).  Explicit decls keep
   the 64-bit unsigned long return of freed_pml4_count() intact. */
extern unsigned long freed_pml4_count(void);
extern int freed_pml4_contains(u64 pml4);

static void waitpid_notify_parent(PCB386 *parent);

/* Retained-child-status queue lock.
 *
 * The exit paths append to parent->waitq_* from whatever CPU the child died
 * on, while the parent compacts the same array in sys_waitpid.  Doing that
 * without exclusion lost statuses under `-smp 4` with user_procs_smp: the
 * parent's compaction loop overwrote the slot a remote exit had just
 * appended, and the parent then reported ECHILD for a child it had already
 * reaped (gccdriver "waitpid /work/apps/as.exe failed" in the parallel
 * self-host cert).
 *
 * This is deliberately NOT processmgr_busy.  That crit is held across
 * closeallfiles(), freeprocessmemory(), and smp_tlb_shootdown(), which waits
 * for a shootdown IPI ack from every other CPU; spinning on it with
 * interrupts masked deadlocks, and spinning on it with taskswitch() puts a
 * scheduler switch inside the waitpid poll loop.  waitq_lock is only ever
 * held for a bounded array update with no calls out. */
static spinlock_t waitq_lock;   /* BSS: zero == unlocked */

/* Append an exited child's status to its parent's queue and drop the parent's
   live-child count.  Returns 0 when the queue is full (status dropped). */
int waitq_publish(PCB386 *parent, int child_pid, int child_status)
{
   spin_irq_flags_t f;
   int ok = 0;

   if (!parent)
      return 0;
   f = spin_lock_irqsave(&waitq_lock);
   if (parent->nlive > 0)
      parent->nlive--;
   if (parent->waitq_n < WAITQ_MAX) {
      parent->waitq_pid[parent->waitq_n] = child_pid;
      parent->waitq_st[parent->waitq_n] = child_status;
      parent->waitq_n++;
      ok = 1;
   }
   spin_unlock_irqrestore(&waitq_lock, f);
   if (!ok) {
      char b[96];
      sprintf(b, "WAITQ FULL parent=%d child=%d n=%d\n",
              (int)parent->processid, child_pid, WAITQ_MAX);
      serial_puts(b);
   }
   return ok;
}

/* Remove one matching retained status (pid == -1 matches any).  Returns 1 when
   one was reaped. */
int waitq_reap(PCB386 *me, int pid, int *out_pid, int *out_st)
{
   spin_irq_flags_t f;
   int i, got = 0;

   if (!me)
      return 0;
   f = spin_lock_irqsave(&waitq_lock);
   for (i = 0; i < me->waitq_n; i++) {
      if (pid == -1 || me->waitq_pid[i] == pid) {
         *out_pid = me->waitq_pid[i];
         *out_st = me->waitq_st[i];
         me->waitq_n--;
         for (; i < me->waitq_n; i++) {
            me->waitq_pid[i] = me->waitq_pid[i + 1];
            me->waitq_st[i] = me->waitq_st[i + 1];
         }
         got = 1;
         break;
      }
   }
   spin_unlock_irqrestore(&waitq_lock, f);
   return got;
}

/* Read-only "does this parent already hold a status for pid" probe used by the
   legacy dex32_waitpid path. */
int waitq_has(PCB386 *me, int pid)
{
   spin_irq_flags_t f;
   int i, got = 0;

   if (!me)
      return 0;
   f = spin_lock_irqsave(&waitq_lock);
   for (i = 0; i < me->waitq_n; i++) {
      if (me->waitq_pid[i] == pid) {
         got = 1;
         break;
      }
   }
   spin_unlock_irqrestore(&waitq_lock, f);
   return got;
}

#define EVENT_WAIT_BUCKETS 32
static wait_queue_t event_wait_queues[EVENT_WAIT_BUCKETS];

static wait_queue_t *wait_event_queue(void *key)
{
   uintptr value=(uintptr)key;
   return &event_wait_queues[(value>>4)&(EVENT_WAIT_BUCKETS-1)];
}

void wait_queue_prepare(wait_queue_t *queue,void *key,DWORD deadline)
{
   PCB386 *process=current_process;
   spin_irq_flags_t flags;
   if (!queue || !process)
      return;
   flags=spin_lock_irqsave(&queue->lock);
   if (!process->wait_queued) {
      process->wait_queue=queue;
      process->wait_key=key;
      process->wait_next=queue->head;
      queue->head=process;
      process->wait_queued=1;
      sched_block_process(process,deadline);
   }
   spin_unlock_irqrestore(&queue->lock,flags);
}

static void wait_queue_remove_locked(wait_queue_t *queue,PCB386 *process)
{
   PCB386 **link=&queue->head;
   while (*link && *link!=process)
      link=&(*link)->wait_next;
   if (*link==process)
      *link=process->wait_next;
   process->wait_queue=0;
   process->wait_key=0;
   process->wait_next=0;
   process->wait_queued=0;
}

void wait_queue_finish(wait_queue_t *queue)
{
   PCB386 *process=current_process;
   spin_irq_flags_t flags;
   if (!queue || !process)
      return;
   flags=spin_lock_irqsave(&queue->lock);
   if (process->wait_queued && process->wait_queue==queue)
      wait_queue_remove_locked(queue,process);
   sched_wake_process(process);
   spin_unlock_irqrestore(&queue->lock,flags);
}

void wait_queue_cancel(PCB386 *process)
{
   wait_queue_t *queue;
   spin_irq_flags_t flags;
   if (!process || !process->wait_queued || !process->wait_queue)
      return;
   queue=process->wait_queue;
   flags=spin_lock_irqsave(&queue->lock);
   if (process->wait_queued && process->wait_queue==queue)
      wait_queue_remove_locked(queue,process);
   sched_wake_process(process);
   spin_unlock_irqrestore(&queue->lock,flags);
}

void wait_queue_wake_all(wait_queue_t *queue)
{
   PCB386 *process,*next;
   spin_irq_flags_t flags;
   if (!queue)
      return;
   flags=spin_lock_irqsave(&queue->lock);
   process=queue->head;
   queue->head=0;
   while (process) {
      next=process->wait_next;
      process->wait_queue=0;
      process->wait_key=0;
      process->wait_next=0;
      process->wait_queued=0;
      sched_wake_process(process);
      process=next;
   }
   spin_unlock_irqrestore(&queue->lock,flags);
}

void wait_event_wake_all(void *key)
{
   wait_queue_t *queue=wait_event_queue(key);
   PCB386 **link,*process;
   spin_irq_flags_t flags=spin_lock_irqsave(&queue->lock);
   link=&queue->head;
   while (*link) {
      process=*link;
      if (process->wait_key!=key) {
         link=&process->wait_next;
         continue;
      }
      *link=process->wait_next;
      process->wait_queue=0;
      process->wait_key=0;
      process->wait_next=0;
      process->wait_queued=0;
      sched_wake_process(process);
   }
   spin_unlock_irqrestore(&queue->lock,flags);
}

int wait_for_completion_until(completion_t *completion,DWORD deadline)
{
   if (!completion)
      return 0;
   while (!completion_done(completion)) {
      if (deadline && (int)(ticks-deadline)>=0)
         return 0;
      wait_queue_prepare(wait_event_queue(completion),completion,deadline);
      if (!completion_done(completion))
         taskswitch();
      wait_queue_finish(wait_event_queue(completion));
   }
   return 1;
}

/*
int lock_var = 0; // actual lock global variable used to provide synchronization
int value; // variable contain lock value merely to explain current state

void mutex_lock()
{
    __asm  // Inline assembly is written this way in c
    {
        mov eax, 1   // EAX is a 32 bit register in which we are assigning 1
        xchg eax, lock_var // Exchange eax and lock_var atomically
        mov value, eax // merely saving for printing purpose
    }
}

void mutex_unlock()
{
    __asm
    {
        mov eax, 0
        xchg eax, lock_var  // This could have been a single assignment lock_var = 0
        mov value, eax // But we are merely using xchg again to see the previous value
    }
}

*/


//the global list of semaphores
semaphore *semaphore_head;

/* The next process to be created will use this process ID
   pids 1-0x8A is reserved, pid 0 is the kernel pid*/
DWORD nextprocessid = 0x10;
    
//used for the busy waiting loops of the process manager.    
sync_sharedvar processmgr_busy;


//total number of process, initially set to 0
int totalprocesses=0;

//global vriables used for triggering taskswitcher events.
DWORD sigpriority = 0; //set this to the process ID of the process that requires immediate attention
DWORD sigterm = 0; //set this to the process ID of the process you wish to terminate
volatile int dex32_child_faulted = 0;
DWORD sigwait = 0; /*set this to the process ID of the process which is not
                   supposed to be interrupted*/
DWORD sigshutdown = 0; //not implemented yet
DWORD pfoccured = 0; /*set this to reset the pf_handler PCB, usually set by
                      the pf handler when a page fault has occured so that
                     its curent state does not get changed*/

/* Guards the read-and-clear of the single sigterm mailbox so that two CPUs
   reaping exits in schedule_from_timer()/taskswitcher() cannot both consume
   (double-kill) the same pid. Only the check-and-clear is held; kill_process()
   and the self-exit teardown run outside the lock. */
static spinlock_t sigterm_lock;
static PCB386 *zombie_head;
static PCB386 *pending_zombie[MAX_CPUS];
static void self_exit_current(void);
static int zfree_pml4_liveness_check(PCB386 *z);
static void zombie_enqueue(PCB386 *z);
static void zombie_drain(void);

/* When set, newly created user processes (and their fork children) are not
   pinned to the BSP so the scheduler may spread them across all online CPUs.
   Only the parallel self-host closure sets this; normal boots keep the BSP pin
   until waitpid/exit migration is fully hardened. */
volatile int user_procs_smp = 0;

/* 1 when booted for a stage-1 self-host closure: the serial "selfhost-stage1"
   or the parallel "selfhost-stage1-parallel" kernel. Both run the cooperative
   GCC closure and must park the APs before the BSP kexec. */
int selfhost_stage1_cmdline(void) {
   extern char kernel_cmdline[];
   return strcmp(kernel_cmdline, "selfhost-stage1") == 0
       || strcmp(kernel_cmdline, "selfhost-stage1-parallel") == 0;
}

DWORD sched_sysmes[3]={0,0,0}; //scheduler system messages [0] = pid, [1] = mes, [2] = data

int ps_notimeincrement = 0;

//pointers to initial processes in the kernel
PCB386   *schedp;                         //pointer to scheduler process
PCB386   *plast;                          //pointer to last process
PCB386   *next_process=0;                 //pointer to next process
PCB386   curp;                            //???

PCB386   kernelPCB;                       //actual PCB for kernel
PCB386   schedpPCB;                       //actual process structure for scheduler

PCB386   sPCB;                            //actual PCB for kernel
PCB386   pfPCB;                           //page fault PCB
PCB386   pfPCB_copy;                      //copy of page fault PCB
PCB386   keyPCB;                          //keyboard PCB
PCB386   mousePCB;                        //mouse PCB                   
/* current_process is a macro over smp_this_cpu()->current */

#ifdef __x86_64__
/* Kernel stacks live in .bss (same pattern as AP stacks).  A fixed PA
   next to the kernel image is what kept colliding with the frame stack. */
#define KSTACK_SIZE  0x10000UL
static unsigned char kstack_dispatcher[KSTACK_SIZE] __attribute__((aligned(16)));
static unsigned char kstack_sched[KSTACK_SIZE] __attribute__((aligned(16)));
static unsigned char kstack_pf[KSTACK_SIZE] __attribute__((aligned(16)));
DWORD dispatcher_stack_loc;
DWORD sched_stack_loc;
DWORD pagefault_stack_loc;
#endif

FPUregs ps_fpustate, ps_kernelfpustate;



//calls the timer interrupt which in turn results to
//a call to the scheduler
extern void switchprocess();


void ps_shutdown(){
   printf("process manager: Shutdown not yet implemented\n");
};

void signal(DWORD sigtype,void* ptr){

};


//returns the process id of the parent process
DWORD getparentid(){
    return current_process->owner;
};

//returns the process id of the current process
/* Leftover current=idle during a user syscall (cert 248119). */
PCB386 *current_mm_process(void)
{
    PCB386 *p = current_process;
    unsigned long cr3;
    PCB386 *by;

    if (!p || !pcb_ptr_ok((unsigned long)(uintptr)p))
        return p;
    __asm__ __volatile__("movq %%cr3, %0" : "=r"(cr3));
    if (!leftover_user_on_other_cr3(p->accesslevel == ACCESS_USER,
                                    (unsigned long)(uintptr)p->pagedirloc,
                                    cr3))
        return p;
    by = ps_find_by_cr3(cr3);
    return by ? by : p;
}

DWORD getprocessid(){
    PCB386 *p = current_process;
    if (!p)
        return 0;
    if (!pcb_ptr_ok((unsigned long)(uintptr)p))
        return 0;
    return p->processid;
};


//returns the process id of the parent process
DWORD getpprocessid(){
    return current_process->owner;
}

/*creates a USER thread (In DEX this is process which shares the same memory space as its parent, BUGGY
 * process, but has its own stack pointer)
 *
 * ptr - function to execute in the thread. 
 * stack - stack for thread
 * stacksize - size of thread stack
*/
DWORD createthread(void *ptr, void *stack, DWORD stacksize){

   int pages;
   DWORD flags;
    
   PCB386 *temp=(PCB386*)malloc(sizeof(PCB386));
   memset(temp,0,sizeof(PCB386));
   spin_init(&temp->fd_lock);
   fpu_init_default(&temp->fpu);

   totalprocesses++;
    
   dex32_stopints(&flags);
    
   temp->size=sizeof(PCB386);
   temp->before=current_process;
   
   sprintf(temp->name,"%s.thread",current_process->name);
   temp->processid   = __sync_fetch_and_add(&nextprocessid, 1);
   temp->accesslevel = ACCESS_USER;
   temp->status     |= PS_ATTB_THREAD;
   current_process->childwait++;
   temp->meminfo     = current_process->meminfo;
   temp->owner       = getprocessid();
   temp->workdir     = current_process->workdir;
   temp->stdout      = current_process->stdout;
   temp->outdev      = current_process->outdev;
   temp->knext       = current_process->knext; 
   temp->mmap_brk    = current_process->mmap_brk;
   temp->pagedirloc  = current_process->pagedirloc;
    
   /*Set up initial contents of the CPU registers*/
   memset(temp,0,sizeof(saveregs));
   temp->regs.EIP    = (DWORD)ptr;
   temp->regs.ESP    = (DWORD)(stack+stacksize-4);
   temp->stackptr    = (void*)temp->regs.ESP;
   temp->regs.CR3    = (DWORD)current_process->pagedirloc;
   temp->regs.ES     = USER_DATA;
   temp->regs.SS     = USER_DATA;
   temp->regs.CS     = USER_CODE;
   temp->regs.DS     = USER_DATA;
   temp->regs.FS     = USER_DATA;
   temp->regs.GS     = USER_DATA;
   temp->regs.SS0    = SYS_STACK_SEL;

   //set up the initial stack pointer for system calls
   temp->stackptr0   = malloc(SYSCALL_STACK);
   temp->regs.ESP0   = temp->stackptr0+SYSCALL_STACK-4;
   temp->regs.EFLAGS = current_process->regs.EFLAGS;
    
   //initialize the current FPU state
   memcpy(&temp->regs2,&ps_kernelfpustate,sizeof(ps_kernelfpustate));
    
   //Tell the scheduler to add it to the process queue
   ps_enqueue(temp);
    
   dex32_restoreints(flags);

   return temp->processid;

};


/**
 * Creates a user thread. A user thread has access to all 
 * the information about the user process. Unique to a thread 
 * are the registers and stack.
 *
 *  FIXME: by jach. not working. :(
 */
DWORD createuthread(void *ptr, void *stack, DWORD stacksize){

   int pages;
   DWORD flags;
    
   PCB386 *temp=(PCB386*)malloc(sizeof(PCB386));
   memset(temp,0,sizeof(PCB386));
   fpu_init_default(&temp->fpu);

   totalprocesses++;
    
   dex32_stopints(&flags);
    
   temp->size=sizeof(PCB386);
   temp->before=current_process;
   
   sprintf(temp->name,"%s.thread",current_process->name);
   temp->processid   = __sync_fetch_and_add(&nextprocessid, 1);
   temp->accesslevel = ACCESS_USER;
   temp->status     |= PS_ATTB_THREAD;
   current_process->childwait++;
   temp->meminfo     = current_process->meminfo;

   temp->owner       = getprocessid();
   temp->workdir     = current_process->workdir;
   temp->stdout      = current_process->stdout;
   temp->outdev      = current_process->outdev;
   temp->knext       = current_process->knext; 
   temp->mmap_brk    = current_process->mmap_brk;
   temp->pagedirloc  = current_process->pagedirloc;
    
   /*Set up initial contents of the CPU registers*/
   memset(temp,0,sizeof(saveregs));
   temp->regs.EIP    = (DWORD)ptr;
   temp->regs.ESP    = (DWORD)(stack+stacksize-4);
   temp->stackptr    = (void*)temp->regs.ESP;
   temp->regs.CR3    = (DWORD)current_process->pagedirloc;
   temp->regs.ES     = USER_DATA;
   temp->regs.SS     = USER_DATA;
   temp->regs.CS     = USER_CODE;
   temp->regs.DS     = USER_DATA;
   temp->regs.FS     = USER_DATA;
   temp->regs.GS     = USER_DATA;
   temp->regs.SS0    = SYS_STACK_SEL;

   //set up the initial stack pointer for system calls
   temp->stackptr0   = malloc(SYSCALL_STACK);
   temp->regs.ESP0   = temp->stackptr0+SYSCALL_STACK-4;
   temp->regs.EFLAGS = current_process->regs.EFLAGS;
    
   //initialize the current FPU state
   memcpy(&temp->regs2,&ps_kernelfpustate,sizeof(ps_kernelfpustate));
    
   //Tell the scheduler to add it to the process queue
   ps_enqueue(temp);
    
   dex32_restoreints(flags);

   return temp->processid;
}

//Tells the scheduler to queue a process, uses aspect-oriented programming
DWORD ps_enqueue(PCB386 *process){
   devmgr_scheduler_extension *cursched = extension_table[CURRENT_SCHEDULER].iface;
   bridges_link((devmgr_generic*)cursched,
                  &cursched->ps_enqueue,
                  process,0,0,0,0,0);
};

//Tells the scheduler to dequeue a process, uses aspect-oriented programming
DWORD ps_dequeue(PCB386 *process){
   devmgr_scheduler_extension *cursched = extension_table[CURRENT_SCHEDULER].iface;
   bridges_link((devmgr_generic*)cursched,
                  &cursched->ps_dequeue,
                  process,0,0,0,0,0);
};


static void freeprocessmemory_metadata(process_mem *memptr)
{
   while (memptr) {
      process_mem *next=memptr->next;
      free(memptr);
      memptr=next;
   }
}

static int cloneprocessmemory_metadata(process_mem *source,process_mem **dest)
{
   process_mem **tail=dest;
   *dest=0;
   while (source) {
      process_mem *node=(process_mem *)malloc(sizeof(process_mem));
      if (!node) {
         freeprocessmemory_metadata(*dest);
         *dest=0;
         return 0;
      }
      node->vaddr=source->vaddr;
      node->pages=source->pages;
      node->next=0;
      *tail=node;
      tail=&node->next;
      source=source->next;
   }
   return 1;
}

#ifdef __x86_64__
extern void fork_child_return(void);

static int fork_has_other_threads(PCB386 *parent)
{
   PCB386 *process,*head;
   int found=0;
   DWORD flags;

   dex32_stopints(&flags);
   sync_entercrit(&processmgr_busy);
   head=sched_phead;
   process=head;
   if (process) {
      do {
         if (process!=parent && process->owner==parent->processid
             && (process->status&PS_ATTB_THREAD)) {
            found=1;
            break;
         }
         process=process->next;
      } while (process && process!=head);
   }
   sync_leavecrit(&processmgr_busy);
   dex32_restoreints(flags);
   return found;
}

#define IRQ_KSTACK_SIZE  0x20000

int pcb_alloc_irq_kstack(PCB386 *p)
{
   void *s;
   if (!p)
      return 0;
   s=malloc(IRQ_KSTACK_SIZE);
   if (!s)
      return 0;
   p->kstack_base=s;
   p->kstack_top=((u64)(uintptr)s+IRQ_KSTACK_SIZE)&~15ULL;
   p->irq_user_rsp=0;
   p->irq_kframe=0;
   return 1;
}

void pcb_free_irq_kstack(PCB386 *p)
{
   if (!p || !p->kstack_base)
      return;
   free(p->kstack_base);
   p->kstack_base=0;
   p->kstack_top=0;
   p->irq_user_rsp=0;
   p->irq_kframe=0;
}

/* Called by IRQ_KSTACK_ENTER once RSP is already on the kstack; rdi is this
   entry's PUSH_ALL pointer (kept in %r13 for the whole handler).  The wrapper
   needs no return value: the switch and the restore are both done in asm.
   All this records is the outermost (user-stack) frame, which fork needs in
   order to build the child's iretq frame. */
void irq_kstack_enter(u64 current_rsp)
{
   extern volatile int ctx_load_in_progress[MAX_CPUS];
   PCB386 *p;
   int me;

   /* Kernel SSE stores must not take #NM here; lazy FPU is not used. */
   __asm__ volatile ("clts");

   /* Do not repair leftover current here.  The wrapper already chose RSP
      from the advertised PCB; retargeting current mid-entry (cert
      STALE-CURRENT-REPAIR then PF64 rip=0x2a00000206 on gcc.exe) drops
      irq_user_rsp and crit tokens for a still-running syscall.
      schedule_from_timer / idle / IPI repair after this frame is done. */
   p=current_process;

   /* Tripwire for the shared-stack corruption class: every legitimate RSP in
      this system lives in the identity-mapped low 4GiB, so a non-zero high half
      means the slot we just came through was overwritten with something else --
      typically another CPU's 32-bit value landing in a 64-bit slot's upper
      dword.  Catching it here names the entry instead of letting it surface
      later as an unrelated #PF on a wild address. */
   if ((current_rsp>>32)!=0) {
      static volatile unsigned long wildrsp_cnt=0;
      if (++wildrsp_cnt<=8) {
         char b[192];
         sprintf(b,"KSTACK-WILDRSP cpu=%d pid=%d rsp=0x%lx top=0x%lx acc=%d\n",
                 smp_cpu_id(),p?(int)p->processid:-1,
                 (unsigned long)current_rsp,
                 (unsigned long)(p?p->kstack_top:0),
                 p?p->accesslevel:-1);
         serial_puts(b);
      }
   }

   if (!p || p->accesslevel!=ACCESS_USER || !p->kstack_top)
      return;

   /* A PCB is claimed by exactly one CPU (on_cpu).  If this CPU is about to
      run kernel C on a kstack whose task is claimed elsewhere, two CPUs share
      one stack and each will overwrite the other's frames.  That corruption is
      otherwise indistinguishable from heap damage, so name it explicitly. */
   me=smp_cpu_id();
   if (p->on_cpu>=0 && p->on_cpu!=me) {
      static volatile unsigned long foreign_cnt=0;
      if (++foreign_cnt<=8) {
         char b[144];
         sprintf(b,"KSTACK-FOREIGN cpu=%d owner=%d pid=%d rsp=0x%lx top=0x%lx\n",
                 me,p->on_cpu,(int)p->processid,
                 (unsigned long)current_rsp,(unsigned long)p->kstack_top);
         serial_puts(b);
      }
      /* Do not record irq_user_rsp or reuse this PCB's kstack: the owner is
         still running it.  The wrapper has moved us onto this CPU's
         irq_safe_stack. */
      return;
   }

   /* A nested entry was already running on the kstack; it must not overwrite
      the user frame recorded by the entry that came off the user stack. */
   if (current_rsp>=(u64)(uintptr)p->kstack_base && current_rsp<p->kstack_top)
      return;

   /* From here we are switching RSP to kstack_top for a FRESH entry, so we are
      about to reuse the top of the stack.  KSTACK-FOREIGN above only catches a
      stale on_cpu; it misses the case that actually corrupts frames, where
      on_cpu is -1 or already reassigned but another CPU still has this PCB as
      its `current` and is therefore still executing on this same stack.  Two
      CPUs starting fresh frames at the same kstack_top overwrite each other,
      which surfaces as a 64-bit stack slot whose high half holds the other
      CPU's 32-bit smp_cpu_id() result.  Record who owns the stack and who is
      running the task, so the offending transition is identifiable. */
   {
      int j;
      for (j=0;j<cpu_count && j<MAX_CPUS;j++) {
         if (j==me || !cpus[j].online)
            continue;
         if ((PCB386 *)cpus[j].current!=p)
            continue;
         {
            static volatile unsigned long shared_cnt=0;
            if (++shared_cnt<=8) {
               char b[224];
               sprintf(b,"KSTACK-SHARED cpu=%d other=%d pid=%d on_cpu=%d "
                         "status=0x%x aff=%d rsp=0x%lx top=0x%lx "
                         "ctxload=%d/%d\n",
                       me,j,(int)p->processid,p->on_cpu,
                       (unsigned)p->status,p->cpu_affinity,
                       (unsigned long)current_rsp,(unsigned long)p->kstack_top,
                       ctx_load_in_progress[me],ctx_load_in_progress[j]);
               serial_puts(b);
            }
         }
      }
   }

   if (current_rsp<MEM_USER_STACK_GUARD || current_rsp>=MEM_USER_STACK)
      return;
   p->irq_user_rsp=current_rsp;
   p->irq_kframe=current_rsp;
}

/* PUSH_ALL frame RIP is at offset 120.  A leftover current that reset
   kstack_top left a non-canonical RIP; timerwrapper then #GP'd on iretq
   (cert rip=0x178818).  Fail closed instead of returning into smash. */
void irq_iretq_cr3_guard(u64 *frame)
{
   unsigned long cr3 = 0;
   unsigned long frsp;

   if (!frame)
      return;
   frsp = (unsigned long)(uintptr)frame;
   __asm__ __volatile__("movq %%cr3, %0" : "=r"(cr3));
   if (irq_iretq_frame_cr3_ok(frsp, cr3))
      return;
   {
      char b[192];
      sprintf(b, "IRETQ-BADCR3 cr3=0x%lx rsp=0x%lx proc=%s pid=%d\n",
              cr3, frsp,
              current_process && current_process->name
              ? current_process->name : "?",
              current_process ? (int)current_process->processid : -1);
      serial_puts(b);
   }
   serial_puts("IRETQ-BADCR3: kernel fault -> halt\n");
   while (1)
      __asm__ volatile ("hlt");
}

void irq_iretq_guard(u64 *frame)
{
   extern void exc_recover(void);
   u64 rip;
   int ok;

   irq_iretq_cr3_guard(frame);
   if (!frame)
      return;
   rip = frame[15];
   ok = irq_iretq_rip_ok((unsigned long)rip);
   if (ok)
      return;
   {
      char b[192];
      sprintf(b, "IRETQ-BADRIP rip=0x%llx cs=0x%llx rsp=0x%llx proc=%s pid=%d\n",
              (unsigned long long)rip,
              (unsigned long long)frame[16],
              (unsigned long long)(uintptr)frame,
              current_process && current_process->name
              ? current_process->name : "?",
              current_process ? (int)current_process->processid : -1);
      serial_puts(b);
   }
   if (leftover_iretq_must_not_kill_user((unsigned long)(uintptr)frame,
                                         (unsigned long)rip)) {
      serial_puts("IRETQ-BADRIP leftover-cpuirq\n");
      serial_puts("IRETQ-BADRIP: kernel fault -> halt\n");
      while (1)
         __asm__ volatile ("hlt");
   }
   if (current_process && current_process->accesslevel == ACCESS_USER) {
      exc_recover();
      /* not reached */
   }
   serial_puts("IRETQ-BADRIP: kernel fault -> halt\n");
   while (1)
      __asm__ volatile ("hlt");
}

/* irqwrap.S IRQ_KSTACK_ENTER walks these offsets without a C helper. */
static char irqwrap_offchk_cpu[(sizeof(cpu_local)==48 &&
   __builtin_offsetof(cpu_local,current)==16 &&
   __builtin_offsetof(cpu_local,kernel_stack)==32)?1:-1];
static char irqwrap_offchk_pcb[(
   __builtin_offsetof(PCB386,accesslevel)==0x4b8 &&
   __builtin_offsetof(PCB386,on_cpu)==0xc84 &&
   __builtin_offsetof(PCB386,kstack_base)==0xc88 &&
   __builtin_offsetof(PCB386,kstack_top)==0xc90)?1:-1];
static char irqwrap_userstack_chk[(
   MEM_USER_STACK_GUARD==0x3FD00000UL &&
   MEM_USER_STACK==0x40000000UL)?1:-1];
volatile char *irqwrap_offchk_use=&irqwrap_offchk_cpu[0];
volatile char *irqwrap_offchk_use2=&irqwrap_offchk_pcb[0];
volatile char *irqwrap_offchk_use3=&irqwrap_userstack_chk[0];

/* Diagnostic: name which user_fork_frame failure path fired, with the
    parent state that drove it.  Bounded so a failing fork storm cannot flood
    the serial oracle. */
 static void fork_frame_fail(const char *tag, PCB386 *parent, long code)
 {
    static volatile unsigned long n = 0;
    char b[192];
    if (n >= 24)
       return;
    n++;
    sprintf(b, "FORK-FAIL %s code=%d cpu=%d parent=%d lvl=%d private=%d thread=%d otherth=%d nlive=%d waitq_n=%d fds=%d frame_ok=%d\n",
           tag, (int)code, smp_cpu_id(),
           parent ? (int)parent->processid : -1,
           parent ? (int)parent->accesslevel : -1,
           parent ? (int)userpd_is_private(parent->pagedirloc) : -1,
           parent ? (int)(parent->status & PS_ATTB_THREAD) : -1,
           parent ? (int)fork_has_other_threads(parent) : -1,
           parent ? (int)parent->nlive : -1,
           parent ? (int)parent->waitq_n : -1,
           parent ? posix_fd_fork_ready(parent) : -1,
           1);
    serial_puts(b);
 }

 long user_fork_frame(u64 *frame)
 {
    PCB386 *parent=current_process;
   PCB386 *child=0;
   u64 *child_pml4=0;
   u64 *child_rax;
   u64 user_frame;
   DWORD flags,entry_flags;

   storeflags(&entry_flags);

   if (!parent || parent->accesslevel!=ACCESS_USER
       || (parent->status&PS_ATTB_THREAD)
       || !userpd_is_private(parent->pagedirloc)
         || fork_has_other_threads(parent)
         || parent->nlive+parent->waitq_n>=WAITQ_MAX
          || posix_fd_fork_ready(parent)<0) {
       restoreflags(entry_flags);
       fork_frame_fail("VALID", parent, -11);
       return -11;
    }

   /* Prefer the wrapper's PUSH_ALL pointer (this syscall). irq_user_rsp is
      only a fallback: it can still name a prior timer frame, and a child
      that iretq's that frame is 16 bytes off -- RIP reads as RFLAGS
      (UD64 rip=0x207). Keep interrupts off until the child's copy of this
      frame exists so a nested timer cannot clobber it mid-clone. */
   user_frame=(u64)(uintptr)frame;
   if (user_frame < (u64)MEM_USER_STACK - 0x100000ULL
       || user_frame + 0x90ULL > (u64)MEM_USER_STACK) {
      u64 alt=parent->irq_user_rsp;
      if (alt >= (u64)MEM_USER_STACK - 0x100000ULL
          && alt + 0x90ULL <= (u64)MEM_USER_STACK)
         user_frame=alt;
      else {
          restoreflags(entry_flags);
          fork_frame_fail("FRAME", parent, -13);
          return -13;
       }
    }
    parent->irq_user_rsp=user_frame;

   child_pml4=userpd_clone_cow((u64 *)(uintptr)parent->pagedirloc,
                               (unsigned long long)user_frame);
   if (!child_pml4) {
       restoreflags(entry_flags);
       fork_frame_fail("CLONE", parent, -12);
       return -12;
    }
    child_rax=(u64 *)userpd_resolve(child_pml4,
                                   (unsigned long long)user_frame+112);
   if (!child_rax)
      goto nomem;
   *child_rax=0;

   startints();

   child=(PCB386 *)malloc(sizeof(PCB386));
   if (!child)
      goto nomem;
   memset(child,0,sizeof(PCB386));
   if (!pcb_alloc_irq_kstack(child)) {
      free(child);
      child=0;
      goto nomem;
   }
   spin_init(&child->fd_lock);

   child->regs=parent->regs;
   child->regs.CR3=(DWORD)(uintptr)child_pml4;
   child->regs2=parent->regs2;
   storeflags(&flags);
   stopints();
   fpu_save(&parent->fpu);
   restoreflags(flags);
   memcpy(&child->fpu,&parent->fpu,sizeof(child->fpu));
   child->size=sizeof(PCB386);
   child->version=parent->version;
   child->pagedirloc=(DWORD *)(uintptr)child_pml4;
   child->owner=parent->processid;
   strcpy(child->name,parent->name);
   child->workdir=parent->workdir;
   child->accesslevel=parent->accesslevel;
   child->priority=parent->priority;
   child->syscallsize=parent->syscallsize;
   child->stackptr=parent->stackptr;
   child->knext=parent->knext;
   child->mmap_brk=parent->mmap_brk;
   child->putc=parent->putc;
   child->getc=parent->getc;
   child->outdev=parent->outdev;
   child->stdin=parent->stdin;
   child->ctty=parent->ctty;
   child->session=parent->session;
   child->pgrp=parent->pgrp;
   child->usercs=parent->usercs;
   child->dex32_signal=parent->dex32_signal;
   child->signaltable=parent->signaltable;
   child->cpu_affinity = user_procs_smp ? -1 : 0;
    child->on_cpu=-1;

    child->ctx.rip=(u64)(uintptr)fork_child_return;
   child->ctx.rsp=user_frame;
   child->ctx.rflags=0x202;
   child->ctx.cs=SYS_CODE_SEL;
   child->ctx.ss=SYS_DATA_SEL;
   child->ctx.cr3=(u64)(uintptr)child_pml4;

   if (parent->parameters) {
      int length=strlen(parent->parameters)+1;
      child->parameters=(char *)malloc(length);
      if (!child->parameters)
         goto fail;
      memcpy(child->parameters,parent->parameters,length);
   }
   if (!cloneprocessmemory_metadata(parent->meminfo,&child->meminfo))
      goto fail;
   if (posix_fd_clone_fork(child,parent)<0)
      goto fail;

   dex32_stopints(&flags);
   sync_entercrit(&processmgr_busy);
   child->processid=__sync_fetch_and_add(&nextprocessid, 1);
   totalprocesses++;
   parent->childwait++;
   parent->nlive++;
   ps_enqueue(child);
   sync_leavecrit(&processmgr_busy);
   dex32_restoreints(flags);
   restoreflags(entry_flags);
   return (long)child->processid;

fail:
   posix_fd_close_all(child);
   freeprocessmemory_metadata(child->meminfo);
   if (child->parameters)
      free(child->parameters);
   pcb_free_irq_kstack(child);
   free(child);
nomem:
    fork_frame_fail("NOMEM", parent, -12);
    userpd_free(child_pml4);
    restoreflags(entry_flags);
    return -12;
 }
 #endif

//duplicates a process using the legacy 32-bit paging path
DWORD forkprocess(PCB386 *parent){
#ifdef __x86_64__
   (void)parent;
   return (DWORD)-1;
#else
   int pages;
   DWORD *pagedir,*pg,flags;
   DWORD parentpd = parent->pagedirloc;
   PCB386 *pcb;
    
#ifdef DEBUG_FORK
   printf("fork process has been called.\n");
#endif

   pcb = (PCB386*) malloc(sizeof(PCB386));//Allocate space for PCB
   memcpy(pcb,parent,sizeof(PCB386));     //Initialize the new process by copying the parent process' PCB
   memset(pcb->fds,0,sizeof(pcb->fds));
   spin_init(&pcb->fd_lock);
   posix_fd_clone(pcb,parent);
   dex32_stopints(&flags);                //disable interrupts
   strcat(pcb->name,".fork");             //Add a 'fork' suffix to indicate that it was created by fork
   totalprocesses++;                      //Increase the total number of processes in the system
   pcb->size       = sizeof(PCB386);      //Save the size of the PCB
   pcb->processid  = __sync_fetch_and_add(&nextprocessid, 1);     //Set the process if of the new process
   pcb->owner      = parent->processid;   //Set the parent to the process id of the parent

   /*Allocate a new page directory*/
   pagedir=(DWORD*)mempop(); //obtain a physical address from the memory manager
   pg=(DWORD*)getvirtaddress((DWORD)pagedir); //convert physical address to a virtual address
   memset(pg,0,0x1000); //initialize the new page directory

   pcb->regs.CR3   = pagedir;
   pcb->pagedirloc = pagedir;

#ifdef DEBUG_FORK
   printf("copying memory..\n");
#endif

   disablepaging();

   dex32_copy_pg(pagedir,parentpd);

   maplineartophysical((DWORD*)pagedir,(DWORD)SYS_PAGEDIR_VIR,(DWORD)pagedir    /*,stackbase*/,1);

   maplineartophysical((DWORD*)pagedir,(DWORD)SYS_PAGEDIR2_VIR,
                        (DWORD)pagedir[SYS_PAGEDIR_VIR >> 22]&0xFFFFF000,1);

   maplineartophysical((DWORD*)pagedir,(DWORD)SYS_PAGEDIR3_VIR,
                        (DWORD)pagedir[SYS_PAGEDIR_VIR >> 22]&0xFFFFF000,1);

   maplineartophysical((DWORD*)pagedir,(DWORD)SYS_KERPDIR_VIR,(DWORD)pagedir1    /*,stackbase*/,1);

   enablepaging();
    
#ifdef DEBUG_FORK
   printf("done. adding to process queue\n");
#endif

   //copies the memory allocation information of the parent to the child
   //(forked processes have the same virtual memory map at time of fork)
   copyprocessmemory(parent->meminfo,&pcb->meminfo);

   //add to the ready queue
   ps_enqueue(pcb);

   dex32_restoreints(flags);
    
#ifdef DEBUG_FORK
   printf("fork done.\n");
#endif

   return pcb->processid;
#endif
};


/*
 * Function that is responsible for creating USER processes.
 * Called by the different modules. The modules represent the 
 * supported executable formats: PE, ELF, COFF, B32, etc. (see kernel/module)
 * This function creates a new PCB and set some of the fields
 * of the new PCB to the values passed as parameters.
 *
 */
DWORD createprocess(
                     void *ptr,
                     char *name, 
                     DWORD *pagedir,
                     process_mem *pmem,
                     void *stack, 
                     DWORD stacksize, 
                     DWORD syscallsize, 
                     void *dex32_signal,
                     char *params, 
                     char *workdir, 
                     PCB386 *parent
                  ){
   int pages;
   DWORD flags , *pg;

   PCB386 *temp=(PCB386*)malloc(sizeof(PCB386));      //allocate the PCB for the process
   memset(temp,0,sizeof(PCB386));                     //Initialize by zeroing it out
#ifdef __x86_64__
   if (!pcb_alloc_irq_kstack(temp)) {
      free(temp);
      return 0;
   }
#endif
   spin_init(&temp->fd_lock);
   fpu_init_default(&temp->fpu);
   temp->before=current_process;                      //add it after the current process 
   strcpy(temp->name,name);                           //set the name of the process
   totalprocesses++;                                  //increase the total number of processes in the system
   temp->size         = sizeof(PCB386);               //set the size to the size of the PCB
   temp->processid    = __sync_fetch_and_add(&nextprocessid, 1);              //set the process id of this process
   temp->accesslevel  = ACCESS_USER;                  //Indicates that the process is a USER process
   temp->meminfo      = pmem;                         //set the memory information
   temp->owner        = parent->processid;            //set the parent id
   temp->dex32_signal = dex32_signal;
   temp->op_success   = 1; 
   temp->arrivaltime  = getprecisetime();             //set the time the process was created 
   temp->stdin        = parent->stdin;                //set stdin to be the same as parent
   {
      posix_fd_clone(temp,parent);
      temp->ctty = parent->ctty;
      temp->session = parent->session;
      temp->pgrp = parent->pgrp;
      temp->usercs = USER_CODE;
   }

   memcpy(&temp->regs2,&ps_kernelfpustate,sizeof(ps_kernelfpustate));

   //get the working directory of this process
   if (workdir==0){
      temp->workdir  = parent->workdir;
   }else{
      temp->workdir  = vfs_searchname(workdir);
        
      //validate workdir
      if (temp->workdir == 0) 
         temp->workdir =parent->workdir;
   };

   //use the same screen as the one who called, the parent
   temp->outdev=parent->outdev;

   //find the parent process and increment it's waiting state
   parent->childwait++;
   parent->nlive++;

   temp->knext       = userheap; //set up the programs' initial break
   temp->mmap_brk    = (char *)(uintptr)MEM_USER_HEAP_LIMIT;
   temp->pagedirloc  = pagedir;  //set the memory page dir
    
   /*Set up the CPU registers*/
   memset(&temp->regs,0,sizeof(saveregs));
   temp->regs.EIP    = (DWORD)(uintptr)ptr;
   temp->regs.ESP    = (DWORD)(uintptr)stack;
   temp->stackptr    = (void*)(uintptr)stack;
   temp->regs.CR3    = (DWORD)(uintptr)pagedir;
   temp->regs.ES     = USER_DATA;
   temp->regs.SS     = USER_DATA;
   temp->regs.CS     = USER_CODE;
   temp->regs.DS     = USER_DATA;
   temp->regs.FS     = USER_DATA;
   temp->regs.GS     = USER_DATA;
   temp->regs.SS0    = SYS_STACK_SEL;
   temp->syscallsize=syscallsize;

   /* Software context. usercs is USER_CODE; actual CS stays kernel until
      TSS.rsp0 + iretq ring-3 is wired (int 0x30 DPL is already 3). */
   {
      uintptr top = (uintptr)stack;
      top &= ~(uintptr)15;
      top -= 8; /* SysV entry RSP ≡ 8 (mod 16) */
      temp->ctx.rip = (u64)(uintptr)ptr;
      temp->ctx.rsp = (u64)top;
      temp->ctx.rflags = 0x202;
      temp->ctx.cs = SYS_CODE_SEL;
      temp->ctx.ss = SYS_DATA_SEL;
      temp->ctx.cr3 = (u64)(uintptr)pagedir;
      temp->regs.ESP = (DWORD)top;
   }
   
   //set up the program parameters (command line arguments)
   if (params!=0){
      int plen = strlen(params) + 1;
      if (plen < 4) plen = 4;
      temp->parameters=(char*)malloc(plen);
      if (temp->parameters)
         strcpy(temp->parameters,params);
      temp->knext = userheap + 16;
   }else{
      temp->parameters=0;
      temp->knext = userheap + 16;
   }

   //prepare to enter critical section
   sync_entercrit(&processmgr_busy);
   dex32_stopints(&flags);

   //allocate memory for the system call stack
   dex32_commitblock((DWORD)(uintptr)syscallstack,syscallsize,&pages,pagedir,PG_WR);
   addmemusage(&(temp->meminfo),syscallstack,pages);
   //set up the initial stack pointer
   temp->regs.ESP0=(DWORD)((uintptr)syscallstack+syscallsize-4);
   //  temp->regs.ESP0=0x9FFFE;
   temp->regs.EFLAGS=0x202;
   temp->semhandle=0;
  /* User processes stay on the BSP until waitpid/exit migration is fully
       hardened; the parallel self-host closure opts in via user_procs_smp. */
    temp->cpu_affinity = user_procs_smp ? -1 : 0;
    temp->on_cpu = -1;

#ifndef __x86_64__
   //some functions that a character device uses
   disablepaging();
   dex32_copy_pagedirU(pagedir,pagedir1);
   enablepaging();
        

   pg = (DWORD*)getvirtaddress((DWORD)pagedir); /*convert to a virtual address so that
                                                 we could use it here without disabling
                                                 the paging mechanism of the 386*/

   maplineartophysical2((DWORD*)pg, (DWORD)SYS_PAGEDIR_VIR,(DWORD)pagedir    /*,stackbase*/,1);
    
   maplineartophysical2((DWORD*)pg, (DWORD)SYS_PAGEDIR2_VIR,
                        (DWORD)pg[SYS_PAGEDIR_VIR >> 22]&0xFFFFF000,1);
    
   maplineartophysical2((DWORD*)pg, (DWORD)SYS_PAGEDIR3_VIR,
                        (DWORD)pg[SYS_PAGEDIR_VIR >> 22]&0xFFFFF000,1);
    
   maplineartophysical2((DWORD*)pg, (DWORD)SYS_KERPDIR_VIR,(DWORD)pagedir1    /*,stackbase*/,1);
#else
   (void)pg;
#endif

   //add the process to the list of processes
   /* Compile jobs (in-OS TinyCC) are the workhorses of the self-host
      pipeline.  The parent console blocks in dex32_waitpid() which
      re-invokes the scheduler on every timer tick; under priority RR
      the child only gets ~50% of the CPU (and each context switch costs
      fxsave/fxrstor + CR3 reload).  Giving user processes priority 1
      lets the compile run uninterrupted while the console still gets its
      slice on the ~20 voluntary taskswitch() calls it makes per second. */
   temp->priority = 1;

   ps_enqueue(temp);

   //end of critical section — exit the crit section WHILE interrupts are
   //still disabled so the console cannot be preempted mid-leavecrit.
   //Do NOT re-enable interrupts here: the caller (user_execp) will
   //re-enable them after entering dex32_waitpid().  Re-enabling here
   //opens a window where a timer IRQ fires, schedule_from_timer runs,
   //and the scheduler picks the newly-created child (priority 1) before
   //the parent is in the waitpid loop — corrupting the context switch.
   sync_leavecrit(&processmgr_busy);
   dex32_restoreints(flags);

   return temp->processid;
};


DWORD dex32_asyncproc(saveregs *r,void *entrypoint,char *name,DWORD stacksize){
   PCB386 *temp=(PCB386*)malloc(sizeof(PCB386));

   temp->before=current_process;
   strcpy(temp->name,name);
   totalprocesses++;
   temp->processid=__sync_fetch_and_add(&nextprocessid, 1);
   temp->accesslevel=ACCESS_SYS;
   temp->owner=1;
   temp->knext=knext;
    temp->pagedirloc=pagedir1;
    memset(temp,0,sizeof(saveregs));
    temp->held_crit_n=0;
    temp->crits_freed=0;
    temp->regs.EIP=(DWORD)entrypoint;
   temp->stackptr=malloc(stacksize);
   temp->regs.ESP=(DWORD)(temp->stackptr+stacksize-4);
   temp->stackptr=(void*)temp->regs.ESP;
   temp->regs.CR3=(DWORD)pagedir1;
   temp->regs.EAX=r->EAX;
   temp->regs.EBX=r->EBX;
   temp->regs.ECX=r->ECX;
   temp->regs.EDX=r->EDX;
   temp->regs.ESI=r->ESI;
   temp->regs.EDI=r->EDI;
   temp->regs.ES=r->ES;
   temp->regs.SS=r->SS;
   temp->regs.CS=r->CS;
   temp->regs.DS=r->DS;
   temp->regs.FS=r->FS;
   temp->regs.GS=r->GS;
   temp->regs.EFLAGS=0x200;

   sync_entercrit(&processmgr_busy);
   //add to the list
   ps_enqueue(temp);
   sync_leavecrit(&processmgr_busy);
};

void ps_seterror(int error){
   current_process->lasterror =  error;
};

int ps_geterror(){
   return current_process->lasterror;
};

//sends a message to all active processes
int broadcastmessage(DWORD sender,DWORD mes,DWORD data){
   PCB386 *ptr;
   int total,i;
    
   total = get_processlist(&ptr);
    
   for (i=0; i < total ; i++){
      sendmessageEX(sender,ptr[i].processid,mes,data);
   } ;
   free(ptr);
   return 1;
};

int sendmessage(DWORD pid,DWORD mes,DWORD data){
   return sendmessageEX(current_process->processid,pid,mes,data);
};

//sends a message to another process
int sendmessageEX(DWORD source, DWORD pid, DWORD mes, DWORD data){
   DWORD cpuflags;
   PCB386 *ptr = (PCB386*)ps_findprocess(pid);

   if (ptr!=-1){

      int index = ptr->meshead;

      if (ptr->mestotal >= MAX_MESSAGE-1) 
         return IPCSTAT_FULL;
        
      dex32_stopints(&cpuflags);
        
      ptr->mesq[index].message = mes;
      ptr->mesq[index].data = data;
      ptr->mesq[index].sender = source;
      ptr->mesq[index].receiver = pid;

      ptr->mestotal++;
      ptr->meshead++;

      if (ptr->meshead >= MAX_MESSAGE) 
         ptr->meshead=0;
        
      dex32_restoreints(cpuflags);
        
      return IPCSTAT_OK;
   };
   return IPCSTAT_ERROR;
};

//gets a message
int getmessage(DWORD *source, DWORD *mes, DWORD *data){
   PCB386 *ptr=current_process;
   DWORD cpuflags;
    
   int index=ptr->curmes;
   if (ptr->mestotal<=0) 
      return 0;

   dex32_stopints(&cpuflags);

   *source = ptr->mesq[index].sender;
   *mes = ptr->mesq[index].message;
   *data = ptr->mesq[index].data;
   ptr->curmes++;
   ptr->mestotal--;
   if (ptr->mestotal<0) 
      ptr->mestotal=0;
   
   if (ptr->curmes>=MAX_MESSAGE) 
      ptr->curmes=0;
    
   dex32_restoreints(cpuflags);
    
   return 1;
};

/*
 * Creates a kernel thread. A kernel thread is owned and managed by the kernel.
 * It uses the address space of the kernel process. 
 * 
 *
*/
DWORD createkthread_on_cpu(void *ptr,char *name,DWORD stacksize,int cpu){
   PCB386 *temp;
   DWORD cpuflags;
   void *stackbase;

   if (cpu < -1 || cpu >= MAX_CPUS)
      return 0;
   temp=(PCB386*)malloc(sizeof(PCB386));
   if (!temp)
      return 0;
   memset(temp,0,sizeof(PCB386));
   temp->before=current_process;
   strcpy(temp->name,name);
   totalprocesses++;
   temp->size        = sizeof(PCB386);
   temp->processid   = __sync_fetch_and_add(&nextprocessid, 1);
   temp->accesslevel = ACCESS_SYS;
   temp->owner       = getprocessid();
   temp->status      |= PS_ATTB_THREAD;
   temp->knext       = knext;
   temp->pagedirloc  = pagedir1;
   temp->workdir     = current_process->workdir;

   stackbase = malloc(stacksize);
   if (!stackbase) {
      free(temp);
      return 0;
   }
   {
      uintptr top = (uintptr)stackbase + stacksize;
      top &= ~(uintptr)15; /* 16-byte align */
      top -= 8;            /* SysV: entry RSP ≡ 8 (mod 16) */
      memset(&temp->regs,0,sizeof(saveregs));
      temp->regs.EIP    = (DWORD)(uintptr)ptr;
      temp->regs.ESP    = (DWORD)top;
      temp->stackptr    = stackbase;
      temp->regs.CR3    = (DWORD)(uintptr)pagedir1;
      temp->regs.ES     = SYS_DATA_SEL;
      temp->regs.SS     = SYS_STACK_SEL;
      temp->regs.CS     = SYS_CODE_SEL;
      temp->regs.DS     = SYS_DATA_SEL;
      temp->regs.FS     = SYS_DATA_SEL;
      temp->regs.GS     = SYS_DATA_SEL;
      temp->regs.EFLAGS = 0x202;

      /* Seed software context immediately (x86_64). */
      temp->ctx.rip = (u64)(uintptr)ptr;
      temp->ctx.rsp = (u64)top;
      temp->ctx.rflags = 0x202;
      temp->ctx.cs = SYS_CODE_SEL;
      temp->ctx.ss = SYS_DATA_SEL;
      temp->ctx.cr3 = (u64)(uintptr)pagedir1;
      fpu_init_default(&temp->fpu);
   }

   temp->arrivaltime = getprecisetime();
   temp->stdin       = current_process->stdin;
   {
      temp->ctty = current_process->ctty;
      temp->session = current_process->session;
      temp->pgrp = current_process->pgrp;
   }
   temp->cpu_affinity = cpu;
   temp->on_cpu = -1;

   sync_entercrit(&processmgr_busy);
   dex32_stopints(&cpuflags);
   ps_enqueue(temp);
   dex32_restoreints(cpuflags);
   sync_leavecrit(&processmgr_busy);
    
   return temp->processid;
};

DWORD createkthread(void *ptr,char *name,DWORD stacksize){
   return createkthread_on_cpu(ptr,name,stacksize,-1);
}

//change the name of a process
int ps_changename(const char *name, int pid){
   PCB386 *ptr;
   sync_entercrit(&processmgr_busy);

   ptr=ps_findprocess(pid);
   if (ptr!=-1) 
      strcpy(ptr->name,name);

   sync_entercrit(&processmgr_busy);
};

//decrements the wait status of the parent
//used for creating services
DWORD dex32_setservice(){
   PCB386 *ptr;
   sync_entercrit(&processmgr_busy);
   ptr = ps_findprocess(current_process->owner);
   if (ptr != -1){
      ptr->childwait = 0;
   };
   sync_leavecrit(&processmgr_busy); 
};

//exit process
DWORD dex32_exitprocess(DWORD ret_value){
   dex32_killkthread(current_process->processid);
   while (1);
      ;
};

//used to kill kernel (Ring0) threads only!!!
DWORD dex32_killkthread(DWORD processid){
   PCB386 *ptr;
   PCB386 *end;
   DWORD flags;

   sync_entercrit(&processmgr_busy);

   //get a pointer to the PCB
   ptr = bridges_ps_findprocess(processid);

   //stop interrupts
   dex32_stopints(&flags);

   if (ptr != -1){
      if (!ptr->status & PS_ATTB_UNLOADABLE){

         PCB386 *parent;

         //kernel thread
         if (ptr->accesslevel == ACCESS_SYS)
            free(ptr->stackptr);
                
         if (ptr->semhandle != 0)
            set_semaphore(ptr->semhandle,SIG_TERM);

         wait_queue_cancel(ptr);
         ps_dequeue(ptr);

         free(ptr);
         dex32_restoreints(flags);

         sync_leavecrit(&processmgr_busy);

         return 1;

      };

   };

   dex32_restoreints(flags);
   sync_leavecrit(&processmgr_busy);
   return 0;
}

/*puts the current process to sleep for a desired amount of time
  in milliseconds*/
void sleep(DWORD val){
    current_process->waiting = val;
};

/* Called in taskswitcher() to terminate a process and perform cleanup. 
 * Also performs garbage collection (reclaims memory 
 * used by the application).
*/
DWORD kill_process(DWORD processid){
   PCB386 *ptr,*parentptr=0;

   sync_entercrit(&processmgr_busy);                           //Try to enter the critical section

   ptr = bridges_ps_findprocess(processid);                    //obtain a handle to the PCB given id

   if (ptr!=-1){                                               //PCB exists

      if (! (ptr->status & PS_ATTB_UNLOADABLE) ){

           PCB386 *parent;

           /* self_exit_current owns teardown of a DYING PCB.  After it
              clears on_cpu but before dequeue, a remote kill used to treat
              the victim as off-CPU, dequeue a second time with stale
              next/before, and free the live PCB (RLBAD / PF64 in scheduler). */
           if (ptr->status & PS_ATTB_DYING) {
              sync_leavecrit(&processmgr_busy);
              return 1;
           }

           /* A victim that is live on another CPU must self-exit there. */
           if (ptr->on_cpu >= 0) {
              ptr->status |= PS_ATTB_DYING;
              sync_leavecrit(&processmgr_busy);
              {
                 spin_irq_flags_t f = spin_lock_irqsave(&sigterm_lock);
                 if (!sigterm)
                    sigterm = ptr->processid;
                 spin_unlock_irqrestore(&sigterm_lock, f);
              }
              return 1;
           }

           /* A blocked victim may still own crits it acquired before it
              blocked. Release them before teardown so closeallfiles() and the
              heap do not spin on a dead owner. */
           if (ptr->on_cpu < 0)
              sync_release_process_crits(ptr, (ptr->processid & 0x007FFFFF) + 1);

          kill_children(processid);                             //kill children processes first

         if (ptr->accesslevel == ACCESS_SYS){                  //a kernel process/thread
            dex32_killkthread(ptr);                            
            sync_leavecrit(&processmgr_busy);
            return 1;
         };

         if ( ptr->status & PS_ATTB_THREAD ){                  //a user thread 
            kill_thread(ptr);                                                                                
            sync_leavecrit(&processmgr_busy);
            return 1;
         };

         while (closeallfiles(ptr->processid)==1)              //close all opened files by the process
            ;
         posix_fd_close_all(ptr);

         //locate the parent process and decrement its waiting
         //status...important for the dex32_wait() function
         parent = ps_findprocess(ptr->owner);
           if (parent != (PCB386 *)-1) {
              parent->childwait = 0;
              /* Publish the status BEFORE ps_dequeue() below: sys_waitpid
                 treats "PCB no longer findable" as proof that a status has
                 already been queued. */
              waitq_publish(parent, (int)ptr->processid, ptr->exit_status);
              if (parent != ptr)
                 waitpid_notify_parent(parent);
           }

          /* Remove the victim from the ready queue before touching its tables
             so no other CPU can reschedule it mid-teardown (remote-kill path
             with user_procs_smp). Then flush any CPU still running with its
             private PML4 so no stale TLB entries survive the free below. */
          wait_queue_cancel(ptr);
          ps_dequeue(ptr);
 #ifdef __x86_64__
          if (!(ptr->status & PS_ATTB_THREAD)
              && userpd_is_private(ptr->pagedirloc))
             (void)smp_tlb_shootdown((u64)(uintptr)ptr->pagedirloc);
 #endif

          if (ptr->accesslevel == ACCESS_SYS)                   //deallocate the stack pointer
             free(ptr->stackptr);
#ifdef __x86_64__
          pcb_free_irq_kstack(ptr);
#endif
            
         /* 
          * Perform memory garbage collection if necessary
          * This includes freeing up memory associated to the process.
          */
         if (ptr->meminfo != 0)
            freeprocessmemory(ptr->meminfo,(DWORD*)ptr->pagedirloc); //

#ifdef __x86_64__
         /* Reclaim a private PML4 whenever CR3 is a pool frame.
            User ELFs enter with kernel CS but still own the directory.
            Threads share the parent's PML4 — only the non-thread PCB
            frees it. */
         if (!(ptr->status & PS_ATTB_THREAD)
             && userpd_is_private(ptr->pagedirloc))
            userpd_free((u64 *)(uintptr)ptr->pagedirloc);
#endif

         /**
          * For user processes
          */
         if (!(ptr->status & PS_ATTB_THREAD) && (ptr->accesslevel != ACCESS_SYS) ) {

#ifdef __x86_64__
            /* userpd_free() above already returned the private tables. */
#else
            //free the page tables used by the application
            dex32_freeuserpagetable((DWORD*)ptr->pagedirloc);

#ifdef MEM_LEAK_CHECK
            printf("1 page freed (page directory).\n");
#endif
            //return it back for others to use
            mempush(ptr->pagedirloc);
#endif
         };
         
         {
             //free command line arguments
             if (ptr->parameters!=0)
                free(ptr->parameters);
             ptr->parameters = 0;

             if (ptr->stdout!=0) {
                free(ptr->stdout);
             };
             ptr->stdout = 0;

             //deallocate the PCB for the process
             free(ptr);

             sync_leavecrit(&processmgr_busy);
          }

          //process successfully killed
          return 1;
      };
   }; 
   sync_leavecrit(&processmgr_busy);
   return 0;
};


//Kill user threads
DWORD kill_thread(PCB386 *ptr){
   DWORD flags;
   PCB386 *parent;
   dex32_stopints(&flags);

   kill_children(ptr->processid); //kill the children of this thread first!!cascade kill

   /*createuthread()/createthread() incremented the parent's childwait;
     undo it here so dex32_wait() and thread_join() are not skewed by
     threads that have already terminated.*/
   parent = ps_findprocess(ptr->owner);
   if (parent != (PCB386*)-1 && parent->childwait > 0)
      parent->childwait--;
    
   //Tell the scheduler to remove this thread from the ready queue
   wait_queue_cancel(ptr);
   ps_dequeue(ptr);

   if (ptr->stackptr0!=0)
        free(ptr->stackptr0);

   free(ptr);
   dex32_restoreints(flags);
   return 1;
   ;
};

//Iterates over the process list to terminate children processes
DWORD kill_children(DWORD processid){
   PCB386 *ptr;
   sync_entercrit(&processmgr_busy);

   ptr = bridges_ps_findprocess(processid);

   if (ptr!=-1){

      if (ptr->owner==processid && !( ptr->status&PS_ATTB_UNLOADABLE ) ){
         //kill_thread(ptr);
         kill_process(ptr->processid);
         sync_leavecrit(&processmgr_busy);
         return 1;
      };
   };

   sync_leavecrit(&processmgr_busy); 
   return 0;
};

//called when a process wishes to terminate itself
DWORD exit(DWORD val){
    current_process->exit_status = (int)(val & 0xff);
    /* Self-exit must be served by the CPU that is actually running this
       process.  Publishing the pid in the global sigterm mailbox let a
       different CPU consume the request and call kill_process() on a PCB
       that was still live, leaving a stale zombie_free pointer behind and
       double-freeing the PCB/metadata later. */
    self_exit_current();
    return 0;
};

/*
 * Kills a process with the specified pid, leaves all files
 * open. Called by the dkill command in kernel/console/console.c
*/
void ps_user_kill(int pid){
   if (ps_findprocess(pid) != -1){
      sigterm = pid;
      taskswitch();  
   };
};


DWORD suspendsem=0;        //global flag to suspend semaphore for busy waiting
DWORD semactive=0;         //global glag wether semaphore is active for busy waiting

//creates a semaphore to be used by the system
DWORD create_semaphore(DWORD val){

   semaphore *sem=(DWORD*)malloc(sizeof(semaphore));
   //generate a handle value for the semaphore, the semaphores
   //location in memory should suffice
   sem->handle=(DWORD)sem;
   //insert the sempahore into memory
   sem->owner=current_process->processid;
   sem->data=val;

   //This is a critical section so we must stop other processes
   //from creating their own semaphores in the middle of
   //creating this semaphore!
   while (semactive)
      ;
   
   while (suspendsem)
      ;

   suspendsem=1;
   sem->next=semaphore_head->next;
   semaphore_head->next->prev=sem;
   semaphore_head->next=sem;
   sem->prev=semaphore_head;
   suspendsem=0;
   ;
};

/*
* Places the list of processes to buf, get_processlist is responsible
* for allocating the necessary size required to store the list 
* Returns: The total nubmer of processes in buf
*/
int get_processlist(PCB386 **buf){
   int total;
   devmgr_scheduler_extension *cursched;
        
   cursched = (devmgr_scheduler_extension*)extension_table[CURRENT_SCHEDULER].iface;
   
   if (cursched->ps_listprocess == 0){
      printf("ERROR: schduler extension module does not support ps_listprocess()..\n");
      return 0;
   };

   //begin mutual exclusion
   sync_entercrit(&processmgr_busy);
    
   total = bridges_call((devmgr_generic*)cursched,&cursched->ps_listprocess,0,0,0);
   *buf = (PCB386*) malloc( sizeof(PCB386) * total );
    
   //make an intermodule call to the scheduler
   bridges_call((devmgr_generic*)cursched,&cursched->ps_listprocess, *buf, sizeof(PCB386), total);
    
   sync_leavecrit(&processmgr_busy);
    
   return total;
};

//returns the processid of a process based on its name
int findprocessname(const char *name){
   PCB386 *ptr;
   int total, i, retval = -1;
    
   total = get_processlist(&ptr);
  
   //Access to process list must be synchronized since a lot of 
   //other process are accessing it also
  
   sync_entercrit(&processmgr_busy);

   //the critical section    
   for (i = 0; i < total ; i++){
      if ( strcmp(ptr[i].name,name) == 0 ){
         retval = ptr[i].processid;       
         break;
      };    
   };
    
   sync_leavecrit(&processmgr_busy);
    
   free(ptr);
   return retval;
};

//returns the PCB386 structure of a process based on its process id
PCB386 *ps_findprocess(DWORD processid){
   PCB386 *ptr;
   PCB386 *end;
   DWORD flags;
   int total,i;
   
   sync_justwait(&processmgr_busy);
   
   return bridges_ps_findprocess(processid);
   return (PCB386*)-1;  //will this ever be called??
};

//returns the name of a process given a process id
int dex32_getname(DWORD processid, int bufsize, char *s){
   PCB386 *ptr;

   sync_justwait(&processmgr_busy);

   ptr = ps_findprocess(processid);
   
   if (ptr != -1){
      int i;

      //copy the name of the process to s, character by character
      for (i=0;i < bufsize&&ptr->name[i]; i++){
         s[i]=ptr->name[i];
      }
      s[i]=0;
      return 1;
   };
   return 0;
};

/* Copy process cmdline into the caller's buffer (identity-mapped). */
unsigned long dex32_getparametersinfo(char *buf){
   if (buf && current_process->parameters)
      strcpy(buf, current_process->parameters);
   else if (buf)
      buf[0] = 0;
   return 1;
};

//waits till a child process terminates
int dex32_wait(){
   int waitval;
   if (current_process->childwait==0) 
      return 1;
   
   waitval=current_process->childwait;
   
   while (current_process->childwait >= waitval && current_process->childwait != 0)
      ;
};

/*Waits until the thread with the given id terminates. Returns 0 on
  success (including a thread that has already exited), -1 if the id
  does not refer to a thread owned by the calling process. Like
  dex32_wait() above, this is a spin-wait that relies on preemption;
  it must therefore be registered with API_REQUIRE_INTS so that the
  timer keeps firing while we spin.*/
int dex32_thread_join(DWORD threadid){
   PCB386 *ptr;

   ptr = ps_findprocess(threadid);
   if (ptr == (PCB386*)-1) return 0;            //already gone: join trivially succeeds
   if (!(ptr->status & PS_ATTB_THREAD)) return -1;       //not a thread
   if (ptr->owner != current_process->processid) return -1; //not our thread

   //spin until the thread disappears from the process table.
   //The PCB is freed by kill_thread(), so existence of the id
   //is the termination signal.
   while (ps_findprocess(threadid) != (PCB386*)-1)
      ;

   return 0;
};


//wait for a given process given its id
int dex32_waitpid(int pid,int status){
    (void)status;
    while (1) {
       PCB386 *self = current_process;
       /* If the child is already in our waitq it has exited.  Its PCB is
          only reclaimed on a later timer pass (deferred zombie free), so
          ps_findprocess() would still return the live (zombie) PCB and we
          would keep re-switching into a dead process forever.  Return now
          instead of re-entering the zombie. */
       if (self != (PCB386*)-1 && waitq_has(self, pid))
          return 0;
       PCB386 *p = ps_findprocess(pid);
       if (p == (PCB386*)-1)
          return 0;
       /* Directly switch to the child. The ctx_load_in_progress guard
          in context_load prevents a nested ps_switchto from clobbering
          the child's saved ctx if a timer fires during context_load. */
       ps_switchto(p);
    }
};


//---------------------- start ofsemaphore related stuff ------------------

//Returns a pointer to a semaphore object given a handle
void *findsemaphore(DWORD handle){
   semaphore *ptr=semaphore_head;
   do{
      if (ptr->handle==handle){
         return (void*)ptr;
      };
      ptr=ptr->next;
   } 
  
   //find the head?? 
   while (ptr!=semaphore_head)
      ;
   
   return 0;
};

//gets the value stored in a semaphore
DWORD get_semaphore(DWORD handle){
   //search semaphores for a match
   semaphore *ptr;
   while (suspendsem)
      ;
   
   semactive=1;
   ptr=(semaphore*)findsemaphore(handle);

   if (ptr==0){
      semactive=0;
      return 0;
   };

   semactive=0;

   return ptr->data;
   ;
};

//sets a value stored in a semaphore
DWORD set_semaphore(DWORD handle, DWORD val){
   semaphore *ptr;
   while (suspendsem)
      ;

   semactive=1;
   ptr=(semaphore*)findsemaphore(handle);
   if (ptr==0) {
      semactive=0;
      return 0;
   };
   semactive=0;
   return (ptr->data=val);
    ;
};

//destroy a semaphore
DWORD free_semaphore(DWORD handle){
   semaphore *ptr;
   
   while (suspendsem)
      ;
   
   semactive=1;
   ptr=(semaphore*)findsemaphore(handle);
   if (ptr == 0) {
      semactive=0;
      return 0;
   };
   semactive=0;

   //reconnect the missing pieces
   ptr->prev->next=ptr->next;
   ptr->next->prev=ptr->prev;
   free(ptr);

   return 1;
    ;
};

//---------------------- end semaphore related stuff ------------------


/*
 * Kill a process or thread.
 * This function is called by the "kill" command in kernel/console/console.c
 * 
 */
DWORD dex32_killkthread_name(char *processname){
   PCB386 *ptr;
   int total, processid , i;

   //Retrieve the list of processes maintained by the scheduler
   //the process list will be pointed to by ptr.
   //get_processlist() is defined in this file.
   total = get_processlist(&ptr);     
   
   //iterate over the process list looking for processname 
   for (i=0; i < total; i++){
      //if the processname matches the one in the list and the process can be killed, 
      //the sigterm global variable is set to the process id to inform taskswitcher() that
      //a process will be terminated
      if (strcmp(ptr[i].name, processname) == 0 && !(ptr[i].status & PS_ATTB_UNLOADABLE) ){
         sigterm = ptr[i].processid;      //inform the taskswitcher that a process will be terminated
         free(ptr);
         return 1;
      };
   };
    
   //maybe the paramater given was a pid?? (fallback). Convert to a number
   processid = atoi(processname);

   //check the process list again
   for (i=0; i < total; i++){
      if ( (ptr[i].processid == processid) && !(ptr[i].status&PS_ATTB_UNLOADABLE) ){
         sigterm=ptr[i].processid;       //inform taskswitcher() that a process is to be terminated
         free(ptr);
         return 1;
      };
   };

   return 0;
}

//adds memory usage to a processes memory descriptor
void addmemusage(process_mem **memptr, DWORD vaddr, DWORD pages){
   process_mem *ptr = (process_mem*)malloc(sizeof(process_mem));
   ptr->next = *memptr;
   *memptr = ptr;
   ptr->vaddr = vaddr;
   ptr->pages = pages;
};

/*This function determines the amount of memory a process
 *is currently using*/
DWORD getprocessmemory(process_mem *memptr,DWORD *pagedir){
   DWORD total_memory=0;
   process_mem *tmpr = memptr; //point to the head of list of mem used
   
   while (tmpr->vaddr != 0 && tmpr != 0){
      process_mem *tmpr2 = tmpr->next;
      total_memory += getmultiple((void*)tmpr->vaddr, pagedir, tmpr->pages);
      tmpr = tmpr2;
   };
   return total_memory;
};

/* Free process memory 
 */
void freeprocessmemory(process_mem *memptr, DWORD *pagedir){
   process_mem *tmpr = memptr;//point to the head

#ifdef MEM_LEAK_CHECK
   printf("freeprocessmemory called.\n");
#endif
   do{
      process_mem *tmpr2=tmpr->next;
      freemultiple((void*)tmpr->vaddr,pagedir,tmpr->pages);
      free(tmpr);
      tmpr=tmpr2;
   }
   while (tmpr!=0);

#ifdef MEM_LEAK_CHECK
   printf("freeprocessmemory ended.\n");
#endif
};

/*Copies memory usage information. Used by the fork() command*/
void copyprocessmemory(process_mem *memptr, process_mem **destmemptr){
   process_mem *tmpr = memptr; //point to the head
   while (tmpr->vaddr!=0&&tmpr!=0){
      process_mem *tmpr2=tmpr->next;
      addmemusage(destmemptr, tmpr->vaddr,tmpr->pages);
      tmpr=tmpr2;
   };
};

//does nothing
void halt(){
    while (1)
      ;
};

/* Valid FXSAVE image — all-zero fxrstor #GPs. */
void fpu_init_default(fpu_state *s){
   memset(s, 0, sizeof(*s));
   *(u16*)(s->fx + 0) = 0x037F;   /* FCW */
   *(u32*)(s->fx + 24) = 0x1F80;  /* MXCSR */
}

void ps_set_affinity(int pid, int cpu){
   PCB386 *p = ps_findprocess(pid);
   if (p != (PCB386*)-1)
      p->cpu_affinity = cpu;
}

//Context switching, dispatcher
// Software context switch (replaces hardware TSS far-jumps).

/* Reentrancy guard: set while we are between fpu_save and the
   context_switch/context_load call.  A timer IRQ that fires in that
   window (context_load restores RFLAGS with IF set BEFORE the final
   `ret`) would otherwise re-enter ps_switchto via schedule_from_timer
   and clobber the in-flight task's saved ctx (live RIP/RSP overwrites
   the seeded entry point).  The guard is per-CPU so SMP is safe. */
volatile int ps_switchto_in_progress[MAX_CPUS];
static volatile int voluntary_switch[MAX_CPUS];
 volatile int ctx_load_in_progress[MAX_CPUS];
 volatile int selfhost_cooperative_ready;
/* Test hook: apply the self-host cooperative user scheduling regime without
   booting an actual stage-1 closure kernel, so QA can reproduce that regime
   (see the "coop-smp" cmdline in kernel32.c).  It deliberately does NOT feed
   selfhost_stage1_cmdline(), which also governs AP parking around kexec. */
 volatile int sched_coop_user_force;


/* Safe page-presence walk for bounded context diagnostics.  This runs under
   the *destination* task's CR3 (a private PML4), which does NOT identity-map
   the page-table frames themselves (only the low 4GiB kernel image + the kept
   PD0 blocks).  Walking via raw physical addresses would demand-page the
   PML4's own frame and fault.  KDIRECT is kept in every private PML4
   (pml4v[256]=boot_pdpt_high), so read the tables through the high canonical
   direct map instead. */
static int ctx_page_present(u64 cr3, u64 va)
{
   u64 *pt;
   int level;

   if (!cr3 || (cr3 & 0xffFULL) != 0)
      return 0;
   pt = (u64 *)KDIRECT(cr3);
   for (level = 3; level >= 0; level--) {
      u64 idx = (va >> (12 + level * 9)) & 0x1ffULL;
      u64 e = pt[idx];
      if (!(e & 1ULL))
         return 0;
      if (e & 0x80ULL)
         return level > 0;
      if (level == 0)
         return 1;
      pt = (u64 *)KDIRECT(e & 0x000ffffffffff000ULL);
      if (!pt)
         return 0;
   }
   return 1;
}

/* Called from context_load after the destination CR3/RSP are active but before
   any destination GPR is restored.  This is the only safe point to inspect the
   destination stack: the CPU is already running on that stack and page table. */
void ctx_load_check(cpu_context *ctx)
{
   u64 rip = ctx->rip;
   u64 rbp = ctx->rbp;
   char cb[256];
   char nm[12];
   int n;
   const char *src;

   /* Only user tasks resuming at the ps_switchto epilogue have a user-stack
      frame whose return slot can be validated.  Kernel idle/thread frames are
      left alone. */
   if (rip < 0x100000ULL || rip >= 0x300000ULL)
      return;
   if (rbp < 0x3fff0000ULL || rbp >= 0x40000000ULL)
      return;
   if (!ctx_page_present(ctx->cr3, rbp) ||
        !ctx_page_present(ctx->cr3, rbp + 8)) {
       /* Diagnostic: walk the destination PML4 for the rbp page and print
          every level entry so the missing level and its parent table frame
          are visible.  frame_diag_report on the last table distinguishes a
          freed-while-mapped table frame (alloc=0) from a wild write to a
          live frame (alloc=1). */
       {
          u64 va = rbp & 0x000FFFFFFFFF000ULL;
          u64 cur = ctx->cr3;
          int lvl;
          int missing = -1;
          if (cur && (cur & 0xFFFULL) == 0) {
              for (lvl = 3; lvl >= 0; lvl--) {
                 u64 idx = (va >> (12 + lvl * 9)) & 0x1FFULL;
                 /* cur holds the physical table frame; read it through KDIRECT
                    because this walk runs under the destination's private
                    PML4, which does not identity-map table frames. */
                 u64 e = ((u64 *)KDIRECT(cur))[idx];
                 char lw[160];
                 sprintf(lw, "CTXPTE L%d idx=%llu entry=0x%llx tbl=0x%llx\n",
                         lvl, (unsigned long long)idx,
                         (unsigned long long)e, (unsigned long long)cur);
                 serial_puts(lw);
                 if (!(e & 1ULL) || (e & 0x80ULL)) { missing = lvl; break; }
                 if (lvl == 0) break;
                 cur = e & 0x000FFFFFFFFFFF000ULL;
              }
           }
          frame_diag_report("CTXTBL", cur);
          {
             char cd[160];
             sprintf(cd, "CTXUNMAP cr3=0x%llx missing_lvl=%d wasfreed=%d fring=%lu\n",
                     (unsigned long long)ctx->cr3, missing,
                     (int)freed_pml4_contains(ctx->cr3),
                     (unsigned long)freed_pml4_count());
             serial_puts(cd);
          }
       }
       src = "ctx";
       n = 0;
       while (src[n] && n < 11) { nm[n] = src[n]; n++; }
       nm[n] = 0;
      sprintf(cb,
              "CTXCANARY UNMAP %s rip=0x%llx rsp=0x%llx rbp=0x%llx cr3=0x%llx canary=0x%llx\n",
              nm,
              (unsigned long long)rip,
              (unsigned long long)ctx->rsp,
              (unsigned long long)rbp,
              (unsigned long long)ctx->cr3,
              (unsigned long long)ctx->retcanary);
      serial_puts(cb);
      for (;;) __asm__ __volatile__("hlt");
   }

   {
       u64 actual = *(u64 *)(rbp + 8);
       if (actual != ctx->retcanary) {
          PCB386 *p = current_process;
          u64 slotva = rbp + 8;
          u64 phys;
          char *rp;
          int i;
          src = (p && p->name) ? p->name : "?";
          n = 0;
          while (src[n] && n < 11) { nm[n] = src[n]; n++; }
          nm[n] = 0;
          /* Resolve the corrupted slot to its physical frame so we can tell a
             direct write to a live frame from a freed-while-mapped frame. */
          rp = userpd_resolve((u64 *)(uintptr)ctx->cr3, slotva);
           phys = rp ? (((u64)(uintptr)rp) & 0x000FFFFFFFFF000ULL) : 0;
           frame_diag_report("CTX", phys);
           {
              int wasfreed = freed_pml4_contains(ctx->cr3);
              unsigned long fring = freed_pml4_count();
              char cd[256];
              sprintf(cd,
                      "CTXDIAG p%d name=%.11s cr3=0x%llx pcbdir=0x%x cr3==pcbdir=%d oncpu=%d status=0x%x slotphys=0x%llx slotoff=0x%llx wasfreed=%d fring=%lu\n",
                      (int)(p ? p->processid : 0), nm,
                      (unsigned long long)ctx->cr3,
                      (int)(p ? p->pagedirloc : 0),
                      (int)(p && (u64)(uintptr)p->pagedirloc == ctx->cr3),
                      (int)(p ? p->on_cpu : -1),
                      (int)(p ? p->status : 0),
                      (unsigned long long)phys,
                      (unsigned long long)(slotva & 0xFFFULL),
                      wasfreed, (unsigned long)fring);
              serial_puts(cd);
           }
          /* Dump the 16 qwords of the frame to see the surrounding state. */
          for (i = 0; i < 16; i++) {
             char fw[64];
             u64 v = *(u64 *)(rbp + (i << 3));
             sprintf(fw, "CTXFRAME f[%d]=0x%llx\n", i, (unsigned long long)v);
             serial_puts(fw);
          }
          sprintf(cb,
                  "CTXCANARY %s p%d rip=0x%llx rsp=0x%llx rbp=0x%llx cr3=0x%llx expected=0x%llx actual=0x%llx status=0x%x oncpu=%d\n",
                  nm,
                  (int)(p ? p->processid : 0),
                  (unsigned long long)rip,
                  (unsigned long long)ctx->rsp,
                  (unsigned long long)rbp,
                  (unsigned long long)ctx->cr3,
                  (unsigned long long)ctx->retcanary,
                  (unsigned long long)actual,
                  (int)(p ? p->status : 0),
                  (int)(p ? p->on_cpu : -1));
          serial_puts(cb);
          for (;;) __asm__ __volatile__("hlt");
       }
    }
}

static int ps_pcb_advertised_elsewhere(PCB386 *p, int me)
{
   int j;
   if (!p)
      return 0;
   for (j = 0; j < cpu_count && j < MAX_CPUS; j++) {
      if (j == me || !cpus[j].online)
         continue;
      if ((PCB386 *)cpus[j].current == p)
         return 1;
   }
   return 0;
}

static int ps_pcb_running_elsewhere(PCB386 *p, int me)
{
   int j;
   if (!p)
      return 0;
   /* on_cpu is authoritative for liveness.  A PCB claimed by another core
      (on_cpu >= 0 && != me) is live there; claiming it here would put two
      CPUs on one PCB and one IRQ kstack.  The relaxed "only block when the
      advertising core also claims it" version let a second core run a task
      mid-exit on a first core (double-run pid27 around SDK_EXIT), which then
      published a cross-CPU idle as current and broke the next fork
      (parent=-65534, FORK-FAIL -11). */
   if (p->on_cpu >= 0 && p->on_cpu != me)
      return 1;
   for (j = 0; j < cpu_count && j < MAX_CPUS; j++) {
      if (j == me || !cpus[j].online)
         continue;
      if ((PCB386 *)cpus[j].current != p)
         continue;
      /* Released on_cpu (p->on_cpu < 0) but another core still advertises it
         and has not published a successor: that core is between releasing the
         claim and switching, so treat the PCB as live there.  The live-claim
         case (p->on_cpu == j) is now caught by the authoritative check above.
         test-fatwrite-coop's stale-current hang is bounded to this on_cpu<0
         window, not the live-claim path. */
      if (p->on_cpu < 0)
         return 1;
   }
   return 0;
}

PCB386 *ps_find_by_cr3(unsigned long cr3)
{
   PCB386 *head, *p;
   int hops = 0;
   u64 want = (u64)cr3 & ~0xFFFULL;

   if (!want)
      return 0;
   if (current_process &&
       (((u64)(uintptr)current_process->pagedirloc) & ~0xFFFULL) == want)
      return current_process;
   head = sched_gethead();
   p = head;
   if (!head)
      return 0;
   do {
      if (p && (((u64)(uintptr)p->pagedirloc) & ~0xFFFULL) == want)
         return p;
      p = p->next;
   } while (p && p != head && ++hops < 512);
   return 0;
}

/* Anomaly logger for the current pointer: fires only when a CPU is about to
   advertise, as its current, another CPU's idle PCB (processid 0xFFFF0000|id
   with id != me).  A cross-CPU idle advertisement means the CPU will run a
   kernel idle whose on_cpu/cpu_affinity name a different core, and the next
   user syscall on this core reads that idle PCB as current_process (fork
   parent=-65535 on cpu=0).  Bounded; the writer tag identifies the path. */
void ps_current_anom_log(const char *fn, int me, PCB386 *task)
{
   DWORD pid;
   if (!task)
      return;
   pid = task->processid;
   if ((pid & 0xFFFF0000) != 0xFFFF0000)
      return;
   if ((int)(pid & 0xFFFF) == me)
      return;
   {
      static volatile unsigned long an = 0;
      char b[160];
      if (an < 32) {
         an++;
         sprintf(b, "CUR-ANOM %s cpu=%d task=pid0x%lx own=%d aff=%d\n",
                 fn, me, (unsigned long)pid, (int)(pid & 0xFFFF),
                 (int)task->cpu_affinity);
         serial_puts(b);
      }
   }
}

static void ps_publish_current(int me, PCB386 *task)
{
    if (me >= 0 && me < MAX_CPUS) {
       ps_current_anom_log("publish", me, task);
       cpus[me].current = task;
    }
    __sync_synchronize();
}

/* Drop a scheduler CAS that this CPU has not yet published as current. */
static void ps_unclaim_if_unused(PCB386 *process, PCB386 *prev, int me)
{
   if (process && process != prev && process->on_cpu == me)
      process->on_cpu = -1;
}

void ps_switchto(PCB386 *process){
   int me = smp_cpu_id();
   PCB386 *prev = (me >= 0 && me < MAX_CPUS) ? (PCB386 *)cpus[me].current
                                             : current_process;
   extern int lapic_send_ipi(u32 apic_id, u32 vector);

   if (!process)
      return;
  if (ps_switchto_in_progress[me])
       return; /* already switching — a nested call from IRQ would corrupt ctx */
   ps_switchto_in_progress[me] = 1;

   /* Always require the claim, including process==prev.  Skipping that left
      current advertising a PCB whose on_cpu was already -1 or another CPU,
      so the next remote CAS migrated it while this CPU kept executing it
      (KSTACK-SHARED pid=fatwr). */
   if (process->on_cpu != me) {
      int spins = 0;
      if (process == prev && process->on_cpu >= 0) {
         ps_switchto_in_progress[me] = 0;
         return;
      }
      while (process != prev && process->on_cpu >= 0 && process->on_cpu != me) {
         int other = process->on_cpu;
         if (other >= 0 && other < cpu_count && cpus[other].online)
            lapic_send_ipi(cpus[other].apic_id, IPI_RESCHEDULE);
         __asm__ __volatile__("pause");
         if (++spins > 1000000) {
            ps_unclaim_if_unused(process, prev, me);
            ps_switchto_in_progress[me] = 0;
            return;
         }
      }
      /* Do not CAS while another CPU still advertises this PCB: it may
         have dropped on_cpu and still be executing.  A leftover
         advertisement with on_cpu already == me is handled below. */
      if (process->on_cpu < 0 && ps_pcb_advertised_elsewhere(process, me)) {
         ps_switchto_in_progress[me] = 0;
         return;
      }
      if (process->on_cpu < 0 &&
          !__sync_bool_compare_and_swap(&process->on_cpu,-1,me)) {
         ps_switchto_in_progress[me] = 0;
         return;
      }
      if (process->on_cpu != me) {
         ps_switchto_in_progress[me] = 0;
         return;
      }
   }

   if (prev && prev != process) {
     fpu_save(&prev->fpu);
   }
   /* Guard is now active: any timer IRQ that fires between here and the
      context_load's final `ret` will see ps_switchto_in_progress[me]==1
      and bail out of schedule_from_timer without touching contexts. */

    /* Seed context from legacy TSS fields on first run. */
    if (process->ctx.rip == 0 && process->regs.EIP != 0) {
      memset(&process->ctx, 0, sizeof(process->ctx));
      process->ctx.rip = (u64)process->regs.EIP;
      process->ctx.rsp = (u64)process->regs.ESP;
      process->ctx.rflags = process->regs.EFLAGS ? process->regs.EFLAGS : 0x202;
      process->ctx.cs = SYS_CODE_SEL;
      process->ctx.ss = SYS_DATA_SEL;
      if (process->pagedirloc)
         process->ctx.cr3 = (u64)(uintptr)process->pagedirloc;
      else {
         extern DWORD *pagedir1;
         process->ctx.cr3 = (u64)(uintptr)pagedir1;
      }
   }

   /* Publish current only after preemption is disabled.  Otherwise a timer
      can observe the destination PCB while RSP/CR3 still belong to prev and
      save that mixed state into the destination context. */
   stopints();
   /* Re-verify the claim under the interrupt-off window.  Publishing a task
      this CPU does not own makes the next IRQ pick up that task's IRQ kstack
      (or user stack) while its real owner is still running it. */
   if (process->on_cpu != me || ps_pcb_running_elsewhere(process, me)) {
      ps_unclaim_if_unused(process, prev, me);
      ps_switchto_in_progress[me] = 0;
      startints();
      return;
   }
   /* Another CPU's idle has that CPU's stack and context.  Loading it
      here is how a migrated self_exit published pid 0xFFFF0003 on CPU 0. */
   if (foreign_idle_pid(me, (unsigned long)process->processid)) {
      ps_current_anom_log("switch", me, process);
      ps_unclaim_if_unused(process, prev, me);
      ps_switchto_in_progress[me] = 0;
      startints();
      return;
   }
   /* Write this CPU's slot with the id captured above.  current_process
      calls smp_cpu_id() again; under a user CR3 a LAPIC fallback can
      return 0 and publish the destination on the BSP slot instead. */
   ps_publish_current(me, process);
   /* One USER_RUN per CPU, and only when leaving kernel/idle so CR3 is
      still the identity map. Logging every steal flooded COM1 (~20k lines)
      and serial_puts used LAPIC MMIO under a user CR3. */
   if (process->accesslevel == ACCESS_USER &&
       (!prev || prev->accesslevel != ACCESS_USER)) {
      static int user_run_seen[MAX_CPUS];
      if (me >= 0 && me < MAX_CPUS && !user_run_seen[me]) {
         char line[80];
         user_run_seen[me] = 1;
         sprintf(line, "USER_RUN cpu=%d pid=%d\n", me,
                 (int)process->processid);
         serial_puts(line);
      }
   }
   fpu_restore(&process->fpu);

   /* Leave ps_switchto_in_progress set until context_load clears it.  Clearing
      it here let a timer on this CPU re-enter the scheduler while RSP/CR3
      still belonged to prev, and let ps_switchto(prev==dest) context_load a
      task another CPU had already claimed (KSTACK-SHARED pid=gcc). */

    /* Diagnostic: valid RIPs are kernel code (0x100000..0x300000) or user
       code (0x400000+).  A RIP in the 0x300000..0x400000 stack hole, or with
       a non-zero high half (PE-attribute / stray high word grafted onto a
       kernel address), means the saved context RIP was corrupted.  Print
       before we ret into it. */
    {
      u64 rr = process->ctx.rip;
       /* Tight code window: kernel image [0x100000,0x400000) and the user
          ELF window [0x400000,0x1800000).  The old check accepted any address
          below 4GiB as "user code", so a wild saved RIP in the 0x1800000..4G
          gap (e.g. 0x779950ba) passed and executed as garbage (emulation
          failure).  Everything else means the context RIP was corrupted. */
       int ok = (rr >= 0x100000ULL && rr < MEM_USER_ELF_END);
      if (!ok) {
         char cb[160];
         char nm[12];
         int n = 0;
         const char *src = process->name ? process->name : "?";
         while (src[n] && n < 11) { nm[n] = src[n]; n++; }
         nm[n] = 0;
         sprintf(cb, "CTXBAD p%d %s ptr=0x%llx rip=0x%llx EIP=0x%lx rsp=0x%llx\n",
                 (int)process->processid, nm,
                 (unsigned long long)(uintptr)process,
                 (unsigned long long)rr,
                 (unsigned long)process->regs.EIP,
                 (unsigned long long)process->ctx.rsp);
         serial_puts(cb);
         if (process != prev) {
            if (process->on_cpu == me)
               process->on_cpu = -1;
            if (prev)
               ps_publish_current(me, prev);
            if (prev)
               fpu_restore(&prev->fpu);
         }
         ps_switchto_in_progress[me] = 0;
         startints();
         return;
       }
    }

      if (prev && prev != process)
        context_switch(&prev->ctx, &process->ctx, &prev->on_cpu, me);
      else
        context_load(&process->ctx, me);
};

/* Leftover timer abandon: load dest without saving prev from this RSP.
   context_switch from a leftover user/CPUIRQ stack smashed idle/make
   (cert 248136).  `held` is a task whose on_cpu this CPU still owns and
   whose stack this IRQ is still on (smp_abandon retargeted current to
   idle without dropping that claim).  The claim is cleared only after
   RSP has moved. */
static void ps_switchto_load_only(PCB386 *process, PCB386 *held)
{
   int me = smp_cpu_id();
   PCB386 *prev;
   PCB386 *drop;

   if (me < 0 || me >= MAX_CPUS)
      return;
   if (!process || ps_switchto_in_progress[me] || ctx_load_in_progress[me]) {
      if (held)
         cpus[me].current = held;
      return;
   }
   prev = (PCB386 *)cpus[me].current;
   if (process->on_cpu >= 0 && process->on_cpu != me) {
      if (held)
         cpus[me].current = held;
      return;
   }
   if (process->on_cpu < 0 &&
       !__sync_bool_compare_and_swap(&process->on_cpu, -1, me)) {
      if (held)
         cpus[me].current = held;
      return;
   }
   ps_switchto_in_progress[me] = 1;
   stopints();
   if (process->on_cpu != me ||
       foreign_idle_pid(me, (unsigned long)process->processid)) {
      if (foreign_idle_pid(me, (unsigned long)process->processid))
         ps_current_anom_log("load", me, process);
      if (process->on_cpu == me && process != prev && process != held)
         process->on_cpu = -1;
      ps_switchto_in_progress[me] = 0;
      if (held)
         cpus[me].current = held;
      startints();
      return;
   }
   /* The timer is still on `held`'s live frame.  Reloading that PCB's
      saved ctx rewinds a syscall onto a snapshot another CPU can also
      load (fork_child_return / torn stack).  Iret the live frame. */
   if (held && held == process) {
      cpus[me].current = held;
      ps_switchto_in_progress[me] = 0;
      startints();
      return;
   }
   /* held is the stack we are leaving when abandon already published
      idle as current, so prev is idle and must not be the PCB we drop. */
   drop = 0;
   if (held && held->on_cpu == me)
      drop = held;
   else if (prev && prev != process && prev->on_cpu == me)
      drop = prev;
   ps_publish_current(me, process);
   fpu_restore(&process->fpu);
   if (drop)
      context_load_release(&process->ctx, me, &drop->on_cpu);
   else
      context_load(&process->ctx, me);
}


/* Notify a parent that one of its children has reached the waitq.  Under
   user_procs_smp the parent may be live on another CPU; do not load its
   context here.  Wake it if it is blocked and let the owning CPU reschedule
   it. */
static void waitpid_notify_parent(PCB386 *parent)
{
   int me = smp_cpu_id();
   extern int lapic_send_ipi(u32 apic_id, u32 vector);
   if (parent == (PCB386 *)-1 || !parent)
      return;
   if (parent->status & PS_ATTB_BLOCKED)
      sched_wake_process(parent);
   if (parent->on_cpu >= 0 && parent->on_cpu != me) {
      int other = parent->on_cpu;
      if (other >= 0 && other < cpu_count && cpus[other].online)
         lapic_send_ipi(cpus[other].apic_id, IPI_RESCHEDULE);
   } else {
      smp_reschedule_others();
   }
}

/* Concurrent self-exits used to overwrite a single zombie_free slot and leak
   earlier PCBs/PML4s. Push onto a CAS stack; the BSP steals the whole list. */
static void zombie_enqueue(PCB386 *z)
{
   PCB386 *old;

   /* Only heap-allocated PCBs are reclaimed.  Leftover current / smash
      enqueued &ready_lock and &cpus (cert ramdisk KHEAP-BADFREE
      rip=zombie_reclaim).  Writing zombie_next through those BSS
      objects corrupts the scheduler. */
   if (!z || !kheap_ptr_in_range((unsigned long)(uintptr)z))
      return;
   do {
      old = zombie_head;
      z->zombie_next = old;
   } while (!__sync_bool_compare_and_swap(&zombie_head, old, z));
}

static int zombie_still_live(PCB386 *z)
{
   int i;

   if (!z)
      return 0;
   for (i = 0; i < cpu_count; i++) {
      if (!cpus[i].online)
         continue;
      if (cpus[i].current == z)
         return 1;
      if (ctx_load_in_progress[i]) {
         /* CR3/RSP of CPU i may still be this PCB until context_load finishes. */
         if (pending_zombie[i] == z)
            return 1;
      }
   }
   return 0;
}

static void zombie_reclaim(PCB386 *z)
{
   if (!z)
      return;
   if (!kheap_ptr_in_range((unsigned long)(uintptr)z)) {
      static volatile unsigned long badz;
      if (++badz <= 8)
         serial_puts("ZOMBIE-BAD\n");
      return;
   }
   if (zombie_still_live(z)) {
      zombie_enqueue(z);
      return;
   }
#ifdef __x86_64__
   {
      extern DWORD *pagedir1;
      if (!(z->status & PS_ATTB_THREAD)
           && userpd_is_private(z->pagedirloc)) {
          if (zfree_pml4_liveness_check(z)) {
             zombie_enqueue(z);
             return;
          }
          userpd_free((u64 *)(uintptr)z->pagedirloc);
          z->pagedirloc = pagedir1;
       }
   }
#endif
    if (z->meminfo) {
       process_mem *m = z->meminfo;
       while (m) {
          process_mem *n = m->next;
          free(m);
          m = n;
       }
       z->meminfo = 0;
    }
    if (z->parameters) {
       free(z->parameters);
       z->parameters = 0;
    }
    if (z->stdout) {
       free(z->stdout);
       z->stdout = 0;
    }
#ifdef __x86_64__
    /* Free the per-process IRQ kstack.  createprocess() allocates one
       (IRQ_KSTACK_SIZE) for every user process; kill_process() frees it, but
       the self-exit path (self_exit_current -> pending_zombie -> here) used
       to leak it.  Every self-host tool exits via self-exit, so each leaked
       128 KiB of the 63 MiB kheap; after ~300 ELF loads the kheap was
       exhausted and createprocess() failed at pcb_alloc_irq_kstack()
       (mapfile: malloc failed knext=0x5fe4000).  No-op for threads, which
       never allocate a kstack (kstack_base stays 0). */
    pcb_free_irq_kstack(z);
#endif
    free(z);
}

static void zombie_drain(void)
{
   PCB386 *z, *n;
   int i;

   if (smp_cpu_id() != 0)
      return;
   for (i = 0; i < cpu_count && i < MAX_CPUS; i++) {
      z = pending_zombie[i];
      if (!z)
         continue;
      if (ctx_load_in_progress[i] || cpus[i].current == z)
         continue;
      if (__sync_bool_compare_and_swap(&pending_zombie[i], z, 0))
         zombie_enqueue(z);
   }
   for (;;) {
      z = zombie_head;
      if (!z)
         return;
      if (!__sync_bool_compare_and_swap(&zombie_head, z, 0))
         continue;
      while (z) {
         if (!kheap_ptr_in_range((unsigned long)(uintptr)z)) {
            static volatile unsigned long badn;
            if (++badn <= 8)
               serial_puts("ZOMBIE-BADNEXT\n");
            return;
         }
         n = z->zombie_next;
         z->zombie_next = 0;
         zombie_reclaim(z);
         z = n;
      }
      return;
   }
}

/* Regression detector for the SMP self-exit race that silently triple-faulted
   SMP=4 self-host certification.  Fires when we are about to free a zombie's
   private PML4 while some CPU is CURRENTLY running a process that still uses
   that same directory -- the exact condition that used to leave a CPU walking
   freed frames.  The PS_ATTB_DYING guard in sched_runnable_here()/
   self_exit_current() should make this unreachable, so a hit means the guard
   regressed.  Marker only:    the DYING guard is the real fix. */
static int zfree_pml4_liveness_check(PCB386 *z)
{
    int i;
    for (i = 0; i < cpu_count; i++) {
        PCB386 *c = cpus[i].current;
        if (!cpus[i].online || !c)
            continue;
        if (c->pagedirloc == z->pagedirloc) {
            char zb[192];
            sprintf(zb, "ZFREE_LIVE_PML4 zpid=%d zpagedir=0x%x live_pid=%d cpu=%d same_zombie=%d\n",
                    (int)z->processid, z->pagedirloc,
                    (int)c->processid, i, (c == z));
            serial_puts(zb);
            return 1;
        }
    }
    return 0;
}

/* This CPU's idle PCB, or NULL when the slot is empty or names another
   CPU.  The BSP never runs ap_prepare_idle(), so cpus[0].idle stays NULL
   and the caller falls back to sPCB. */
static PCB386 *ps_cpu_idle(int me)
{
   PCB386 *idle;
   if (me < 0 || me >= MAX_CPUS)
      return 0;
   idle = (PCB386 *)cpus[me].idle;
   if (!idle || foreign_idle_pid(me, (unsigned long)idle->processid))
      return 0;
   return idle;
}

static void self_exit_current(void)
{
   PCB386 *dying = current_process;
   PCB386 *parent;
   PCB386 *readyprocess;
   devmgr_scheduler_extension *cursched;
   int me;

   if (!dying)
       return;
   if (!kheap_ptr_in_range((unsigned long)(uintptr)dying))
       return;

  /* Close files before processmgr_busy so teardown I/O cannot invert with
     bio_submit_sync (io_devlock then processmgr) under SMP=4 -j4. */
  posix_fd_close_all(dying);
  closeallfiles(dying->processid);

  sync_entercrit(&processmgr_busy);

   parent = ps_findprocess(dying->owner);
   if (parent != (PCB386 *)-1 && parent != dying) {
      parent->childwait = 0;
      /* Published before the dequeue further down; see kill_process(). */
      waitq_publish(parent, (int)dying->processid, dying->exit_status);
      waitpid_notify_parent(parent);
   }

   /* Resuming the parent (or the idle task) directly is only allowed if we can
      CLAIM it.  The old code tested parent->on_cpu and then assigned
      on_cpu = me further down; another CPU could claim the parent in between,
      leaving two CPUs running one PCB -- and therefore sharing one IRQ kstack
      (KSTACK-FOREIGN). */
   /* Close and processmgr acquire can taskswitch.  A preempted exit
      resumes on another CPU with the stack-local cpu id still naming the
      old core, then smp_this_cpu()->idle is the new core's idle and
      ps_publish_current(old me, that idle) installs it on the original
      slot (CUR-ANOM cpu=0 task=pid0xffff0003, FORK-FAIL parent=-65533).
      Sample me only after that window, with interrupts off, and keep
      them off through context_load. */
   stopints();
   me = smp_cpu_id();
   if (me < 0 || me >= MAX_CPUS)
      me = 0;

   readyprocess = 0;
   /* Only CAS from -1.  `on_cpu == me` here is a leftover claim: current
      is the dying child, so the parent is not running on this CPU.  Taking
      it anyway, then storing on_cpu=me below, stole a parent still live
      on another CPU (KSTACK-SHARED after SDK_EXIT). */
   if (parent != (PCB386 *)-1 && parent != dying &&
        !(parent->status & PS_ATTB_BLOCKED) && !parent->waiting &&
        (parent->cpu_affinity < 0 || parent->cpu_affinity == me) &&
        parent->on_cpu < 0 &&
        !ps_pcb_advertised_elsewhere(parent, me) &&
        __sync_bool_compare_and_swap(&parent->on_cpu, -1, me) &&
        !ps_pcb_running_elsewhere(parent, me))
       readyprocess = parent;
   if (!readyprocess) {
      readyprocess = ps_cpu_idle(me);
      if (readyprocess && readyprocess != dying
          && readyprocess->on_cpu != me
          && !__sync_bool_compare_and_swap(&readyprocess->on_cpu, -1, me))
         readyprocess = 0;
      if (!readyprocess || readyprocess == dying) {
         cursched = (devmgr_scheduler_extension*)extension_table[CURRENT_SCHEDULER].iface;
         readyprocess = 0;
         if (cursched && cursched->scheduler)
            readyprocess = (PCB386*)bridges_link((devmgr_generic*)cursched,
                                                 &cursched->scheduler,
                                                 current_process,0,0,0,0,0);
         if (readyprocess && readyprocess != dying
             && readyprocess->on_cpu != me
             && !__sync_bool_compare_and_swap(&readyprocess->on_cpu, -1, me))
            readyprocess = 0;
      }
      /* Last resort: this CPU's own idle task, force-claimed.  It belongs to
         this CPU, so a stale on_cpu from a previous switch must not push us
         onto sPCB: sPCB is a SINGLE global PCB with a single ctx.rsp, so two
         CPUs reaching this fallback would resume one context on one stack and
         scribble over each other.  That shows up as a 64-bit stack slot whose
         high half is another CPU's 32-bit smp_cpu_id() result -- a return
         address read back as 0x1_xxxxxxxx, faulting in the serial path. */
      if (!readyprocess || readyprocess == dying) {
         PCB386 *idle = ps_cpu_idle(me);
         if (idle && idle != dying) {
            idle->on_cpu = me;
            readyprocess = idle;
         } else {
            readyprocess = &sPCB;
         }
      }
   }

   /* Mark DYING and dequeue while still claimed.  on_cpu stays ours
      until context_load_release has left this stack: clearing it here
      let another CPU enter the exiting task (torn 0x100000003).
      kill_process sees DYING and does not treat the PCB as off-CPU. */
    dying->status |= PS_ATTB_DYING;
    dying->status |= PS_ATTB_UNLOADABLE;
    __sync_synchronize();
    ps_dequeue(dying);
        sync_leavecrit(&processmgr_busy);
        /* Held-crit sweep MUST run before the PCB is published to the
           zombie list: the BSP reclaims zombies in schedule_from_timer()
           without processmgr_busy, so publishing first lets the BSP
           free(dying) while we still read dying->held_crits[]. */
        sync_release_process_crits(dying, (dying->processid & 0x007FFFFF) + 1);
   stopints();
   {
      int now = smp_cpu_id();
      if (now < 0 || now >= MAX_CPUS)
         now = 0;
      /* scheduler() restores the caller's flags.  If that re-enabled
         interrupts and this exit migrated, the successor was chosen for
         the old cpu id.  Drop that claim and pick again for `now`. */
      if (now != me) {
         if (readyprocess && readyprocess != dying && readyprocess->on_cpu == me)
            readyprocess->on_cpu = -1;
         me = now;
         readyprocess = 0;
      }
   }
     if (!readyprocess || readyprocess == dying
         || ps_pcb_running_elsewhere(readyprocess, me)
         || foreign_idle_pid(me, (unsigned long)readyprocess->processid)) {
        PCB386 *idle = ps_cpu_idle(me);
        if (readyprocess && readyprocess != dying && readyprocess->on_cpu == me
            && readyprocess != idle)
           readyprocess->on_cpu = -1;
        if (idle && idle != dying) {
           idle->on_cpu = me;
           readyprocess = idle;
        } else {
           readyprocess = &sPCB;
           readyprocess->on_cpu = me;
        }
     }
     ps_publish_current(me, readyprocess);
     fpu_restore(&readyprocess->fpu);
     /* Publish the zombie only after this CPU's current_process is the
        successor, and keep ctx_load_in_progress set until context_load
        finishes CR3/RSP so the BSP cannot free the live PML4. */
     ctx_load_in_progress[me] = 1;
     pending_zombie[me] = dying;
     if (dying->on_cpu == me)
        context_load_release(&readyprocess->ctx, me, &dying->on_cpu);
     else
        context_load(&readyprocess->ctx, me);
  }

/*Calls the scheduler voluntarily*/
inline void taskswitch(){
   int me=smp_cpu_id();
   ps_notimeincrement = 1;
   voluntary_switch[me] = 1;
   schedule_from_timer();
};

/* Invoked from the timer IRQ wrapper after time_handler(). */

void schedule_from_timer(void){
    PCB386 *readyprocess;
    devmgr_scheduler_extension *cursched;
   int me=smp_cpu_id();
    int voluntary;
    int leftover_load_only = 0;
    PCB386 *abandoned = 0;

    {
       extern void smp_repair_stale_current(void);
       smp_repair_stale_current();
    }
    voluntary = voluntary_switch[me];
    voluntary_switch[me] = 0;

   if (ctx_load_in_progress[me] || ps_switchto_in_progress[me])
      return;

   if (sigwait || !current_process)
      return;
   {
      unsigned long rsp, cr3;
      int access_user = current_process->accesslevel == ACCESS_USER;
      int crit_wait = current_process->crit_wait;
      PCB386 *idle = (me >= 0 && me < MAX_CPUS)
                     ? (PCB386 *)cpus[me].idle : 0;
      __asm__ __volatile__("movq %%rsp, %0" : "=r"(rsp));
      __asm__ __volatile__("movq %%cr3, %0" : "=r"(cr3));
      /* leftover_load_only claimed idle smashed 248142 (UD64 make,
         GPF64 context_switch leftover idle). leftover schedule smashed
         248141. leftover-skip leftover advertised idle on a user AS
         (248139 reached GCC_DRIVER_OK). Dest'd leftover ACCESS_SYS on
         CPUIRQ leftover-skips via leftover_timer. */
      if (leftover_idle_timer_must_skip(idle && current_process == idle,
                                        cr3, rsp)) {
         return;
      } else if (leftover_timer_must_abandon(current_process->on_cpu, me,
                                      access_user, rsp,
                                      (unsigned long)(uintptr)
                                      current_process->pagedirloc,
                                      cr3, crit_wait)) {
         abandoned = smp_abandon_leftover_current();
         leftover_load_only = 1;
         if (!current_process)
            return;
      } else if (leftover_timer_must_skip(current_process->on_cpu, me,
                                          access_user, rsp,
                                          (unsigned long)(uintptr)
                                          current_process->pagedirloc,
                                          cr3, crit_wait))
         return;
   }

    /* Long GCC cc1 runs expose a legacy timer-context race.  Stage-1
       certification (serial "selfhost-stage1" and parallel
       "selfhost-stage1-parallel") is cooperatively scheduled: spawned tools
       run to exit, then taskswitch() resumes make or the console.  Do not
       preempt a running USER tool from the timer.  Idle and kernel threads
       must still fall through and claim ready user work; returning here for
       every task left APs parked in idle except for a one-shot IPI. */
    {
         /* ...unless that tool is SPINNING for a crit it does not hold.  A
            waiter makes no progress by keeping the CPU, and the crit's owner
            may be pinned to this very CPU -- with no preemption the owner never
            runs and the spin never ends.  That is how the -j4 bootstrap wedged
            with mkdir.exe holding fat_volume_busy and spinning on pc_busy while
            pc_busy's owner sat in the ready queue (CRITHANG).  scheduler()
            already declines to pin a crit_wait task; it just never got asked. */
         if (!leftover_load_only && !voluntary && selfhost_cooperative_ready &&
            (selfhost_stage1_cmdline() || sched_coop_user_force) &&
            current_process &&
            current_process->accesslevel == ACCESS_USER &&
            !current_process->crit_wait) {
          zombie_drain();
          return;
         }
    }

#ifdef __x86_64__
   {
      extern volatile int smp_sched_enabled;
      if (smp_cpu_id() != 0 && !smp_sched_enabled)
         return;
   }
#endif

   /* Finish deferred frees from prior self-exits (safe once off that stack).
      Only the BSP reclaims so we never free a PCB from the dying CPU. */
   zombie_drain();

   cursched = (devmgr_scheduler_extension*)extension_table[CURRENT_SCHEDULER].iface;
   if (!cursched || !cursched->scheduler)
      return;

   /* Reap exit/kill before choosing the next task. The read-and-clear is
       atomic (sigterm_lock) so two CPUs cannot both consume the same pid. */
    {
       DWORD victim = 0;
       spin_irq_flags_t sl_flags = spin_lock_irqsave(&sigterm_lock);
       if (sigterm) {
          victim = sigterm;
          sigterm = 0;
       }
       spin_unlock_irqrestore(&sigterm_lock, sl_flags);
      if (victim) {
        if (current_process->processid == victim) {
           self_exit_current();
           return;
        }
       kill_process(victim);
        if (!current_process)
           return;
        }
     }

   readyprocess = (PCB386*)bridges_link((devmgr_generic*)cursched, &cursched->scheduler,
                                          current_process,0,0,0,0,0);
    if (leftover_load_only) {
       if (!readyprocess)
          readyprocess = current_process;
       ps_switchto_load_only(readyprocess, abandoned);
       return;
    }
    if (!readyprocess || readyprocess == current_process)
       return;

    current_process->totalcputime++;
    ps_switchto(readyprocess);
}

//The taskswitcher() is basically the program that runs all the time, aka CPU scheduler.
//It selects the process to execute on the CPU. The selection of the 
//next process to run is delegated to an scheduler extension.
void taskswitcher(){
   char temp[255];
   DWORD cputime=0;
   PCB386 *readyprocess;

   do{
      stopints(); //disable interrupts first

      //fetch a ready process from the ready queue
      do{
         if (!sigwait){
            //if the wait register is not set, switch to
            //another process, the wait is used to prevent
            //other processes from taking control during
            //a critical section, sigwait should be
            //returned to its original state after the critical
            //section is over

            //obtain next job from the scheduler
            //readyprocess=extension_current->ps_scheduler->scheduler(&kernelPCB);

            //get handle to the current scheduler extension
            devmgr_scheduler_extension *cursched = (devmgr_scheduler_extension*)extension_table[CURRENT_SCHEDULER].iface;

            //get the ready process returned by the scheduler extension
            readyprocess = (PCB386*)bridges_link((devmgr_generic*)cursched, &cursched->scheduler,
                                                   current_process,0,0,0,0,0);
         };
         //readyprocess=bridges_ps_scheduler(current_process);

         /* Do NOT publish current_process here.  ps_switchto() claims the
            task with a CAS on on_cpu and only then sets current_process; if
            the claim fails (another CPU already runs it) it returns without
            switching.  Publishing first left this CPU's current_process
            pointing at a task owned by another CPU, and the next IRQ then
            switched RSP to that task's IRQ kstack -- two CPUs on one kernel
            stack, each overwriting the other's frames (KSTACK-FOREIGN). */
         ps_switchto(readyprocess);

         /*Make sure the taskwitcher was really called by the timer, since
            another way of calling the taskswithcer is through taskswitch().
            In that case we must not call time_handler in order to make
            the time as accurate as possible
         */
         if (!ps_notimeincrement){
            //Increment system time... etc.
            time_handler();
         }else{
            ps_notimeincrement = 0;
         }
            
         //record the number of milleseconds the process used
         readyprocess->totalcputime++;
            
         //A process wants to get immediate control, usually set by
         //device drivers that are hooked to IRQs or high priority process
         //The pid of the high priority process is sigpriority 
         if (sigpriority){
            PCB386 *priorityprocess = ps_findprocess(sigpriority);
            if (priorityprocess!=-1){
               //give the high priotiry process to the CPU       
               ps_switchto(priorityprocess);       
            };
            sigpriority = 0;     //reset the variable
         };
      }while(sigwait);

      //check the registers, and act if necessary
      
      //page fault
      if (pfoccured){
         setattb(PF_TSS,0x89);
         memcpy(&pfPCB.regs, &pfPCB_copy.regs, sizeof(pfPCB.regs));
         pfoccured=0;
      };

      //At this point, sigterm is check if a process is to be terminated.
      //A non-zero value in sigterm indicates the process id of the process to be terminated.
      //sigterm is usually set in the dex32_killkthread_name(),ps_user_kill(), and exit() functions.
      //do clean up for terminate process
      //a request to terminate a process is received, pid of process to end is the value of sigterm
      {
          DWORD victim = 0;
          spin_irq_flags_t sl_flags = spin_lock_irqsave(&sigterm_lock);
          if (sigterm) {
             victim = sigterm;
             sigterm = 0;   //atomic read-and-clear; the dying process re-asserts until served
          }
          spin_unlock_irqrestore(&sigterm_lock, sl_flags);
          if (victim && !flushing){
             flushok = 0;
             kill_process(victim);
             flushok = 1;
          };
       };

      if (sched_sysmes[0]){
         sendmessage(sched_sysmes[0],sched_sysmes[1],sched_sysmes[2]);
         sched_sysmes[0]=0;
      };

      //Shutdown sequence initiated
      if (sigshutdown){
         broadcastmessage(1, SIG_KILL, 0);
      };
    }while (1);
};


//Show register values for process
void show_process_stat(int pid){
   int total,i;
   PCB386* ptr;
   char temp[20],temp1[20],temp2[20],temp3[20];
   total = get_processlist(&ptr);    
   for (i=0;i<total;i++){
      if (pid ==  ptr[i].processid){
         printf("=========================================================\n");
         printf("Name: %s\n", ptr[i].name);
         printf("Parent PID: %d\n", ptr[i].owner);
         printf("EIP=0x%s\n",itoa(ptr[i].regs.EIP,temp,16));
         printf("EAX=0x%s EBX=0x%s ECX=0x%s EDX=0x%s\n",itoa(ptr[i].regs.EAX,temp,16),
                  itoa(ptr[i].regs.EBX,temp1,16),itoa(ptr[i].regs.ECX,temp2,16),
                  itoa(ptr[i].regs.EDX,temp3,16));

         printf("EDI=0x%s ESI=0x%s ESP=0x%s Flags=0x%s\n",itoa(ptr[i].regs.EDI,temp,16),
                  itoa(ptr[i].regs.ESI,temp1,16),itoa(ptr[i].regs.ESP,temp2,16),
                  itoa(ptr[i].regs.EFLAGS,temp3,16));
                         
         printf("Waiting: %d\n", ptr[i].waiting);
         printf("Last system calls:(1) : 0x%s ,(2-last): 0x%s\n",
                  itoa(ptr[i].cursyscall[0],temp2,16),itoa(ptr[i].cursyscall[1],temp,16));

      };
   };
};

/*An auxillary function for qsort for comparing two elements*/
int process_pid_sorter(PCB386 *n1, PCB386 *n2){
   if (n1->processid > n2->processid) 
      return 1;
   if (n2->processid > n1->processid) 
      return -1;
   return 0;
};


//displays the list of processes. called in the ps command
void show_process(){
   int total=0,i;
   DWORD totalsize=0 , grandtotalcputime = 0;
   PCB386* ptr;
   char levelstr[13];
        
   textbackground(BLUE);
   printf("dex32_scheduler  v1.00\n");
   textbackground(BLACK);
   printf("Processes in memory:\n\n");
   textcolor(MAGENTA);
   printf("%-5s %-17s %-11s %-14s %6s %6s %11s\n","PID","Name","Access Lvl","PPID","Size","AT","CT");
   textcolor(WHITE);
    
   /*Tell the scheduler to give us an array of PCBs which contain the PCBs of the processes
      running in the system*/  
   total = get_processlist(&ptr);
    
   qsort(ptr,total,sizeof(PCB386),process_pid_sorter);
    /*first we obtain the total cputime of all the processes, this is computed by
      the summation of the delta cputimes*/
      
   for (i=0; i<total; i++) 
      grandtotalcputime += ( ptr[i].totalcputime - ptr[i].lastcputime );

    
   for (i=0; i<total; i++){
      char temp[255];
      PCB386 *ps;
      int percent_cpu_time;
      
      //obtain the size of the memory used by the process (no. of pages used)
      DWORD psize=getprocessmemory(ptr[i].meminfo,ptr[i].pagedirloc);	

      //convert to Kilobytes
      psize=((psize*0x1000)/1024)+4;

      //update the global total
      totalsize+=psize;
        
      printf("[%-3s]",itoa(ptr[i].processid,temp,10));
      if ( ptr[i].status & PS_ATTB_UNLOADABLE ) 
         textcolor(RED);
      else if (ptr[i].accesslevel == ACCESS_SYS) 
         textcolor(LIGHTBLUE);
      else
         textcolor(GREEN);
            
      if ( ptr[i].status & PS_ATTB_THREAD )     //This is a thread
         printf(" %-14s(t)",ptr[i].name);
      else
         printf(" %-14s   ",ptr[i].name);

      textcolor(WHITE);
      strcpy(levelstr,"?");

      //determine the access level and then show it on the screen
      if (ptr[i].accesslevel == ACCESS_SYS) 
         strcpy(levelstr,"kernel");
      else if (ptr[i].accesslevel == ACCESS_USER) 
         strcpy(levelstr,"user");

      //obtain the name of the parent process
      //dex32_getname(ptr[i].owner,sizeof(temp),temp);
      
      sprintf(temp,"%d",ptr[i].owner);

      /*compute for the percent CPU time*/
      percent_cpu_time = (ptr[i].totalcputime - ptr[i].lastcputime) * 100 / grandtotalcputime;
     
      sync_entercrit(&processmgr_busy);
     
      ps = ps_findprocess(ptr[i].processid);
      if (ps!=-1){
         ps->lastcputime = ptr[i].totalcputime;
      };
        
      sync_leavecrit(&processmgr_busy);
        
      printf(" %-11s %-14s %5dK %5ds %5ds(%2d)%%\n",levelstr,temp,psize,
         ptr[i].arrivaltime/100, ptr[i].totalcputime/100, percent_cpu_time);
   };

   printf("\nTotal              : %d processes (%d KB)\n",total,totalsize);
   printf("Time Since Startup : %d\n", ticks / context_switch_rate);
   printf("Legend: AT = Arrival Time, CT = CPU Time, %%CT = Percent CPU Time\n");
    
   free(ptr);
};


//get the total number of process
DWORD totalprocess(){
   int total=0;
   PCB386* ptr;
    
   total = get_processlist(&ptr);
   free(ptr);
   return total;
};

//get some stats from process
DWORD getprocessinfo(DWORD processid,PCB386 *data){
   PCB386 *ptr;
   sync_justwait(&processmgr_busy);
  
   ptr = ps_findprocess(processid);
   if (ptr!=-1){
      memcpy(data,ptr,ptr->size);
      return 1;
   };
   return 0;
   ;
};

//dex32_locktasks is used only by system functions to temporarily prevent
//other processes from taking control of the CPU
DWORD dex32_locktasks(){
    sigwait=1;
    return 1;
};

DWORD dex32_unlocktasks(){
    sigwait=0;
    return 1;
    ;
};

extern loadtsr();  // startup/asmlib.asm

void systemcall(){
    printf("A system call has been called\n");
    ;
};


/* This procedure gets called when the kernel boots up, it sets up
   the initial processes that would be run. Use the ps command to
   view the details of these processes. Hardcoded. EIP field of the 
   PCB points to the function that will be executed in the process.
*/
void process_init(){
   char tmp[255];
   PCB386 *kernel;

#ifdef __x86_64__
   dispatcher_stack_loc = (DWORD)(uintptr)(kstack_dispatcher + KSTACK_SIZE);
   sched_stack_loc = (DWORD)(uintptr)(kstack_sched + KSTACK_SIZE);
   pagefault_stack_loc = (DWORD)(uintptr)(kstack_pf + KSTACK_SIZE);
#endif
    
   //initialize the FPU
   asm volatile ("fninit");
   asm volatile ("fnsave ps_kernelfpustate");
    
   //Add the first process in memory which is the process kernel
   kernel=&sPCB;                                         //get a reference to the PCB
   memset(kernel,0,sizeof(PCB386));                      //initialize by zeroing it out
   kernel->next=kernel;                                  //next points to itself
   kernel->before=kernel;                                //before points to itself
   kernel->processid=0;                                  //process id is 0
   kernel->meminfo=0;                                    //no memory information
   strcpy(kernel->name,"dex_kernel");                    //sPCB
   kernel->accesslevel=ACCESS_SYS;                       //kernel mode
   kernel->status = PS_ATTB_LOCKED | PS_ATTB_UNLOADABLE; //it cannot be locked and unloaded, its the kernel!
   kernel->knext=knext;                                  //top of the heap
   kernel->outdev=consoleDDL;                            //the console defined in kernel32.c, output of kernel goes here
   kernel->pagedirloc=pagedir1;                          //set page directory to the first location
   kernel->cpu_affinity = 0;                             /* BSP-only */
   kernel->on_cpu = 0;

   //initialize the current FPU state
   memcpy(&kernel->regs2,&ps_kernelfpustate,sizeof(ps_kernelfpustate));
   fpu_init_default(&kernel->fpu);
    
   memset(&kernel->regs,0,sizeof(saveregs));             //initialize the execution context 
   kernel->regs.EIP=(DWORD)dex_init;                     //dex_init() in kernel32.c
   /* SysV entry: RSP ≡ 8 (mod 16). context_load does push;ret so leave RSP as final. */
   kernel->regs.ESP= DISPATCHER_STACK_LOC - 8;
   kernel->regs.CR3=pagedir1;
   kernel->regs.ES=SYS_DATA_SEL;
   kernel->regs.SS=SYS_STACK_SEL;
   kernel->regs.CS=SYS_CODE_SEL;
   kernel->regs.DS=SYS_DATA_SEL;
   kernel->regs.FS=SYS_DATA_SEL;
   kernel->regs.GS=SYS_DATA_SEL;
   kernel->regs.ESP0= DISPATCHER_STACK_LOC;
   kernel->regs.SS0= SYS_DATA_SEL;
   kernel->regs.EFLAGS=0x202;
   kernel->ctx.rip = (u64)(uintptr)dex_init;
   kernel->ctx.rsp = (u64)(DISPATCHER_STACK_LOC - 8);
   kernel->ctx.rflags = 0x202;
   kernel->ctx.cs = SYS_CODE_SEL;
   kernel->ctx.ss = SYS_DATA_SEL;
   kernel->ctx.cr3 = (u64)(uintptr)pagedir1;
   memcpy(&kernelPCB,kernel,sizeof(PCB386));             //create a copy of the kernel PCB
   setgdt(SYS_TSS,&kernelPCB.regs,103,0x89,0);

   sched_phead=kernel;                                   //set the head of the ready queue to the kernel                          

   /*----------------------------------------------------------------------------*/
   /*Set up the PCB of the scheduler*/
   schedp=&schedpPCB;
   memset(schedp,0,sizeof(PCB386));
   schedp->processid=1;                                  //process id is 1
   strcpy(schedp->name,"scheduler");                     //schedpPCB
   schedp->accesslevel=ACCESS_SYS;
   schedp->status = PS_ATTB_LOCKED | PS_ATTB_UNLOADABLE;
   schedp->knext=knext;
   schedp->pagedirloc=pagedir1;
   schedp->outdev = consoleDDL;
   schedp->regs.EIP=(DWORD)taskswitcher;                 //taskswitcher() defined here.
   schedp->regs.ESP= SCHED_STACK_LOC;
   schedp->regs.ES=SYS_DATA_SEL;
   schedp->regs.SS=SYS_STACK_SEL;
   schedp->regs.CS=SYS_CODE_SEL;
   schedp->regs.DS=SYS_DATA_SEL;
   schedp->regs.FS=SYS_DATA_SEL;
   schedp->regs.CR3=pagedir1;
   schedp->regs.GS=SYS_DATA_SEL;
   schedp->regs.EFLAGS=0;
   schedp->regs.SS0=SYS_STACK_SEL;
   schedp->regs.ESP0= SCHED_STACK_LOC;
   setgdt(SCHED_TSS,&(schedp->regs),103,0x89,0);

   loadtsr();  //terminate and stay resident the scheduler 

   //--------------------------------------------------------------------------------------
   //directly manipulate the keyboard handler PCB. uses dot(.)
   keyPCB.next=0;
   keyPCB.processid=2;
   strcpy(keyPCB.name,"keybhandler");
   keyPCB.accesslevel=ACCESS_SYS;
   keyPCB.priority=0;
   keyPCB.status = PS_ATTB_LOCKED | PS_ATTB_UNLOADABLE;
   keyPCB.knext=knext;
   keyPCB.pagedirloc=pagedir1;
   keyPCB.outdev= consoleDDL;
   memset(&keyPCB.regs,0,sizeof(saveregs));
   keyPCB.regs.EIP=(DWORD)kbdwrapper;                // defined in irqwrap.asm to follow calling convention
   keyPCB.regs.ESP= PAGEFAULT_STACK_LOC;
   keyPCB.regs.ES=SYS_DATA_SEL;
   keyPCB.regs.SS=SYS_STACK_SEL;
   keyPCB.regs.CS=SYS_CODE_SEL;
   keyPCB.regs.DS=SYS_DATA_SEL;
   keyPCB.regs.FS=SYS_DATA_SEL;
   keyPCB.regs.CR3=pagedir1;
   keyPCB.regs.GS=SYS_DATA_SEL;
   keyPCB.regs.EFLAGS=0;
   keyPCB.regs.SS0=SYS_STACK_SEL;
   keyPCB.regs.ESP0 = PAGEFAULT_STACK_LOC;
   setgdt(KEYB_TSS,&(keyPCB.regs),103,0x89,0);

   //------------------------------------------------------------------------------------
   //directly manipulate the mouse handler PCB. uses dot(.)
   mousePCB.next=0;
   mousePCB.processid=3;
   strcpy(mousePCB.name,"mousehandler");
   mousePCB.accesslevel=ACCESS_SYS;
   mousePCB.priority=0;
   mousePCB.status = PS_ATTB_LOCKED | PS_ATTB_UNLOADABLE;
   mousePCB.knext=knext;
   mousePCB.pagedirloc=pagedir1;
   mousePCB.outdev= consoleDDL;
   memset(&mousePCB.regs,0,sizeof(saveregs));
   mousePCB.regs.EIP=(DWORD)mousewrapper;            //defined in irqwrap.asm
   mousePCB.regs.ESP= PAGEFAULT_STACK_LOC;
   mousePCB.regs.ES=SYS_DATA_SEL;
   mousePCB.regs.SS=SYS_STACK_SEL;
   mousePCB.regs.CS=SYS_CODE_SEL;
   mousePCB.regs.DS=SYS_DATA_SEL;
   mousePCB.regs.FS=SYS_DATA_SEL;
   mousePCB.regs.CR3=pagedir1;
   mousePCB.regs.GS=SYS_DATA_SEL;
   mousePCB.regs.EFLAGS=0;
   mousePCB.regs.SS0=SYS_STACK_SEL;
   mousePCB.regs.ESP0 = PAGEFAULT_STACK_LOC;
   setgdt(MOUSE_TSS,&(mousePCB.regs),103,0x89,0);

   //------------------------------------------------------------------------------------
   /*set up the PCB of the pagefault handler*/
   pfPCB.next=0;
   pfPCB.processid=4;
   strcpy(pfPCB.name,"dex32_pfhandler");
   pfPCB.accesslevel=ACCESS_SYS;
   pfPCB.priority=0;
   pfPCB.status = PS_ATTB_LOCKED | PS_ATTB_UNLOADABLE;
   pfPCB.knext=knext;
   pfPCB.pagedirloc=pagedir1;
   memset(&pfPCB.regs,0,sizeof(saveregs));
   pfPCB.regs.EIP=(DWORD)pfwrapper;                            //defined in irqwrap.asm
   pfPCB.regs.ESP=0x7FFFE;
   pfPCB.regs.ES=SYS_DATA_SEL;
   pfPCB.regs.SS=SYS_STACK_SEL;
   pfPCB.regs.CS=SYS_CODE_SEL;
   pfPCB.regs.DS=SYS_DATA_SEL;
   pfPCB.regs.FS=SYS_DATA_SEL;
   pfPCB.regs.CR3=pagedir1;
   pfPCB.regs.GS=USER_DATA;
   pfPCB.regs.EFLAGS=0;
   setgdt(PF_TSS,&(pfPCB.regs),103,0x89,0);
   //create a duplicate copy
   memcpy(&pfPCB_copy.regs,&pfPCB.regs,sizeof(pfPCB.regs));
   setgdt(USER_CODE,0,0xFFFFF,0xFA,0xAF); /* long-mode 64-bit user CS (DPL=3) */
   setgdt(USER_DATA,0,0xFFFFF,0xF2,0xCF);
   setgdt(USER_STACK,0,0xFFFFF,0xF2,0xCF);
   setgdt(USER_TSS,0,103,0xE9,0);
   setcallgate(DEX_SYSCALL,SYS_CODE_SEL,systemcall,0,3);


   //set up the semaphore manager
   semaphore_head=(semaphore*)malloc(sizeof(semaphore));
   semaphore_head->owner=0;
   semaphore_head->next=0;
   semaphore_head->prev=0;

   //initialize current_process, which is the kernel
   current_process = kernel;
   current_process->workdir = vfs_root;

   /* processmgr_busy must be valid BEFORE the scheduler is
      installed: extension_override() calls sync_entercrit() on it,
      which busy-waits on var->busy.  (BSS is zeroed at startup, but
      make the dependency explicit and robust.) */
   processmgr_busy.busy = 0;
   processmgr_busy.wait = 0;
   ps_scheduler_install();    //defined in kernel/process/scheduler.c

#ifdef DEBUG_STARTUP
   printf("process manager: done.\n");
#endif

    
   printf("Starting process manager...\n");
};

