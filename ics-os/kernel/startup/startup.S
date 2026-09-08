/* Multiboot2 + long-mode bring-up for ICS-OS (x86_64). */
.section .multiboot, "a"
.align 8
mb2_header_start:
    .long 0xE85250D6            /* magic */
    .long 0                     /* arch: i386 */
    .long mb2_header_end - mb2_header_start
    .long -(0xE85250D6 + 0 + (mb2_header_end - mb2_header_start))
    /* framebuffer header tag: request a linear framebuffer (UEFI GOP / VBE)
       so the kernel gets a visible console under firmware without a legacy
       VGA text mode. Bootloader fills an MB2_TAG_FRAMEBUFFER info tag. */
    .word 5                 /* MB2 header tag: framebuffer */
    .word 1                 /* optional */
    .long 20                /* tag size: type(2)+flags(2)+size(4)+w(4)+h(4)+depth(4) */
    .long 1024              /* preferred width */
    .long 768               /* preferred height */
    .long 32                /* color depth */
    .align 8                /* MB2: each tag starts at an 8-byte aligned address */
    /* end tag */
    .word 0
    .word 0
    .long 8
mb2_header_end:

/* Also keep Multiboot1 for QEMU -kernel compatibility on some paths */
.align 4
mb1_header:
    .long 0x1BADB002
    .long 0x00000003
    .long -(0x1BADB002 + 0x00000003)

.section .bss
.align 4096
.globl boot_pml4
boot_pml4:
    .skip 4096
.globl boot_pdpt
boot_pdpt:
    .skip 4096
.globl boot_pd0
boot_pd0:
    .skip 4096
.globl boot_pd1
boot_pd1:
    .skip 4096
.globl boot_pd2
boot_pd2:
    .skip 4096
.globl boot_pd3
boot_pd3:
    .skip 4096
.globl boot_pdpt_high
boot_pdpt_high:
    .skip 4096
.align 16
boot_stack:
    .skip 65536
boot_stack_top:

.data
.globl multiboothdr
multiboothdr:
    .quad 0
.globl multiboot_magic
multiboot_magic:
    .long 0
.align 16
gdt64:
    .quad 0                         /* 0: null */
    .quad 0x00AF9A000000FFFF        /* 8: kernel code 64 */
    .quad 0x00CF92000000FFFF        /* 16: kernel data */
    .quad 0x00AFFA000000FFFF        /* 24: user code 64 */
    .quad 0x00CFF2000000FFFF        /* 32: user data */
    .quad 0                         /* 40: TSS low (filled later) */
    .quad 0                         /* 48: TSS high */
gdt64_end:

gdtr32:
    .word gdt64_end - gdt64 - 1
    .long gdt64

.align 16
.globl gdtr64
gdtr64:
    .word gdt64_end - gdt64 - 1
    .quad gdt64

.code32
.text
.globl startup
.globl _start
.extern main
.extern syscallentry
.extern long_mode_entry

.equ KERNEL_CS64, 0x08
.equ KERNEL_DS,   0x10

_start:
startup_alias:
startup:
    /* Preserve Multiboot EAX/EBX before any clobber (serial uses %al!). */
    movl %eax, 0x9000
    movl %ebx, 0x9004
    /* Early serial 'S' so we know entry ran */
    movw $0x3F8, %dx
    movb $'S', %al
    outb %al, %dx
    jmp multiboot_entry

multiboot_entry:
    movl 0x9000, %eax
    movl 0x9004, %ebx
    movl %eax, multiboot_magic
    movl %ebx, multiboothdr
    movb $'1', 0xb8000
    movb $0x0F, 0xb8001

    /* Build identity map: 0-4GiB with 2MiB pages */
    /* PML4[0] -> PDPT */
    movl $boot_pdpt, %eax
    orl  $0x03, %eax
    movl %eax, boot_pml4

    /* PDPT[0..3] -> PD0..PD3 */
    movl $boot_pd0, %eax
    orl  $0x03, %eax
    movl %eax, boot_pdpt
    movl $boot_pd1, %eax
    orl  $0x03, %eax
    movl %eax, boot_pdpt + 8
    movl $boot_pd2, %eax
    orl  $0x03, %eax
    movl %eax, boot_pdpt + 16
    movl $boot_pd3, %eax
    orl  $0x03, %eax
    movl %eax, boot_pdpt + 24

    /* Fill each PD with 512 x 2MiB pages (present|write|PS) */
    movl $boot_pd0, %edi
    xorl %ebx, %ebx                 /* physical base */
    movl $4, %ecx                   /* 4 page directories */
1:
    pushl %ecx
    movl $512, %ecx
2:
    movl %ebx, %eax
    orl  $0x83, %eax                /* P|RW|PS */
    movl %eax, (%edi)
    movl $0, 4(%edi)
    addl $8, %edi
    addl $0x200000, %ebx
    loop 2b
    popl %ecx
    loop 1b

   /* High canonical direct map: 0xFFFF800000000000 + phys for low 4GiB.
       The kernel uses this alias to touch physical frames/page tables while
       CR3 may point at a user PML4 whose low identity mappings have been
       privatized.  The 64-bit base 0xFFFF800000000000 is the sign-extension
       of 48-bit 0x800000000000, whose PML4 index is 256, not 511. */
    xorl %eax, %eax
    movl $boot_pdpt_high, %edi
    movl $512, %ecx
3:
    movl %eax, (%edi)
    addl $8, %edi
    loop 3b
    movl $boot_pd0, %eax
    orl  $0x03, %eax
    movl %eax, boot_pdpt_high
    movl $boot_pd1, %eax
    orl  $0x03, %eax
    movl %eax, boot_pdpt_high + 8
    movl $boot_pd2, %eax
    orl  $0x03, %eax
    movl %eax, boot_pdpt_high + 16
    movl $boot_pd3, %eax
    orl  $0x03, %eax
    movl %eax, boot_pdpt_high + 24
    movl $boot_pdpt_high, %eax
    orl  $0x03, %eax
    movl %eax, boot_pml4 + 8 * 256

    movb $'2', 0xb8002
    movb $0x0F, 0xb8003

    /* Load early GDT (still 32-bit protected) */
    lgdt gdtr32

    /* Enable PAE */
    movl %cr4, %eax
    orl  $(1 << 5), %eax            /* PAE */
    orl  $(1 << 7), %eax            /* PGE */
    movl %eax, %cr4

    /* Load CR3 with PML4 */
    movl $boot_pml4, %eax
    movl %eax, %cr3

    /* Enable LME in EFER */
    movl $0xC0000080, %ecx
    rdmsr
    orl  $(1 << 8), %eax            /* LME */
    wrmsr

    /* Enable paging (+ protected mode already on from GRUB) */
    movl %cr0, %eax
    orl  $0x80000001, %eax          /* PG | PE */
    movl %eax, %cr0

    movb $'3', 0xb8004
    movb $0x0F, 0xb8005

    ljmp $KERNEL_CS64, $long_mode_start

.code64
.globl long_mode_start
long_mode_start:
    movw $KERNEL_DS, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    lgdt gdtr64

    /* Force 4-level paging. Hosts that expose LA57 may boot the guest with
       CR4.LA57=1; the current kernel page tables are 4-level.  Clear LA57
       before reloading CR3 so the PML4 high direct map (PML4[256]) is used.
       Phase 2 will rebuild these tables as true 5-level for TB-scale RAM. */
    movq %cr4, %rax
    andq $(~(1 << 16)), %rax
    movq %rax, %cr4

    /* Reload CR3 now that LME is active: the same physical PML4 was loaded
       while the CPU was in 32-bit PAE mode.  Reloading makes the CPU treat
       it as a 4-level PML4 and flushes stale 3-level TLB state. */
    movabs $boot_pml4, %rax
    mov %rax, %cr3

    /* SYSCALL MSRs are programmed after the kernel stack is live (see
       below, after zero_bss) so a #GP from WRMSR is diagnosable. */

    movq $boot_stack_top, %rsp
    xorq %rbp, %rbp

    /* Enable SSE / FXSAVE */
    movq %cr0, %rax
    andq $~0x4, %rax                /* clear EM */
    orq  $((1 << 16) | 0x2), %rax   /* set WP | MP */
    movq %rax, %cr0
    movq %cr4, %rax
    orq  $(1 << 9), %rax            /* OSFXSR */
    orq  $(1 << 10), %rax           /* OSXMMEXCPT */
    movq %rax, %cr4

    movb $'4', 0xb8006
    movb $0x0F, 0xb8007

    /* Do NOT disable LAPIC — leave enabled for SMP later */

    /* Zero the C-global BSS region: boot_stack_top .. bssEnd.
       .bss also holds the in-use boot page tables (boot_pml4..boot_pd3,
       CR3 points here) and the boot stack below boot_stack_top; zeroing
       those would destroy paging immediately.  Everything from
       boot_stack_top to bssEnd is NOBITS holding stale RAM, so every
       uninitialized C global (sync locks, flags, PCBs) is garbage and
       boot is non-deterministic (e.g. processmgr_busy.busy != 0
       deadlocks sync_entercrit in ps_scheduler_install). */
    movabs $boot_stack_top, %rsi
    movabs $bssEnd, %rcx
zero_bss:
    movl $0, (%rsi)
    addq $4, %rsi
    cmpq %rcx, %rsi
    jne zero_bss

    /* SYSCALL MSRs: WRMSR uses EDX:EAX. Also set EFER.SCE. */
    movl  $0xC0000080, %ecx
    rdmsr
    orl   $(1 << 0), %eax             /* SCE */
    wrmsr
    xorl  %eax, %eax
    movl  $((KERNEL_CS64 << 16) | KERNEL_CS64), %edx
    movl  $0xC0000081, %ecx
    wrmsr
    movabs $syscallentry, %rax
    movq  %rax, %rdx
    shrq  $32, %rdx
    movl  $0xC0000082, %ecx
    wrmsr
    movl  $0x3000, %eax               /* NT|IOPL — do not clear IF here */
    xorl  %edx, %edx
    movl  $0xC0000084, %ecx
    wrmsr

    call main

.hang:
    cli
    hlt
    jmp .hang

.globl reset_gdtr
reset_gdtr:
    lgdt gdtr64
    ret
