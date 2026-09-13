/*
  Name: Exception handling module
  Copyright: 
  Author: Joseph Emmanuel DL Dayo
  Date: 13/03/04 06:30
  Description: This module provides exception handlers for the operating system. There are
  exception handlers for GPFs, page faults and divide by zero. The page fault handler also
  handles demand loading requests.
  
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

void GPFhandler(DWORD address)
  {
  char temp[255];
    stopints();
    
    exc_showdump(address,GENERAL_PROTECTION_FAULT,0);
    exc_recover();
        
    while (1) {};

  };

/* x86_64 #13 handler with full fault context (RIP, CR2, error code, CS/SS/RSP, CR3).
   The legacy saveregs in the PCB is not populated by the software context switch,
   so this prints live values captured in the wrapper instead. */
void wrapper_call_trap(unsigned long retaddr, unsigned long cs, unsigned long rflags)
{
  static volatile unsigned long wcnt = 0;
  char ab[160];
  if (wcnt < 16) {
    wcnt++;
    sprintf(ab, "WRAPCALL ret=0x%lx cs=0x%lx rflags=0x%lx\n",
            retaddr, cs, rflags);
    serial_puts(ab);
  }
}

static void exc_kstack_report(const char *tag, unsigned long frsp,
                              unsigned long rip, unsigned long rbp);

void GPFhandler64(struct gpf_info *fi, unsigned long saved_rax, unsigned long saved_rcx)
  {
    static volatile int gpf_busy[MAX_CPUS];
    unsigned int err = fi->err;
    const char *what = "unknown";
    int cpu = smp_cpu_id();

   if (gpf_busy[cpu]) {
        serial_puts("GPF64: re-entered -> halt\n");
        while (1) {}
    }
    gpf_busy[cpu] = 1;

    if (err & 2)
        what = (err & 1) ? "reserved-bit" : "segment-not-present";
    else if (err & 4)
        what = (err & 1) ? "TSS-invalid" : (err & 2) ? "stack" : "TSS-bad";
    else
        what = (err == 0) ? "noncanonical-or-other" :
               ((err & 1) ? "base/limit" : "segment-index");

    /* Keep the #GP path small.  The previous register dump and gpf_probe_store()
       ran while the faulting context was still live and could itself fault,
       turning a user #GP into a cascade.  Print one bounded line, then kill
       user processes or halt for kernel faults. */
    {
        static volatile unsigned long gpf_print_cnt = 0;
        char line[220];
        gpf_print_cnt++;
        if (gpf_print_cnt <= 8 || (gpf_print_cnt & 0x3f) == 0) {
           sprintf(line,
                 "GPF64: err=0x%x(%s) rip=0x%llx cr2=0x%llx cs=0x%x ss=0x%x cr3=0x%llx proc=%s\n",
                 (unsigned)err, what, fi->rip, fi->cr2,
                 (unsigned)fi->cs, (unsigned)fi->ss,
                 (unsigned long long)fi->cr3,
                 current_process ? current_process->name : "?");
           serial_puts(line);
           /* Any RIP outside plausible user text -- kernel text, kernel BSS,
              or the kernel heap -- means a corrupted return address or a wild
              indirect call.  Report the stack it happened on before we kill or
              halt, otherwise the evidence dies with the process. */
           if (!(fi->rip >= (unsigned long long)MEM_USER_ELF_BASE
                 && fi->rip < (unsigned long long)MEM_KHEAP_BASE))
              exc_kstack_report("GPF64", (unsigned long)fi->rsp,
                                (unsigned long)fi->rip, 0);
        }
    }
    (void)saved_rax;
     (void)saved_rcx;
     /* A GPF on a KERNEL thread (ACCESS_SYS) is a genuine kernel bug. The old
        discriminator (processid != 0) misclassified kernel threads such as
        ap_work (pid 16) as user processes and called exc_recover() -> exit(1),
        killing the thread while it held ready_lock and wedging every CPU.
        Only true user processes (ACCESS_USER) may be killed-and-resumed. */
#ifdef __x86_64__
     {
        /* Kernel-image RIP is a kernel bug (cert ps_switchto GPF).  Do not
           retarget current to the CR3 user and kill it: that leaked vfs_busy
           and WATCHDOG-spun gcc after as.exe was recovered. */
        int kernel_rip = (fi->rip >= 0x100000ULL &&
                          fi->rip < (unsigned long long)MEM_KERNEL_LIMIT);
        PCB386 *owner = kernel_rip ? 0
                          : ps_find_by_cr3((unsigned long)fi->cr3);
        if (owner && owner->accesslevel == ACCESS_USER &&
            owner != current_process &&
            cpu >= 0 && cpu < MAX_CPUS)
           cpus[cpu].current = owner;
     }
#endif
     if (current_process && current_process->accesslevel == ACCESS_USER
         && (fi->rip < 0x100000ULL
             || fi->rip >= (unsigned long long)MEM_USER_ELF_BASE)) {
          /* User-process fault: recover like the page-fault path. exc_recover()
             sets dex32_child_faulted and calls exit(1), which kills the faulted
             child and context-switches to its parent, abandoning this wrapper's
             stack frame (the gpfwrapper iretq is never reached). The parent's
             waitpid then sees the crash instead of the child spinning. */
          serial_puts("\nGPF64: user fault -> killing process, resuming parent\n");
         gpf_busy[cpu] = 0;   /* allow a later fault (e.g. the fallback run) to dump */
         exc_recover();
         /* not reached: exc_recover() -> exit(1) switched away */
      }

    gpf_busy[cpu] = 0;
    serial_puts("\nGPF64: kernel fault -> halt\n");
    while (1) {}
  };
  
void exc_invalidtss(DWORD address)
  {
  char temp[255];
    stopints();
    
    exc_showdump(address,INVALID_TSS,0);
    exc_recover();
        
    while (1) {};
    startints();

  };

/* #8 double fault diagnostic. Called by doublefaultwrapper with the
   hardware-frame RIP/CS/RFLAGS and CR2. Prints then halts. */
void exc_doublefault(unsigned long rip, unsigned long cs,
                     unsigned long rflags, unsigned long cr2)
  {
    char line[160];
    stopints();
    sprintf(line, "DBLFLT: rip=0x%lx cs=0x%04lx rflags=0x%lx cr2=0x%lx\n",
            rip, cs & 0xFFFF, rflags, cr2);
    serial_puts(line);
    while (1) {}
  };

void divide_error(DWORD address)
  {
    stopints();
    exc_showdump(address,DIVIDE_ERROR,0);
    exc_recover();
    while (1) {};
    startints();

  };
  

//show the contents of the EFLAGS register
void exc_dumpflags(DEX32_DDL_INFO *output, DWORD flags)
{
    int ID,VIP,VIF,AC,VM,RF,NT,IOPL,OF;
    int DF,IF,TF,SF,ZF,AF,PF,CF;
    
    //Perform a bit by bit comparitson
    CF   =  (flags & 1) ?   1 : 0;
    PF   =  (flags & 4) ?   1 : 0;
    AF   =  (flags & 16) ?  1 : 0;
    ZF   =  (flags & 64) ?  1 : 0;
    SF   =  (flags & 128) ? 1 : 0;
    TF   =  (flags & 256) ? 1 : 0;
    IF   =  (flags & 512) ? 1 : 0;
    DF   =  (flags & 1024) ? 1 : 0;
    OF   =  (flags & 2048) ? 1 : 0;
    IOPL =  (flags & 0x3000) >> 12;
    NT   =  (flags & 0x4000) ? 1 : 0;
    RF   =  (flags & 0x10000) ? 1 : 0;
    VM   =  (flags & 0x20000) ? 1 : 0;
    AC   =  (flags & 0x40000) ? 1 : 0;
    
    DDLprintf(output,
    "CF=%d PF=%d AF=%d ZF=%d SF=%d TF=%d IF=%d DF=%d OF=%d IO=%d NT=%d RF=%d VM=%d AC=%d\n",
    CF,PF,AF,ZF,SF,TF,IF,DF,OF,IOPL,NT,RF,VM,AC);

};



volatile unsigned long pf64_rip;
volatile unsigned long pf64_cr2;

//show the contents of the CPU registers and other information
void exc_showdump(DWORD location,int type,DWORD pf_info)
{
   int ret;
   char fault_type[80];
   DWORD pageentry,direntry,ktopheap = knext;
   DEX32_DDL_INFO *beforeout,*showdumpout;


   strcpy(fault_type,"Exception -");

   //convert the value given in type to string  
   if (location >= 0xFFFF0000 && location <=0xFFFFFFF0) strcat(fault_type,"unresolved import error");
      else
   if (type == PAGE_FAULT) sprintf(fault_type,"Page fault (0x%x)",pf_info);
      else
   if (type == GENERAL_PROTECTION_FAULT) strcat(fault_type,"General Protection fault");
      else
   if (type == DIVIDE_ERROR) strcat(fault_type,"Divide by zero ");
      else
   if (type == INVALID_TSS) strcat(fault_type,"Invalid Task State Segment");
      else
   strcat(fault_type,"unknown fault");   
   
   #ifdef FULLSCREENERROR   
   direntry=getpagetablephys(location, current_process->pagedirloc);
   pageentry=getphys(location,current_process->pagedirloc);
   showdumpout = Dex32CreateDDL();
   beforeout = Dex32SetActiveDDL(showdumpout);
   
   Dex32Clear(showdumpout);
   Dex32SetTextBackground(showdumpout,RED);
   Dex32SetTextColor(showdumpout,WHITE);
   DDLprintf(&showdumpout,"%-79s\n",fault_type);
   Dex32SetTextBackground(showdumpout,BLACK);
   DDLprintf(&showdumpout,"Faulting process                       :%s\n",current_process->name);
   DDLprintf(&showdumpout,"Process ID                             :%d\n",current_process->processid);
   
   DDLprintf(&showdumpout,"\n============ <Memory Access Information>=================\n");
   DDLprintf(&showdumpout,"Tried to access invalid memory location: 0x%x\n",location);
   DDLprintf(&showdumpout,"Page directory entry at that address is: 0x%x\n",direntry);
   DDLprintf(&showdumpout,"Page table entry at that address is    : 0x%x\n",pageentry);
   DDLprintf(&showdumpout,"Address of faulting instruction is     : 0x%x\n",current_process->regs.EIP);
   DDLprintf(&showdumpout,"Kernel Page Directory : 0x%x  Process Page Directory: 0x%x",pagedir1,current_process->regs.CR3);

   DDLprintf(&showdumpout,"\n============ <Register values at time of fault>==========\n");
   DDLprintf(&showdumpout,"EAX=0x%x EBX=0x%x ECX=0x%x EDX=0x%x ",current_process->regs.EAX,
         current_process->regs.EBX,current_process->regs.ECX,current_process->regs.EDX);

   DDLprintf(&showdumpout,"EBP=0x%x EDI=0x%x \nESI=0x%x ESP=0x%x ",
         current_process->regs.EBP,current_process->regs.EDI,
         current_process->regs.ESI,current_process->regs.ESP);
   
   DDLprintf(&showdumpout,"CS=0x%x DS=0x%x ES=0x%x SS=0x%x ",
      current_process->regs.CS,current_process->regs.DS,current_process->regs.ES,
      current_process->regs.SS);
   
   DDLprintf(&showdumpout,"FS=0x%x GS=0x%x \n",current_process->regs.FS,current_process->regs.GS);
   
   exc_dumpflags(&showdumpout,current_process->regs.EFLAGS);
    
   DDLprintf(&showdumpout,"\n\nAPI call information\n");
   DDLprintf(&showdumpout,"=========================================================\n"); 
   DDLprintf(&showdumpout,"Process Top of Heap location: 0x%x\n",(DWORD)current_process->knext);
   DDLprintf(&showdumpout,"Kernel Top of Heap location: 0x%x\n",ktopheap);
   DDLprintf(&showdumpout,"Last system calls:(1) : 0x%x ,(2-last): 0x%x\n",
      current_process->cursyscall[0],current_process->cursyscall[1]);
   DDLprintf(&showdumpout,"Context information:  device # %d(%s), function # %d(%s)\n",
             current_process->context,
             devmgr_getname(current_process->context),
             current_process->function,
             get_function_name(current_process->context,current_process->function));


   if (current_process->op_success==1)    DDLprintf(&showdumpout,"syscall terminated normally\n");
      else
      {
         DDLprintf(&showdumpout,"Fault occured during system call.\n");
      };

   /* Always copy a compact dump to COM1 so headless qemu tests can see
      faults. Do not wait for a key — kb_pause() hangs -display none. */
   {
      char line[192];
        sprintf(line, "%s process=%s eip=0x%x rip=0x%lx cr2=0x%lx\n",
                fault_type, current_process->name,
                current_process->regs.EIP, pf64_rip, pf64_cr2);
       serial_puts(line);
   }

   Dex32SetActiveDDL(beforeout);
  
   #else
      printf("%s\n",fault_type);
      printf("faulting process                       :%s\n",current_process->name);
      printf("Tried to access invalid memory location: 0x%x\n",location);
      printf("Page directory entry at that address is: 0x%x\n",pageentry);
      printf("Address of faulting instruction is     : 0x%x\n",current_process->regs.EIP);
   #endif
   
   stopints();

   enable_taskswitching();
   
};

/* Bounded kernel-stack report for a fault taken with a kernel RIP.  A smashed
   return address on the per-process IRQ kstack shows up as a RIP outside the
   kernel text (BSS / heap), and without the stack window there is no way to
   tell an overflow from a shared or stale kstack.  Prints the kstack bounds,
   the nesting depth, and every kernel-text-looking qword near the fault RSP.
   Reads are guarded by the caller's pf_busy latch; a nested fault here is
   dropped by the nested path rather than recursing. */
/* Which process owns the IRQ kstack containing `addr`?  A kernel RIP or RSP
   that lands in some OTHER process's kstack is the signature of two contexts
   sharing one stack, which is indistinguishable from generic heap corruption
   without this lookup. */
static int exc_kstack_owner(unsigned long addr, unsigned long *base_out)
{
#ifdef __x86_64__
    PCB386 *head = sched_gethead();
    PCB386 *p = head;

    if (!head || !addr)
       return -1;
    do {
       unsigned long b = (unsigned long)(uintptr)p->kstack_base;
       if (b && addr >= b && addr < (unsigned long)p->kstack_top) {
          if (base_out)
             *base_out = b;
          return (int)p->processid;
       }
       p = p->next;
    } while (p && p->before && p != head);
#endif
    (void)addr; (void)base_out;
    return -1;
}

static void exc_kstack_report(const char *tag, unsigned long frsp,
                              unsigned long rip, unsigned long rbp)
{
    char line[256];
    unsigned long base = 0, top = 0, obase = 0, uframe = 0;
    int inside = 0;
    unsigned long i;

    if (current_process) {
#ifdef __x86_64__
       base = (unsigned long)(uintptr)current_process->kstack_base;
       top = (unsigned long)current_process->kstack_top;
       uframe = (unsigned long)current_process->irq_user_rsp;
#endif
       inside = (base && frsp >= base && frsp < top);
    }
    sprintf(line,
            "%s-KSTACK rip=0x%lx frsp=0x%lx rsp&15=%lu rbp=0x%lx rbp&7=%lu "
            "base=0x%lx top=0x%lx used=%ld uframe=0x%lx inside=%d pid=%d\n",
            tag, rip, frsp, frsp & 15UL, rbp, rbp & 7UL, base, top,
            inside ? (long)(top - frsp) : -1L, uframe, inside,
            current_process ? (int)current_process->processid : -1);
    serial_puts(line);

    /* Name the owner of whatever kstack the fault RSP and RIP fell into. */
    {
       int rsp_owner = exc_kstack_owner(frsp, &obase);
       int rip_owner = exc_kstack_owner(rip, 0);
       sprintf(line, "%s-KOWNER rsp_owner=%d rsp_base=0x%lx rip_owner=%d\n",
               tag, rsp_owner, obase, rip_owner);
       serial_puts(line);
    }

    /* Only walk a stack we can prove is mapped kernel memory. */
    if (!(frsp >= MEM_KHEAP_BASE && frsp < MEM_KHEAP_END)
        && !(frsp >= MEM_KERNEL_LOAD && frsp < MEM_KERNEL_LIMIT))
       return;
    for (i = 0; i < 24; i++) {
       unsigned long slot = frsp + i * 8;
       if (slot + 8 >= MEM_KHEAP_END)
          break;
       sprintf(line, "%s-KRAW +%lu 0x%lx\n", tag, i * 8,
               *(volatile unsigned long *)slot);
       serial_puts(line);
    }
}

/* Global demand-paging diagnostics (PF64-DIAG). Exposed via the 'pf64stats'
   console / shell2 command for live introspection without a reboot. */
static volatile unsigned long pf64_demand_total = 0;
static volatile unsigned long pf64_demand_recovered = 0;
static volatile unsigned long pf64_elf_region = 0;
static volatile unsigned long pf64_committed_heap = 0;
static volatile unsigned long pf64_past_brk = 0;
static volatile unsigned long pf64_print_cnt = 0;

void pf64_stats(unsigned long *total, unsigned long *recovered,
                unsigned long *elf, unsigned long *heap,
                unsigned long *pastbrk)
{
    if (total) *total = pf64_demand_total;
    if (recovered) *recovered = pf64_demand_recovered;
    if (elf) *elf = pf64_elf_region;
    if (heap) *heap = pf64_committed_heap;
    if (pastbrk) *pastbrk = pf64_past_brk;
}

// the core function that handles page faults, it is also
// dirctly linked to the virtual memory manager of the operating system
DWORD pagefaulthandler(unsigned long location, DWORD fault_info,
                        unsigned long rip, unsigned long *saved_regs)
  {
   DWORD ret,mm,i;
    static volatile int pf_busy[MAX_CPUS];
    int fault_cpu=smp_cpu_id();
    int nested = pf_busy[fault_cpu];
    int kernel_rip = (rip >= 0x100000UL &&
                      rip < (unsigned long)MEM_USER_ELF_BASE);
    PCB386 *fault_proc;
    unsigned long fault_cr3 = 0;
    pf_busy[fault_cpu] = 1;
     stopints();
     pfoccured=1;//set the pfoccured register of the task scheduler
     pf64_rip = rip;
     pf64_cr2 = location;
#ifdef __x86_64__
     __asm__ __volatile__("movq %%cr3, %0" : "=r"(fault_cr3));
     fault_proc = ps_find_by_cr3(fault_cr3);
     if (!fault_proc)
        fault_proc = current_process;
     if (fault_proc && fault_proc != current_process &&
         (pf64_print_cnt < 16)) {
        char sl[96];
        sprintf(sl, "PF64-STALE-CURRENT cur=%d cr3pid=%d cr3=0x%lx\n",
                current_process ? (int)current_process->processid : -1,
                (int)fault_proc->processid, fault_cr3);
        serial_puts(sl);
     }
#else
     fault_proc = current_process;
#endif
    if (nested) {
       /* serial_puts from this handler previously re-entered uart_putc_raw
          and halted the cert CPU.  Do not print.  Kernel RIP: drop the
          nested frame so the outer handler can unwind; user RIP: kill. */
       pf_busy[fault_cpu] = 0;
       if (!kernel_rip && current_process &&
           current_process->accesslevel == ACCESS_USER) {
          exc_recover();
       }
       return 0;
    }
    if (!current_process || !(DWORD)(uintptr)current_process->pagedirloc) {
       serial_puts("PF64: bad current_process/pagedirloc -> halt\n");
       while (1) {}
    }
   {
        char line[256];
        unsigned long cr3 = 0, cr4 = 0;
        pf64_print_cnt++;
        if (pf64_print_cnt <= 16 || (pf64_print_cnt & 0x3f) == 0) {
           __asm__ __volatile__("movq %%cr3, %0" : "=r"(cr3));
           __asm__ __volatile__("movq %%cr4, %0" : "=r"(cr4));
           sprintf(line, "PF64 cr2=0x%lx rip=0x%lx err=0x%x cr3=0x%lx cr4=0x%lx\n",
                   location, rip, (unsigned)fault_info, cr3, cr4);
           serial_puts(line);
        }
      }
      if (saved_regs && (pf64_print_cnt <= 16 || (pf64_print_cnt & 0x3f) == 0)) {
          char line[256];
          /* PUSH_ALL saves 15 qwords (r15..rax), then the hardware frame is
             error/RIP/CS/RFLAGS. The RSP at the faulting instruction is not
             stored in the frame; it is the frame address plus 15*8 + 4*8. Do
             not dereference it here: a corrupt RSP would turn a diagnostic
             into a nested fault. */
          unsigned long frsp = (unsigned long)(saved_regs + 21);
          sprintf(line,
                  "PF64 regs rax=0x%lx rbx=0x%lx rcx=0x%lx rdx=0x%lx "
                  "rsi=0x%lx rdi=0x%lx rbp=0x%lx frsp=0x%lx\n",
                  saved_regs[14], saved_regs[13], saved_regs[12],
                  saved_regs[11], saved_regs[10], saved_regs[9],
                  saved_regs[8], frsp);
          serial_puts(line);
          if (kernel_rip)
             exc_kstack_report("PF64", frsp, rip, saved_regs[8]);
      }

#ifdef __x86_64__
    {
       int cow_result=userpd_handle_cow(
         (u64 *)(uintptr)fault_proc->pagedirloc,
         (unsigned long long)location,(unsigned)fault_info);
      if (cow_result>0) {
         pf_busy[fault_cpu]=0;
         return 0;
      }
      if (cow_result<0) {
         serial_puts("PF64: COW resolution failed\n");
         pf_busy[fault_cpu]=0;
         exc_recover();
         while (1) {}
      }
   }
   mm=getphys64((unsigned long long)location,fault_proc->pagedirloc);
   /* Not present: first try to restore a missing kernel identity page.
       userpd_create() zeroes most 2MiB identity blocks in the private PD0,
       but kernel code can still run under the user CR3 and reference low
       identity addresses.  Mapping a fresh zero frame for those addresses
       silently corrupts kernel state, so map VA==PA for kernel RIPs. */
    if ((mm & PG_PRESENT) == 0) {
       DWORD *pd = (DWORD *)(uintptr)fault_proc->pagedirloc;
       if (pd && pd != pagedir1
           && rip < (unsigned long)MEM_USER_ELF_BASE
           && (unsigned long long)location < 0x40000000ULL) {
          if (userpd_map_identity_page((u64 *)(uintptr)pd,
                                       (unsigned long long)location,
                                       PG_WR | PG_USER)) {
             pf_busy[fault_cpu] = 0;
             return 0;
          }
       }
       if (pd && pd != pagedir1
           && (unsigned long long)location >= (unsigned long long)MEM_USER_ELF_BASE
            && (unsigned long long)location < (unsigned long long)MEM_USER_WIN_END) {
           /* Round-4 self-host diagnostics: classify a not-present user-window
              fault against the committed heap (knext) and catch runaway
              demand-paging loops that would otherwise hang the guest silently.
              A committed-heap fault means sbrk/commit under-mapped; PAST-BRK
              means the user code wrote beyond its break (overflow). */
           {
              static volatile unsigned long pdemand_cnt = 0;
              static volatile void *pdemand_proc = 0;
              unsigned long loc   = (unsigned long)location;
              unsigned long knext = (unsigned long)(uintptr)fault_proc->knext;
              const char *cls;
              if (loc < (unsigned)MEM_USER_HEAP)        cls = "elf-region";
               else if (loc < knext)                     cls = "committed-heap";
               else                                      cls = "PAST-BRK-OVERFLOW";
               pf64_demand_total++;
               if (cls == "elf-region")       pf64_elf_region++;
               else if (cls == "committed-heap") pf64_committed_heap++;
               else                            pf64_past_brk++;
               if (pdemand_proc == (void *)(uintptr)fault_proc)
                 pdemand_cnt++;
              else { pdemand_proc = (void *)(uintptr)fault_proc; pdemand_cnt = 1; }
              if (pdemand_cnt <= 8 || (pdemand_cnt & 0x3f) == 0) {
                  char line[256];
                  sprintf(line,
                    "PF64-DIAG %s n=%lu cls=%s loc=0x%lx knext=0x%lx rip=0x%lx\n",
                    fault_proc->name ? fault_proc->name : "?",
                    (unsigned long)pdemand_cnt, cls, loc, knext, rip);
                  serial_puts(line);
               }
            }
            /* Do not lazily map zero pages for user-window faults.  The ELF
              loader and sbrk/commit paths eagerly map all pages a well-behaved
              process should touch; a not-present fault here is a bad pointer
              or an under-commit bug.  Silently mapping it hides the bug and
              can cascade into kernel corruption.  Skip exc_showdump() here:
              the faulting context may already have a bad stack, and the dump
              path has previously turned a user fault into a nested PF64. */
           pf_busy[fault_cpu] = 0;
           if (fault_proc->accesslevel == ACCESS_USER) {
              if (fault_proc != current_process &&
                  fault_cpu >= 0 && fault_cpu < MAX_CPUS)
                 cpus[fault_cpu].current = fault_proc;
              serial_puts("PF64: user not-present -> killing process\n");
              exc_recover();
           }
           serial_puts("PF64: not-present ACCESS_SYS -> ignored\n");
           return 0;
       }
       {
          char line[180];
          sprintf(line,
                  "PF64: not-present cr2=0x%lx rip=0x%lx err=0x%x proc=%s mm=0x%x\n",
                  (unsigned long)location, rip, (unsigned)fault_info,
                  fault_proc->name ? fault_proc->name : "?",
                  (unsigned)mm);
          serial_puts(line);
       }
       pf_busy[fault_cpu] = 0;
       if (fault_proc->accesslevel == ACCESS_USER) {
          if (fault_proc != current_process &&
              fault_cpu >= 0 && fault_cpu < MAX_CPUS)
             cpus[fault_cpu].current = fault_proc;
          serial_puts("PF64: user not-present -> killing process\n");
          exc_recover();
       }
       serial_puts("PF64: not-present ACCESS_SYS -> ignored\n");
       return 0;
    }
#else
   (void)rip;
   mm=getphys(location,current_process->pagedirloc);
#endif
   
   if (mm&PG_DEMANDLOAD)
         {
             //A page marked as demand paged has been called so
             //we allocate a physical frame to this page
             DWORD pg,pageadr=(DWORD)mempop();

           //  printf("demand paged %s..",itoa(location,temp,16));
             if (pageadr==0) //we're out of physical frames, find a way to get one
             pageadr=obtainpage(); //call the VMM to get a page

             pg=(DWORD)getvirtaddress((DWORD)current_process->pagedirloc);

             maplineartophysical2(pg,location,
             pageadr,PG_PRESENT | PG_USER | PG_WR);
   
             pg=(DWORD)getvirtaddress(pageadr);
             memset(pg,0,0x1000);
                
             //Finished allocation, let the task scheduler take over
             setattb(PF_TSS,0x89); //reset the TSS attribute
             taskswitch();
             pf_busy[fault_cpu] = 0;
             return 0;
        ;};
   
   // a copy-on-write page has been written to, we therefore duplicate this page
   // so that forked processes have their own unique set of data.     
   if (mm&PG_COPYWRITE) 
        {
                DWORD destpg,pdirpg;
                DWORD pageadr= (DWORD) mempop(); //allocate new memory
                char pagebuf[0x1000];
                if (pageadr==0) //we're out of physical frames, find a way to get one
                //call the VMM to get a page, "assume" it works
                pageadr=obtainpage(); 

                                
                //obtain a virtual memory address for this physical address
                destpg=(DWORD)getvirtaddress(pageadr); 
                                                       
                //copy the content of the copy-on-write page to the new page         
                memcpy(destpg, location&0xFFFFF000 ,0x1000);
                
                //obtain a virtual address for the page directory
                pdirpg=(DWORD)getvirtaddress((DWORD)current_process->pagedirloc);
                
                //update the page directory of the process to reflect this change
                maplineartophysical2(pdirpg,location,
                pageadr,PG_PRESENT | PG_USER | PG_WR);

                #ifdef DEBUG_FORK                 
                printf("COPY ON WRITE DETECTED.\n");
                #endif
                
                setattb(PF_TSS,0x89); //reset the TSS attribute
                taskswitch();
#ifdef __x86_64__
                pf_busy[fault_cpu] = 0;
                return 0;
#endif
        };
#ifdef __x86_64__
   /* Present PTE but CPU still faulted (TLB/private-PD mismatch, or a
      write to a mapped stack hole).  Never halt the CPU: that wedged the
      SMP=4 cert after make.exe PF64 rip in the user stack.  getphys() used
      to truncate CR2 to 32 bits so an execute at 0xb000000000 looked like
      a present page at 0. */
   pf_busy[fault_cpu] = 0;
   if (!kernel_rip) {
      if (fault_proc && fault_proc->accesslevel == ACCESS_USER) {
         if (fault_proc != current_process &&
             fault_cpu >= 0 && fault_cpu < MAX_CPUS)
            cpus[fault_cpu].current = fault_proc;
         serial_puts("PF64: user unhandled -> killing process\n");
         exc_recover();
      }
      serial_puts("PF64: user RIP with stale current -> ignored\n");
      return 0;
   }
   if (fault_proc && fault_proc->accesslevel == ACCESS_USER) {
      serial_puts("PF64: kernel unhandled -> killing user\n");
      if (fault_proc != current_process &&
          fault_cpu >= 0 && fault_cpu < MAX_CPUS)
         cpus[fault_cpu].current = fault_proc;
      exc_recover();
   }
   serial_puts("PF64: kernel unhandled (ignored)\n");
   return 0;
#else
   exc_showdump(location,PAGE_FAULT,mm);
   exc_recover();
   while (1) {};
   startints();
#endif
  };

  
//This function is called when DEX is trying to recover from a fault
 void exc_recover(){
    /* Keep this path small and re-entrancy-safe. The faulting context may
       already have a bad stack, serial pointer, or current-process slot;
       printf() here previously turned a recoverable user fault into a nested
       PF64 inside serial_puts. */
    if (!current_process || current_process->processid == 0) {
       serial_puts("PF64: kernel fault -> halt\n");
       while (1);
    }
    /* Non-zero so waiters can distinguish crash from clean exit(0). */
    dex32_child_faulted = 1;
    exit(1);
    startints();
    while (1);
 };

