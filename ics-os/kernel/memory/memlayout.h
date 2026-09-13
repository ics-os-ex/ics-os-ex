#ifndef ICSOS_MEMLAYOUT_H
#define ICSOS_MEMLAYOUT_H

/*
 * ICS-OS x86-64 identity-map layout (physical == virtual in the low 4GiB).
 *
 * This is the single source of truth.  Add a region here first; the page
 * allocator (mempop) skips every reserved range automatically.
 *
 *            4GiB identity (2MiB pages)
 *  +------------------+ 0x00000000
 *  | firmware / IVT   |          never allocated
 *  | GDT 0x1000       |
 *  | IDT 0x2000       |
 *  | AP tramp 0x8000  |
 *  | kexec tramp      |
 *  +------------------+ 0x00100000  MEM_KERNEL_LOAD
 *  | kernel ELF       |          linker .text/.data/.bss (grows down)
 *  |  + kstacks in BSS|
 *  |  + guard region  |          64KiB slack before the user ELF (the former
 *  |                  |          256KiB "frame stack" was replaced by the
 *  |                  |          global E820-seeded frame pool in dexmem.c)
 *  +------------------+ 0x00400000  MEM_KERNEL_LIMIT / MEM_USER_ELF_BASE
 *  | user ELF window  |          ELF_START_ADDR; private PTEs (cc1 ~22MB)
 *  +------------------+ 0x01800000  MEM_KEXEC_STAGE
 *  | kexec staging    |          8MiB, not in mempop (kernel image < 4MiB)
 *  +------------------+ 0x02000000  MEM_CPUIRQ_BASE
 *  | per-CPU IRQ stks |          256KiB reserved (32KiB * MAX_CPUS)
 *  +------------------+ 0x02040000  MEM_KHEAP_BASE
 *  | kernel heap      |          sbrk/dlmalloc; identity; ~64MiB cap
 *  +------------------+ 0x06000000
 *  | mempop frames    |          anonymous 4KiB pages (PF, legacy PT)
 *  +------------------+ 0x08000000  MEM_USER_WIN_BASE
 *  | linux_userspace  |
 *  | sharedmem        |
 *  | syscall stack    | 0x09000000
 *  | user heap        | 0x0A000000  grows up
 *  +------------------+ 0x10000000  MEM_LMODE_BASE (identity map only)
 *  ...
 *  | user stack       | 0x3FF00000-0x40000000 grows down (private PD0 VA)
 *  +------------------+ 0x40000000  MEM_USER_VA_END
 *  | lmode / modules  |
 *  +------------------+ 0xFE000000  PCI MMIO (PCD|PWT)
 *
 * Rules:
 *  1. The kernel image (bssEnd) MUST stay <= MEM_KERNEL_BSS_LIMIT (linker
 *     ASSERT), leaving 64KiB of slack below the 4MiB user-ELF base.  TinyCC
 *     user ELFs start at 4MiB.  The frame pool skips the whole
 *     MEM_KERNEL_LOAD..MEM_KERNEL_LIMIT reserved range, so growing the kernel
 *     image into that gap is safe.
 *  2. AP/idle stacks still live in .bss.  IRQ stacks are MEM_CPUIRQ_* so
 *     they can be 16KiB without blowing the 4MiB kernel image cap.
 *  3. Kernel heap is a closed interval; sbrk must not mempop and must not
 *     walk past MEM_KHEAP_END.
 *  4. userpd pool must not overlap any identity range the kernel writes
 *     (heap, image, user windows).
 *  5. New consumers: add a MEM_* range and a mem_reserved[] entry.
 */

#define MEM_PAGE_SHIFT         12
#define MEM_PAGE_SIZE          0x1000UL

/* High canonical direct map of the low 4GiB (see startup.S).  Kernel code
   may dereference physical frames/page tables through this alias even when
   CR3 points at a user PML4 whose low identity mappings are privatized. */
#define KDIRECT_BASE           0xFFFF800000000000ULL
#define KDIRECT(phys)          ((void *)(KDIRECT_BASE | ((phys) & 0xFFFFFFFFULL)))
#define KMMIO_BASE             0xFFFF800100000000ULL
#define KMMIO_SIZE             0x00200000UL

#define MEM_LOW_END            0x00100000UL
#define MEM_GDT                0x00001000UL
#define MEM_IDT                0x00002000UL
#define MEM_AP_TRAMP           0x00008000UL
#define MEM_MB_STASH           0x00009000UL
#define MEM_KEXEC_TRAMP        0x00080000UL
#define MEM_KEXEC_MB2          0x00091000UL

#define MEM_KERNEL_LOAD        0x00100000UL
#define MEM_KERNEL_LIMIT       0x00400000UL   /* TinyCC ELF_START_ADDR */

/* Linker ASSERT bound: bssEnd must stay <= this.  It is MEM_KERNEL_LIMIT
   (the 4MiB user-ELF base) minus a 64KiB guard.  The former 256KiB "frame
   stack" that occupied that gap was replaced by the global E820-seeded frame
   allocator (dexmem.c), which skips the reserved kernel range entirely. */
#define MEM_KERNEL_BSS_LIMIT   0x003F0000UL

#define MEM_USER_ELF_BASE      0x00400000UL
#define MEM_USER_ELF_END       0x01800000UL   /* 20MiB window for large EXEs (cc1 ~22MB) */

#define MEM_KEXEC_STAGE        0x01800000UL
#define MEM_KEXEC_STAGE_SIZE   0x00800000UL   /* 8MiB (kernel image < 4MiB) */
#define MEM_KEXEC_STAGE_END    (MEM_KEXEC_STAGE + MEM_KEXEC_STAGE_SIZE)

/* Per-CPU IRQ stacks live here, not in kernel BSS: 16 KiB * MAX_CPUS would
   fail the 4 MiB user-ELF ASSERT, and BSS-adjacent stacks overflowed into
   the neighbour.  mempop skips this range. */
#define MEM_CPUIRQ_BASE        MEM_KEXEC_STAGE_END
#define MEM_CPUIRQ_SIZE        0x00040000UL   /* 256 KiB: 8 * 16 KiB + slack */
#define MEM_CPUIRQ_END         (MEM_CPUIRQ_BASE + MEM_CPUIRQ_SIZE)
#define MEM_CPUIRQ_STACK       0x8000UL       /* 32 KiB; 8 CPUs fill 256 KiB */

#define MEM_KHEAP_BASE         MEM_CPUIRQ_END
#define MEM_KHEAP_SIZE         (0x04000000UL - MEM_CPUIRQ_SIZE)
#define MEM_KHEAP_END          (MEM_KHEAP_BASE + MEM_KHEAP_SIZE)

/* Former 16MiB kmode slot is now part of the kernel heap.  MEM_KMODE_*
   marks the anonymous mempop-frame gap that follows the heap; it is NOT a
   reserved range, so mempop() still seeds pages from it. */
#define MEM_KMODE_BASE         0x06000000UL
#define MEM_KMODE_END          0x08000000UL

#define MEM_USERPD_BASE        0x08000000UL
#define MEM_USERPD_END         0x08000000UL

#define MEM_SHARED_BASE        0x08000000UL
#define MEM_LINUX_USER_BASE    0x08000000UL
#define MEM_SYSCALL_STACK      0x09000000UL
#define MEM_USER_HEAP          0x0A000000UL
/* User VA space for sbrk/mmap is the private PD0 (0-1GiB).  The stack sits
   at the top of that range so the heap can grow from 0x0A000000 up to nearly
   1GiB instead of colliding with a stack at 0x0E000000.  MEM_USER_WIN_END
   remains the identity-mapped physical window reserved from the frame pool;
   MEM_USER_VA_END is the private user VA limit. */
#define MEM_USER_VA_END        0x40000000UL
#define MEM_USER_STACK         0x40000000UL
#define MEM_USER_STACK_GUARD   (MEM_USER_STACK - 0x300000UL) /* 1MiB commit + 2MiB reserve */
#define MEM_USER_HEAP_LIMIT    MEM_USER_STACK_GUARD
/* sbrk grows up from MEM_USER_HEAP; anonymous mmap grows down from
   MEM_USER_HEAP_LIMIT.  They fail rather than meet. */
#define MEM_USER_WIN_BASE      0x08000000UL
#define MEM_USER_WIN_END       0x10000000UL

#define MEM_LMODE_BASE         0x10000000UL

#define MEM_FREE_SCAN_END      0x08000000UL   /* 128MiB: default QEMU -m */

/* Compile-time overlap checks (gnu89: negative array size on failure). */
typedef char memlayout_kernel_below_userelf[
   (MEM_KERNEL_LIMIT == MEM_USER_ELF_BASE) ? 1 : -1];
typedef char memlayout_kexec_after_elf[
   (MEM_KEXEC_STAGE == MEM_USER_ELF_END) ? 1 : -1];
typedef char memlayout_cpuirq_after_kexec[
   (MEM_CPUIRQ_BASE == MEM_KEXEC_STAGE_END) ? 1 : -1];
typedef char memlayout_heap_after_cpuirq[
   (MEM_KHEAP_BASE == MEM_CPUIRQ_END) ? 1 : -1];
typedef char memlayout_kmode_after_heap[
   (MEM_KMODE_BASE == MEM_KHEAP_END) ? 1 : -1];
typedef char memlayout_userpd_after_kmode[
   (MEM_USERPD_BASE >= MEM_KMODE_END) ? 1 : -1];
typedef char memlayout_userwin_after_pool[
   (MEM_USER_WIN_BASE == MEM_USERPD_END) ? 1 : -1];
typedef char memlayout_shared_not_in_pool[
   (MEM_SHARED_BASE >= MEM_USERPD_END) ? 1 : -1];

int  mem_is_reserved(unsigned long phys);
void mem_layout_dump(void);

#endif
