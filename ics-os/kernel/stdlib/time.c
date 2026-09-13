DWORD time_count = 0,  //used to store the number of seconds since dex was booted
aux_time2=0;   //since the OS has the timer set to interrupt 200 times a second
               //an auxillary counter is required so that it increments time_count
               //if it reaches 200
               
int time_monthdays[]= {0,31,59,90,120,151,181,212,243,273,304,334,365}; 

//the tme returned by the timer chip is in BCD, so we have to
//perform some conversions to binary
DWORD bcdtobinary(DWORD b)
  {

   DWORD x= b & 0xff,c,r;
   r = x & 0xF;
   c = x >> 4;
   return (c*10+r);

  ;};

char *getmonthname(int month,char *str)
  {
     switch (month)
       {
         case 1 : strcpy(str,"January"); break;
         case 2 : strcpy(str,"Febuary"); break;
         case 3 : strcpy(str,"March");break;
         case 4 : strcpy(str,"April");break;
         case 5 : strcpy(str,"May");break;
         case 6 : strcpy(str,"June");break;
         case 7 : strcpy(str,"July");break;
         case 8 : strcpy(str,"August");break;
         case 9 : strcpy(str,"September");break;
         case 10: strcpy(str,"October");break;
         case 11: strcpy(str,"November");break;
         case 12: strcpy(str,"Decemeber");break;
       };
    return str;
  };

char *datetostr(dex32_datetime *d,char *str)
  {
     char temp1[20],temp2[20],temp3[20];
     sprintf(str,"%s/%s/%s",itoa(d->month,temp1,10),
            itoa(d->day,temp2,10),itoa(d->year,temp3,10));
     return str;
  };

void getdatetime(dex32_datetime *d) //gets the date nd time
  {
     DWORD x;

     //seconds
     outportb(0x70,0);
     //delay(1);
     x=inportb(0x71);
     d->sec=bcdtobinary(x); //convert to binary

     //minutes
     outportb(0x70,2);
     //delay(1);
     x=inportb(0x71);
     d->min=bcdtobinary(x);

     //hours
     outportb(0x70,4);
     //delay(1);
     x=inportb(0x71);
     d->hour=bcdtobinary(x);

     outportb(0x70,0x7);
     //delay(1);
     x=inportb(0x71);
     d->day=bcdtobinary(x);

     outportb(0x70,0x8);
     //delay(1);
     x=inportb(0x71);
     d->month=bcdtobinary(x);

     outportb(0x70,9);
     //delay(1);
     x=inportb(0x71);
     d->year=bcdtobinary(x);
     if (d->year<80) d->year+=2000; //adjust for the year 2000
  };
  
//returns time in milliseconds
DWORD time_gettime()
{
return time_count;
};

//returns time in milliseconds
DWORD getprecisetime()
{
 return (time_count*100+(aux_time2/2));
};

int time()
   {
     int totaldays = (time_systime.year - 1970)*365;
     int totalseconds,totalminutes,totalhours;
     if (time_systime.year%4 !=0 || time_systime.month>2)
     totaldays+=time_systime.year/4;
        else
     { 
     totaldays+=(time_systime.year/4) - 1;   
     };
     
     if (time_systime.month>2&&time_systime.year%4==0) totaldays+=1;
     totaldays+=time_monthdays[time_systime.month-1];
     totaldays+=time_systime.day;
     totalhours = totaldays*24 + time_systime.hour;
     totalminutes = totalhours*60 + time_systime.min;
     totalseconds = totalminutes * 60;
     totalseconds += time_systime.sec;
     return totalseconds;
   };    

int time_getmycputime()
{
    return current_process->totalcputime;
};
//increments the system time by one millisecond
void time_incrementtime()
{
    time_systime.adj++;
    if (time_systime.adj>context_switch_rate/100)
      {
       time_systime.ms ++;
       time_systime.adj = 0;
      }; 
    if (time_systime.ms>=100)
      {
         time_systime.ms = 0;
         time_systime.sec++;
         if (time_systime.sec>=60)
           {
               time_systime.sec=0;
               time_systime.min++;
               if (time_systime.min>=60)
               {
               time_systime.min=0;
               time_systime.hour++;
               if (time_systime.hour>=24)
                   time_systime.hour=0;
               };
           };
      };
};
   
/* Diagnostic spin watchdog for selfhost-stage1.  In cooperative mode the
    timer no longer preempts (schedule_from_timer returns early), so a user
    process that never calls taskswitch() holds the CPU forever.  time_handler()
    still runs on every tick, so track the current process and report any that
    keeps the CPU for more than a few seconds without a voluntary switch, along
    with the running syscall total (delta between reports = syscall rate) and the
    last two syscalls it issued.  Diagnostic-only: never changes scheduling. */
 /* Wide interrupt-frame window captured by irqwrap.S timerwrapper at
     [rsp+0..240] (31 qwords) plus the pre-PUSH_ALL RSP (wd_krsp).  The watchdog
     filters for code-range values so the live user RIP (user ELF range) or the
     kernel return addresses (kernel text range) stand out for symbolization. */
  volatile unsigned long wd_frame_cpu[8][31];
   volatile unsigned long wd_krsp_cpu[8] = {0};

  /* 0 = not a code pointer, 1 = kernel text, 2 = user ELF (non-PIE) code. */
  static int wd_kind(unsigned long v)
    {
     if (v >= 0x100000 && v < 0x300000)
        return 1;
     if (v >= 0x400000 && v < 0x1800000)
        return 2;
     return 0;
    };

  static void selfhost_spin_watchdog(void)
    {
     extern volatile int selfhost_cooperative_ready;
      extern volatile unsigned long diag_sc_count;
      extern int smp_cpu_id(void);
      extern int smp_idle_guard_check(int id);

      /* Idle tasks run all IRQ/scheduler C on their own small stack; check the
         guard words below it so an overflow is named here instead of silently
         scribbling on the neighbouring CPU's idle stack (see smp.c). */
      smp_idle_guard_check(smp_cpu_id());
      extern volatile int kheap_diag_op[8];
        extern volatile int kheap_diag_state[8];
        extern volatile int kheap_diag_pid[8];
        extern volatile unsigned int kheap_diag_size[8];
        extern volatile unsigned int sbrk_diag_calls[8];
        extern volatile long sbrk_diag_last_amt[8];
        extern volatile long sbrk_diag_last_ret[8];
        extern volatile unsigned long sbrk_diag_knext[8];
        extern volatile unsigned long dlm_diag_loops[8];
        extern volatile unsigned long dlm_diag_p[8];
        extern volatile unsigned long dlm_diag_next[8];
        extern volatile unsigned long dlm_diag_size[8];
        extern volatile unsigned long dlm_diag_fb[8];
        extern volatile unsigned long dlm_diag_top[8];
        extern volatile unsigned long dlm_diag_nextchunk[8];
       extern volatile unsigned long dlm_diag_nextsize[8];
        extern volatile unsigned long dlm_diag_prev[8];
        extern volatile unsigned int dlm_free_hit[8];
        extern volatile unsigned long dlm_free_p[8];
        extern volatile unsigned long dlm_free_old[8];
        extern volatile unsigned long dlm_free_size[8];
        extern volatile unsigned long dlm_free_fb[8];
        extern volatile unsigned long dlm_free_steps[8];
        extern volatile unsigned long dlm_free_rip[8][4];
        extern volatile unsigned long kheap_free_rip[8];
        extern volatile unsigned long kheap_malloc_rip[8];
        extern volatile unsigned long kheap_realloc_rip[8];
        extern volatile unsigned long kheap_ev_ptr[128];
        extern volatile unsigned long kheap_ev_rip[128];
        extern volatile unsigned long kheap_ev_size[128];
        extern volatile int kheap_ev_type[128];
        extern volatile int kheap_ev_cpu[128];
        extern volatile int kheap_ev_pid[128];
        extern volatile unsigned long kheap_ev_seq;
        static PCB386 *wd_last[8];
       static unsigned long wd_ticks[8];
       unsigned i;
       int me = smp_cpu_id();

      if (!selfhost_cooperative_ready)
         return;
      if (me < 0 || me >= 8)
         return;

      if (current_process != wd_last[me])
         {
          wd_last[me] = current_process;
          wd_ticks[me] = 0;
         };
      wd_ticks[me]++;
      /* Cooperative cert expects long no-yield user compiles.  The old
         watchdog printed multi-page RAW/KHEAP dumps from the timer IRQ and
         itself faulted (PF64 while dumping gcc's user stack).  One quiet
         serial line every ~20s is enough to see progress. */
      if (wd_ticks[me] < 2000 || (wd_ticks[me] % 2000) != 0)
         return;
      if (current_process) {
         extern volatile unsigned long sync_wait_var[8];
         extern volatile int sync_wait_owner[8];
         extern volatile unsigned long sync_wait_spins[8];
         char wline[256];
         sprintf(wline,
                 "WATCHDOG cpu=%d pid=%d '%s' no-yield=%lu sc=%lu last=%02x/%02x rip=0x%lx "
                 "crit=0x%lx critowner=%d critspins=%lu held=%d critwait=%d\n",
                 me, current_process->processid, current_process->name,
                 wd_ticks[me], diag_sc_count,
                 (unsigned)current_process->cursyscall[0],
                 (unsigned)current_process->cursyscall[1],
                 wd_frame_cpu[me][15],
                 sync_wait_var[me], sync_wait_owner[me], sync_wait_spins[me],
                 current_process->held_crit_n, current_process->crit_wait);
         serial_puts(wline);
      }
      return;

#if 0
      if (wd_ticks[me] < 500)
         return;
      if ((wd_ticks[me] - 500) % 2000 != 0)
         return;

      static int wd_raw_done[8];


      if (current_process)
            printf("WATCHDOG cpu=%d pid=%d '%s' no-yield=%lu sc_total=%lu last_sc=%02x/%02x krsp=0x%lx rip=0x%lx khop=%d khst=%d khpid=%d khsize=%u\n",
                   me, current_process->processid, current_process->name,
                   wd_ticks[me],
                   diag_sc_count,
                   (unsigned)current_process->cursyscall[0],
                   (unsigned)current_process->cursyscall[1],
                   wd_krsp_cpu[me], wd_frame_cpu[me][15],
                   kheap_diag_op[me], kheap_diag_state[me],
                   kheap_diag_pid[me], kheap_diag_size[me]);
        else
           printf("WATCHDOG cpu=%d idle no-yield=%lu sc_total=%lu krsp=0x%lx rip=0x%lx\n",
                  me, wd_ticks[me], diag_sc_count, wd_krsp_cpu[me], wd_frame_cpu[me][15]);

       /* One-shot raw dump of the full frame window + kernel C stack so the live
           RIP and the exact stack layout can be read without range filtering. */
        if (!wd_raw_done[me] && current_process)
           {
            wd_raw_done[me] = 1;
           for (i = 0; i < 31; i++)
              printf("  RAW f[%lu]=0x%lx\n", (unsigned long)(i*8), wd_frame_cpu[me][i]);
           if (wd_krsp_cpu[me] >= 0x100000 && wd_krsp_cpu[me] < 0x1000000)
              for (i = 0; i < 32; i++)
                 printf("  RAW k[%lu]=0x%lx\n", (unsigned long)(24 + i*8),
                        *(volatile unsigned long *)(wd_krsp_cpu[me] + 24 + i*8));
          }

       /* One-shot per-process PCB/context dump: does the SAVED ctx.rip match the
          wild live RIP, and what does the legacy regs.EIP hold? */
      static PCB386 *wd_dumped[8];
        if (current_process && current_process != wd_dumped[me])
          {
           PCB386 *p = current_process;
           wd_dumped[me] = p;
          printf("PCBDUMP pid=%d '%s':\n", p->processid, p->name);
          printf("  ctx.rip=0x%lx ctx.rsp=0x%lx ctx.rflags=0x%lx ctx.cs=0x%lx ctx.ss=0x%lx ctx.cr3=0x%lx\n",
                 p->ctx.rip, p->ctx.rsp, p->ctx.rflags, p->ctx.cs, p->ctx.ss, p->ctx.cr3);
          printf("  regs.EIP=0x%lx regs.ESP=0x%lx regs.CS=0x%lx status=0x%lx pagedir=0x%lx\n",
                 (unsigned long)p->regs.EIP, (unsigned long)p->regs.ESP,
                 (unsigned long)p->regs.CS, p->status, (unsigned long)p->pagedirloc);
          if (p->ctx.rsp >= 0x100000 && p->ctx.rsp < 0x1000000)
             printf("  [rsp+120]=0x%lx [rsp+112]=0x%lx [rsp+136]=0x%lx\n",
                    *(volatile unsigned long *)(p->ctx.rsp + 120),
                    *(volatile unsigned long *)(p->ctx.rsp + 112),
                    *(volatile unsigned long *)(p->ctx.rsp + 136));
         }

       if (me == 0 && current_process && wd_ticks[me] >= 500
           && (wd_ticks[me] - 500) % 2000 == 0)
         {
          printf("WDCPU START\n");
          for (i = 0; i < (unsigned)cpu_count; i++)
            {
             PCB386 *c = cpus[i].current;
             if (c)
                 printf("WDCPU cpu=%u pcb=0x%lx pid=%d oncpu=%d '%s' rip=0x%lx rsp=0x%lx status=0x%lx\n",
                        (unsigned)i, (unsigned long)c, c->processid, (int)c->on_cpu, c->name,
                        (unsigned long)c->ctx.rip, (unsigned long)c->ctx.rsp,
                        (unsigned long)c->status);
             else
                 printf("WDCPU cpu=%u idle\n", (unsigned)i);
             }
          for (i = 0; i < (unsigned)cpu_count && i < 8; i++)
              {
              printf("KHEAPDIAG cpu=%u op=%d state=%d pid=%d size=%u\n",
                      (unsigned)i, kheap_diag_op[i], kheap_diag_state[i],
                      kheap_diag_pid[i], kheap_diag_size[i]);
               printf("KHEAPRIP cpu=%u free=0x%lx malloc=0x%lx realloc=0x%lx\n",
                      (unsigned)i, kheap_free_rip[i], kheap_malloc_rip[i],
                      kheap_realloc_rip[i]);
               printf("SBRKDIAG cpu=%u calls=%u amt=%ld ret=0x%lx knext=0x%lx\n",
                     (unsigned)i, sbrk_diag_calls[i], sbrk_diag_last_amt[i],
                     (unsigned long)sbrk_diag_last_ret[i], sbrk_diag_knext[i]);
              printf("DLMCONS cpu=%u loops=%lu p=0x%lx next=0x%lx size=0x%lx fb=0x%lx\n",
                     (unsigned)i, dlm_diag_loops[i], dlm_diag_p[i],
                     dlm_diag_next[i], dlm_diag_size[i], dlm_diag_fb[i]);
              printf("DLMCONS2 cpu=%u top=0x%lx nextchunk=0x%lx nextsize=0x%lx prev=%lu\n",
                      (unsigned)i, dlm_diag_top[i], dlm_diag_nextchunk[i],
                      dlm_diag_nextsize[i], dlm_diag_prev[i]);
               if (dlm_free_hit[i])
                 {
                  printf("DLMFREE cpu=%u hit=%u p=0x%lx old=0x%lx size=0x%lx fb=0x%lx steps=%lu\n",
                         (unsigned)i, dlm_free_hit[i], dlm_free_p[i],
                         dlm_free_old[i], dlm_free_size[i], dlm_free_fb[i],
                         dlm_free_steps[i]);
                  printf("DLMFRIP cpu=%u rip0=0x%lx rip1=0x%lx rip2=0x%lx rip3=0x%lx\n",
                         (unsigned)i, dlm_free_rip[i][0], dlm_free_rip[i][1],
                         dlm_free_rip[i][2], dlm_free_rip[i][3]);
             }
                }
             {
               unsigned long n;
               int anyhit = 0;
               for (i = 0; i < (unsigned)cpu_count && i < 8; i++)
                  if (dlm_free_hit[i])
                     anyhit = 1;
               printf("KHEAPEV PING seq=%lu anyhit=%d\n", (unsigned long)kheap_ev_seq, anyhit);
               if (anyhit)
                 {
               for (i = 0; i < 128; i++)
                 {
                  int j;
                  int match = 0;
                  for (j = 0; j < (int)cpu_count && j < 8; j++)
                     if (dlm_free_hit[j] &&
                         (kheap_ev_ptr[i] == dlm_free_p[j] ||
                          kheap_ev_ptr[i] == dlm_free_p[j] + 16UL))
                        match = 1;
                  if (!match)
                     continue;
                  printf("KHEAPEV MATCH i=%u t=%d cpu=%d pid=%d ptr=0x%lx size=0x%lx rip=0x%lx\n",
                         (unsigned)i, kheap_ev_type[i], kheap_ev_cpu[i],
                         kheap_ev_pid[i], kheap_ev_ptr[i], kheap_ev_size[i],
                         kheap_ev_rip[i]);
                 }
               for (n = 0; n < 8; n++)
                 {
                  unsigned int idx = (unsigned int)((kheap_ev_seq - n) & 127UL);
                  if (kheap_ev_ptr[idx])
                     printf("KHEAPEV LAST n=%lu i=%u t=%d cpu=%d pid=%d ptr=0x%lx size=0x%lx rip=0x%lx\n",
                            (unsigned long)n, (unsigned)idx, kheap_ev_type[idx],
                            kheap_ev_cpu[idx], kheap_ev_pid[idx],
                            kheap_ev_ptr[idx], kheap_ev_size[idx],
                            kheap_ev_rip[idx]);
                }
                 }
               }
               printf("WDCPU END\n");
           }

       for (i = 0; i < 31; i++)
           if (wd_kind(wd_frame_cpu[me][i]))
              printf("  FR f[%lu]=0x%lx %s\n", (unsigned long)(i*8),
                     wd_frame_cpu[me][i], wd_kind(wd_frame_cpu[me][i])==1 ? "K" : "U");
       if (wd_krsp_cpu[me] >= 0x100000 && wd_krsp_cpu[me] < 0x1000000)
          {
          for (i = 0; i < 32; i++)
             {
              unsigned long v = *(volatile unsigned long *)(wd_krsp_cpu[me] + 24 + i*8);
              if (wd_kind(v))
                 printf("  KS k[%lu]=0x%lx %s\n", (unsigned long)(24 + i*8),
                        v, wd_kind(v)==1 ? "K" : "U");
             }
         }
#endif
    };

 //the timer handler used by the task switcher
 void time_handler()
    {
     {
        extern volatile unsigned int *lapic_mmio;
        extern volatile unsigned long long lapic_expected_base;
        extern int cpu_count;
        static volatile int bsscan_done = 0;
        unsigned long lm = (unsigned long)(void *)lapic_mmio;
        if (!bsscan_done &&
            ((lapic_expected_base && lm != (unsigned long)lapic_expected_base) ||
             (cpu_count < 1 || cpu_count > 8))) {
           bsscan_done = 1;
           printf("BSSCANARY lapic_mmio=0x%lx expected=0x%lx cpu_count=%d\n",
                  lm, (unsigned long)lapic_expected_base, cpu_count);
        }
     }
     //update the real-time clock
   //DEX32 is programmed to switch process every
   // 1/200 of a second so we use a counter that counts
   //up to 200 and then increments time_count which
   //holds the time elasped in seconds since the
   //system has started
   
   aux_time2++;
   
   
   if (aux_time2>=context_switch_rate)
         {
             aux_time2=0;
             time_count++;

             //synchronize with the clock every 10 minutes
             if (time_systime.min%10==0)
                 getdatetime(&time_systime);
         };

   ticks++;
    time_incrementtime();
    selfhost_spin_watchdog();
    
    /* Floppy motor timeout and PIC EOI are BSP-only. APs use the LAPIC. */
   {
      extern int smp_cpu_id(void);
      if (smp_cpu_id() == 0) {
         fdctimer();
         outportb(0x20,0x20);
      }
   }
   {
      extern void lapic_eoi(void);
      extern volatile unsigned int *lapic_mmio;
      if (lapic_mmio)
         lapic_eoi();
   }
;};

void cpu_idle(void)
 {
   /* Interrupts must be on so the PIT/keyboard can wake us. */
   asm volatile ("sti; hlt");
 };

//delays the execution of a program for a specified number of milliseconds
void delay(DWORD w)
 {
   DWORD t1, cpuflags;
   storeflags(&cpuflags);
   stopints();
   
   t1 = ticks+w*2;
   restoreflags(cpuflags);
      
   while (ticks<t1)
      cpu_idle();
 };

//sets the rate of context switch
//It is best to set the value between 100-300 to prevent
//erratic behavior

void dex32_set_timer(DWORD rate)
{
    WORD time_val;
    BYTE time_val_high,time_val_low;
    DWORD flags;
    storeflags(&flags);
    stopints(); //stop interrupts

    time_val= 1193180 / rate;
    time_val_low = time_val & 0xFF;
    time_val_high = time_val >> 8;

    outportb(0x43,0x36); //tell which timer to reprogram
    outportb(0x40,time_val_low);
    outportb(0x40,time_val_high);
    restoreflags(flags);
};


void time_init()
{
    //update system time
    getdatetime(&time_systime);
};
