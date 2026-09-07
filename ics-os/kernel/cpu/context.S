/* Software context switch for x86_64 (kernel threads). */
.text
.globl context_switch
.globl context_load
.globl fpu_save
.globl fpu_restore
.extern smp_cpu_id
.extern smp_fpu_scratch
.extern ctx_load_in_progress
.extern ctx_load_check

.set CTX_R15, 0
.set CTX_R14, 8
.set CTX_R13, 16
.set CTX_R12, 24
.set CTX_R11, 32
.set CTX_R10, 40
.set CTX_R9,  48
.set CTX_R8,  56
.set CTX_RBP, 64
.set CTX_RDI, 72
.set CTX_RSI, 80
.set CTX_RDX, 88
.set CTX_RCX, 96
.set CTX_RBX, 104
.set CTX_RAX, 112
.set CTX_RIP, 120
.set CTX_CS,  128
.set CTX_RFLAGS, 136
.set CTX_RSP, 144
.set CTX_SS,  152
.set CTX_CR3, 160
.set CTX_RETCANARY, 168

context_switch:
    /* rdi=old, rsi=new, rdx=&old->on_cpu (or NULL) — save then release claim */
    cli
    movq %r15, CTX_R15(%rdi)
    movq %r14, CTX_R14(%rdi)
    movq %r13, CTX_R13(%rdi)
    movq %r12, CTX_R12(%rdi)
    movq %r11, CTX_R11(%rdi)
    movq %r10, CTX_R10(%rdi)
    movq %r9,  CTX_R9(%rdi)
    movq %r8,  CTX_R8(%rdi)
    movq %rbp, CTX_RBP(%rdi)
    movq %rdi, CTX_RDI(%rdi)
    movq %rsi, CTX_RSI(%rdi)
    movq %rdx, CTX_RDX(%rdi)
    movq %rcx, CTX_RCX(%rdi)
    movq %rbx, CTX_RBX(%rdi)
    movq %rax, CTX_RAX(%rdi)
    /* Save the return address of the live ps_switchto frame as a canary.
       context_load later verifies it on the destination stack before ret. */
    movq 8(%rbp), %rax
    movq %rax, CTX_RETCANARY(%rdi)
    movq (%rsp), %rax
    movq %rax, CTX_RIP(%rdi)
    leaq 8(%rsp), %rax
    movq %rax, CTX_RSP(%rdi)
    pushfq
    popq %rax
    /* Force IF=1: context_switch runs under the timer's cli (IF=0), so the
       raw RFLAGS would otherwise be saved with IF=0 and the task would later
       resume with interrupts disabled, freezing the timer/scheduler. A
       scheduled task must always resume interruptible. */
    orl $0x200, %eax
    movq %rax, CTX_RFLAGS(%rdi)
    movq %cr3, %rax
    movq %rax, CTX_CR3(%rdi)

    /* Old task is fully saved — allow another CPU to pick it up. */
    testq %rdx, %rdx
    jz 3f
    movl $-1, (%rdx)
3:
    movq %rsi, %rdi

/* Ring-3 note: CTX_CS/SS are saved for a future iretq path. Today we
   ret to kernel CS; user processes record USER_CODE in PCB.usercs. */
/* Ring-3: CTX_CS/SS saved for a future iretq. Today we `ret` in kernel CS. */
context_load:
      /* CR3, RSP, and GPR restoration is not interruptible.  The destination
          RFLAGS below re-enables interrupts after ctx_load_in_progress is set.
          Query the CPU id BEFORE loading the destination CR3/RSP so the call
          cannot clobber the destination task's live stack frame. */
      cli
    movq %rdi, %r13
    call smp_cpu_id
    movl %eax, %r11d
    movq CTX_CR3(%r13), %rax
    testq %rax, %rax
    jz 1f
    /* Always reload CR3 to flush TLB when entering a task. */
    movq %rax, %cr3
1:
    movq CTX_RSP(%r13), %rsp
    /* Run the canary check only for the user-task/kernel-epilogue case.  The
       C call executes on the destination stack, so avoid it for early kernel
       idle/thread contexts.  R12 is callee-saved and carries the CPU id. */
     /* Run the canary check only for the user-task/kernel-epilogue case.  The
       C call executes on the destination stack, so avoid it for early kernel
       idle/thread contexts.  R12 is callee-saved and carries the CPU id. */
    movq CTX_RIP(%r13), %r10
    movq CTX_RBP(%r13), %r9
    cmpq $0x100000, %r10
    jb 2f
    cmpq $0x300000, %r10
    jae 2f
    cmpq $0x3fff0000, %r9
    jb 2f
    cmpq $0x40000000, %r9
    jae 2f
    movq %r11, %r12
    movq %r13, %rdi
    call ctx_load_check
    movq %r12, %r11
2:
    /* Set the in-context-load flag NOW (RSP is the new task's).
        A timer IRQ that fires from here until the flag is cleared
        will see ctx_load_in_progress==1 and bail out of
        schedule_from_timer without calling ps_switchto. */
    leaq ctx_load_in_progress(%rip), %rax
    movl $1, (%rax,%r11,4)
    movq CTX_R15(%r13), %r15
    movq CTX_R14(%r13), %r14
    movq CTX_R12(%r13), %r12
    movq CTX_R11(%r13), %r10
    movq CTX_RBP(%r13), %rbp
    movq CTX_RBX(%r13), %rbx
    movq CTX_RDI(%r13), %rdi
    movq CTX_RSI(%r13), %rsi
    movq CTX_RDX(%r13), %rdx
    movq CTX_RCX(%r13), %rcx
    movq CTX_R8(%r13), %r8
    movq CTX_R9(%r13), %r9
    /* Clear the flag while IF is still 0, then publish the destination RIP
       before enabling interrupts.  This removes the old window where IF was
       enabled while the flag was already clear and the RIP had not yet been
       pushed. */
    leaq ctx_load_in_progress(%rip), %rax
    movl $0, (%rax,%r11,4)
    movq %r10, %r11
    movq CTX_R10(%r13), %r10
    movq CTX_RFLAGS(%r13), %r14
    movq CTX_RIP(%r13), %r12
    movq CTX_RAX(%r13), %rax
    movq CTX_R13(%r13), %r13
    pushq %r12
    /* Force IF=1 on resume as well (belt-and-suspenders for any context
       saved with IF=0 before the save-side fix, and for the first switch of
       a freshly-created task). */
    orl $0x200, %r14d
    pushq %r14
    popfq
    ret

fpu_save:
    /* rdi may be unaligned inside PCB; use this CPU's aligned scratch. */
    pushq %rdi
    call smp_cpu_id
    movslq %eax, %rax
    shlq $9, %rax
    leaq smp_fpu_scratch(%rip), %rsi
    addq %rax, %rsi
    fxsave (%rsi)
    popq %rdi
    pushq %rdi
    cld
    movl $512, %ecx
    rep movsb
    popq %rdi
    ret

fpu_restore:
    pushq %rdi
    call smp_cpu_id
    movslq %eax, %rax
    shlq $9, %rax
    leaq smp_fpu_scratch(%rip), %rdi
    addq %rax, %rdi
    popq %rsi
    cld
    movl $512, %ecx
    rep movsb
    subq $512, %rdi
    fxrstor (%rdi)
    ret
