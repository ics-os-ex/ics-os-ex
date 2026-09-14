/* x86_64 SysV assembler helpers for ICS-OS kernel. */
.data
.globl CsrX
.globl CsrY
.globl attb
.globl lock_var
.globl lock_val

CsrX:     .byte 0
CsrY:     .byte 0
attb:     .byte 0
          .byte 0
.align 16
idtr:
    .word 4095
    .quad 0x2000
lock_var: .long 0
lock_val: .long 0

.equ KERNEL_TSS, 0x28

.text

.globl mutex_lock
mutex_lock:
    movl $1, %eax
    lock xchgl %eax, lock_var
    movl %eax, lock_val
    ret

.globl mutex_unlock
mutex_unlock:
    xorl %eax, %eax
    lock xchgl %eax, lock_var
    movl %eax, lock_val
    ret

.globl pci_finddevice
pci_finddevice:
    movl $0xFF, %eax
    ret
.globl pci_findclass
pci_findclass:
    movl $0xFF, %eax
    ret
.globl pci_readconfigbyte
pci_readconfigbyte:
    xorl %eax, %eax
    ret
.globl pci_readconfigword
pci_readconfigword:
    xorl %eax, %eax
    ret
.globl pci_readconfigdword
pci_readconfigdword:
    xorl %eax, %eax
    ret
.globl pci_writeconfigbyte
pci_writeconfigbyte:
    ret
.globl pci_writeconfigword
pci_writeconfigword:
    ret
.globl pci_writeconfigdword
pci_writeconfigdword:
    ret
.globl pcibios_call
pcibios_call:
    ret

.globl invtlb
invtlb:
    invlpg (%rdi)
    ret

/* rdi=port, rsi=buf, rdx=count */
/* PIO helpers — SysV AMD64:
 *   rdi = bufSeg (ignored; flat long mode)
 *   rsi = bufOff (linear buffer address)
 *   rdx = count
 *   rcx = I/O port
 */
.globl repinword
repinword:
    movq %rsi, %rdi          /* dest buffer */
    movl %edx, %r8d          /* save count */
    movw %cx, %dx            /* port */
    movl %r8d, %ecx
    cld
    rep insw
    ret

.globl repindword
repindword:
    movq %rsi, %rdi
    movl %edx, %r8d
    movw %cx, %dx
    movl %r8d, %ecx
    cld
    rep insl
    ret

.globl repoutdword
repoutdword:
    /* rsi already = source buffer */
    movl %edx, %r8d
    movw %cx, %dx
    movl %r8d, %ecx
    cld
    rep outsl
    ret

.globl repoutword
repoutword:
    movl %edx, %r8d
    movw %cx, %dx
    movl %r8d, %ecx
    cld
    rep outsw
    ret

.globl repoutbyte
repoutbyte:
    movl %edx, %r8d
    movw %cx, %dx
    movl %r8d, %ecx
    cld
    rep outsb
    ret

.globl reptrans
reptrans:
    /* byte input from port → buffer (was incorrectly movsb) */
    movq %rsi, %rdi
    movl %edx, %r8d
    movw %cx, %dx
    movl %r8d, %ecx
    cld
    rep insb
    ret

/* void getcpuid(DWORD leaf, DWORD *eax, DWORD *ebx, DWORD *ecx, DWORD *edx)
   SysV: rdi=leaf, rsi=a, rdx=b, rcx=c, r8=d. The 32-bit port passed the
   leaf by value; treating rdi as a pointer wrote CPUID results to
   addresses 0, 1 and 0x80000002 (getcpuid(0)/ (1)/ brand-string). */
.globl getcpuid
getcpuid:
    pushq %rbx
    movq %r8, %r11
    movl %edi, %eax
    movq %rsi, %r8
    movq %rdx, %r9
    movq %rcx, %r10
    xorl %ecx, %ecx
    cpuid
    movl %eax, (%r8)
    movl %ebx, (%r9)
    movl %ecx, (%r10)
    movl %edx, (%r11)
    popq %rbx
    ret

.globl storeflags
storeflags:
    pushfq
    popq %rax
    movl %eax, (%rdi)
    ret

.globl restoreflags
restoreflags:
    /* rdi holds flags value (DWORD passed in register) */
    pushq %rdi
    popfq
    ret

.globl loadtsr
loadtsr:
    /* TSS slot left unused until a 64-bit TSS is installed; no-op */
    ret

.globl switchuserprocess
switchuserprocess:
    ret
.globl switchprocess
switchprocess:
    ret

.globl loadregisters
loadregisters:
    lidt idtr
    ret

/* Switch from the AP trampoline GDT to the kernel gdt64 and reload CS. */
.extern gdtr64
.globl ap_load_kernel_gdt
ap_load_kernel_gdt:
    movabs $gdtr64, %rax
    lgdt (%rax)
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    pushq $0x08                 /* KERNEL_CS64 */
    leaq 1f(%rip), %rax
    pushq %rax
    lretq
1:
    ret

.globl set_idtr
set_idtr:
    lidt (%rdi)
    ret

.globl offcomputer
offcomputer:
    ret

.globl setpagedir
setpagedir:
    movq %rdi, %cr3
    ret

.globl refreshpages
refreshpages:
    movq %cr3, %rax
    movq %rax, %cr3
    ret

.globl disablepaging
disablepaging:
    ret

.globl enablepaging
enablepaging:
    ret

.globl getCR0
getCR0:
    movq %cr0, %rax
    ret

.globl setCR0
setCR0:
    movq %rdi, %cr0
    ret

.globl getCR3
getCR3:
    movq %cr3, %rax
    ret

.globl getCR4
getCR4:
    movq %cr4, %rax
    ret

.globl setCR4
setCR4:
    movq %rdi, %cr4
    ret

.globl itoa
itoa:
    pushq %rbx
    movq %rsi, %r8
    movl %edi, %eax
    movl %edx, %ebx
    testl %eax, %eax
    jnz 1f
    movb $'0', (%r8)
    movb $0, 1(%r8)
    movq %r8, %rax
    popq %rbx
    ret
1:
    movl %eax, %r9d
    cmpl $10, %ebx
    jne 2f
    testl %eax, %eax
    jns 2f
    negl %eax
2:
    xorl %ecx, %ecx
3:
    xorl %edx, %edx
    divl %ebx
    addb $'0', %dl
    cmpb $'9', %dl
    jbe 4f
    addb $39, %dl
4:
    pushq %rdx
    incl %ecx
    testl %eax, %eax
    jnz 3b
    cmpl $10, %ebx
    jne 5f
    testl %r9d, %r9d
    jns 5f
    movb $'-', (%r8)
    incq %r8
5:
6:
    popq %rax
    movb %al, (%r8)
    incq %r8
    loop 6b
    movb $0, (%r8)
    movq %rsi, %rax
    popq %rbx
    ret

.globl gotoxy
gotoxy:
    movb %dil, CsrX
    movb %sil, CsrY
    ret

/* void move_cursor(unsigned char row, unsigned int col). rbx is
   callee-saved; the old sequence clobbered it and ignored the args. */
.globl move_cursor
move_cursor:
    pushq %rbx
    movzbl %dil, %eax
    imull $80, %eax
    addl %esi, %eax
    movl %eax, %ebx
    movw $0x3D4, %dx
    movb $0x0F, %al
    outb %al, %dx
    movw $0x3D5, %dx
    movb %bl, %al
    outb %al, %dx
    movw $0x3D4, %dx
    movb $0x0E, %al
    outb %al, %dx
    movw $0x3D5, %dx
    movb %bh, %al
    outb %al, %dx
    popq %rbx
    ret

/* COM1/COM2 TX using only GPRs. C uart_putc_raw reloaded uart_dev* from the
   user stack after inportb(); that slot was 0xfb002398a0 and #PF'd in
   kernel text, then the PF handler's serial_puts re-entered and halted. */
.globl uart_com1_putc
uart_com1_putc:
    movl $0x3FD, %edx
    movl $100000, %ecx
1:
    inb %dx, %al
    testb $0x20, %al
    jnz 2f
    decl %ecx
    jz 3f
    pause
    jmp 1b
2:
    movl $0x3F8, %edx
    movl %edi, %eax
    outb %al, %dx
3:
    ret

.globl uart_com2_putc
uart_com2_putc:
    movl $0x2FD, %edx
    movl $100000, %ecx
1:
    inb %dx, %al
    testb $0x20, %al
    jnz 2f
    decl %ecx
    jz 3f
    pause
    jmp 1b
2:
    movl $0x2F8, %edx
    movl %edi, %eax
    outb %al, %dx
3:
    ret
