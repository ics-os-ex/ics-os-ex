/*
  Name: mmap VMA type shared header
  Description:
    The VMA (virtual memory area) type and its constants, shared between the
     memory subsystem (dexmem.h / dexmem.c) and the process control block
     (process.h), which stores a pointer to a lazily-allocated VMA_MAX-slot
     table per process (NULL until the first mmap).

    This header is deliberately kept free of any global variable definitions.
    dexmem.h is a "global-state" header that defines the kernel memory globals
    exactly once per translation unit; it must NOT be pulled into every TU that
    just needs the VMA type (e.g. scheduler.o / posixfd.o via process.h), or
    the header-resident globals would be redefined and fail to link.
 */
#ifndef _DEX_VMA_H_
#define _DEX_VMA_H_

#ifdef __x86_64__
/* ---- mmap VMAs (anonymous + file-backed, see dexmem.c) ----
    A process's VMAs live in a VMA_MAX-slot table referenced by the
    vm_area *vmas pointer in PCB386.  The table is heap-allocated on the first
    mmap and freed on exit/fork-cleanup, so the PCB itself stays small.
    `file` is a file_PCB* (kernel ref held) or NULL for anonymous maps; it is
    typed void* here so this header stays VFS-independent. */
#define VMA_MAX 32
#define VMF_PRIVATE 0x1
#define VMF_SHARED  0x2

typedef struct vm_area {
    unsigned long start;   /* page-aligned base          */
    unsigned long end;     /* page-aligned, exclusive    */
    unsigned long prot;    /* requested prot as PG bits  */
    unsigned long flags;   /* VMF_PRIVATE / VMF_SHARED   */
    void *file;            /* file_PCB* (ref held) / NULL */
    unsigned long foff;    /* file offset backing `start` */
} vm_area;

/* mmap() argument block, passed by the SDK by pointer as the single syscall
   argument (6 values do not fit the 5-register DEX int 0x30 ABI).  The layout
   is shared verbatim with sdk/include/sys/mman.h; keep the two in sync.  All
   fields are pointer-width (unsigned long) so there is no padding ambiguity on
   x86-64.  fd == (unsigned long)-1 selects an anonymous mapping. */
struct mmap_args {
     unsigned long addr;    /* hint, or fixed base when MAP_FIXED */
     unsigned long length;
     unsigned long prot;    /* PROT_* */
     unsigned long flags;   /* MAP_* */
     unsigned long fd;      /* (unsigned long)-1 = anonymous */
     unsigned long offset;  /* file offset */
};
#endif

#endif /* _DEX_VMA_H_ */
