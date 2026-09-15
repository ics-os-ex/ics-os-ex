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
 * PROCESS:  first entry from the user stack of a claimed user, or a
 *           leftover-executing user (on_cpu < 0, still on that stack).
 *           Switch to kstack_top.  Fork children start unclaimed.
 * CPU:      FOREIGN user (another CPU holds on_cpu) still on the user
 *           stack, or leftover current on the idle BSS stack.
 */
#define IRQ_KSTACK_STAY     0
#define IRQ_KSTACK_PROCESS  1
#define IRQ_KSTACK_CPU      2

/* Already on this PCB's kstack: FOREIGN must leave it.  Cert 248116
   stayed, two CPUs tore cc1 frames (GPF64 in sync_owner_pcb).
   Unclaimed (on_cpu < 0) is the runner (release-before-switch / fork). */
static inline int irq_kstack_foreign_must_leave(int on_cpu, int me)
{
   return on_cpu >= 0 && on_cpu != me;
}

static inline int irq_kstack_dest(unsigned long rsp,
                                  unsigned long kbase,
                                  unsigned long ktop,
                                  int on_cpu, int me)
{
   /* Yanking FOREIGN off a valid kstack failed test-fork (GPF64 TSS-bad
      fork_child_return).  A torn top is not a range bound: leftover
      current stayed on make's kstack (cert 248123 KSTACK-FOREIGN
      top=(0x400000<<32)|kheap). */
   if (ktop >> 32)
      return IRQ_KSTACK_CPU;
   if (kbase && ktop && rsp >= kbase && rsp < ktop)
      return IRQ_KSTACK_STAY;
   if (rsp >= MEM_USER_STACK_GUARD && rsp < MEM_USER_STACK)
      return irq_kstack_foreign_must_leave(on_cpu, me)
             ? IRQ_KSTACK_CPU : IRQ_KSTACK_PROCESS;
   /* Leftover current often interrupts the idle BSS stack.  Staying there
      overflows 8KiB; resetting process kstack_top from there smashes a
      yielded syscall (cert PF64 / task_mgr halt).  Park those on MEM_CPUIRQ.
      A kheap RSP is publish-before-switch or another process kstack: stay. */
   if (rsp < MEM_KERNEL_LIMIT ||
       (rsp >= MEM_IDLE_STACK_BASE && rsp < MEM_IDLE_STACK_END))
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
 *   - unclaimed USER (on_cpu < 0) whose CR3 has already left,
 *   - unclaimed ACCESS_SYS on idle BSS (leftover task_mgr: PF64 torn
 *     RBP, UD64 into cpus[], held_crit_n=kheap).
 * Do not repair:
 *   - mid ps_switchto / context_load (publish-before-stack-switch),
 *   - a live claim (on_cpu == me): dest is published before CR3/RSP
 *     switch; stealing it GPF'd in ps_switchto (cert rip=0x13c0bc),
 *   - released user still on its CR3 (release-before-switch),
 *   - unclaimed ACCESS_SYS on a kheap stack (release-before-switch;
 *     cert 248114 idle token on vfs_busy).
 * A false claim (on_cpu == me while executing idle) is dropped only
 * by smp_cpu_idle, not from IRQ C.
 */
static inline int leftover_current_should_repair(int on_cpu, int me,
                                                 int access_user,
                                                 unsigned long pcb_cr3,
                                                 unsigned long hw_cr3,
                                                 int mid_switch,
                                                 unsigned long rsp)
{
   unsigned long pcb = pcb_cr3 & ~0xFFFUL;
   unsigned long hw = hw_cr3 & ~0xFFFUL;

   if (mid_switch)
      return 0;
   if (on_cpu == me)
      return 0;
   /* Kernel leftover: CR3 is shared with idle.
      FOREIGN (on_cpu is another CPU) always drops.
      Unclaimed on a kheap/kstack RSP is release-before-switch
      (cert 248114: idle token on vfs_busy, UD64 disk_mgr).
      Unclaimed on idle BSS is a stale advertisement: timer C ran as
      leftover task_mgr, overflowed 16KiB BSS, tore 64-bit slots
      (PF64 cr2=0x10253e5d0, UD64 into cpus[], held_crit_n=kheap). */
   if (!access_user) {
      if (on_cpu >= 0 && on_cpu != me)
         return 1;
      if (on_cpu < 0 &&
          (rsp < MEM_KERNEL_LIMIT ||
           (rsp >= MEM_IDLE_STACK_BASE && rsp < MEM_IDLE_STACK_END)))
         return 1;
      return 0;
   }
   /* FOREIGN leftover: drop the advertisement only after CR3 has left.
      Setting current=idle while still on that user CR3/stack left the
      next IRQ on the user stack (ACCESS_SYS stay) and GPF'd in idle
      (cert rip=0x12b943 frsp=0x3fffac10 proc=cpu_idle). */
   if (pcb && pcb == hw)
      return 0;
   /* HW CR3 is some other user AS (cert: leftover cc1, CR3=make).
      Setting current=idle smashed make's kstack (GPF64 rip=ps_switchto). */
   if (hw >= MEM_KERNEL_LIMIT)
      return 0;
   /* Unclaimed USER on kernel CR3: a kheap/kstack RSP is still in that
      syscall (cert CRIT-NONOWNER idle token on vfs_busy).  The user
      stack is also above 4MiB, but leftover make on 0x3fffdac0 with
      pagedir1 is not a live syscall (cert 248128). */
   if (on_cpu < 0 && rsp >= MEM_KERNEL_LIMIT) {
      if (rsp >= MEM_USER_STACK_GUARD && rsp < MEM_USER_STACK &&
          hw < MEM_KERNEL_LIMIT)
         return 1;
      return 0;
   }
   return 1;
}

/* USER parked on MEM_CPUIRQ (irqwrap .Lbss idle leftover, or FOREIGN).
   Do not require on_cpu < 0: a false on_cpu==me while RSP is the CPU
   stack still scheduled (cert 248127 IRETQ-BADRIP rip=0x18 as make
   on 0x2060100, idle-stack top / CPUIRQ). */
static inline int leftover_user_on_cpuirq(int on_cpu, int access_user,
                                          unsigned long rsp)
{
   (void)on_cpu;
   if (!access_user)
      return 0;
   if (rsp >= MEM_CPUIRQ_BASE && rsp < MEM_CPUIRQ_END)
      return 1;
   return rsp >= MEM_IDLE_STACK_BASE && rsp < MEM_IDLE_STACK_END;
}

/* ACCESS_SYS (idle) diverted from a user stack onto MEM_CPUIRQ.
   schedule_from_timer must not save that continuation on the CPU stack
   (GPF64 TSS-bad fork_child_return, test-fork 77-storm). */
static inline int leftover_sys_on_cpuirq(int access_user, unsigned long rsp)
{
   if (access_user)
      return 0;
   return rsp >= MEM_CPUIRQ_BASE && rsp < MEM_CPUIRQ_END;
}

/* ACCESS_SYS still on a reserved idle stack in IRQ C.  Do not dest
   that to CPUIRQ (leftover user-stack dests already live there;
   a second dest is the fork 77-storm).  Enlarge the idle slot. */
static inline int leftover_sys_on_idle_stack(int access_user, unsigned long rsp)
{
   if (access_user)
      return 0;
   return rsp >= MEM_IDLE_STACK_BASE && rsp < MEM_IDLE_STACK_END;
}

/* Leftover idle advertised while RSP is still the user stack. */
static inline int leftover_sys_on_userstack(int access_user, unsigned long rsp)
{
   if (access_user)
      return 0;
   return rsp >= MEM_USER_STACK_GUARD && rsp < MEM_USER_STACK;
}

/* USER still on the user stack in IRQ C.  IRQ_KSTACK_ENTER should have
   moved a claimed/unclaimed runner onto the process kstack.  Remaining
   here is leftover (cert 248128 make iretq on 0x3fffdac0). */
static inline int leftover_user_on_userstack(int access_user, unsigned long rsp)
{
   if (!access_user)
      return 0;
   return rsp >= MEM_USER_STACK_GUARD && rsp < MEM_USER_STACK;
}

/* Advertised USER while HW CR3 is still the kernel identity (pagedir1).
   Cert 248128: leftover make syscallwrapper iretq with cr3=0x191000. */
static inline int leftover_user_on_kernel_cr3(int access_user,
                                              unsigned long pcb_cr3,
                                              unsigned long hw_cr3)
{
   pcb_cr3 &= ~0xFFFUL;
   hw_cr3 &= ~0xFFFUL;
   if (!access_user || !pcb_cr3 || pcb_cr3 == hw_cr3)
      return 0;
   return hw_cr3 < MEM_KERNEL_LIMIT;
}

/* User-stack iretq frame is only valid on a user AS.  pagedir1 does not
   map 0x3FD00000–0x40000000 (cert 248128 GPF64 on syscallwrapper iretq).
   Do not dereference the frame before this check. */
static inline int irq_iretq_frame_cr3_ok(unsigned long frame,
                                         unsigned long cr3)
{
   cr3 &= ~0xFFFUL;
   if (frame < MEM_USER_STACK_GUARD || frame >= MEM_USER_STACK)
      return 1;
   return cr3 >= MEM_KERNEL_LIMIT;
}

/* Crit owner token is (pid & 0x7fffff)+1.  Leftover current must not
   mint a token for a different MM PCB (cert 248122 vfs_busy 0x1f vs 0x2a). */
/* Timer iretq after schedule_from_timer.  Kernel .text ends ~0x19562b
   after virtio-net + IPv4 stack; keep a margin under the 4MiB user ELF
   window.  0x300000..4GiB used to accept BSS and kheap (cert 248124
   IRETQ-BADRIP then halt as leftover idle with no RIP printed). */
#define IRQ_IRETQ_KTEXT_END  0x1C0000UL

/* x86-64 canonical: bits 48..63 are sign-extension of bit 47. */
static inline int addr_canonical(unsigned long a)
{
   long s = (long)a;
   return ((s << 16) >> 16) == s;
}

/* PF walker may only follow the 4GiB identity or KDIRECT.
   Cert 248126: cr2=rip=0x100000001 is canonical but unmapped; getphys64
   then GPF64'd in pagefaulthandler as task_mgr. */
static inline int pf64_walkable(unsigned long a)
{
   if (!addr_canonical(a))
      return 0;
   if (a < 0x100000000UL)
      return 1;
   if (a >= (unsigned long)KDIRECT_BASE &&
       a < (unsigned long)KDIRECT_BASE + 0x100000000UL)
      return 1;
   return 0;
}

static inline int irq_iretq_rip_ok(unsigned long rip)
{
   long s = (long)rip;
   if (((s << 16) >> 16) != s)
      return 0;
   if (rip >= MEM_KERNEL_LOAD && rip < IRQ_IRETQ_KTEXT_END)
      return 1;
   if (rip >= MEM_USER_ELF_BASE && rip < MEM_USER_ELF_END)
      return 1;
   return 0;
}

/* Leftover USER advertised while the iretq frame is on MEM_CPUIRQ /
   idle (cert 248135 IRETQ-BADRIP rip=0 cs=0 rsp=0x2000130 killed
   make).  That is a kernel leftover frame, not a user opcode. */
static inline int leftover_iretq_must_not_kill_user(unsigned long frame,
                                                    unsigned long rip)
{
   if (irq_iretq_rip_ok(rip))
      return 0;
   if (frame >= MEM_CPUIRQ_BASE && frame < MEM_CPUIRQ_END)
      return 1;
   if (frame >= MEM_IDLE_STACK_BASE && frame < MEM_IDLE_STACK_END)
      return 1;
   return 0;
}

static inline int crit_token_for_pids(unsigned leftover_pid, unsigned mm_pid)
{
   unsigned use = mm_pid ? mm_pid : leftover_pid;
   /* Leftover USER on pagedir1: MM retarget is kernel pid 0.  Keep the
      USER token (cert 248132 self=0x1 on vfs_busy). */
   if (leftover_pid && !mm_pid)
      use = leftover_pid;
   return (int)((use & 0x007FFFFF) + 1);
}

/* current=idle while HW CR3 is a user AS: leftover advertisement.
   sbrk/file_ok then used idle knext/token (cert 248119 sbrk FAIL
   cpu_idle, CRIT-NONOWNER self=0x7f0003). */
static inline int idle_on_user_cr3(int access_user, unsigned long hw_cr3)
{
   if (access_user)
      return 0;
   return (hw_cr3 & ~0xFFFUL) >= MEM_KERNEL_LIMIT;
}

/* Leftover advertised idle still on a user AS must skip (test-fork
   77-storm).  Dest'd reserved idle on CPUIRQ with kernel CR3 must
   load_only (cert 248137). */
static inline int leftover_idle_timer_must_skip(int is_idle_pcb,
                                                unsigned long hw_cr3,
                                                unsigned long rsp)
{
   if (!is_idle_pcb)
      return 0;
   return idle_on_user_cr3(0, hw_cr3) || leftover_sys_on_userstack(0, rsp);
}

static inline int leftover_idle_timer_load_only(int is_idle_pcb,
                                                unsigned long hw_cr3,
                                                unsigned long rsp)
{
   if (!is_idle_pcb)
      return 0;
   if (leftover_idle_timer_must_skip(is_idle_pcb, hw_cr3, rsp))
      return 0;
   return leftover_sys_on_cpuirq(0, rsp) || leftover_sys_on_idle_stack(0, rsp);
}

/* Leftover claimed idle still on its reserved slot while HW CR3 is a
   user AS.  leftover-skip hung cert 248140 (IRET leftover idle, leftover
   cc1 never scheduled).  leftover schedule smashed make (cert 248141
   GPF64 scheduler frsp=0x230, WATCHDOG gcc at smp_cpu_idle).
   leftover_load_only picks leftover user without saving leftover idle.
   Dest'd leftover ACCESS_SYS on CPUIRQ still leftover-skips. */
static inline int leftover_idle_claimed_user_cr3_load_only(int is_idle_pcb,
                                                           int on_cpu, int me,
                                                           unsigned long hw_cr3,
                                                           unsigned long rsp)
{
   if (!is_idle_pcb)
      return 0;
   if (on_cpu != me)
      return 0;
   if (!idle_on_user_cr3(0, hw_cr3))
      return 0;
   return leftover_sys_on_idle_stack(0, rsp);
}

/* Leftover USER A advertised while HW CR3 is USER B (cert 248125
   PF64-STALE-CURRENT cur=42 cr3pid=25).  current_mm_process must
   retarget; do not change the advertisement (idle-repair smashed). */
static inline int leftover_user_on_other_cr3(int access_user,
                                             unsigned long pcb_cr3,
                                             unsigned long hw_cr3)
{
   pcb_cr3 &= ~0xFFFUL;
   hw_cr3 &= ~0xFFFUL;
   if (!access_user)
      return idle_on_user_cr3(0, hw_cr3);
   if (!pcb_cr3 || pcb_cr3 == hw_cr3)
      return 0;
   /* USER A on USER B only.  Leftover USER on pagedir1 must keep the
      advertised USER for crit tokens (cert 248132 minted 0x1 / idle
      and CRIT-NONOWNER on vfs_busy). */
   return hw_cr3 >= MEM_KERNEL_LIMIT;
}

/* Timer leftover: FOREIGN current, leftover idle on a user/CPUIRQ
   stack, or leftover USER on a bad stack/CR3 who is crit_wait.
   Abandon the advertisement and load-only a local task — do not
   context_switch from the leftover RSP (cert 248136 leftover make
   spun on io_devlock while disk_mgr stayed claimed on CPU 0). */
static inline int leftover_timer_stack_leftover(int on_cpu, int me,
                                                int access_user,
                                                unsigned long rsp,
                                                unsigned long pcb_cr3,
                                                unsigned long hw_cr3)
{
   /* ACCESS_SYS dest'd onto CPUIRQ leftover-skips even when idle is
      claimed (on_cpu==me).  Scheduling that continuation smashed
      context_switch (cert 248138).  Leftover ACCESS_SYS still on a
      reserved idle stack leftover-skips only when leftover; claimed
      idle on its 64KiB slot must still schedule (cert 248137). */
   return leftover_user_on_cpuirq(on_cpu, access_user, rsp)
       || leftover_user_on_userstack(access_user, rsp)
       || leftover_sys_on_cpuirq(access_user, rsp)
       || leftover_sys_on_userstack(access_user, rsp)
       || (on_cpu != me && leftover_sys_on_idle_stack(access_user, rsp))
       || leftover_user_on_kernel_cr3(access_user, pcb_cr3, hw_cr3);
}

static inline int leftover_timer_must_abandon(int on_cpu, int me,
                                              int access_user,
                                              unsigned long rsp,
                                              unsigned long pcb_cr3,
                                              unsigned long hw_cr3,
                                              int crit_wait)
{
   /* FOREIGN leftover without crit_wait is the fork 77-storm.
      Leftover idle on a user/CPUIRQ stack must stay skip, not abandon.
      Only abandon a leftover waiter (cert 248136). */
   if (on_cpu >= 0 && on_cpu != me)
      return crit_wait;
   if (!leftover_timer_stack_leftover(on_cpu, me, access_user, rsp,
                                      pcb_cr3, hw_cr3))
      return 0;
   return crit_wait;
}

static inline int leftover_timer_must_skip(int on_cpu, int me,
                                           int access_user,
                                           unsigned long rsp,
                                           unsigned long pcb_cr3,
                                           unsigned long hw_cr3,
                                           int crit_wait)
{
   if (leftover_timer_must_abandon(on_cpu, me, access_user, rsp,
                                   pcb_cr3, hw_cr3, crit_wait))
      return 0;
   if (on_cpu >= 0 && on_cpu != me)
      return 1;
   return leftover_timer_stack_leftover(on_cpu, me, access_user, rsp,
                                        pcb_cr3, hw_cr3);
}

/* Claimed leftover USER A on USER B (cert 248131 gcc on cc1): CPUIRQ.
   Unclaimed leftover parent on a child CR3 stays PROCESS (test-fork
   77-storm).  mid_switch is applied by irqwrap before this dest. */
static inline int irq_kstack_dest_mm(unsigned long rsp,
                                     unsigned long kbase,
                                     unsigned long ktop,
                                     int on_cpu, int me,
                                     unsigned long pcb_cr3,
                                     unsigned long hw_cr3)
{
   if (rsp >= MEM_USER_STACK_GUARD && rsp < MEM_USER_STACK &&
       on_cpu == me &&
       (leftover_user_on_other_cr3(1, pcb_cr3, hw_cr3) ||
        leftover_user_on_kernel_cr3(1, pcb_cr3, hw_cr3)))
      return IRQ_KSTACK_CPU;
   return irq_kstack_dest(rsp, kbase, ktop, on_cpu, me);
}

/* Leftover idle or leftover USER on pagedir1 must not mint/leave
   vfs_busy with token 0x1 (cert 248132 CRIT-NONOWNER busy=0x12
   self=0x1).  Legitimate idle on its reserved stack may still take
   early-boot crits. */
/* Ready-ring next/before must be a kernel/kheap PCB, not a torn
   0x100000001 slot (cert 248133 GPF64 in scheduler at
   lastprocess->next->before). */
static inline int sched_link_ok(unsigned long a)
{
   if (!addr_canonical(a))
      return 0;
   if (a >= MEM_KERNEL_LOAD && a < MEM_KERNEL_LIMIT)
      return 1;
   if (a >= MEM_KHEAP_BASE && a < MEM_KHEAP_END)
      return 1;
   return 0;
}

static inline int leftover_vfs_must_skip(int access_user,
                                         unsigned long pcb_cr3,
                                         unsigned long hw_cr3,
                                         unsigned long rsp,
                                         int is_idle_pcb)
{
   if (is_idle_pcb) {
      if (idle_on_user_cr3(0, hw_cr3))
         return 1;
      if (leftover_sys_on_userstack(0, rsp))
         return 1;
      if (leftover_sys_on_cpuirq(0, rsp))
         return 1;
      if (rsp >= MEM_KHEAP_BASE && rsp < MEM_KHEAP_END)
         return 1;
      return 0;
   }
   return leftover_user_on_kernel_cr3(access_user, pcb_cr3, hw_cr3);
}

/* Leave must use the nest token stored on the lock-owner PCB, not
   leftover current's token.  Cert ramdisk: busy=0x12 (disk_mgr)
   self=0x1 (kernel) leaked kheap_crit because non-owner leave
   returns without unlocking. */
static inline int leftover_leave_token(int busy, int sampled,
                                       int owner_nest_tok,
                                       int owner_nest_var_is_this)
{
   if (busy > 0 && owner_nest_var_is_this && owner_nest_tok)
      return owner_nest_tok;
   return sampled;
}

/* current_process must be a canonical kernel/heap PCB.  Cert GPF64 in
   crit_nest_push (rip=0x1357ba mov 0xdf0(%rax)) was a non-canonical
   cpus[].current from leftover smash of the 64-bit slot. */
static inline int pcb_ptr_ok(unsigned long a)
{
   long s = (long)a;
   if (((s << 16) >> 16) != s)
      return 0;
   if (a >= MEM_KERNEL_LOAD && a < MEM_KERNEL_LIMIT)
      return 1;
   if (a >= MEM_KHEAP_BASE && a < MEM_KHEAP_END)
      return 1;
   return 0;
}

/* Two CPUs writing 32-bit values into one 64-bit slot.  Live cert:
   RSI=(kheap<<32)|0xe, R14/RBP=(cpu_id<<32)|kheap. */
static inline int u64_half_kernel(unsigned x)
{
   unsigned long a = (unsigned long)x;
   if (a >= MEM_KERNEL_LOAD && a < MEM_KERNEL_LIMIT)
      return 1;
   if (a >= MEM_KHEAP_BASE && a < MEM_KHEAP_END)
      return 1;
   return 0;
}

static inline int u64_slot_torn(unsigned long v)
{
   unsigned lo = (unsigned)v;
   unsigned hi = (unsigned)(v >> 32);
   if (hi == 0)
      return 0;
   if (hi <= 16 && u64_half_kernel(lo))
      return 1;
   if (lo <= 0xff && u64_half_kernel(hi))
      return 1;
   return 0;
}

/* held_crit_n is a 32-bit nest count.  Cert WATCHDOG held=0x35BC8E0
   was a kheap pointer stored into the slot. */
static inline int held_crit_n_ok(int n)
{
   return n >= 0 && n <= 16;
}

/* RIP in kernel-reserved identity is a smashed kernel, not a user opcode.
   Cert 248121: UD64 rip=MEM_KHEAP_BASE+0x41 (cs=0x8) was classified as
   userfault because rip >= MEM_USER_ELF_BASE, killed make.exe, then
   GPF64 in context_switch on make's kstack. */
static inline int exc_kernel_reserved_rip(unsigned long rip)
{
   if (rip >= MEM_KERNEL_LOAD && rip < MEM_KERNEL_LIMIT)
      return 1;
   if (rip >= MEM_KEXEC_STAGE && rip < MEM_KEXEC_STAGE_END)
      return 1;
   if (rip >= MEM_CPUIRQ_BASE && rip < MEM_CPUIRQ_END)
      return 1;
   if (rip >= MEM_IDLE_STACK_BASE && rip < MEM_IDLE_STACK_END)
      return 1;
   if (rip >= MEM_KHEAP_BASE && rip < MEM_KHEAP_END)
      return 1;
   return 0;
}

/* A fetchable user opcode, not leftover smash.  Cert 248129 recovered
   cc1 at torn rip=0x10390d900 ((1<<32)|kheap) and then killed gcc at
   rip=0x3fffe4c0 (user stack) because anything >= 4MiB looked like
   user text. */
static inline int exc_user_opcode_rip(unsigned long rip)
{
   if (!addr_canonical(rip) || u64_slot_torn(rip) || !pf64_walkable(rip))
      return 0;
   if (exc_kernel_reserved_rip(rip))
      return 0;
   if (rip >= MEM_USER_STACK_GUARD && rip < MEM_USER_STACK)
      return 0;
   if (rip < 0x100000UL)
      return 1;
   return rip >= MEM_USER_ELF_BASE && rip < MEM_USER_ELF_END;
}

static inline int exc_user_fault_rip(int access_user, unsigned long rip)
{
   if (!access_user)
      return 0;
   return exc_user_opcode_rip(rip);
}

/* Leftover idle advertised while HW CR3 is a user AS and RIP is not
   kernel-reserved.  Do not halt leftover idle (cert 248139 UD64
   rip=0xac10000); retarget the CR3 owner.  irq_iretq must not reject
   ACCESS_SYS IRET to user ELF — that is the fork 77-storm. */
static inline int leftover_sys_ud_retarget(int is_idle_pcb,
                                           unsigned long hw_cr3,
                                           unsigned long rip)
{
   if (!is_idle_pcb)
      return 0;
   if (exc_kernel_reserved_rip(rip))
      return 0;
   return idle_on_user_cr3(0, hw_cr3);
}

#endif
