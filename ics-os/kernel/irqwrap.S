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

.globl syscallwrapper
syscallwrapper:
    PUSH_ALL
    /* After PUSH_ALL, stack top is r15..rax (rax at offset 14*8=112).
       Legacy DEX int 0x30 ABI: rax=fxn, rbx=a1, rcx=a2, rdx=a3, rsi=a4, rdi=a5. */
    cmpq $0x90, 112(%rsp)
    jne .Lapi_syscall
    movq %rsp, %rdi
    call user_fork_frame
    movq %rax, 112(%rsp)
    jmp .Lsyscall_return
.Lapi_syscall:
    movq 112(%rsp), %rdi        /* saved rax -> fxn */
    movq 104(%rsp), %rsi        /* saved rbx -> a1 */
    movq 96(%rsp), %rdx         /* saved rcx -> a2 */
    movq 88(%rsp), %rcx         /* saved rdx -> a3 */
    movq 80(%rsp), %r8          /* saved rsi -> a4 */
    movq 72(%rsp), %r9          /* saved rdi -> a5 */
    call api_syscall
    movq %rax, 112(%rsp)        /* return value in caller's rax */
.Lsyscall_return:
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
    movq 112(%rsp), %rdi          /* sysno */
    movq 72(%rsp), %rsi           /* user rdi */
    movq 80(%rsp), %rdx           /* user rsi */
    movq 88(%rsp), %rcx           /* user rdx */
    movq 40(%rsp), %r8            /* user r10 */
    movq 56(%rsp), %r9            /* user r8 */
    call syscallentry64
    movq %rax, 112(%rsp)
    POP_ALL
    pushq %r11                    /* RFLAGS */
    pushq $0x08                   /* CS */
    pushq %rcx                    /* RIP */
    iretq

.globl CPUintwrapper
CPUintwrapper:
    cli
    PUSH_ALL
    call CPUint
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
    call fdchandler
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
    call nocoprocessor
    POP_ALL
    iretq

.globl timerwrapper
timerwrapper:
    cli
    PUSH_ALL
    /* Per-CPU selfhost spin watchdog diagnostics: save the pre-PUSH_ALL RSP
       (RSP_entry, pointing at the CPU-pushed RIP) so the C watchdog can walk
       the kernel C stack; r10/rax are already saved by PUSH_ALL and restored
       by POP_ALL, so clobbering them here is safe. */
    call smp_cpu_id
    movl %eax, %r10d
    leaq wd_krsp_cpu(,%r10,8), %r11
    leaq 120(%rsp), %r12
    mov %r12, (%r11)
    imull $248, %r10d, %r10d
    leaq wd_frame_cpu(%r10), %r11
    /* Dump a wide interrupt-frame window ([rsp+0..240]) to locate the live RIP. */
    movq 0(%rsp), %rax
    movq %rax, 0(%r11)
    movq 8(%rsp), %rax
    movq %rax, 8(%r11)
    movq 16(%rsp), %rax
    movq %rax, 16(%r11)
    movq 24(%rsp), %rax
    movq %rax, 24(%r11)
    movq 32(%rsp), %rax
    movq %rax, 32(%r11)
    movq 40(%rsp), %rax
    movq %rax, 40(%r11)
    movq 48(%rsp), %rax
    movq %rax, 48(%r11)
    movq 56(%rsp), %rax
    movq %rax, 56(%r11)
    movq 64(%rsp), %rax
    movq %rax, 64(%r11)
    movq 72(%rsp), %rax
    movq %rax, 72(%r11)
    movq 80(%rsp), %rax
    movq %rax, 80(%r11)
    movq 88(%rsp), %rax
    movq %rax, 88(%r11)
    movq 96(%rsp), %rax
    movq %rax, 96(%r11)
    movq 104(%rsp), %rax
    movq %rax, 104(%r11)
    movq 112(%rsp), %rax
    movq %rax, 112(%r11)
    movq 120(%rsp), %rax
    movq %rax, 120(%r11)
    movq 128(%rsp), %rax
    movq %rax, 128(%r11)
    movq 136(%rsp), %rax
    movq %rax, 136(%r11)
    movq 144(%rsp), %rax
    movq %rax, 144(%r11)
    movq 152(%rsp), %rax
    movq %rax, 152(%r11)
    movq 160(%rsp), %rax
    movq %rax, 160(%r11)
    movq 168(%rsp), %rax
    movq %rax, 168(%r11)
    movq 176(%rsp), %rax
    movq %rax, 176(%r11)
    movq 184(%rsp), %rax
    movq %rax, 184(%r11)
    movq 192(%rsp), %rax
    movq %rax, 192(%r11)
    movq 200(%rsp), %rax
    movq %rax, 200(%r11)
    movq 208(%rsp), %rax
    movq %rax, 208(%r11)
    movq 216(%rsp), %rax
    movq %rax, 216(%r11)
    movq 224(%rsp), %rax
    movq %rax, 224(%r11)
    movq 232(%rsp), %rax
    movq %rax, 232(%r11)
    movq 240(%rsp), %rax
    movq %rax, 240(%r11)
    call time_handler
    call schedule_from_timer
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
    /* After PUSH_ALL (15 regs): error at 120, rip at 128, cs at 136 */
    /* struct gpf_info layout (gcc): rip(0) cr2(8) err(16) pad(20) cs(20) ss(24)
       rsp(32) cr3(40) sizeof=48 */
    subq $64, %rsp
    movq 192(%rsp), %rax        /* saved rip (128 + 64) */
    movq %rax, 0(%rsp)
    movq %cr2, %rax
    movq %rax, 8(%rsp)
    movl 184(%rsp), %eax        /* error code (120 + 64) */
    movl %eax, 16(%rsp)
    movw 200(%rsp), %ax         /* cs (136 + 64) -> field cs @20 */
    movl %eax, 20(%rsp)
    movw %ss, %ax
    movl %eax, 24(%rsp)
    leaq 216(%rsp), %rax        /* active RSP at fault (CPL0 frame) */
    movq %rax, 32(%rsp)
    movq %cr3, %rax
    movq %rax, 40(%rsp)
    movq %rsp, %rdi
    movq 176(%rsp), %rsi        /* saved rax */
    movq 160(%rsp), %rdx        /* saved rcx */
    call GPFhandler64
    addq $64, %rsp
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
    movq %cr2, %rdi             /* location */
    movq 120(%rsp), %rsi        /* error code */
    movq 128(%rsp), %rdx        /* faulting RIP */
    movq %rsp, %rcx             /* saved register frame */
    call pagefaulthandler
    POP_ALL
    addq $8, %rsp
    iretq

.globl div_wrapper
div_wrapper:
    cli
    PUSH_ALL
    call divide_error
    POP_ALL
    iretq

.globl invalidtsswrapper
invalidtsswrapper:
    cli
    PUSH_ALL
    call exc_invalidtss
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
    call virtio_blk_irq
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
