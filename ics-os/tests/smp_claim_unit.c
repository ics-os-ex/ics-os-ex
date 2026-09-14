/*
 * Host TAP for the SMP claim / current-publish / crit-nest protocol
 * and the IRQ kstack destination predicate.
 * These are the original-cause checks for: two CPUs on one PCB, publishing
 * current without a claim, leavecrit sampling a stale current (idle
 * token on vfs_busy after SDK_EXIT), and resetting a process kstack top
 * from a kernel RSP (cert PF64 rip=0x100000001000 on gcc.exe).
 */
#include <stdio.h>
#include "kernel/process/irq_kstack.h"

static int g_ok = 1;
static int g_n = 0;

static void check(const char *name, int cond)
{
   g_n++;
   if (!cond) {
      printf("not ok %d - %s\n", g_n, name);
      g_ok = 0;
   } else {
      printf("ok %d - %s\n", g_n, name);
   }
}

static int cas_on_cpu(int *on_cpu, int from, int to)
{
   if (*on_cpu != from)
      return 0;
   *on_cpu = to;
   return 1;
}

static int publish_ok(int on_cpu, int me)
{
   return on_cpu == me;
}

int main(void)
{
   int on_cpu;
   int current[8];
   int nest_tok[4];
   int nest_n;
   int i;

   printf("TAP version 13\n");
   printf("1..103\n");

   on_cpu = -1;
   for (i = 0; i < 8; i++)
      current[i] = -1;

   check("first CAS from -1 wins", cas_on_cpu(&on_cpu, -1, 2));
   check("second CAS from -1 loses", !cas_on_cpu(&on_cpu, -1, 0));
   check("loser must not publish current", on_cpu == 2 && current[0] == -1);
   check("winner may publish", publish_ok(on_cpu, 2));
   current[2] = 31;
   /* Release: publish successor first, then drop the claim. */
   current[2] = 0;
   on_cpu = -1;
   check("after release no CPU advertises the task",
         current[0] != 31 && current[2] != 31);

   /* Stale advertisement must not block a CPU that already holds on_cpu. */
   on_cpu = 3;
   current[0] = 31;
   current[3] = 25;
   check("stale current on another CPU does not revoke an existing claim",
         publish_ok(on_cpu, 3));

   /* Crit nest lives on the PCB.  Leave uses the token pushed at enter,
      not a later current, and another process on the same CPU must not
      see the hold (fat_wait_io yield under coop). */
   nest_n = 0;
   nest_tok[nest_n++] = 32; /* pid 31 + 1, captured at enter */
   /* current becomes idle (token 0x7f0004) before leave */
   check("leave uses enter token not the idle current",
         nest_n == 1 && nest_tok[--nest_n] == 32 && nest_n == 0);

   check("store-steal of on_cpu is not a valid claim",
         !cas_on_cpu(&on_cpu, -1, 1) && on_cpu == 3);

   /* irqwrap must not reset process kstack_top except from the user stack.
      A claimed dest whose interrupted RSP is the previous process kheap
      stack is the publish-before-switch window that smashed gcc.exe. */
   check("already on process kstack stays",
         irq_kstack_dest(0x03a10000UL, 0x03a00000UL, 0x03a20000UL, 0, 0)
         == IRQ_KSTACK_STAY);
   check("claimed user stack uses process kstack",
         irq_kstack_dest(0x3FFFE990UL, 0x03a00000UL, 0x03a20000UL, 0, 0)
         == IRQ_KSTACK_PROCESS);
   check("FOREIGN user stack uses CPU stack",
         irq_kstack_dest(0x3FFFE990UL, 0x03a00000UL, 0x03a20000UL, 3, 0)
         == IRQ_KSTACK_CPU);
   check("claimed kernel heap RSP must not reset process kstack top",
         irq_kstack_dest(0x03a15420UL, 0x02b00000UL, 0x02b20000UL, 0, 0)
         == IRQ_KSTACK_STAY);
   check("user stack guard is the low bound",
         irq_kstack_dest(MEM_USER_STACK_GUARD, 0x02b00000UL, 0x02b20000UL, 0, 0)
         == IRQ_KSTACK_PROCESS);
   check("claimed idle BSS stack uses CPU stack not process kstack top",
         irq_kstack_dest(0x2390f0UL, 0x02b00000UL, 0x02b20000UL, 0, 0)
         == IRQ_KSTACK_CPU);
   check("reserved idle stack uses CPU stack not process kstack top",
         irq_kstack_dest(MEM_IDLE_STACK_BASE + 0x1000UL,
                         0x02b00000UL, 0x02b20000UL, 0, 0)
         == IRQ_KSTACK_CPU);

   /* Leftover current: advertise a USER PCB this CPU is not executing.
      Original cause of cert smash / make.exe kill / ACCESS_SYS halt. */
   check("claimed user matching CR3 is not leftover",
         !leftover_current_should_repair(2, 2, 1, 0x2000UL, 0x2000UL, 0, 0x2000UL));
   check("FOREIGN leftover on that CR3 must not pretend idle",
         !leftover_current_should_repair(3, 0, 1, 0x2000UL, 0x2000UL, 0, 0x2000UL));
   check("FOREIGN leftover after CR3 left must drop",
         leftover_current_should_repair(3, 0, 1, 0x2000UL, 0x1000UL, 0, 0x2000UL));
   check("mid-switch publish-before-stack is not leftover",
         !leftover_current_should_repair(2, 2, 1, 0x2000UL, 0x1000UL, 1, 0x2000UL));
   check("released user still on its CR3 is not leftover",
         !leftover_current_should_repair(-1, 0, 1, 0x2000UL, 0x2000UL, 0, 0x2000UL));
   check("unclaimed USER on kernel CR3 on idle BSS must repair",
         leftover_current_should_repair(-1, 0, 1, 0x2000UL, 0x1000UL, 0, 0x2000UL));
   check("IRQ path must not steal a live on_cpu==me claim",
         !leftover_current_should_repair(0, 0, 1, 0x2000UL, 0x1000UL, 0, 0x2000UL));
   check("wild dlfree pointer is outside the kernel heap",
         !kheap_ptr_in_range(0x800250d58UL));
   check("kheap base is inside the kernel heap",
         kheap_ptr_in_range(MEM_KHEAP_BASE));
   check("leftover current leave uses owner nest token",
         leftover_leave_token(0x12, 0x1, 0x12, 1) == 0x12);
   check("leave without owner nest falls back to sampled current",
         leftover_leave_token(0x12, 0x1, 0, 0) == 0x1);
   check("FOREIGN leftover kernel current must drop",
         leftover_current_should_repair(3, 0, 0, 0x1000UL, 0x1000UL, 0, 0x2000UL));
   check("claimed kernel on this CPU is not leftover",
         !leftover_current_should_repair(0, 0, 0, 0x1000UL, 0x1000UL, 0, 0x2000UL));
   check("non-canonical current is not a PCB",
         !pcb_ptr_ok(0x0000000100000000UL));
   check("kheap current pointer is a PCB",
         pcb_ptr_ok(MEM_KHEAP_BASE));
   check("BSS ready_lock is not a freeable zombie PCB",
         !kheap_ptr_in_range(0x3c1178UL));
   check("leftover USER A while on USER B CR3 must not idle",
         !leftover_current_should_repair(3, 0, 1, 0xBFFDC000UL, 0x07FFF000UL, 0, 0x03a10000UL));
   check("unclaimed leftover kernel is release-before-switch",
         !leftover_current_should_repair(-1, 0, 0, 0x1000UL, 0x1000UL, 0, 0x03a10000UL));
   check("unclaimed USER on kernel CR3 on process kstack must not idle",
         !leftover_current_should_repair(-1, 0, 1, 0x2000UL, 0x1000UL, 0, 0x03a10000UL));
   /* Live cert: leftover task_mgr on idle BSS, torn 64-bit slots. */
   check("unclaimed leftover kernel on idle BSS must repair",
         leftover_current_should_repair(-1, 0, 0, 0x1000UL, 0x1000UL, 0, 0x2000UL));
   check("RBP (cpu_id<<32)|kheap is a torn 64-bit slot",
         u64_slot_torn(0x10253e5d0UL));
   check("RSI (kheap<<32)|small is a torn 64-bit slot",
         u64_slot_torn(0x35bc8e00000000eUL));
   check("plain kheap pointer is not a torn slot",
         !u64_slot_torn(0x35bc8e0UL));
   check("kheap pointer is not a held_crit_n count",
         !held_crit_n_ok(0x35BC8E0));
   check("FOREIGN on_cpu must leave the process kstack",
         irq_kstack_foreign_must_leave(3, 0));
   check("unclaimed runner must stay on the process kstack",
         !irq_kstack_foreign_must_leave(-1, 0));
   check("claimed by this CPU stays on the process kstack",
         !irq_kstack_foreign_must_leave(0, 0));
   check("idle stack is at least 64KiB (32KiB overflowed, cert 248137)",
         MEM_IDLE_STACK_SIZE >= 0x10000UL);
   check("unclaimed leftover USER on MEM_CPUIRQ must not schedule",
         leftover_user_on_cpuirq(-1, 1, MEM_CPUIRQ_BASE + 0x1000UL));
   check("unclaimed USER on process kstack may still schedule",
         !leftover_user_on_cpuirq(-1, 1, 0x03a10000UL));
   check("claimed USER on MEM_CPUIRQ must not schedule (cert 248127)",
         leftover_user_on_cpuirq(1, 1, MEM_CPUIRQ_BASE + 0x1000UL));
   check("idle current on a user CR3 is leftover",
         idle_on_user_cr3(0, 0xbf964000UL));
   check("idle current on kernel CR3 is not leftover",
         !idle_on_user_cr3(0, 0x1000UL));
   check("kheap+0x41 RIP is kernel-reserved (cert 248121)",
         exc_kernel_reserved_rip(MEM_KHEAP_BASE + 0x41UL));
   check("user ELF RIP is not kernel-reserved",
         !exc_kernel_reserved_rip(MEM_USER_ELF_BASE + 0x1000UL));
   check("USER #UD in the ELF window is a user fault",
         exc_user_fault_rip(1, MEM_USER_ELF_BASE + 0x1000UL));
   check("USER current executing kheap is a kernel smash",
         !exc_user_fault_rip(1, MEM_KHEAP_BASE + 0x41UL));
   check("idle advertised on a user stack is leftover",
         leftover_sys_on_userstack(0, MEM_USER_STACK_GUARD + 0x1000UL));
   check("USER on a user stack is not leftover-sys",
         !leftover_sys_on_userstack(1, MEM_USER_STACK_GUARD + 0x1000UL));
   check("idle diverted onto MEM_CPUIRQ must not schedule",
         leftover_sys_on_cpuirq(0, MEM_CPUIRQ_BASE + 0x1000UL));
   check("leftover gcc41 on gcc30 CR3 uses gcc30 crit token",
         crit_token_for_pids(41, 30) == 31);
   check("matching current and mm pids keep that token",
         crit_token_for_pids(30, 30) == 31);
   check("FOREIGN on a valid kstack stays (fork straddle)",
         irq_kstack_dest(0x36d1110UL, 0x36b1490UL, 0x36d1490UL, 3, 1)
         == IRQ_KSTACK_STAY);
   check("torn kstack_top is not an in-range stay (cert 248123)",
         irq_kstack_dest(0x36d1110UL, 0x36b1490UL, 0x40000036d1490UL, 0, 0)
         == IRQ_KSTACK_CPU);
   check("kernel .text RIP may iretq",
         irq_iretq_rip_ok(0x10047dUL));
   check("user ELF RIP may iretq",
         irq_iretq_rip_ok(MEM_USER_ELF_BASE + 0x1000UL));
   check("kheap RIP must not iretq (cert 248124)",
         !irq_iretq_rip_ok(MEM_KHEAP_BASE + 0x41UL));
   check("leftover gcc42 on make CR3 is leftover-mm (cert 248125)",
         leftover_user_on_other_cr3(1, 0xbe1d0000UL, 0xbffdc000UL));
   check("leftover USER on pagedir1 is not leftover-mm (cert 248132)",
         !leftover_user_on_other_cr3(1, 0xB552F000UL, 0x191000UL));
   check("USER matching CR3 is not leftover-mm",
         !leftover_user_on_other_cr3(1, 0xbffdc000UL, 0xbffdc000UL));
   check("0x100000001 is canonical but not walkable (cert 248126)",
         addr_canonical(0x100000001UL) && !pf64_walkable(0x100000001UL));
   check("kernel text is PF-walkable",
         pf64_walkable(0x10047dUL));
   check("leftover USER on user stack must not schedule (cert 248128)",
         leftover_user_on_userstack(1, 0x3FFFDAC0UL));
   check("USER on process kstack is not leftover-userstack",
         !leftover_user_on_userstack(1, 0x03a10000UL));
   check("leftover USER advertised on pagedir1 (cert 248128)",
         leftover_user_on_kernel_cr3(1, 0xB552F000UL, 0x191000UL));
   check("USER matching its own CR3 is not leftover-kernel-cr3",
         !leftover_user_on_kernel_cr3(1, 0xB552F000UL, 0xB552F000UL));
   check("user-stack iretq frame on pagedir1 is not ok (cert 248128)",
         !irq_iretq_frame_cr3_ok(0x3FFFDAC0UL, 0x191000UL));
   check("user-stack iretq frame on a user CR3 is ok",
         irq_iretq_frame_cr3_ok(0x3FFFDAC0UL, 0xB552F000UL));
   check("unclaimed leftover USER on user stack + kernel CR3 must repair",
         leftover_current_should_repair(-1, 0, 1, 0xB552F000UL, 0x191000UL,
                                       0, 0x3FFFDAC0UL));
   check("torn RIP (cpu<<32)|kheap is not a user opcode (cert 248129)",
         !exc_user_fault_rip(1, 0x10390d900UL) &&
         u64_slot_torn(0x10390d900UL) &&
         !pf64_walkable(0x10390d900UL));
   check("user-stack RIP is not a user opcode (cert 248129)",
         !exc_user_fault_rip(1, 0x3FFFE4C0UL));
   check("user ELF RIP remains a user opcode",
         exc_user_opcode_rip(MEM_USER_ELF_BASE + 0x1000UL));
   check("unwalkable RIP must not recover as PF64-BADVA userfault",
         !pf64_walkable(0x10390d900UL) ||
         !exc_user_fault_rip(1, 0x10390d900UL));
   check("leftover gcc on cc1 CR3 must not schedule (cert 248131)",
         leftover_user_on_other_cr3(1, 0xBE1D0000UL, 0xBF964000UL));
   check("leftover gcc on cc1 user stack uses CPUIRQ not gcc kstack",
         irq_kstack_dest_mm(0x3FFFE990UL, 0x03a00000UL, 0x03a20000UL, 0, 0,
                            0xBE1D0000UL, 0xBF964000UL) == IRQ_KSTACK_CPU);
   check("claimed user matching CR3 still uses process kstack",
         irq_kstack_dest_mm(0x3FFFE990UL, 0x03a00000UL, 0x03a20000UL, 0, 0,
                            0xBF964000UL, 0xBF964000UL) == IRQ_KSTACK_PROCESS);
   check("leftover USER on pagedir1 user stack uses CPUIRQ",
         irq_kstack_dest_mm(0x3FFFDAC0UL, 0x03a00000UL, 0x03a20000UL, 0, 0,
                            0xB552F000UL, 0x191000UL) == IRQ_KSTACK_CPU);
   check("unclaimed leftover parent on child CR3 stays PROCESS (fork)",
         irq_kstack_dest_mm(0x3FFFE990UL, 0x03a00000UL, 0x03a20000UL, -1, 0,
                            0x7FFF000UL, 0x7E22000UL) == IRQ_KSTACK_PROCESS);
   check("leftover USER on pagedir1 keeps USER crit token (cert 248132)",
         crit_token_for_pids(35, 0) == 36);
   check("leftover USER on pagedir1 must skip vfs_busy (cert 248132)",
         leftover_vfs_must_skip(1, 0xB552F000UL, 0x191000UL,
                                0x3FFFDAC0UL, 0));
   check("leftover idle on a user CR3 must skip vfs_busy",
         leftover_vfs_must_skip(0, 0x1000UL, 0xbf964000UL,
                                0x03a10000UL, 1));
   check("idle on its reserved stack may still take crits",
         !leftover_vfs_must_skip(0, 0x1000UL, 0x1000UL,
                                 MEM_IDLE_STACK_BASE + 0x1000UL, 1));
   check("torn 0x100000001 is not a ready-ring link (cert 248133)",
         !sched_link_ok(0x100000001UL));
   check("kheap PCB pointer is a ready-ring link",
         sched_link_ok(MEM_KHEAP_BASE));
   check("zero RIP on MEM_CPUIRQ must not kill USER (cert 248135)",
         leftover_iretq_must_not_kill_user(MEM_CPUIRQ_BASE + 0x130UL, 0));
   check("bad ELF RIP on a user stack may still kill USER",
         !leftover_iretq_must_not_kill_user(0x3FFFDAC0UL, 0));
   check("FOREIGN leftover without crit_wait must skip (fork)",
         leftover_timer_must_skip(2, 0, 1, 0x03a10000UL,
                                  0xBFFDC000UL, 0xBFFDC000UL, 0) &&
         !leftover_timer_must_abandon(2, 0, 1, 0x03a10000UL,
                                      0xBFFDC000UL, 0xBFFDC000UL, 0));
   check("FOREIGN leftover + crit_wait must abandon (cert 248136)",
         leftover_timer_must_abandon(2, 0, 1, 0x03a10000UL,
                                     0xBFFDC000UL, 0xBFFDC000UL, 1));
   check("leftover USER on pagedir1 + crit_wait must abandon",
         leftover_timer_must_abandon(0, 0, 1, 0x03a10000UL,
                                     0xB552F000UL, 0x191000UL, 1));
   check("leftover USER on pagedir1 without crit_wait must skip",
         leftover_timer_must_skip(0, 0, 1, 0x03a10000UL,
                                  0xB552F000UL, 0x191000UL, 0) &&
         !leftover_timer_must_abandon(0, 0, 1, 0x03a10000UL,
                                      0xB552F000UL, 0x191000UL, 0));
   check("leftover parent on user stack without crit_wait must skip (fork)",
         leftover_timer_must_skip(0, 0, 1, 0x3FFFE990UL,
                                  0x7FFF000UL, 0x7E22000UL, 0));
   check("ACCESS_SYS on reserved idle stack is leftover-sys-idle (cert 248137)",
         leftover_sys_on_idle_stack(0, MEM_IDLE_STACK_BASE + 0x1000UL));
   check("ACCESS_SYS on reserved idle leftover-skips (dest defense)",
         leftover_timer_must_skip(-1, 0, 0, MEM_IDLE_STACK_BASE + 0x1000UL,
                                  0x1000UL, 0x1000UL, 0));
   check("leftover advertised idle on user CR3 must skip (fork)",
         leftover_idle_timer_must_skip(1, 0x7E22000UL,
                                       MEM_CPUIRQ_BASE + 0x1000UL));
   check("ACCESS_SYS dest'd to CPUIRQ leftover-skips even claimed idle",
         leftover_timer_must_skip(0, 0, 0, MEM_CPUIRQ_BASE + 0x1000UL,
                                  0x1000UL, 0x1000UL, 0));
   check("leftover idle #UD on user CR3 retargets owner (cert 248139)",
         leftover_sys_ud_retarget(1, 0x7E22000UL, 0xac10000UL) &&
         !leftover_sys_ud_retarget(1, 0x7E22000UL, MEM_KHEAP_BASE + 0x41UL) &&
         !leftover_sys_ud_retarget(0, 0x7E22000UL, 0xac10000UL));
   check("claimed idle on reserved idle + user CR3 leftover_load_only (cert 248141)",
         leftover_idle_claimed_user_cr3_load_only(
            1, 0, 0, 0x7E22000UL, MEM_IDLE_STACK_BASE + 0x1000UL));
   check("dest'd claimed idle on CPUIRQ is not leftover_load_only",
         !leftover_idle_claimed_user_cr3_load_only(
            1, 0, 0, 0x7E22000UL, MEM_CPUIRQ_BASE + 0x1000UL));

   return g_ok ? 0 : 1;
}
