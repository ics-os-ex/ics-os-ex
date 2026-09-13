/* 64-bit interrupt / exception wrappers — System V AMD64. */
.text

.extern time_handler
.extern kbd_irq
.extern mouse_irq
.extern pagefaulthandler
.extern syscallentry64
.extern exc_doublefault
.extern fdchandler
.extern api_syscall
.extern GPFhandler
.extern GPFhandler64
.extern CPUint
.extern nocoprocessor
.extern wrapper_call_trap
.extern divide_error
.extern exc_invalidtss
.extern irq_activate
.extern schedule_from_timer
.extern lapic_eoi
.extern smp_cpu_id
.extern smp_reschedule_ipi
.extern smp_tlb_shootdown_ipi
.extern virtio_blk_irq
.extern xhci_irq
.extern user_fork_frame
.extern irq_kstack_enter
.extern cpus
.extern smp_have_rdtscp
.extern irq_safe_stack_top
.extern irq_safe_stack_base

/* Must match cpu_local / PCB386 (see irqwrap_offchk_* in process.c). */
.set CPU_LOCAL_SIZE, 48
.set CPU_LOCAL_CURRENT, 16
.set CPU_LOCAL_KSTACK, 32
.set PCB_ACCESSLEVEL, 0x4b8
.set PCB_ON_CPU, 0xc84
.set PCB_KSTACK_BASE, 0xc88
.set PCB_KSTACK_TOP, 0xc90
.set ACCESS_USER_VAL, 1
.set MAX_CPUS_VAL, 8

.macro PUSH_ALL
    cld
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
.endm

.macro POP_ALL
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
.endm

/* Move kernel C off the interrupted stack.
 *
 * User ELFs still run in kernel CS, so a same-privilege interrupt keeps the
 * user RSP and kernel C would otherwise execute on (and smash) the user
 * stack.  Switch to the per-process kheap kstack in pure assembly: any C
 * prologue would already have written to the user stack.  rdtscp/TSC_AUX is
 * the same CPU-id source as smp_cpu_id(); LAPIC MMIO is not readable under a
 * user CR3.
 *
 * %r13 holds THIS entry's PUSH_ALL frame for the whole handler and is what
 * IRQ_KSTACK_LEAVE restores.  It is callee-saved, so every C callee and
 * context_switch/context_load preserve it.  Keeping the restore in a register
 * instead of a PCB field is what makes nesting correct: a #PF or #GP taken
 * while already on the kstack returns to its own frame, with its own error
 * code, rather than to the outermost entry's frame.  The previous
 * PCB.irq_kstack_nest counter got this wrong and made a nested fault iretq
 * from the syscall's user frame after `addq $8` for an error code that frame
 * never had, which surfaced as "GPF64 rip=0x8 cs=0x206" (CS read as RIP).
 *
 * Claimed user tasks keep the 128KiB per-process kstack for syscall depth.
 * FOREIGN user IRQs use this CPU's reserved MEM_CPUIRQ stack so kernel C
 * never runs on another CPU's user stack or process kstack.
 *
 * ACCESS_SYS (idle, disk_mgr, …) stay on the interrupted stack.  Moving
 * them onto MEM_CPUIRQ made schedule_from_timer save their continuation
 * there; the next FOREIGN/kernel IRQ reused the same top and smashed the
 * saved RIP into `cpus[]` (UD64 rip=cpus+0x21, opcodes from cpu_local).
 * Nested: do not reset RSP to either top.
 */
.macro IRQ_KSTACK_ENTER
    movq %rsp, %r13
    movabs $smp_have_rdtscp, %rax
    cmpl $1, (%rax)
    jne .Lkenter\@
    rdtscp                             /* ecx = IA32_TSC_AUX = cpu id */
    movl %ecx, %eax
    cmpl $MAX_CPUS_VAL, %eax
    jae .Lkenter\@
    /* Already on this CPU's IRQ stack? nested. */
    movabs $irq_safe_stack_top, %rdx
    movq (%rdx,%rax,8), %r8
    testq %r8, %r8
    jz .Llookup\@
    cmpq %r8, %r13
    jae .Llookup\@
    movabs $irq_safe_stack_base, %rdx
    cmpq (%rdx,%rax,8), %r13
    jae .Lkenter\@
.Llookup\@:
    movq %rax, %rdx
    shlq $4, %rax
    shlq $5, %rdx
    addq %rdx, %rax                    /* cpu * sizeof(cpu_local) */
    movabs $cpus, %rdx
    movq CPU_LOCAL_CURRENT(%rdx,%rax), %rax
    testq %rax, %rax
    jz .Lkenter\@
    cmpl $ACCESS_USER_VAL, PCB_ACCESSLEVEL(%rax)
    jne .Lkenter\@                     /* ACCESS_SYS: keep own stack */
    movq PCB_KSTACK_TOP(%rax), %rdx
    testq %rdx, %rdx
    jz .Lnokstk\@
    cmpq %rdx, %r13
    jae .Lnokstk\@
    cmpq PCB_KSTACK_BASE(%rax), %r13
    jae .Lkenter\@
.Lnokstk\@:
    cmpl $0, PCB_ON_CPU(%rax)
    jl .Lmayusr\@
    cmpl %ecx, PCB_ON_CPU(%rax)
    jne .Lcpuirq\@
.Lmayusr\@:
    movabs $0x3FD00000, %r9
    cmpq %r9, %r13
    jb .Lbss\@
    movabs $0x40000000, %r9
    cmpq %r9, %r13
    jae .Lbss\@
    testq %rdx, %rdx
    jz .Lcpuirq\@
    movq %rdx, %rsp
    jmp .Lkenter\@
.Lbss\@:
    movabs $0x00400000, %r9
    cmpq %r9, %r13
    jae .Lkenter\@
    jmp .Lcpuirq\@
.Lcpuirq\@:
    testq %r8, %r8
    jz .Lkenter\@
    movq %r8, %rsp
.Lkenter\@:
    movq %r13, %rdi
    call irq_kstack_enter
.endm

.macro IRQ_KSTACK_LEAVE
    cli
    movq %r13, %rsp
.endm

.globl syscallwrapper
syscallwrapper:
    cli
    PUSH_ALL
    IRQ_KSTACK_ENTER
    movq %r13, %r12               /* this entry's PUSH_ALL frame */
    /* After PUSH_ALL, stack top is r15..rax (rax at offset 14*8=112).
       Legacy DEX int 0x30 ABI: rax=fxn, rbx=a1, rcx=a2, rdx=a3, rsi=a4, rdi=a5. */
    cmpq $0x90, 112(%r12)
    jne .Lapi_syscall
    /* Pass THIS syscall's PUSH_ALL frame (%r13), not a NULL placeholder.
       user_fork_frame used to ignore the argument and trust irq_user_rsp,
       which can still name a prior timer IRQ's user frame. The child then
       iretq'd 16 bytes too high -- RIP took RFLAGS (UD64 rip=0x207). */
    movq %r13, %rdi
    call user_fork_frame
    movq %rax, 112(%r13)
    jmp .Lsyscall_return
.Lapi_syscall:
    movq 112(%r12), %rdi        /* saved rax -> fxn */
    movq 104(%r12), %rsi        /* saved rbx -> a1 */
    movq 96(%r12), %rdx         /* saved rcx -> a2 */
    movq 88(%r12), %rcx         /* saved rdx -> a3 */
    movq 80(%r12), %r8          /* saved rsi -> a4 */
    movq 72(%r12), %r9          /* saved rdi -> a5 */
    call api_syscall
    movq %rax, 112(%r13)        /* return value in caller's rax */
.Lsyscall_return:
    IRQ_KSTACK_LEAVE
    POP_ALL
    iretq

.globl fork_child_return
fork_child_return:
    POP_ALL
    iretq

/* Linux x86-64 SYSCALL entry (IA32_LSTAR). SYSCALL saves next-RIP in RCX and
   RFLAGS in R11 (no hardware stack frame). After PUSH_ALL, sysno=rax@112,
   arg0=rdi@72, arg1=rsi@80, arg2=rdx@88, arg3=r10@40, arg4=r8@56.
   Return via IRETQ (CPL0 / kernel CS): push RFLAGS, CS, RIP — IRETQ pops
   RIP, CS, RFLAGS. */
.globl syscallentry
syscallentry:
    PUSH_ALL
    IRQ_KSTACK_ENTER
    movq %r13, %r12               /* this entry's PUSH_ALL frame */
    movq 112(%r12), %rdi          /* sysno */
    movq 72(%r12), %rsi           /* user rdi */
    movq 80(%r12), %rdx           /* user rsi */
    movq 88(%r12), %rcx           /* user rdx */
    movq 40(%r12), %r8            /* user r10 */
    movq 56(%r12), %r9            /* user r8 */
    call syscallentry64
    movq %rax, 112(%r13)
    IRQ_KSTACK_LEAVE
    POP_ALL
    pushq %r11                    /* RFLAGS */
    pushq $0x08                   /* CS */
    pushq %rcx                    /* RIP */
    iretq

.globl CPUintwrapper
CPUintwrapper:
    cli
    PUSH_ALL
    IRQ_KSTACK_ENTER
    call CPUint
    IRQ_KSTACK_LEAVE
    POP_ALL
    iretq

.globl fdcwrapper
fdcwrapper:
    movw 8(%rsp), %ax
    cmpw $0x8, %ax
    je .Lfdc_exception
    cmpw $0x20, %ax
    je .Lfdc_exception
    cmpw $0x2b, %ax
    je .Lfdc_exception
    cmpw $0x23, %ax
    je .Lfdc_exception
    movq 0(%rsp), %rdi
    movq %rax, %rsi
    movq 16(%rsp), %rdx
    call wrapper_call_trap
    ret
.Lfdc_exception:
    cli
    PUSH_ALL
    IRQ_KSTACK_ENTER
    call fdchandler
    IRQ_KSTACK_LEAVE
    POP_ALL
    iretq

/* #UD (vector 6) and #MF (vector 16) have nothing to do with CR0.TS, so they
   must not share copwrapper: clearing TS and returning retries the faulting
   instruction forever, and when the frame did not match copwrapper's
   call-vs-exception heuristic it surfaced as a "#GP inside copwrapper" that hid
   the first fault entirely.  Neither vector pushes an error code, so after
   PUSH_ALL the frame is RIP@120, CS@128 -- %r13 is handed to C to decode. */
.globl udwrapper
udwrapper:
    cli
    PUSH_ALL
    IRQ_KSTACK_ENTER
    movq %r13, %rdi
    call invalid_opcode64
    IRQ_KSTACK_LEAVE
    POP_ALL
    iretq

.globl mfwrapper
mfwrapper:
    cli
    PUSH_ALL
    IRQ_KSTACK_ENTER
    movq %r13, %rdi
    call fpu_error64
    IRQ_KSTACK_LEAVE
    POP_ALL
    iretq

.globl copwrapper
copwrapper:
    movw 8(%rsp), %ax
    cmpw $0x8, %ax
    je .Lcop_exception
    cmpw $0x20, %ax
    je .Lcop_exception
    cmpw $0x2b, %ax
    je .Lcop_exception
    cmpw $0x23, %ax
    je .Lcop_exception
    movq 0(%rsp), %rdi
    movq %rax, %rsi
    movq 16(%rsp), %rdx
    call wrapper_call_trap
    ret
.Lcop_exception:
    cli
    PUSH_ALL
    IRQ_KSTACK_ENTER
    call nocoprocessor
    IRQ_KSTACK_LEAVE
    POP_ALL
    iretq

.globl timerwrapper
timerwrapper:
    cli
    PUSH_ALL
    IRQ_KSTACK_ENTER
    /* Watchdog dump from the interrupted PUSH_ALL (%r13), not %rsp: C
       smp_cpu_id must not run on a user stack. */
    call smp_cpu_id
    movl %eax, %r10d
    leaq wd_krsp_cpu(,%r10,8), %r11
    leaq 120(%r13), %r12
    mov %r12, (%r11)
    imull $248, %r10d, %r10d
    leaq wd_frame_cpu(%r10), %r11
    movq 0(%r13), %rax
    movq %rax, 0(%r11)
    movq 8(%r13), %rax
    movq %rax, 8(%r11)
    movq 16(%r13), %rax
    movq %rax, 16(%r11)
    movq 24(%r13), %rax
    movq %rax, 24(%r11)
    movq 32(%r13), %rax
    movq %rax, 32(%r11)
    movq 40(%r13), %rax
    movq %rax, 40(%r11)
    movq 48(%r13), %rax
    movq %rax, 48(%r11)
    movq 56(%r13), %rax
    movq %rax, 56(%r11)
    movq 64(%r13), %rax
    movq %rax, 64(%r11)
    movq 72(%r13), %rax
    movq %rax, 72(%r11)
    movq 80(%r13), %rax
    movq %rax, 80(%r11)
    movq 88(%r13), %rax
    movq %rax, 88(%r11)
    movq 96(%r13), %rax
    movq %rax, 96(%r11)
    movq 104(%r13), %rax
    movq %rax, 104(%r11)
    movq 112(%r13), %rax
    movq %rax, 112(%r11)
    movq 120(%r13), %rax
    movq %rax, 120(%r11)
    movq 128(%r13), %rax
    movq %rax, 128(%r11)
    movq 136(%r13), %rax
    movq %rax, 136(%r11)
    movq 144(%r13), %rax
    movq %rax, 144(%r11)
    movq 152(%r13), %rax
    movq %rax, 152(%r11)
    movq 160(%r13), %rax
    movq %rax, 160(%r11)
    movq 168(%r13), %rax
    movq %rax, 168(%r11)
    movq 176(%r13), %rax
    movq %rax, 176(%r11)
    movq 184(%r13), %rax
    movq %rax, 184(%r11)
    movq 192(%r13), %rax
    movq %rax, 192(%r11)
    movq 200(%r13), %rax
    movq %rax, 200(%r11)
    movq 208(%r13), %rax
    movq %rax, 208(%r11)
    movq 216(%r13), %rax
    movq %rax, 216(%r11)
    movq 224(%r13), %rax
    movq %rax, 224(%r11)
    movq 232(%r13), %rax
    movq %rax, 232(%r11)
    movq 240(%r13), %rax
    movq %rax, 240(%r11)
    call time_handler
    call schedule_from_timer
    IRQ_KSTACK_LEAVE
    POP_ALL
    iretq

.globl reschedwrapper
reschedwrapper:
    cli
    PUSH_ALL
    call smp_reschedule_ipi
    POP_ALL
    iretq

.globl tlbshootdownwrapper
tlbshootdownwrapper:
    cli
    PUSH_ALL
    call smp_tlb_shootdown_ipi
    POP_ALL
    iretq

.globl kbdwrapper
kbdwrapper:
    PUSH_ALL
    call kbd_irq
    POP_ALL
    iretq

.globl mousewrapper
mousewrapper:
    PUSH_ALL
    call mouse_irq
    POP_ALL
    iretq

.globl gpfwrapper
gpfwrapper:
    cli
    /* On entry: 0(rsp)=error, 8(rsp)=rip, 16(rsp)=cs, 24(rsp)=rflags */
    PUSH_ALL
    IRQ_KSTACK_ENTER
    movq %r13, %r12               /* this entry's PUSH_ALL frame */
    /* After PUSH_ALL (15 regs): error at 120, rip at 128, cs at 136 */
    /* struct gpf_info layout (gcc): rip(0) cr2(8) err(16) pad(20) cs(20) ss(24)
       rsp(32) cr3(40) sizeof=48 */
    subq $64, %rsp
    movq 128(%r12), %rax        /* saved rip */
    movq %rax, 0(%rsp)
    movq %cr2, %rax
    movq %rax, 8(%rsp)
    movl 120(%r12), %eax        /* error code */
    movl %eax, 16(%rsp)
    movw 136(%r12), %ax         /* cs -> field cs @20 */
    movl %eax, 20(%rsp)
    movw %ss, %ax
    movl %eax, 24(%rsp)
    leaq 152(%r12), %rax        /* approx RSP at fault (CPL0 + error) */
    movq %rax, 32(%rsp)
    movq %cr3, %rax
    movq %rax, 40(%rsp)
    movq %rsp, %rdi
    movq 112(%r12), %rsi        /* saved rax */
    movq 96(%r12), %rdx         /* saved rcx */
    call GPFhandler64
    addq $64, %rsp
    IRQ_KSTACK_LEAVE
    POP_ALL
    addq $8, %rsp
    iretq

/* #8 double fault. Vector 8 pushes an error code. A bare C handler installed
   directly here would see the pushed frame (rip,cs,rflags,err) under its assumed
   return address and triple-fault. This wrapper captures the fault state, prints
   it, then halts (a double fault is not recoverable). */
.globl doublefaultwrapper
doublefaultwrapper:
    cli
    /* On entry: 0(rsp)=err, 8(rsp)=rip, 16(rsp)=cs, 24(rsp)=rflags */
    movq 8(%rsp), %rdi
    movq 16(%rsp), %rsi
    movq 24(%rsp), %rdx
    movq %cr2, %rcx
    call exc_doublefault
    jmp 1f
1:  hlt
    jmp 1b

.globl pfwrapper
pfwrapper:
    cli
    /* On entry: 0(rsp)=error, 8(rsp)=rip, ... */
    PUSH_ALL
    IRQ_KSTACK_ENTER
    movq %r13, %r12             /* this entry's PUSH_ALL frame */
    movq %cr2, %rdi             /* location */
    movq 120(%r12), %rsi        /* error code */
    movq 128(%r12), %rdx        /* faulting RIP */
    movq %r12, %rcx             /* saved register frame */
    call pagefaulthandler
    IRQ_KSTACK_LEAVE
    POP_ALL
    addq $8, %rsp
    iretq

.globl div_wrapper
div_wrapper:
    cli
    PUSH_ALL
    IRQ_KSTACK_ENTER
    call divide_error
    IRQ_KSTACK_LEAVE
    POP_ALL
    iretq

.globl invalidtsswrapper
invalidtsswrapper:
    cli
    PUSH_ALL
    IRQ_KSTACK_ENTER
    call exc_invalidtss
    IRQ_KSTACK_LEAVE
    POP_ALL
    addq $8, %rsp
    iretq

.globl irq1wrapper
irq1wrapper:
    PUSH_ALL
    movq $1, %rdi
    call irq_activate
    POP_ALL
    iretq

.globl irq2wrapper
irq2wrapper:
    PUSH_ALL
    movq $2, %rdi
    call irq_activate
    POP_ALL
    iretq

.globl irq3wrapper
irq3wrapper:
    PUSH_ALL
    movq $3, %rdi
    call irq_activate
    POP_ALL
    iretq

.globl irq4wrapper
irq4wrapper:
    PUSH_ALL
    movq $4, %rdi
    call irq_activate
    POP_ALL
    iretq

.globl irq5wrapper
irq5wrapper:
    PUSH_ALL
    movq $5, %rdi
    call irq_activate
    POP_ALL
    iretq

.globl irq6wrapper
irq6wrapper:
    PUSH_ALL
    movq $6, %rdi
    call irq_activate
    POP_ALL
    iretq

.globl irq7wrapper
irq7wrapper:
    PUSH_ALL
    movq $7, %rdi
    call irq_activate
    POP_ALL
    iretq

.globl irq8wrapper
irq8wrapper:
    PUSH_ALL
    movq $8, %rdi
    call irq_activate
    POP_ALL
    iretq

.globl irq9wrapper
irq9wrapper:
    PUSH_ALL
    movq $9, %rdi
    call irq_activate
    POP_ALL
    iretq

.globl irq10wrapper
irq10wrapper:
    PUSH_ALL
    movq $10, %rdi
    call irq_activate
    POP_ALL
    iretq

.globl irq11wrapper
irq11wrapper:
    PUSH_ALL
    movq $11, %rdi
    call irq_activate
    POP_ALL
    iretq

.globl irq12wrapper
irq12wrapper:
    PUSH_ALL
    movq $12, %rdi
    call irq_activate
    POP_ALL
    iretq

.globl virtio_msixwrapper
virtio_msixwrapper:
    cli
    PUSH_ALL
    IRQ_KSTACK_ENTER
    call virtio_blk_irq
    IRQ_KSTACK_LEAVE
    POP_ALL
    iretq

.macro XHCI_MSIX_WRAPPER index
.globl xhci_msixwrapper\index
xhci_msixwrapper\index:
    cli
    PUSH_ALL
    movq $\index, %rdi
    call xhci_irq
    POP_ALL
    iretq
.endm

XHCI_MSIX_WRAPPER 0
XHCI_MSIX_WRAPPER 1
XHCI_MSIX_WRAPPER 2
XHCI_MSIX_WRAPPER 3
XHCI_MSIX_WRAPPER 4
XHCI_MSIX_WRAPPER 5
XHCI_MSIX_WRAPPER 6
XHCI_MSIX_WRAPPER 7
