#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#include <stddef.h>

#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_SHARED    1
#define MAP_PRIVATE   2
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_FAILED ((void*)-1)

/* msync() flags.  Only MS_SYNC is meaningful today (the others are accepted
   and ignored); they are defined for source compatibility. */
#define MS_ASYNC      1
#define MS_INVALIDATE 2
#define MS_SYNC       4

/* mmap() argument block, passed to the kernel by pointer as the single syscall
   argument (the six values do not fit the 5-register DEX int 0x30 ABI).  The
   layout is shared verbatim with kernel/memory/dexmem.h; keep the two in sync.
   All fields are pointer-width (unsigned long) so there is no padding ambiguity
   on x86-64.  fd == (unsigned long)-1 selects an anonymous mapping. */
struct mmap_args {
     unsigned long addr;    /* hint, or fixed base when MAP_FIXED */
     unsigned long length;
     unsigned long prot;    /* PROT_* */
     unsigned long flags;   /* MAP_* */
     unsigned long fd;      /* (unsigned long)-1 = anonymous */
     unsigned long offset;  /* file offset */
};

void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset);
int munmap(void *addr, size_t length);
int mprotect(void *addr, size_t len, int prot);
int msync(void *addr, size_t length, int flags);

#endif
