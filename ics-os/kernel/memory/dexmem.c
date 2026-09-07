/*
  Name: dex low-level memory management library
  Copyright: 
  Author: Joseph Emmanuel DL Dayo
  Date: 02/03/04 18:06
  Description: This module handles everything that has to do
  with memory, except the high-level memory functions like malloc....
 */

#include "../cpu/spinlock.h"
#include "../cpu/smp.h"

/* Stop Interrupts */
inline void stopints(){
  asm ("cli");
}

/*start Interrupts */

inline void startints(){
  asm("sti");
};

inline void wbinvd(){
  asm ("wbinvd");
};

inline void hlt(){
  asm ("hlt");
};

extern void refreshpages();

/*Displays memory information provided by the GRUB bootloader*/
void mem_interpretmemory(mmap *map,int size){
   int i;
   for (i=0; i <(size/sizeof(mmap)); i++){
      DWORD base = map[i].base_addr_low;
      DWORD base_end = base + map[i].length_low;
      printf("region: 0x%X - 0x%X . ", base, base_end);
      if (map[i].type==1) 
         printf("FREE\n");
      else
         printf("RESERVED.\n");
   }; 
};

/*using the memory map provided by grub, create the stack of physical frames.
  On modern machines the map can describe many gigabytes; a 32-bit kernel can
  only use the first 4GB and we cap usable RAM at 256MB so the free-page stack
  cannot overrun the identity-mapped region. The stack itself sits just after
  kernel BSS — a fixed 0x200000 address overlaps .bss once the kernel grows
  (virtio-blk, userpd meta, ...). */
#define MEM_MIN_FRAME   0x00300000
#define MEM_MAX_PHYS    0x10000000

/* The fixed 32MiB userpd pool is gone.  Every 4KiB physical frame now comes
   from the global, scalable frame allocator below (seeded from the Multiboot2
   E820 map), so user processes can use all available RAM.  The MEM_USERPD_*
   constants in memlayout.h are kept only for documentation. */

#ifdef __x86_64__
typedef struct {
   unsigned long base;
   unsigned long end;
   const char *name;
} mem_range;

/* Every range the page allocator must never hand out. */
static const mem_range mem_reserved[] = {
   { 0,                    MEM_LOW_END,         "low/firmware" },
   { MEM_KERNEL_LOAD,      MEM_KERNEL_LIMIT,    "kernel+framestack" },
   { MEM_USER_ELF_BASE,    MEM_USER_ELF_END,    "user-elf" },
   { MEM_KEXEC_STAGE,      MEM_KEXEC_STAGE_END, "kexec-stage" },
   { MEM_KHEAP_BASE,       MEM_KHEAP_END,       "kheap" },
    { MEM_USER_WIN_BASE,    MEM_USER_WIN_END,    "user-windows" },
};

int mem_is_reserved(unsigned long phys)
{
   unsigned i;
   for (i = 0; i < sizeof(mem_reserved) / sizeof(mem_reserved[0]); i++) {
      if (phys >= mem_reserved[i].base && phys < mem_reserved[i].end)
         return 1;
   }
   return 0;
}

extern void serial_puts(const char *s);
extern int printf(const char *fmt, ...);

/* ---------------------------------------------------------------------------
 * Global physical frame allocator (x86-64).
 *
 * Replaces the fixed 32MiB userpd pool and the bssEnd "frame stack".  It is an
 * in-place LIFO free list: each free 4KiB frame stores the physical address of
 * the next free frame in its first 8 bytes (reachable through the identity
 * map).  It is seeded once from the Multiboot2 E820 map (frame_init) and then
 * scales to the full amount of installed RAM.
 *
 * Phase 1 (this change) only seeds frames below 4GiB, which the identity map
 * always covers.  Phase 2 will extend the direct map so frames above 4GiB can
 * also be seeded for terabyte-scale machines.
 * ------------------------------------------------------------------------- */
static u64 frame_head;          /* phys addr of next free frame, 0 = empty  */
static u64 frame_total;         /* total frames seeded from E820            */
static u64 frame_free;          /* frames currently in the free list        */
static int frame_ready;
static spinlock_t frame_lock;
static spinlock_t userpd_lock;
static u64 cow_faults;
static u64 cow_copies;
static u64 cow_fastpaths;
static u64 cow_shootdowns;
static u64 cow_oom;
static unsigned cow_trace_count;
static unsigned cow_fault_trace_count;
static volatile int cow_fail_next;

/* Allocation tracker for double-free / double-alloc diagnostics.
   Bit N is set iff frame N (physical addr N<<12) is currently allocated.
   Phase 1 frames are < 4GiB, so this is 4GiB/4KiB/8 = 128KiB of BSS. */
static u8 frame_allocmap[0x100000000ull / 0x1000 / 8];
static u8 frame_refs[0x100000000ull / 0x1000];
static int frame_dbg;           /* enable alloc/free anomaly reporting      */

static void frame_report(const char *tag, u64 phys)
{
   char buf[64];
   sprintf(buf, "FRAME %s phys=0x%llx free=%llu total=%llu\n",
           tag, (unsigned long long)phys,
           (unsigned long long)frame_free,
           (unsigned long long)frame_total);
   serial_puts(buf);
}

/* Pop one free frame.  Returns the physical address, or 0 if exhausted. */
u64 frame_alloc(void)
{
   u64 head;
   DWORD flags;
   if (!frame_ready)
      return 0;
   storeflags(&flags);
   stopints();
   spin_lock(&frame_lock);
   head = frame_head;
    if (head) {
       u64 next = *(volatile u64 *)KDIRECT(head);
       u64 idx = head >> 12;
      frame_head = next;
      frame_free--;
      if (frame_allocmap[idx >> 3] & (1u << (idx & 7))) {
         if (!frame_dbg) { frame_dbg = 1; }
         frame_report("DBL-ALLOC", head);
      }
      frame_allocmap[idx >> 3] |= (1u << (idx & 7));
      frame_refs[idx] = 1;
   }
   spin_unlock(&frame_lock);
   restoreflags(flags);
   return head;
}

/* Acquire another mapping reference. Returns 0 for invalid/free frames or
   when the bounded reference count would overflow. */
int frame_retain(u64 phys)
{
   u64 idx;
   DWORD flags;
   int retained = 0;

   if (!frame_ready || phys == 0 || (phys & 0xFFFULL))
      return 0;
   idx = phys >> 12;
   if (idx >= sizeof(frame_refs))
      return 0;
   storeflags(&flags);
   stopints();
   spin_lock(&frame_lock);
   if ((frame_allocmap[idx >> 3] & (1u << (idx & 7)))
       && frame_refs[idx] != 0 && frame_refs[idx] != 0xFF) {
      frame_refs[idx]++;
      retained = 1;
   } else {
      frame_report(frame_refs[idx] == 0xFF ? "REF-OVERFLOW" : "RETAIN-FREE",
                   phys);
   }
   spin_unlock(&frame_lock);
   restoreflags(flags);
   return retained;
}

unsigned frame_refcount(u64 phys)
{
   u64 idx = phys >> 12;
   DWORD flags;
   unsigned refs = 0;

   if (!frame_ready || phys == 0 || (phys & 0xFFFULL)
       || idx >= sizeof(frame_refs))
      return 0;
   storeflags(&flags);
   stopints();
   spin_lock(&frame_lock);
   refs = frame_refs[idx];
   spin_unlock(&frame_lock);
   restoreflags(flags);
   return refs;
}

/* Drop one frame reference and return it to the pool after the last owner. */
void frame_release(u64 phys)
{
   DWORD flags;
   if (!frame_ready || phys == 0)
      return;
   if (mem_is_reserved((unsigned long)phys))
      return;
   storeflags(&flags);
   stopints();
   spin_lock(&frame_lock);
   {
      u64 idx = phys >> 12;
      if (!(frame_allocmap[idx >> 3] & (1u << (idx & 7)))
          || frame_refs[idx] == 0) {
         if (!frame_dbg) { frame_dbg = 1; }
         frame_report("DBL-FREE", phys);
         spin_unlock(&frame_lock);
         restoreflags(flags);
         return;
      }
      frame_refs[idx]--;
      if (frame_refs[idx] != 0) {
         spin_unlock(&frame_lock);
         restoreflags(flags);
         return;
      }
      frame_allocmap[idx >> 3] &= ~(1u << (idx & 7));
    }
    *(volatile u64 *)KDIRECT(phys) = frame_head;
    frame_head = phys;
   frame_free++;
   spin_unlock(&frame_lock);
   restoreflags(flags);
}

/* Diagnostic: report the allocation state of a physical frame.  Used by the
   CTXCANARY path to decide whether a corrupted user-stack frame is still
   allocated (direct write to a live frame) or has been freed (freed-while-
   mapped lifecycle bug). */
void frame_diag_report(const char *tag, u64 phys)
{
   u64 idx;
   char buf[160];

   if (!frame_ready) {
      serial_puts("FRDIAG !ready\n");
      return;
   }
   if (phys == 0 || (phys & 0xFFFULL)) {
      sprintf(buf, "FRDIAG %s phys=0x%llx UNMAP\n", tag,
              (unsigned long long)phys);
      serial_puts(buf);
      return;
   }
   idx = phys >> 12;
   if (idx >= sizeof(frame_refs)) {
      sprintf(buf, "FRDIAG %s phys=0x%llx OOB idx=%llu\n", tag,
              (unsigned long long)phys, (unsigned long long)idx);
      serial_puts(buf);
      return;
   }
   {
      int alloc = (frame_allocmap[idx >> 3] & (1u << (idx & 7))) ? 1 : 0;
      int inhead = (frame_head == phys) ? 1 : 0;
      sprintf(buf,
              "FRDIAG %s phys=0x%llx idx=%llu refs=%u alloc=%d ishead=%d free=%llu total=%llu\n",
              tag, (unsigned long long)phys, (unsigned long long)idx,
              (unsigned)frame_refs[idx], alloc, inhead,
              (unsigned long long)frame_free,
              (unsigned long long)frame_total);
      serial_puts(buf);
   }
}

/* Cheap, lock-free ring of recently freed PML4s for post-mortem correlation.
   userpd_free() runs in several lock contexts (kill_process holds
   processmgr_busy; frame releases run IRQ-off), so we must NOT do serial
   I/O there.  Record the page-aligned PML4 and let a safe context (the
   CTXCANARY handler) inspect the ring later. */
#define FREED_PML4_RING_N 128u
static u64 freed_pml4_ring[FREED_PML4_RING_N];
static volatile unsigned long freed_pml4_head;

static void freed_pml4_record(u64 pml4)
{
    unsigned long idx = __sync_fetch_and_add(&freed_pml4_head, 1);
    freed_pml4_ring[idx & (FREED_PML4_RING_N - 1u)] = pml4;
}

unsigned long freed_pml4_count(void)
{
    unsigned long n = freed_pml4_head;
    return (n < FREED_PML4_RING_N) ? n : FREED_PML4_RING_N;
}

int freed_pml4_contains(u64 pml4)
{
    unsigned long n = freed_pml4_head;
    unsigned long lim = (n < FREED_PML4_RING_N) ? n : FREED_PML4_RING_N;
    unsigned long i;
    u64 want = pml4 & 0x000FFFFFFFFF000ULL;
    for (i = 0; i < lim; i++)
        if (freed_pml4_ring[i] == want)
            return 1;
    return 0;
}

u64 frame_free_count(void) { return frame_free; }
u64 frame_total_count(void) { return frame_total; }

/* Seed the free list from the E820 map (type 1 = RAM). */
static void frame_init(mmap *map, int size)
{
   unsigned char *cursor, *end;
   frame_head = 0;
   frame_total = 0;
   frame_free = 0;
   frame_dbg = 0;
   memset(frame_refs, 0, sizeof(frame_refs));
   spin_init(&frame_lock);
   spin_init(&userpd_lock);
   cow_faults = cow_copies = cow_fastpaths = 0;
   cow_shootdowns = cow_oom = 0;
   cow_trace_count = 0;
   cow_fault_trace_count = 0;
   cow_fail_next = 0;
  frame_ready = 0;
   if (map == 0 || size <= 0)
        return;
    cursor = (unsigned char *)map;
   end = cursor + size;
   while (cursor + sizeof(mmap) <= end) {
      mmap *entry = (mmap *)cursor;
      u64 raw_base, raw_len, base, base_end, page;
      u64 cap = 0x100000000ull;   /* Phase 1: identity map covers low 4GiB */
      if (entry->size == 0)
         break;
      if (entry->type == 1) {
         raw_base = (u64)entry->base_addr_low | ((u64)entry->base_addr_high << 32);
         raw_len  = (u64)entry->length_low    | ((u64)entry->length_high    << 32);
         base     = (raw_base + 0xFFF) & ~0xFFFull;   /* first full page  */
         base_end = (raw_base + raw_len) & ~0xFFFull; /* last full page   */
         if (base_end > cap)
            base_end = cap;
         for (page = base; page + 0x1000 <= base_end && page < base_end;
              page += 0x1000) {
           if (mem_is_reserved((unsigned long)page))
                continue;
             /* push frame to the head (highest addresses allocated first) */
             *(volatile u64 *)KDIRECT(page) = frame_head;
             frame_head = page;
            frame_total++;
            frame_free++;
         }
      }
      cursor += entry->size + 4;
   }
   frame_ready = 1;
}

void mem_layout_dump(void)
{
   unsigned i;
   extern char bssEnd[];
   printf("MEM layout (identity, phys==virt):\n");
   printf("  kernel  %p .. bssEnd=%p  limit=%p\n",
          (void *)MEM_KERNEL_LOAD, (void *)&bssEnd, (void *)MEM_KERNEL_LIMIT);
   printf("  kheap   %p-%p  userwin %p-%p\n",
          (void *)MEM_KHEAP_BASE, (void *)MEM_KHEAP_END,
          (void *)MEM_USER_WIN_BASE, (void *)MEM_USER_WIN_END);
   printf("  frame pool: %llu free / %llu total\n",
          (unsigned long long)frame_free, (unsigned long long)frame_total);
   for (i = 0; i < sizeof(mem_reserved) / sizeof(mem_reserved[0]); i++)
      printf("  reserve %s  %p-%p\n", mem_reserved[i].name,
             (void *)mem_reserved[i].base, (void *)mem_reserved[i].end);
}
#else
int mem_is_reserved(unsigned long phys) { (void)phys; return 0; }
void mem_layout_dump(void) {}
#endif

DWORD mem_detectmemory(mmap *grub_meminfo , int size ){
   DWORD mem_size = 0;
   unsigned char *cursor;
   unsigned char *end;
  #ifdef __x86_64__
   {
      /* Seed the global frame allocator from the real E820 map.  The
         identity-mapped low 4GiB is always available; Phase 2 extends the
         direct map so frames above 4GiB can also be used for TB-scale. */
      frame_init(grub_meminfo, size);
      totalpages = (DWORD)frame_total;
      return (DWORD)(frame_total * 0x1000ull);
   }
   #else
   {
      volatile DWORD *fps = (volatile DWORD *)0x200000UL;
      stackbase = (DWORD *)0x200000UL;
      fps[0] = 0;
      if (grub_meminfo == 0 || size <= 0)
         return 0;
      cursor = (unsigned char*)grub_meminfo;
      end = cursor + size;
   }
   #endif

   while (cursor + sizeof(mmap) <= end){
      mmap *entry = (mmap*)cursor;
      DWORD base, length, base_end, page;

      if (entry->size == 0)
         break;

      /* Skip regions above 4GB (base_addr_high) and reserved RAM. */
      if (entry->type == 1 && entry->base_addr_high == 0){
         base = entry->base_addr_low;
         length = entry->length_low;

         if (base >= MEM_MAX_PHYS)
            length = 0;
         else if (length > MEM_MAX_PHYS - base)
            length = MEM_MAX_PHYS - base;

         if (length){
            base_end = base + length;
            for (page = base; page < base_end; page += 0x1000){
               if (page >= MEM_MIN_FRAME && page + 0x1000 > page){
                  stackbase[0]++;
                  stackbase[stackbase[0]] = page;
               }
            }
            mem_size += length;
         }
      }

      cursor += entry->size + 4;
   }

   totalpages = stackbase[0];
   return mem_size;
};



DWORD *mempop(){
#ifdef __x86_64__
   /* Global frame allocator: returns a physical 4KiB frame (< 4GiB in Phase 1). */
   u64 phys = frame_alloc();
   return (DWORD *)(uintptr)phys;
#else
   DWORD *ret;
   if (stackbase[0]==0)
      return 0; //no more free pages available!
   ret=(DWORD*)stackbase[stackbase[0]];
   stackbase[0]--;
   return ret;
#endif
};

void mempush(DWORD mem){
#ifdef __x86_64__
   /* Return the frame to the global pool.  (Phase 1 frames are < 4GiB, so a
      DWORD physical address is sufficient; Phase 2 will widen this to u64.) */
   frame_release((u64)(unsigned)mem);
#else
   //make sure that the physcial memory location pushed is within
   //the range of the computer's physical memory
   if (stackbase+0x100000>=mem<memamount-0x1000){
      stackbase[0]++;
      stackbase[stackbase[0]]=mem;
   }else{
      char temp[255];
      printf("memory manager: An invalid value (%s) was tried to be added to the\n");
      printf("memory manager: free physical pages list.\n");
   };
#endif
};

void clearpagetable(DWORD *pagetable){
   DWORD i;
   for (i=0;i<4096;i++)
      pagetable[i]=0;
};

/* This function maps the linear address to a physical address:
   The function automatically allocates a page for a page table
   the bottom 12 bits of the linear address is discarded since
   it is not used by the mapper*/
void maplineartophysical(unsigned int *pagedir, /*the location of the page directory*/
                              unsigned int linearaddr, /*the paged aligned linear address requested*/
                              unsigned int physical,   /*the paged aligned physical address to map to*/
									 unsigned int attribute /*used to specify the page attributes to be applied*/
					    ){
#ifdef __x86_64__
   /* First 4GiB are identity-mapped with 2MiB pages at boot. */
   (void)pagedir; (void)linearaddr; (void)physical; (void)attribute;
   return;
#else
   unsigned int pagedirindex,pagetableindex,*pagetable;
   /*get the index of the page directory and the pagetable respectively*/
   pagedirindex= linearaddr >> 22;
   pagetableindex= (linearaddr  & 0x3FFFFF) >> 12;
   /*get the location of the page table and mask the first 12 bits*/
   pagetable=(unsigned int*)(pagedir[pagedirindex] & 0xFFFFF000);
   if (pagetable==0){  /*there is no entry?*/
      /*map a new memory location*/
      pagedir[pagedirindex]=(DWORD) mempop();
      pagetable=(unsigned int*)pagedir[pagedirindex];
      /*clear the locations of the page table to zero*/
      memset(pagetable,0,4096);
      /*set the present bit of the pagetable dir entry*/
      pagedir[pagedirindex]=pagedir[pagedirindex] | 1 | PG_USER | PG_WR;
   };
   physical=(physical & 0xFFFFF000) | attribute;
   pagetable[pagetableindex]=physical;
      /*done!*/
#endif
};

DWORD tlb_address;
extern void invtlb();

/*same as map linear to physical except that it does its tricks without
 modifying the paging bit   */
int maplineartophysical2(unsigned int *pagedir, /*the location of the page directory*/
                              unsigned int linearaddr, /*the paged aligned linear address requested*/
                              unsigned int physical,   /*the paged aligned physical address to map to*/
									 unsigned int attribute /*used to specify the page attributes to be applied*/
					    ){
#ifdef __x86_64__
   /* Boot identity-maps the low 4GiB with 4-level tables. The legacy
      2-level walker below would corrupt PML4 — treat as already mapped. */
   (void)pagedir;
   (void)linearaddr;
   (void)physical;
   (void)attribute;
   return 0;
#else
   unsigned int pagedirindex,pagetableindex,*pagetable;
   DWORD pg;
   DWORD *kicker=(DWORD*)SYS_PAGEDIR2_VIR;
      
	/*get the index of the page directory and the pagetable respectively*/
	pagedirindex= linearaddr >> 22;
	pagetableindex= (linearaddr  & 0x3FFFFF) >> 12;
	  
	/*get the location of the page table and mask the first 12 bits*/
	pg=(pagedir[pagedirindex] & 0xFFFFF000);
	  
	if (pg==0){  /*there is no entry?*/
      /*map a new memory location*/
      pagedir[pagedirindex]=(DWORD) mempop();
      kicker[4]=(pagedir[pagedirindex]&0xFFFFF000) | 1;
                
      refreshpages();
                
      pagetable=(DWORD*)SYS_PAGEDIR4_VIR;

      tlb_address = pagetable;
      invtlb();                
				
      /*clear the locations of the page table to zero*/
		memset(pagetable,0,4096);
		/*set the present bit of the pagetable dir entry*/
		pagedir[pagedirindex] = pagedir[pagedirindex] | 1 | PG_USER | PG_WR;
		refreshpages();
				
		pg = (pagedir[pagedirindex] & 0xFFFFF000);
   };
             
   kicker[4]=pg | 1;
   refreshpages();
        

   pagetable=(DWORD*)SYS_PAGEDIR4_VIR;

   physical = (physical & 0xFFFFF000) | attribute;
   pagetable[pagetableindex]=physical;

   refreshpages();
    
   tlb_address = linearaddr;
   invtlb();
    
   /*done!*/
   return 0;
#endif
};


//quickly gives a physical address a corresponding virtual address
//--used for modifying page tables without disabling paging
DWORD getvirtaddress(DWORD physicaladdr){
#ifdef __x86_64__
   /* Boot identity-maps the first 4GiB; physical == virtual. */
   return physicaladdr;
#else
   DWORD *kicker=(DWORD*)SYS_PAGEDIR2_VIR; //obtain the aux pagetable
   kicker[2]=physicaladdr | 1;
   refreshpages();
   return SYS_PAGEDIR3_VIR;
#endif
};

DWORD getvirtaddress2(DWORD physicaladdr,DWORD hdl){
#ifdef __x86_64__
   (void)hdl;
   return physicaladdr;
#else
   DWORD *kicker=(DWORD*)SYS_PAGEDIR2_VIR; //obtain the aux pagetable
   kicker[hdl]=physicaladdr | 1;
   refreshpages();
   return SYS_PAGEDIR_VIR+hdl;
#endif
};


DWORD xmaplineartophysical(const DWORD linearmemory,const DWORD physicalmemory,
                              DWORD *pagedir,const DWORD attb){ //ATOMIC
   DWORD w=linearmemory;
   DWORD dirindex=(w&0xFFC00000) >> 22;
   DWORD pageindex=(w&0x3FF000) >> 12;
   DWORD *pagetbl;
   DWORD flags;
   dex32_stopints(&flags);
   disablepaging();
   if (pagedir[dirindex]&PG_PRESENT==0){ //no page table allocated?
      pagetbl=mempop();
      if (pagetbl==0) {
         dex32_restoreints(flags);
         enablepaging();
         return 0;
      };
      pagedir[dirindex]=(DWORD)pagetbl | 1;
   };
        
   if (pagedir[dirindex]&1){
      pagetbl=(DWORD*)(pagedir[dirindex]&0xFFFFF000);
      pagetbl[pageindex]=physicalmemory | (attb&0xFFF);
       
      dex32_restoreints(flags);
      enablepaging();
      return 1;
   }
      
   dex32_restoreints(flags);
   enablepaging();
   return 0;
};

int ints_enabled=1;

void dex32_stopints(DWORD *flags){
   storeflags(flags);
   stopints();
};

void dex32_restoreints(DWORD flags){
   restoreflags(flags);
};


DWORD getphys(DWORD vaddr,DWORD *pagedir){
#ifdef __x86_64__
   /* Walk the real 4-level (or 2MiB-PS) tables. The old stub always
      returned identity|present, so a genuine not-present user PF was
      treated as a protection fault; the dump path then faulted under
      the user CR3 and double-faulted. */
   unsigned long long va = (unsigned long long)(unsigned)vaddr;
   u64 *pml4, *pdpt, *pd, *pte;
   u64 pe, de, be, e;
   int pmi, pi, bi, gi;

   if (!pagedir)
      return 0;
   pml4 = (u64 *)pagedir;
   pmi = (int)((va >> 39) & 0x1FF);
   pe = pml4[pmi];
   if (!(pe & 1))
      return 0;
   if (pe & 0x80)
      return (DWORD)((pe & 0x000FFFFFFFFF000ULL) | (va & 0x7FFFFFFFFFULL) | (pe & 0xFFFULL));
   pdpt = (u64 *)(pe & 0x000FFFFFFFFF000ULL);
   pi = (int)((va >> 30) & 0x1FF);
   de = pdpt[pi];
   if (!(de & 1))
      return 0;
   if (de & 0x80)
      return (DWORD)((de & 0x000FFFFFFFFF000ULL) | (va & 0x3FFFFFFFULL) | (de & 0xFFFULL));
   pd = (u64 *)(de & 0x000FFFFFFFFF000ULL);
   bi = (int)((va >> 21) & 0x1FF);
   be = pd[bi];
   if (!(be & 1))
      return 0;
   if (be & 0x80)
      return (DWORD)((be & 0x000FFFFFFFE00000ULL) | (va & 0x1FFFFFULL) | (be & 0xFFFULL));
   pte = (u64 *)(be & 0x000FFFFFFFFF000ULL);
   gi = (int)((va >> 12) & 0x1FF);
   e = pte[gi];
   if (!(e & 1))
      return 0;
   return (DWORD)((e & 0x000FFFFFFFFF000ULL) | (e & 0xFFFULL));
#else
   DWORD dirindex=(vaddr&0xFFC00000) >> 22;
   DWORD pageindex=(vaddr&0x3FF000) >> 12;
   DWORD *pagetbl;
   DWORD ret;
   DWORD *pg;

   pg=(DWORD*)getvirtaddress((DWORD)pagedir);
   if (pg[dirindex]&1==0)
      return 0;
   pagetbl=(DWORD*)(pg[dirindex]&0xFFFFF000);
   pg=(DWORD*)getvirtaddress((DWORD)pagetbl);
   ret=pg[pageindex];
   return ret;
#endif
};

DWORD getpagetablephys(DWORD vaddr,DWORD *pagedir){
   DWORD dirindex=(vaddr&0xFFC00000) >> 22;
   DWORD *pg;
   pg=(DWORD*)getvirtaddress((DWORD)pagedir);
   return pg[dirindex];
};

void dex32_freeuserpagetable(DWORD *pgd){  //ATOMIC function
   /* Process page directories may drop everything above 4MB (userspace).
      The kernel page directory (pagedir1) identity-maps 0-16MB for the
      kernel image and heap. ELF/PE loaders dual-map user programs into
      pagedir1 then call this; they must not free PDE 1-3 (4-16MB) or the
      next disk read / malloc above 4MB hangs. */
   DWORD userstart, userend  =0xC0000000 >> 22;
   DWORD auxstart=0xFFC00000 >> 22;
   DWORD *pagedir,cpuflags;
   DWORD *pagetbl,address;
   DWORD i;
   DWORD pages=0;
   storeflags(&cpuflags);
   stopints();

   if (pgd == pagedir1)
      userstart = 0x01000000 >> 22; /* preserve 0-16MB identity map */
   else
      userstart = (DWORD)userspace >> 22;
     
   pagedir =(DWORD*)getvirtaddress((DWORD)pgd);
   for (i=userstart;i<userend;i++)
      if (pagedir[i]&1){ //check if present
         mempush(pagedir[i]&0xFFFFF000);
         pages++;
         pagedir[i]=0;
      };
             
   if (pagedir[auxstart]&1&&(pgd!=pagedir1)){
      mempush(pagedir[auxstart]&0xFFFFF000);
      pages++;
      pagedir[auxstart]=0;
   };
   restoreflags(cpuflags);
   #ifdef MEM_LEAK_CHECK
   printf("freeuserpagetable() frees %d pages.\n",pages);
   #endif
};


void freeuserheap(DWORD *pagedir){
   DWORD start=0xA0000000,end=0xB0000000;
   DWORD i;
   for (i=start;i<end;i++){
      DWORD w=getphys(i,pagedir);
      if (w&1){
         mempush(w&0xFFFFF000);
         maplineartophysical2(pagedir,i,0,0);
      };
   };
};


void freelinearloc(void *linearmemory,DWORD *pagedir){  //ATOMIC function
   DWORD w=(DWORD)linearmemory;
   DWORD dirindex=(w&0xFFC00000) >> 22;
   DWORD pageindex=(w&0x3FFFFF) >> 12;
   DWORD *pagetbl,address,flags;
   char temp[255];

   dex32_stopints(&flags); 
   // disablepaging();
   address = getphys(linearmemory,pagedir);
   /* pagetbl=(DWORD*)(pagedir[dirindex]&0xFFFFF000);
   address=pagetbl[pageindex];*/
#ifdef __x86_64__
    /* No-op: with the global frame allocator, user frames (PML4/PDPT/PD/PTE
       tables and heap pages) are reclaimed by userpd_free()'s PML4 walk at
       process exit.  Freeing them here as well would double-free and corrupt
       the free list.  The 32-bit path below still returns frames to the
       fixed frame stack. */
    (void)dirindex;
    (void)pageindex;
    (void)pagetbl;
    (void)temp;
    (void)address;
 #else
    if (address&1)
       mempush(address&0xFFFFF000);
 #endif
   //pagetbl[pageindex]=0;
   // enablepaging();
   dex32_restoreints(flags);
};

//frees multiple pages and returns them back to the stack
void freemultiple(void *linearmemory,DWORD *pagedir,DWORD pages)
  {
  int i;
  #ifdef MEM_LEAK_CHECK
  printf("freemultiple() frees %d pages.\n",pages);
  #endif
  for (i=0;i<pages;i++)
    {
      freelinearloc(linearmemory,pagedir);
      linearmemory=(void*)(linearmemory+0x1000);
    };
  };

DWORD getlinearloc(void *linearmemory,DWORD *pagedir)  //ATOMIC function
   { 
     DWORD w=(DWORD)linearmemory,ret=0,*pg;
     DWORD dirindex=(w&0xFFC00000) >> 22;
     DWORD pageindex=(w&0x3FFFFF) >> 12;
     DWORD *pagetbl,address,flags;
     char temp[255];

     dex32_stopints(&flags);
     pg=(DWORD*)getvirtaddress((DWORD)pagedir); //convert to a virtual address so that
               
     pagetbl=(DWORD*)(pg[dirindex]&0xFFFFF000);
     if (pagetbl!=0)
     {
          pg=(DWORD*)getvirtaddress((DWORD)pagetbl); //convert to a virtual address so that
          address=pg[pageindex];
          if (address&1)
          ret = 1;
     }
     else
     ret =0;    
     
     dex32_restoreints(flags);
     return ret;
   };


//Determines the amount of physical pages commited based on a
//given linear memory
DWORD getmultiple(void *linearmemory,DWORD *pagedir,DWORD pages)
  {
  int i;
  DWORD total=0;
  
  for (i=0;i<pages;i++)
    {
      total+=getlinearloc(linearmemory,pagedir);
      linearmemory=(void*)(linearmemory+0x1000);
    };
    return total;
  };


void setpageattb(DWORD *pagedir,DWORD vaddr,DWORD attb)
  {
    DWORD pg,phys;
    phys=getphys(vaddr,pagedir);
    pg=(DWORD)getvirtaddress((DWORD)pagedir);

    maplineartophysical2(pg,vaddr,
    phys,attb);
          //   printf("ok\n");

  };


void *dex32_setpageattb(DWORD virtualaddr,DWORD pages,DWORD *pagedir,DWORD pattb)
    {
     int i;
     char temp[255],temp2[255];
     void *ret=(void*)virtualaddr;
     DWORD *pg;
     for (i=0;i<pages;i++)
        {
          DWORD pageadr;
          setpageattb(pagedir,virtualaddr,pattb);
          if (current_process->accesslevel==ACCESS_SYS)
          setpageattb(pagedir1,virtualaddr,pattb);
          virtualaddr+=0x1000;
         };
     refreshpages();
     return ret;
    };

void *dex32_setpageattbblock(DWORD virtualaddr,int amt,DWORD *pagecount,DWORD *pagedir,DWORD attb)
   {
     int pages=(amt/4096)+1;
     char *ret=0;
     if (amt==0) pages=1;
        else
     if (amt%4096==0) pages=amt/4096;
     ret=dex32_setpageattb(virtualaddr,pages,pagedir,attb);
     *pagecount=pages;
     return ret;
   };

//returns the total number of free physical pages on the system

DWORD dex32_getfreepages()
   {
       return totalpages; //the totalpages global variable holds the number of pages left

   };



void setgdtentry(DWORD index,void *base,gdtentry *t,DWORD limit,
           BYTE attb1,BYTE attb2)
   {
      DWORD b=(DWORD)base;
      t->lowaddr=b;
      t->lowaddr2=b >> 16;
      t->limit=limit;
      t->att1=attb1;
      t->att2=attb2;
      t->highaddr=b >> 24;
   };

#define RING0_TSS 0x89

WORD addgdt(DWORD base,DWORD limit,BYTE attb1,BYTE attb2)
  {
    int index=totalgdtentries;
    dex_gdtbase[index].lowaddr=base;
    dex_gdtbase[index].lowaddr2=base >> 16;
    dex_gdtbase[index].limit=limit;
    dex_gdtbase[index].att1=attb1;
    dex_gdtbase[index].att2=attb2 | ((limit >> 8)&0xF);
    dex_gdtbase[index].highaddr=base >> 24;
    return (index << 3);
  };


void setgdt(WORD sel,DWORD base,DWORD limit,BYTE attb1,BYTE attb2)
  {
#ifdef __x86_64__
    (void)sel; (void)base; (void)limit; (void)attb1; (void)attb2;
    return;
#else
    sel=sel >> 3;
    dex_gdtbase[sel].lowaddr=base;
    dex_gdtbase[sel].lowaddr2=base >> 16;
    dex_gdtbase[sel].limit=limit;
    dex_gdtbase[sel].att1=attb1;
    dex_gdtbase[sel].att2=attb2 | ((limit >> 16)&0xF);
    dex_gdtbase[sel].highaddr=base >> 24;
#endif
  };

void setattb(WORD sel,BYTE attb1)
  {
#ifdef __x86_64__
    (void)sel; (void)attb1;
    return;
#else
    sel=sel >> 3;
    dex_gdtbase[sel].att1=attb1;
#endif
  };

void setcallgate(DWORD sel,DWORD funcsel,void *entry,BYTE params,BYTE access)
  {
    CALLGATE *ptr=(CALLGATE*)dex_gdtbase;
    DWORD loc=(DWORD)entry;
    sel=sel>>3 ; //convert selector to an index
    ptr[sel].lowoffset=loc;
    ptr[sel].selector=funcsel;
    ptr[sel].attb1=params;
    ptr[sel].attb2=0x8c | (access << 5 );
    ptr[sel].highoffset=loc >> 16;
  };
extern void reset_gdtr();

void dex32_setbase(WORD sel,DWORD addr)
  {
#ifdef __x86_64__
    (void)sel; (void)addr;
    return;
#else
    DWORD cpuflags;
    sel=sel >> 3;
    dex_gdtbase[sel].lowaddr=addr;
    dex_gdtbase[sel].lowaddr2=addr >> 16;
    dex_gdtbase[sel].highaddr=addr >> 24;
#endif
  };



void  setinterruptvector(DWORD index,idtentry *t,unsigned char attr,
                           void (*handler)(int irq), WORD sel){
#ifdef __x86_64__
   uintptr addr = (uintptr)handler;
   /* Never install task gates on x86_64 — force interrupt/trap gate. */
   if ((attr & 0x0F) == 0x05)
      attr = (attr & 0xF0) | 0x0E;
   t[index].lowphy  = (WORD)(addr & 0xFFFF);
   t[index].selector = sel;
   t[index].ist = 0;
   t[index].attr = attr;
   t[index].midphy = (WORD)((addr >> 16) & 0xFFFF);
   t[index].highphy = (DWORD)((addr >> 32) & 0xFFFFFFFF);
   t[index].reserved = 0;
#else
   t[index].lowphy=(WORD)handler; //set the low word
   t[index].highphy=((DWORD)handler >> 16);	//set the high word
   t[index].selector=sel;
   t[index].reserved=0;
   t[index].attr=attr;
#endif
};

DWORD obtainpage()
    {
      // print("DEX 32: out of memory error\n");
      return 0;
    };



void *commit(DWORD virtualaddr,DWORD pages)
    {
     int i;
     char temp[255];
     void *ret=(void*)virtualaddr;
     DWORD flags;
     storeflags(&flags);
     stopints();
     
     #ifdef MEM_LEAK_CHECK
     printf("system committed %d pages.\n",pages);
     #endif
   
     for (i=0;i<pages;i++)
        {
          DWORD pageadr=(DWORD)mempop();
          
          //if out of physical address, call the VMM
          if (pageadr==0) pageadr=obtainpage();
          
          //out of memory error
          if (pageadr == -1) {ret = -1;break;};
          
          maplineartophysical2((DWORD*)SYS_PAGEDIR_VIR,virtualaddr,pageadr,PG_PRESENT);
          maplineartophysical2((DWORD*)SYS_KERPDIR_VIR,virtualaddr,pageadr,PG_PRESENT);
          virtualaddr+=0x1000;

        };
        
     restoreflags(flags);   
   
     return ret;
    };

void *commitb(DWORD virtualaddr,int amt,DWORD *pagecount)
   {
     int pages=(amt/4096)+1;
     char *ret=0;
     if (amt==0) pages=1;
        else
     if (amt%4096==0) pages=amt/4096;
     ret=commit(virtualaddr,pages);
     /*DEBUG*/

     *pagecount=pages;
     return ret;
   };

extern int smp_cpu_id(void);
volatile unsigned int sbrk_diag_calls[8];
volatile long sbrk_diag_last_amt[8];
volatile long sbrk_diag_last_ret[8];
volatile unsigned long sbrk_diag_knext[8];
static void sbrk_diag(long amt, unsigned long ret, unsigned long kn, int fail)
    {
     int me = smp_cpu_id();
     if (me < 0 || me >= 8) return;
     sbrk_diag_calls[me]++;
     sbrk_diag_last_amt[me] = amt;
     sbrk_diag_last_ret[me] = fail ? -1 : (long)ret;
     sbrk_diag_knext[me] = kn;
    };

void *sbrk(int amt)
    {
      int pages=(amt/4096)+1;
      char *ret=0;

      if (amt<0) { sbrk_diag(amt, -1, (unsigned long)(uintptr)knext, 1); return (void*)-1; }

      if (amt==0) { sbrk_diag(0, (unsigned long)(uintptr)(knext-1), (unsigned long)(uintptr)knext, 0); return knext-1; }

      if (amt%4096==0) pages=amt/4096;

#ifdef __x86_64__
      /* Identity map already covers the kernel heap.  Do not mempop() —
         that drained unrelated frames and let knext walk into the old
         4MiB hole. */
      {
         unsigned long next = (unsigned long)(uintptr)knext +
                              (unsigned long)pages * 4096UL;
         if (next > MEM_KHEAP_END)
            { sbrk_diag(amt, -1, (unsigned long)(uintptr)knext, 1); return (void*)-1; }
         ret = knext;
         knext = (char *)next;
         sbrk_diag(amt, (unsigned long)(uintptr)ret, (unsigned long)(uintptr)knext, 0);
         return (void*)ret;
      }
#else
      ret=commit((DWORD)knext,pages);
      knext+=(pages)*4096;
      sbrk_diag(amt, (unsigned long)(uintptr)ret, (unsigned long)(uintptr)knext, 0);
      return (void*)ret;
#endif
    };



//defines the USER MODE function for changing the break of an
//application
void *dex32_sbrk(unsigned int amt)
   {
     DWORD pages=(amt/4096)+1;
      DWORD flags;
      char *ret=current_process->knext;
      DWORD span_pages=pages;
      dex32_stopints(&flags);
      if (amt==0)
         {
         dex32_restoreints(flags);
         return ((void*)current_process->knext);
         };
  if (amt%4096==0) pages=amt/4096;
      /* knext starts at userheap+16 (the 16-byte parameter block) and is not
         page-aligned, so [ret, ret+pages*4096) can span one extra page.
         Commit every page the range touches; otherwise the page holding the
         new break is left uncommitted and the first write to it faults as a
         committed-heap not-present (cc1 self-host regression). */
      span_pages = pages + ((((unsigned long)(uintptr)ret) & 0xFFFUL) != 0);

   /* Phase 1: user private VA space is limited to the private PD0 (0-1GiB).
       Growing the heap beyond MEM_USER_VA_END would require splitting the
       shared boot_pd1..3 2MiB identity pages, which would corrupt the kernel
       map.  Stop below the stack guard so sbrk can never walk into the
       process stack (commit + reserve). */
    {
       unsigned long long mmap_lim = current_process->mmap_brk
          ? (unsigned long long)(uintptr)current_process->mmap_brk
          : (unsigned long long)MEM_USER_HEAP_LIMIT;
       if (((unsigned long long)ret & ~0xFFFULL) + (unsigned long long)span_pages * 4096ULL > mmap_lim)
        {
        printf("sbrk DENIED %s: ret=0x%llx pages=%u span=%u limit=0x%llx\n",
               current_process->name, (unsigned long long)ret, pages, span_pages,
               mmap_lim);
       dex32_restoreints(flags);
       return (void*)-1;
       };
    }

   /* Commit pages into the process page directory so malloc/sbrk
        used by the in-OS compiler can grow beyond the initial heap. */
     if (!dex32_commit((DWORD)ret, span_pages,
                         (DWORD*)current_process->pagedirloc, PG_USER | PG_WR))
         {
         unsigned long heapbytes =
            (unsigned long)(uintptr)current_process->knext -
            (unsigned long)(uintptr)userheap;
         printf("sbrk FAIL %s: want %u pages heap=%luKiB free=%llu/%llu\n",
                 current_process->name, pages, heapbytes>>10,
                 (unsigned long long)frame_free_count(),
                 (unsigned long long)frame_total_count());
         dex32_restoreints(flags);
         return (void*)-1;
         };

      /* Log pool pressure as a user heap grows (power-of-two KiB marks). */
      {
         unsigned long hb =
            (unsigned long)(uintptr)current_process->knext -
            (unsigned long)(uintptr)userheap;
         if (hb >= (1UL<<20) && (hb & (hb-1)) == 0)
            printf("sbrk: %s heap=%luKiB free=%llu/%llu\n",
                    current_process->name, hb>>10,
                    (unsigned long long)frame_free_count(),
                    (unsigned long long)frame_total_count());
      }


#ifdef __x86_64__
     /* userpd_map_page() already zeros each private frame. Do NOT
        memset() the user VA here: after splitting a 2MiB identity page
        the CPU may still have that large-page TLB entry, so a VA write
        would hit the shared identity physical page instead of the
        private frame. Drop the 2MiB TLB via invlpg on each new page. */
     {
         DWORD pi;
         for (pi = 0; pi < span_pages; pi++) {
            unsigned long va = (unsigned long)(uintptr)ret + (unsigned long)pi * 0x1000UL;
            __asm__ __volatile__("invlpg (%0)" :: "r"(va) : "memory");
         }
      }
#else
     /* Zero the newly committed user memory before handing it out. */
     memset(ret, 0, (size_t)(pages * 4096));
#endif

      current_process->knext+=pages*4096;
      dex32_restoreints(flags);
      return (void*)ret;
    };

/* Anonymous mmap: grow down from mmap_brk so GGC pages are not in the
   sbrk/malloc arena.  File-backed maps stay in the SDK. */
void *dex32_mmap(unsigned long length, unsigned long flags,
                 unsigned long a3, unsigned long a4, unsigned long a5)
{
#ifdef __x86_64__
     DWORD irq;
     unsigned long pages, i, va, start, heap;
     (void)flags;
     (void)a3;
     (void)a4;
     (void)a5;

     if (length == 0)
        return (void *)(uintptr)-1;
     pages = (length + 4095UL) / 4096UL;
     length = pages * 4096UL;

     dex32_stopints(&irq);
     if (!current_process->mmap_brk)
        current_process->mmap_brk = (char *)(uintptr)MEM_USER_HEAP_LIMIT;
     start = (unsigned long)(uintptr)current_process->mmap_brk;
     heap = (unsigned long)(uintptr)current_process->knext;
     if (start < length + 0x100000UL || start - length < heap + 0x100000UL) {
        dex32_restoreints(irq);
        return (void *)(uintptr)-1;
     }
     va = (start - length) & ~0xFFFUL;
     for (i = 0; i < pages; i++) {
        if (!userpd_map_page((u64 *)(uintptr)current_process->pagedirloc,
                             (unsigned long long)va + (unsigned long long)i * 0x1000ULL,
                             PG_USER | PG_WR)) {
           unsigned long j;
           for (j = 0; j < i; j++)
              userpd_unmap_page((u64 *)(uintptr)current_process->pagedirloc,
                                (unsigned long long)va + (unsigned long long)j * 0x1000ULL);
           dex32_restoreints(irq);
           return (void *)(uintptr)-1;
        }
        {
           unsigned long pva = va + i * 0x1000UL;
           __asm__ __volatile__("invlpg (%0)" :: "r"(pva) : "memory");
        }
     }
     current_process->mmap_brk = (char *)(uintptr)va;
     dex32_restoreints(irq);
     return (void *)(uintptr)va;
#else
     (void)length; (void)flags; (void)a3; (void)a4; (void)a5;
     return (void *)(uintptr)-1;
#endif
}

int dex32_munmap(unsigned long addr, unsigned long length,
                 unsigned long a3, unsigned long a4, unsigned long a5)
{
#ifdef __x86_64__
     DWORD irq;
     unsigned long pages, i, heap, mmap_lim;
     (void)a3;
     (void)a4;
     (void)a5;

     if (length == 0)
        return 0;
     if (addr & 0xFFFUL)
        return -1;
     pages = (length + 4095UL) / 4096UL;
     heap = (unsigned long)(uintptr)current_process->knext;
     mmap_lim = current_process->mmap_brk
        ? (unsigned long)(uintptr)current_process->mmap_brk
        : (unsigned long)MEM_USER_HEAP_LIMIT;
     /* Only unmap pages in the anonymous mmap window. */
     if (addr < mmap_lim || addr + pages * 4096UL > MEM_USER_HEAP_LIMIT)
        return -1;
     if (addr < heap)
        return -1;

     dex32_stopints(&irq);
     for (i = 0; i < pages; i++) {
        if (!userpd_unmap_page((u64 *)(uintptr)current_process->pagedirloc,
                               (unsigned long long)addr + (unsigned long long)i * 0x1000ULL)) {
           dex32_restoreints(irq);
           return -1;
        }
     }
     dex32_restoreints(irq);
     return 0;
#else
     (void)addr; (void)length; (void)a3; (void)a4; (void)a5;
     return -1;
#endif
}

void dex32_copy_on_write(DWORD *directory)
{
  /*   DWORD i;
     DWORD userstart=(DWORD)userspace >> 22,
           userend  =0xC0000000 >> 22,


     for (i=userstart;i<=userend;i++)
       {


       ;};

    */
;};


//copies a pagedirectory except the user space area
//0xA0000000-0xC0000000
void dex32_copy_pagedirU(DWORD *destdir,DWORD *source)
   {
     DWORD i;
     /* Kernel identity-maps 0-16MB (image + heap + VFS nodes). User
        processes must keep that map or syscalls like fopen() page-fault
        once the kernel heap grows past 4MB. */
     DWORD kernelend = 0x01000000 >> 22,
           userend  =0xC0000000 >> 22,
           stopaddr =0xF0000000 >> 22;
     disablepaging();
    // memset(destdir,0,0x1000);
     for (i=0;i<kernelend;i++)
        destdir[i]=source[i];
     for (i=userend;i<stopaddr;i++)
        destdir[i]=source[i];
   };

int dex32_copy_pg(DWORD *destdir, DWORD *source)
   {
      DWORD i,i2;
      DWORD start = (DWORD) userspace >> 22;
      DWORD end = 0xC0000000 >> 22;
      DWORD syscallstart = 0x90000000 >> 22, syscallend=0xA0000000 >> 22;
      dex32_copy_pagedirU(destdir,source);
      for (i=start;i<end;i++)
        {
          if (source[i]&1)
            {
                  DWORD *destpagetable,*srcpagetable;
                  destdir[i]= (DWORD)mempop() | 1 | PG_USER | PG_WR;

                  destpagetable= (DWORD)destdir[i]&0xFFFFF000;
                  srcpagetable = (DWORD)source[i]&0xFFFFF000;

                  for (i2= 0 ;i2 < 1024; i2++)
                  {

                     if (srcpagetable[i2]&1)
                      {
                 		      DWORD free_mem = (DWORD)mempop();
                              //out of memory?
                              if (free_mem==-1) return -1;

                              destpagetable[i2] = free_mem | 1;

                              if (i>=syscallstart&&i<syscallend)
                                  {
                                        memcpy(destpagetable[i2]&0xFFFFF000,srcpagetable[i2]&0xFFFFF000,0x1000);
                                  }
                                    else
                                  {
                                  		memcpy(destpagetable[i2]&0xFFFFF000,srcpagetable[i2]&0xFFFFF000,0x1000);
                                        destpagetable[i2]= destpagetable[i2] | PG_WR | PG_USER;
                                  };
                      }
                        else
                     destpagetable[i2]= srcpagetable[i2];
                  };
            }
              else
            destdir[i] = source[i];
        };
   };

//copies a pagedirectory given a source and a destination
void dex32_copy_pagedir(DWORD *destdir,DWORD *source)
   {
     int i;
     for (i=0;i<(0x1000/2);i++)
        destdir[i]=source[i];
   };

//designed for  USER MODE applications
//commits a block of memory at a time in bytes
void *dex32_commitblock(DWORD virtualaddr,int amt,
    DWORD *pagecount,DWORD *pagedir,DWORD attb)
   {
     int pages=(amt/4096)+1;
     char *ret=0;
     if (amt==0) pages=1;
        else
     if (amt%4096==0) pages=amt/4096;      
     
     ret=dex32_commit(virtualaddr,pages,pagedir,attb);
     
     *pagecount=pages;
     return ret;
   };


void *dex32_reserveblock(DWORD virtualaddr,int amt,
    DWORD *pagecount,DWORD *pagedir,DWORD attb)
   {
     int pages=(amt/4096)+1;
     char *ret=0;
     if (amt==0) pages=1;
        else
     if (amt%4096==0) pages=amt/4096;
     ret=dex32_reserve(virtualaddr,pages,pagedir,attb);
     *pagecount=pages;
     return ret;
   };
//sets a virtual address location as demand paged..
//meaning, the OS commits a pge only if it has been accessed
void *dex32_reserve(DWORD virtualaddr,DWORD pages,DWORD *pagedir,DWORD attb)
   {
     int i;
     char temp[255],temp2[255];
     void *ret=(void*)virtualaddr;
     DWORD *pg;
     for (i=0;i<pages;i++)
        {
          DWORD pageadr; //holds a physical frame, if possible

          pg=(DWORD*)getvirtaddress((DWORD)pagedir);
          maplineartophysical2(pg,virtualaddr,
          0,PG_USER | PG_DEMANDLOAD | attb);

          if (current_process->accesslevel==ACCESS_SYS)
          {
          pg=(DWORD*)getvirtaddress((DWORD)pagedir1);
          maplineartophysical2(pg,virtualaddr,
          pageadr,PG_DEMANDLOAD | PG_USER | attb);
          virtualaddr+=0x1000;
          };
         };
    refreshpages();
     return ret;
    };

//designed for USER MODE applications
//commits a block of memory at a time in pages
#ifdef __x86_64__
u64 *userpd_map_page(u64 *pml4, unsigned long long vaddr, unsigned long attb);
#endif

void *dex32_commit(DWORD virtualaddr,DWORD pages,DWORD *pagedir,DWORD pattb)
    {
     int i;
     char temp[255],temp2[255];
     void *ret=(void*)virtualaddr;
     DWORD *pg;
     DWORD flags;
     
 #ifdef __x86_64__
     /* A user process with a private PML4: lazily map the committed pages
        (heap/sbrk growth) to fresh private frames. Kernel identity map or
        already-mapped regions are left alone. */
     if (pagedir && pagedir != (DWORD *)(uintptr)pagedir1) {
         int pi;
         for (pi = 0; pi < pages; pi++)
            if (!userpd_map_page((u64 *)(uintptr)pagedir,
                                 (unsigned long long)(virtualaddr + (unsigned long long)pi * 0x1000),
                                 (unsigned long)pattb))
               return 0; /* pool exhausted: not all pages committed */
      }
      (void)i;
      (void)temp;
      (void)temp2;
      (void)pg;
      (void)flags;
      return ret;
 #else
     storeflags(&flags);
     stopints();

     /*DEBUG*/

     #ifdef MEM_LEAK_CHECK
     printf("user committed %d pages.\n",pages);
     #endif
     for (i=0;i<pages;i++)
        {
          DWORD pageadr; //holds a physical frame, if possible
        //if page is set to demand paged, we do not commit
        //a physical frame yet
          pageadr=(DWORD)mempop();

          if (pageadr==0) //we're out of physical frames, find a way to get one
          pageadr=obtainpage(); //call the VMM to get a page


          pg=(DWORD*)getvirtaddress((DWORD)pagedir);


          maplineartophysical2(pg,virtualaddr, pageadr,PG_PRESENT | /*PG_USER |*/ pattb);
              //if this is called from the KERNEL map it to kernel memory too
              if (current_process->accesslevel==ACCESS_SYS)
               {
                   pg=(DWORD*)getvirtaddress((DWORD)pagedir1);
        
                   maplineartophysical2(pg,virtualaddr,
                   pageadr,PG_PRESENT | /*PG_USER |*/ pattb);
               };
           
          virtualaddr+=0x1000;
         };
         
     refreshpages();
     restoreflags(flags);
     
     return ret;
#endif
    };

void dex32copyblock(DWORD vdest,DWORD vsource,DWORD pages,DWORD *pagedir)
   {
     int i;
     for (i=0;i<pages;i++)
          {
           DWORD pdest=getphys(vdest,pagedir);
           DWORD pg=(DWORD*)getvirtaddress((DWORD)pdest);
           memcpy((void*)pg,(void*)vsource,0x1000);
           vdest+=0x1000;
           vsource+=0x1000;
          };

   };


#define MMIO_UC_MAX 16
static u64 mmio_uc_base[MMIO_UC_MAX];
static u64 mmio_uc_len[MMIO_UC_MAX];
static int mmio_uc_state[MMIO_UC_MAX];
static int mmio_uc_n;
static spinlock_t mmio_uc_lock;
#ifdef __x86_64__
static u64 mmio_high_pd[512] __attribute__((aligned(4096)));
static u64 mmio_high_pt[512] __attribute__((aligned(4096)));
static u8 mmio_high_used[512];
static int mmio_high_initialized;
static spinlock_t mmio_high_lock;
typedef struct {
   u64 phys;
   u64 end;
   unsigned first;
   int state;
} mmio_high_mapping;
static mmio_high_mapping mmio_high_maps[16];
static unsigned mmio_high_map_count;
#endif

static void mmio_apply_one(u64 phys, u64 len)
{
#ifdef __x86_64__
   extern u64 boot_pd0[], boot_pd1[], boot_pd2[], boot_pd3[];
   u64 *pds[4];
   u64 addr, end;

   pds[0] = boot_pd0;
   pds[1] = boot_pd1;
   pds[2] = boot_pd2;
   pds[3] = boot_pd3;
   end = phys + len;
   addr = phys & ~0x1FFFFFULL;
   for (; addr < end; addr += 0x200000ULL) {
      unsigned pd = (unsigned)(addr >> 30);
      unsigned idx = (unsigned)((addr >> 21) & 0x1FF);
      if (pd > 3)
         continue;
      pds[pd][idx] |= (u64)(PG_WRITETHROUGH | PG_PCD);
   }
#else
   (void)phys;
   (void)len;
#endif
}

int mmio_mark_uncacheable(u64 phys, u64 len)
{
#ifdef __x86_64__
   int i, entry = -1;
   spin_irq_flags_t flags;
retry:
   flags = spin_lock_irqsave(&mmio_uc_lock);
   if (!len || phys + len < phys) {
      spin_unlock_irqrestore(&mmio_uc_lock, flags);
      return 0;
   }
   for (i = 0; i < mmio_uc_n; i++) {
      if (phys >= mmio_uc_base[i] &&
          phys + len <= mmio_uc_base[i] + mmio_uc_len[i]) {
         if (!mmio_uc_state[i]) {
            spin_unlock_irqrestore(&mmio_uc_lock, flags);
            __asm__ __volatile__("pause");
            goto retry;
         }
         if (mmio_uc_state[i] < 0) {
            mmio_uc_state[i] = 0;
            entry = i;
            break;
         }
         spin_unlock_irqrestore(&mmio_uc_lock, flags);
         return 1;
      }
      if (entry < 0 && mmio_uc_state[i] < 0)
         entry = i;
   }
   if (entry < 0 && mmio_uc_n == MMIO_UC_MAX) {
      spin_unlock_irqrestore(&mmio_uc_lock, flags);
      return 0;
   }
   if (entry < 0)
      entry = mmio_uc_n++;
   mmio_uc_base[entry] = phys;
   mmio_uc_len[entry] = len;
   mmio_uc_state[entry] = 0;
   mmio_apply_one(phys, len);
   spin_unlock_irqrestore(&mmio_uc_lock, flags);
   if (!smp_tlb_shootdown_all()) {
      printf("mmio: TLB shootdown failed for low mapping\n");
      flags = spin_lock_irqsave(&mmio_uc_lock);
      mmio_uc_state[entry] = -1;
      spin_unlock_irqrestore(&mmio_uc_lock, flags);
      return 0;
   }
   flags = spin_lock_irqsave(&mmio_uc_lock);
   mmio_uc_state[entry] = 1;
   spin_unlock_irqrestore(&mmio_uc_lock, flags);
   return 1;
#else
   (void)phys;
   (void)len;
   return 0;
#endif
}

void *mmio_map(u64 phys, u64 len)
{
#ifdef __x86_64__
   extern u64 boot_pdpt_high[];
   u64 aligned, end, page_count;
   unsigned offset, pages, first, map_index, i, phys_bits = 36;
   spin_irq_flags_t flags;
   u32 eax, ebx, ecx, edx;
   if (!len || phys + len < phys)
      return 0;
   if (phys + len <= 0x100000000ULL) {
      if (!mmio_mark_uncacheable(phys, len))
         return 0;
      return (void *)(uintptr)phys;
   }
   aligned = phys & ~0xFFFULL;
   offset = (unsigned)(phys - aligned);
   end = phys + len;
   page_count = (end - aligned + 0xFFFULL) >> 12;
   eax = 0x80000000u;
   __asm__ volatile ("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
   if (eax >= 0x80000008u) {
      eax = 0x80000008u;
      __asm__ volatile ("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
      phys_bits = eax & 0xFF;
   }
   if (!page_count || page_count > KMMIO_SIZE / 0x1000 ||
       !phys_bits || phys_bits > 63 || ((end - 1) >> phys_bits))
      return 0;
   pages = (unsigned)page_count;
retry:
   flags = spin_lock_irqsave(&mmio_high_lock);
   for (i = 0; i < mmio_high_map_count; i++) {
      if (aligned >= mmio_high_maps[i].phys &&
          end <= mmio_high_maps[i].end) {
         if (!mmio_high_maps[i].state) {
            spin_unlock_irqrestore(&mmio_high_lock, flags);
            __asm__ __volatile__("pause");
            goto retry;
         }
         if (mmio_high_maps[i].state < 0)
            continue;
         void *mapped = (void *)(uintptr)(KMMIO_BASE +
            ((u64)mmio_high_maps[i].first << 12) +
            (phys - mmio_high_maps[i].phys));
         spin_unlock_irqrestore(&mmio_high_lock, flags);
         return mapped;
      }
   }
   map_index = mmio_high_map_count;
   for (i = 0; i < mmio_high_map_count; i++) {
      if (mmio_high_maps[i].state < 0) {
         map_index = i;
         break;
      }
   }
   if (map_index == sizeof(mmio_high_maps) /
                    sizeof(mmio_high_maps[0])) {
      spin_unlock_irqrestore(&mmio_high_lock, flags);
      return 0;
   }
   for (first = 0; first + pages <= 512; first++) {
      for (i = 0; i < pages && !mmio_high_used[first + i]; i++);
      if (i == pages)
         break;
      first += i;
   }
   if (first + pages > 512) {
      spin_unlock_irqrestore(&mmio_high_lock, flags);
      return 0;
   }
   if (!mmio_high_initialized) {
      memset(mmio_high_pd, 0, sizeof(mmio_high_pd));
      memset(mmio_high_pt, 0, sizeof(mmio_high_pt));
      boot_pdpt_high[4] = (u64)(uintptr)mmio_high_pd | PG_PRESENT | PG_WR;
      mmio_high_pd[0] = (u64)(uintptr)mmio_high_pt | PG_PRESENT | PG_WR;
      mmio_high_initialized = 1;
   }
   for (i = 0; i < pages; i++) {
      mmio_high_used[first + i] = 1;
      mmio_high_pt[first + i] = (aligned + ((u64)i << 12)) |
         PG_PRESENT | PG_WR | PG_WRITETHROUGH | PG_PCD;
   }
   mmio_high_maps[map_index].phys = aligned;
   mmio_high_maps[map_index].end = aligned + ((u64)pages << 12);
   mmio_high_maps[map_index].first = first;
   mmio_high_maps[map_index].state = 0;
   if (map_index == mmio_high_map_count)
      mmio_high_map_count++;
   spin_unlock_irqrestore(&mmio_high_lock, flags);
   if (!smp_tlb_shootdown_all()) {
      printf("mmio: TLB shootdown failed for high mapping\n");
      flags = spin_lock_irqsave(&mmio_high_lock);
      for (i = 0; i < pages; i++) {
         mmio_high_pt[first + i] = 0;
         mmio_high_used[first + i] = 0;
      }
      mmio_high_maps[map_index].state = -1;
      spin_unlock_irqrestore(&mmio_high_lock, flags);
      smp_tlb_shootdown_all();
      return 0;
   }
   flags = spin_lock_irqsave(&mmio_high_lock);
   mmio_high_maps[map_index].state = 1;
   spin_unlock_irqrestore(&mmio_high_lock, flags);
   return (void *)(uintptr)(KMMIO_BASE + ((u64)first << 12) + offset);
#else
   (void)phys;
   (void)len;
   return 0;
#endif
}

void mmio_reapply_uncacheable(void)
{
   int i;
   spin_irq_flags_t flags = spin_lock_irqsave(&mmio_uc_lock);
   for (i = 0; i < mmio_uc_n; i++) {
      if (mmio_uc_state[i] >= 0)
         mmio_apply_one(mmio_uc_base[i], mmio_uc_len[i]);
   }
   spin_unlock_irqrestore(&mmio_uc_lock, flags);
}

void dex32_restore_identity_map(void)
{
#ifdef __x86_64__
   extern u64 boot_pd0[], boot_pd1[], boot_pd2[], boot_pd3[];
   extern u64 boot_pml4[];
   u64 *pds[4];
   u64 phys = 0;
   int pd, i;

   /* User processes currently run in kernel CS and can corrupt the boot
      2MiB identity map (e.g. clear R/W). Reinstall P|RW|PS before each exec. */
   pds[0] = boot_pd0;
   pds[1] = boot_pd1;
   pds[2] = boot_pd2;
   pds[3] = boot_pd3;
   for (pd = 0; pd < 4; pd++) {
      for (i = 0; i < 512; i++) {
         pds[pd][i] = phys | 0x83ULL; /* present | writable | page-size */
         phys += 0x200000ULL;
      }
   }
   mmio_reapply_uncacheable();
   __asm__ volatile ("mov %0, %%cr3" :: "r"((unsigned long)(uintptr)boot_pml4) : "memory");
#endif
}

/* ------------------------------------------------------------------
 * Per-process user page directories (x86_64)
 *
 * On x86_64 every user ELF previously ran with pagedir1 (the shared
 * kernel identity map), so ALL user processes shared the SAME physical
 * stack/heap/ELF/syscall-stack memory. Concurrent user processes (e.g.
 * in-OS tcc compiling tccnew.exe) clobbered each other's C-stack frames
 * in the shared 0xE000000 stack window; on resume the restored frame had
 * a corrupted SS and the CPU raised #13 (GPF) with the live RIP inside
 * the process's own BSS and RAX == CR3.
 *
 * userpd_create() builds a PRIVATE PML4 for one user process: it shares
 * the kernel's four 1GiB superpage page tables (boot_pd0..3) so the
 * kernel image, its BSS and the frame pool are reachable, but it
 * privatizes the user-window 2MiB blocks (the ELF image at 0x400000+,
 * the syscall stack at 0x9000000, the heap at 0xA000000+, the stack at
 * 0xE000000) with per-process 4KiB frames drawn from a DEDICATED pool
 * (below). Those frames are zeroed at allocation, matching brk()/mmap()
 * semantics. context.S already switches CR3 per task (ctx.cr3), and
 * createprocess() sets ctx.cr3 = pagedir, so the private PML4 is simply
 * installed into the new task's context.
 *
 * The frames come from a dedicated pool, NOT the shared mempop() freelist:
 * getphys() is identity on x86_64 and freelinearloc() would mempush() the
 * identity addresses back into the shared pool, which overlaps the kernel
 * heap (knext grows from 0x03000000 inside the [0x300000,0x8000000)
 * frame range) and corrupts the kernel. The pool below is a disjoint
 * physical range, so it never contends with the kernel allocator.
 * ------------------------------------------------------------------ */

/* Physical range reserved for per-process user page tables + PTE frames.
   It sits BELOW the identity-mapped user window [0x08000000,0x10000000)
    (syscallstack 0x09000000 / userheap 0x0A000000 / userstackloc 0x40000000)
   and is carved out of the shared mempop() freelist (seeded
   [0x00400000,0x08000000)), with mem_detectmemory() told to skip it so the
   kernel allocator never double-allocates a pool frame.

   The pool MUST NOT overlap the identity-mapped user window: the kernel runs
   under pagedir1 (identity), so its writes to the user heap/stack (loader
   memsets, sbrk) hit those PHYSICAL addresses. If the pool lived there, the
   kernel's identity writes would clobber the process's own private frames
   (its code/stack) -> wild RIP + GPF. Placing it below 0x08000000 keeps the
   process's private frames disjoint from everything the kernel writes by
   identity. 32MiB = 8192 frames must hold a PARENT plus a large child
    concurrently (e.g. the in-OS gcc driver spawning the ~18MiB cc1): the
    child image + a 2MiB up-front heap commit + 1MiB stack + syscall stack +
    page tables, twice over. The heap commit is kept small (see
    ELF_HEAP_COMMIT in elf_module.c) and grows via sbrk so two processes fit. */

/* Frame sourcing for the user page-table allocator.
 *
 * upop()/upush() are thin wrappers over the global physical frame allocator
 * (frame_alloc()/frame_release()), so private user frames (PML4/PDPT/PD/PTE
 * tables and sbrk heap pages) now come from the full E820-seeded pool instead
 * of a fixed 32MiB carve-out.  There is no per-frame owner table: a process's
 * frames are reclaimed by userpd_free() walking its PML4, and
 * userpd_is_private() simply distinguishes a private PML4 from the shared
 * kernel one (pagedir1).  The in-place free list keeps its next-free pointer
 * in a frame's first 8 bytes; that is safe because the kernel only ever writes
 * a frame after it has been allocated (removed from the free list), never
 * while the frame is still free. */

int userpd_is_private(const void *pml4)
{
   return (pml4 != 0) && (pml4 != (const void *)(uintptr)pagedir1);
}

int userpd_used(void)
{
   /* Frames currently handed out (allocated) from the global pool. */
   return (int)(frame_total - frame_free);
}

/* Pop a physical frame from the global pool. Returns 0 when empty. */
static DWORD *upop(void)
{
   u64 phys = frame_alloc();
   return (DWORD *)(uintptr)phys;
}

/* Return a physical frame to the global pool. */
static void upush(DWORD *fr)
{
   if (fr)
      frame_release((u64)(uintptr)fr);
}

/* Map one 4KiB page (vaddr) in a user PML4 to a fresh, zeroed private
   frame. attb: e.g. PG_USER|PG_WR. Returns the physical frame (for the
   kernel to copy/zero) or 0 on allocation failure.

   Walks the real 4-level hierarchy that startup.S builds:
       PML4[0] -> PDPT ; PDPT[block] -> PD(2MiB pages) ; PD[page] -> 4KiB.
   The user window lives in the first GiB, so PML4 index is 0 and the
   relevant PD is PDPT[0] (our private copy of boot_pd0). To map a 4KiB
   page inside a 2MiB block we replace that block's 2MiB page entry (PS)
   with a private 4KiB PTE table on first touch, then map the page. */
u64 *userpd_map_page(u64 *pml4, unsigned long long vaddr, unsigned long attb)
 {
    int pmi = (int)((vaddr >> 39) & 0x1FF);   /* PML4 index   (GiB)  */
    int pi  = (int)((vaddr >> 30) & 0x1FF);   /* PDPT index   (1GiB) */
    int bi  = (int)((vaddr >> 21) & 0x1FF);   /* 2MiB block   within PD */
    int gi  = (int)((vaddr >> 12) & 0x1FF);   /* 4KiB page    within block */
    u64 pe, de;
    u64 *pdpt, *pd, *pte, *fr;
    u64 *pml4v = (u64 *)KDIRECT((u64)(uintptr)pml4 & 0x000FFFFFFFFF000ULL);

    /* Phase 1: only the private PD0 (0-1GiB) may be split.  Mapping a VA in
       1-4GiB would replace a shared 2MiB identity PTE in boot_pd1..3 with a
       private 4KiB PTE table, corrupting the kernel identity map.  Refuse
       anything beyond the private PD0. */
    if (vaddr >= 0x40000000ULL || pmi != 0 || pi != 0)
       return 0;

    pe = pml4v[pmi];
    if (!(pe & 1) || (pe & 0x80)) return 0;   /* PML4[pmi] bad */
    pdpt = (u64 *)KDIRECT(pe & 0x000FFFFFFFFF000ULL);

    de = pdpt[pi];
    if (!(de & 1) || (de & 0x80)) return 0;   /* PDPT[pi] bad */
    pd = (u64 *)KDIRECT(de & 0x000FFFFFFFFF000ULL);

    /* Resolve the 2MiB block: an absent entry or a 2MiB identity page (PS)
       becomes a fresh private 4KiB PTE table; an existing 4KiB PTE table is
       reused. (upop() returns a frame pointer or NULL, never a PTE.) */
    {
       u64 be = pd[bi];
       if (be & 1 && !(be & 0x80)) {
          /* already a private 4KiB PTE table: reuse */
       } else {
       u64 *t = upop();
            if (!t) {
               static int up_empty_log = 0;
               if (up_empty_log < 8) {
                  up_empty_log++;
                  printf("userpd: POOL EMPTY (PTE table) va=0x%llx free=%llu/%llu\n",
                         vaddr,
                         (unsigned long long)frame_free_count(),
                         (unsigned long long)frame_total_count());
               }
               return 0;   /* pool empty (PTE table) */
            }
            memset(KDIRECT((u64)(uintptr)t), 0, 0x1000);
           pd[bi] = (u64)(uintptr)t | 0x03ULL;
          /* Drop any leftover 2MiB identity TLB for this block. INVLPG
             of one address in the large page invalidates that entry. */
          {
             unsigned long long block = vaddr & ~0x1FFFFFULL;
             __asm__ __volatile__("invlpg (%0)" :: "r"((unsigned long)block) : "memory");
          }
       }
    }
    pte = (u64 *)KDIRECT(pd[bi] & 0x000FFFFFFFFF000ULL);

   {
         u64 e = pte[gi];
         if (e & 1)
             return (u64 *)(e & 0x000FFFFFFFFF000ULL); /* already mapped */
          fr = upop();
          if (!fr) {
             static int up_page_empty_log = 0;
             if (up_page_empty_log < 8) {
                up_page_empty_log++;
                printf("userpd: POOL EMPTY (4KiB page) va=0x%llx free=%llu/%llu\n",
                       vaddr,
                       (unsigned long long)frame_free_count(),
                       (unsigned long long)frame_total_count());
             }
             return 0;   /* pool empty (4KiB page) */
          }
          memset(KDIRECT((u64)(uintptr)fr), 0, 0x1000);
         /* A mapped leaf page MUST be present: force PG_PRESENT. Callers pass
           only the additional attributes (PG_WR|PG_USER|...), so OR in the
           Present bit here or the page would be invisible to the CPU. */
        pte[gi] = (u64)(uintptr)fr | (attb | PG_PRESENT);
          return fr;
      }
  }

/* Map one absent PD0 VA to its identity physical frame.  Used only for
   kernel-code faults under a user PML4: userpd_create() zeroes most 2MiB
   identity blocks in PD0, but kernel code running with the user CR3 may still
   reference low identity addresses.  Mapping a fresh zero frame here would
   silently corrupt the kernel, so restore the identity translation. */
int userpd_map_identity_page(u64 *pml4, unsigned long long vaddr, unsigned long attb)
  {
     int pmi = (int)((vaddr >> 39) & 0x1FF);
     int pi  = (int)((vaddr >> 30) & 0x1FF);
     int bi  = (int)((vaddr >> 21) & 0x1FF);
     int gi  = (int)((vaddr >> 12) & 0x1FF);
     u64 pe, de, be;
     u64 *pdpt, *pd, *pte, *pml4v;

     if (!pml4 || vaddr >= 0x40000000ULL || pmi != 0 || pi != 0)
        return 0;
     if (memamount && vaddr >= (unsigned long long)memamount)
        return 0;

     pml4v = (u64 *)KDIRECT((u64)(uintptr)pml4 & 0x000FFFFFFFFF000ULL);
     pe = pml4v[pmi];
     if (!(pe & 1) || (pe & 0x80)) return 0;
     pdpt = (u64 *)KDIRECT(pe & 0x000FFFFFFFFF000ULL);

     de = pdpt[pi];
     if (!(de & 1) || (de & 0x80)) return 0;
     pd = (u64 *)KDIRECT(de & 0x000FFFFFFFFF000ULL);

     be = pd[bi];
     if (!(be & 1) || (be & 0x80)) {
        u64 *t = upop();
        if (!t)
           return 0;
        memset(KDIRECT((u64)(uintptr)t), 0, 0x1000);
        pd[bi] = (u64)(uintptr)t | 0x03ULL;
        {
           unsigned long long block = vaddr & ~0x1FFFFFULL;
           __asm__ __volatile__("invlpg (%0)" :: "r"((unsigned long)block) : "memory");
        }
     }
     pte = (u64 *)KDIRECT(pd[bi] & 0x000FFFFFFFFF000ULL);
     if (pte[gi] & 1)
        return 1;
     pte[gi] = (vaddr & 0x000FFFFFFFFF000ULL) | (attb | PG_PRESENT);
     __asm__ __volatile__("invlpg (%0)" :: "r"((unsigned long)vaddr) : "memory");
     return 1;
  }

 /* Unmap one private 4KiB leaf.  Returns 1 if the PTE was absent or released,
   0 if the walk failed (shared 2MiB page, bad PML4, VA out of PD0). */
int userpd_unmap_page(u64 *pml4, unsigned long long vaddr)
 {
    int pmi = (int)((vaddr >> 39) & 0x1FF);
    int pi  = (int)((vaddr >> 30) & 0x1FF);
    int bi  = (int)((vaddr >> 21) & 0x1FF);
    int gi  = (int)((vaddr >> 12) & 0x1FF);
    u64 pe, de, e;
    u64 *pdpt, *pd, *pte;
    u64 *pml4v;

    if (vaddr >= 0x40000000ULL || pmi != 0 || pi != 0 || !pml4)
       return 0;

    pml4v = (u64 *)KDIRECT((u64)(uintptr)pml4 & 0x000FFFFFFFFF000ULL);
    pe = pml4v[pmi];
    if (!(pe & 1) || (pe & 0x80)) return 0;
    pdpt = (u64 *)KDIRECT(pe & 0x000FFFFFFFFF000ULL);
    de = pdpt[pi];
    if (!(de & 1) || (de & 0x80)) return 0;
    pd = (u64 *)KDIRECT(de & 0x000FFFFFFFFF000ULL);
    de = pd[bi];
    if (!(de & 1) || (de & 0x80))
       return 0; /* shared 2MiB identity: do not split/unmap from munmap */
    pte = (u64 *)KDIRECT(de & 0x000FFFFFFFFF000ULL);
    e = pte[gi];
    if (e & 1) {
       frame_release(e & 0x000FFFFFFFFF000ULL);
       pte[gi] = 0;
       __asm__ __volatile__("invlpg (%0)" :: "r"((unsigned long)vaddr) : "memory");
    }
    return 1;
 }

void *userpd_resolve(u64 *pml4, unsigned long long vaddr)
{
   const u64 phys_mask = 0x000FFFFFFFFF000ULL;
   u64 *pml4v, *pdpt, *pd, *pte;
   u64 entry;

   if (!userpd_is_private(pml4) || vaddr >= 0x40000000ULL)
      return 0;
   pml4v = (u64 *)KDIRECT((u64)(uintptr)pml4 & phys_mask);
   entry = pml4v[(vaddr >> 39) & 0x1FF];
   if (!(entry & PG_PRESENT) || (entry & 0x80ULL))
      return 0;
   pdpt = (u64 *)KDIRECT(entry & phys_mask);
   entry = pdpt[(vaddr >> 30) & 0x1FF];
   if (!(entry & PG_PRESENT) || (entry & 0x80ULL))
      return 0;
   pd = (u64 *)KDIRECT(entry & phys_mask);
   entry = pd[(vaddr >> 21) & 0x1FF];
   if (!(entry & PG_PRESENT) || (entry & 0x80ULL))
      return 0;
   pte = (u64 *)KDIRECT(entry & phys_mask);
   entry = pte[(vaddr >> 12) & 0x1FF];
   if (!(entry & PG_PRESENT))
      return 0;
   return (void *)((uintptr)KDIRECT(entry & phys_mask) + (vaddr & 0xFFFULL));
}

/* Allocate a private PML4 mirroring the kernel's hierarchy (startup.S):
   PML4[0]->PDPT ; PDPT[0]->PD0 (private copy) ; PDPT[1..3]->boot_pd1..3.
   The private PD0 is a copy of boot_pd0 (512 x 2MiB identity pages) so the
   kernel image/heap/BSS stay reachable; user-window 2MiB blocks are later
   replaced by 4KiB PTE tables by userpd_map_page(). */
u64 *userpd_create(void)
 {
    extern u64 boot_pd0[], boot_pd1[], boot_pd2[], boot_pd3[], boot_pdpt_high[];
    u64 *pml4, *pdpt, *pd0;
    u64 *pml4v, *pdptv, *pd0v;
     pml4 = upop();
     if (!pml4) return 0;
     pml4v = (u64 *)KDIRECT((u64)(uintptr)pml4);
     memset(pml4v, 0, 0x1000);
     pdpt = upop();
     if (!pdpt) { upush((DWORD *)pml4); return 0; }
     pdptv = (u64 *)KDIRECT((u64)(uintptr)pdpt);
     memset(pdptv, 0, 0x1000);
     pd0 = upop();
     if (!pd0) { upush((DWORD *)pdpt); upush((DWORD *)pml4); return 0; }
     pd0v = (u64 *)KDIRECT((u64)(uintptr)pd0);
      memcpy(pd0v, KDIRECT((u64)(uintptr)boot_pd0), 0x1000);
      /* Keep only the 2MiB identity mappings the kernel itself needs while
         running under this private PML4 (syscall/interrupt entry): kernel
         image 0-4MiB and the kernel-managed identity range 32-128MiB
         (kheap, legacy mempop frames, userpd pool).  Every other block in
         the private PD0 is user-private VA (ELF segments, heap, stack,
         syscall stack) and must be mapped as fresh 4KiB pages.  Leaving 2MiB
         identity pages there forces userpd_map_page() to split large pages,
         and stale user-TLB entries for the old 2MiB identity translation can
         then route accesses to the wrong physical frame. */
      {
         int bi;
         for (bi = 0; bi < 512; bi++) {
            int keep = 0;
            if (bi >= 0 && bi <= 1) keep = 1;
            if (bi >= 16 && bi <= 63) keep = 1;
            if (!keep)
               pd0v[bi] = 0;
         }
      }
      pdptv[0] = (u64)(uintptr)pd0  | 0x03ULL;
    pdptv[1] = (u64)(uintptr)boot_pd1 | 0x03ULL;
    pdptv[2] = (u64)(uintptr)boot_pd2 | 0x03ULL;
    pdptv[3] = (u64)(uintptr)boot_pd3 | 0x03ULL;
 /* Share the kernel high direct map so kernel frame/page-table management
       stays reachable through KDIRECT() while this PML4 is installed.
       KDIRECT_BASE 0xFFFF800000000000 is PML4 index 256. */
     pml4v[0]  = (u64)(uintptr)pdpt | 0x03ULL;
     pml4v[256] = (u64)(uintptr)boot_pdpt_high | 0x03ULL;
    return pml4;
 }

/* Map the whole user window [base, base+size) with private frames.
   Returns 1 on success, 0 if the pool ran dry (some pages may be mapped). */
int userpd_map_region(u64 *pml4, unsigned long long base, unsigned long long size,
                      unsigned long attb)
{
   unsigned long long va;
   for (va = base & ~0xFFFULL; va < base + size; va += 0x1000)
      if (!userpd_map_page(pml4, va, attb))
         return 0;
   return 1;
}

/* Clone private 4KiB leaves in PDPT[0]. Shared 2MiB identity mappings belong
   to the kernel and are supplied by userpd_create(). */
u64 *userpd_clone_eager(const u64 *parent)
{
   const u64 phys_mask = 0x000FFFFFFFFF000ULL;
   u64 *child;
   u64 *parent_pml4, *parent_pdpt, *parent_pd, *parent_pte;
   int bi, gi;

   if (!userpd_is_private(parent))
      return 0;

   child = userpd_create();
   if (!child)
      return 0;

   parent_pml4 = (u64 *)KDIRECT((u64)(uintptr)parent & phys_mask);
   if (!(parent_pml4[0] & PG_PRESENT) || (parent_pml4[0] & 0x80ULL))
      goto fail;
   parent_pdpt = (u64 *)KDIRECT(parent_pml4[0] & phys_mask);
   if (!(parent_pdpt[0] & PG_PRESENT) || (parent_pdpt[0] & 0x80ULL))
      goto fail;
   parent_pd = (u64 *)KDIRECT(parent_pdpt[0] & phys_mask);

   for (bi = 0; bi < 512; bi++) {
      u64 pde = parent_pd[bi];
      if (!(pde & PG_PRESENT) || (pde & 0x80ULL))
         continue;
      parent_pte = (u64 *)KDIRECT(pde & phys_mask);
      for (gi = 0; gi < 512; gi++) {
         u64 entry = parent_pte[gi];
         u64 *child_frame;
         unsigned long long va;
         unsigned long leaf_flags;

         if (!(entry & PG_PRESENT))
            continue;
         va = ((unsigned long long)bi << 21)
            | ((unsigned long long)gi << 12);
         leaf_flags = (unsigned long)(entry & ~phys_mask);
         child_frame = userpd_map_page(child, va,
                                       leaf_flags & ~PG_PRESENT);
         if (!child_frame)
            goto fail;
         memcpy(KDIRECT((u64)(uintptr)child_frame),
                KDIRECT(entry & phys_mask), 0x1000);
      }
   }
   return child;

fail:
   userpd_free(child);
   return 0;
}

/* Share writable user leaves read-only between parent and child. Parent PTEs
   are changed only after every child table and frame reference is secured. */
u64 *userpd_clone_cow(u64 *parent, unsigned long long private_vaddr)
{
   const u64 phys_mask = 0x000FFFFFFFFF000ULL;
   const u64 cow_new = 0x800ULL;
   u64 *child, *parent_pml4, *parent_pdpt, *parent_pd;
   u64 *child_pml4, *child_pdpt, *child_pd;
   DWORD flags;
   int bi, gi;

   if (!userpd_is_private(parent))
      return 0;
   if (cow_trace_count < 4)
      printf("COW_TRACE clone-begin cr3=%llx\n",
             (unsigned long long)(uintptr)parent);
   child = userpd_create();
   if (!child)
      return 0;

   storeflags(&flags);
   stopints();
   spin_lock(&userpd_lock);
   parent_pml4 = (u64 *)KDIRECT((u64)(uintptr)parent & phys_mask);
   child_pml4 = (u64 *)KDIRECT((u64)(uintptr)child & phys_mask);
   if (!(parent_pml4[0] & PG_PRESENT) || !(child_pml4[0] & PG_PRESENT))
      goto fail;
   parent_pdpt = (u64 *)KDIRECT(parent_pml4[0] & phys_mask);
   child_pdpt = (u64 *)KDIRECT(child_pml4[0] & phys_mask);
   if (!(parent_pdpt[0] & PG_PRESENT) || !(child_pdpt[0] & PG_PRESENT))
      goto fail;
   parent_pd = (u64 *)KDIRECT(parent_pdpt[0] & phys_mask);
   child_pd = (u64 *)KDIRECT(child_pdpt[0] & phys_mask);

   for (bi = 0; bi < 512; bi++) {
      u64 pde = parent_pd[bi];
      u64 *parent_pte, *child_pte, *table;
      if (!(pde & PG_PRESENT) || (pde & PG_PAGESIZE))
         continue;
      table = upop();
      if (!table)
         goto fail;
      child_pte = (u64 *)KDIRECT((u64)(uintptr)table);
      memset(child_pte, 0, 0x1000);
      child_pd[bi] = (u64)(uintptr)table | (pde & ~phys_mask);
      parent_pte = (u64 *)KDIRECT(pde & phys_mask);
      for (gi = 0; gi < 512; gi++) {
         u64 entry = parent_pte[gi];
         unsigned long long va;
         if (!(entry & PG_PRESENT))
            continue;
         va = ((unsigned long long)bi << 21)
            | ((unsigned long long)gi << 12);
          if ((va & ~0xFFFULL) == (private_vaddr & ~0xFFFULL)
             || (va >= MEM_USER_STACK - 0x100000ULL
                && va < MEM_USER_STACK)
             || (va >= MEM_SYSCALL_STACK
                && va < MEM_SYSCALL_STACK + 0x80000ULL)) {
            u64 private_phys = frame_alloc();
            if (!private_phys)
               goto fail;
            memcpy(KDIRECT(private_phys), KDIRECT(entry & phys_mask), 0x1000);
            child_pte[gi] = private_phys | (entry & ~phys_mask);
            continue;
         }
         if (!frame_retain(entry & phys_mask))
            goto fail;
         child_pte[gi] = entry;
      }
   }

   if (cow_trace_count < 4)
      printf("COW_TRACE clone-shared child=%llx\n",
             (unsigned long long)(uintptr)child);

   for (bi = 0; bi < 512; bi++) {
      u64 pde = child_pd[bi];
      u64 *parent_pte, *child_pte;
      if (!(pde & PG_PRESENT) || (pde & PG_PAGESIZE))
         continue;
      parent_pte = (u64 *)KDIRECT(parent_pd[bi] & phys_mask);
      child_pte = (u64 *)KDIRECT(pde & phys_mask);
      for (gi = 0; gi < 512; gi++) {
         u64 entry = child_pte[gi];
          unsigned long long va = ((unsigned long long)bi << 21)
                           | ((unsigned long long)gi << 12);
          if ((va & ~0xFFFULL) == (private_vaddr & ~0xFFFULL)
             || (va >= MEM_USER_STACK - 0x100000ULL
                && va < MEM_USER_STACK)
             || (va >= MEM_SYSCALL_STACK
                && va < MEM_SYSCALL_STACK + 0x80000ULL))
            continue;
         if ((entry & (PG_PRESENT | PG_WR)) == (PG_PRESENT | PG_WR)) {
            entry = (entry & ~PG_WR) | PG_COPYWRITE;
            parent_pte[gi] = entry;
            child_pte[gi] = entry | cow_new;
         }
      }
   }
   if (cow_trace_count < 4)
      printf("COW_TRACE clone-protected\n");
   if (!smp_tlb_shootdown((u64)(uintptr)parent)) {
      for (bi = 0; bi < 512; bi++) {
         u64 pde = child_pd[bi];
         u64 *parent_pte, *child_pte;
         if (!(pde & PG_PRESENT) || (pde & PG_PAGESIZE))
            continue;
         parent_pte = (u64 *)KDIRECT(parent_pd[bi] & phys_mask);
         child_pte = (u64 *)KDIRECT(pde & phys_mask);
         for (gi = 0; gi < 512; gi++) {
            if (child_pte[gi] & cow_new)
               parent_pte[gi] = (parent_pte[gi] | PG_WR) & ~PG_COPYWRITE;
         }
      }
      smp_tlb_shootdown((u64)(uintptr)parent);
      goto fail;
   }
   cow_shootdowns++;
   for (bi = 0; bi < 512; bi++) {
      u64 pde = child_pd[bi];
      u64 *child_pte;
      if (!(pde & PG_PRESENT) || (pde & PG_PAGESIZE))
         continue;
      child_pte = (u64 *)KDIRECT(pde & phys_mask);
      for (gi = 0; gi < 512; gi++)
         child_pte[gi] &= ~cow_new;
   }
   spin_unlock(&userpd_lock);
   restoreflags(flags);
   if (cow_trace_count < 4) {
      printf("COW_TRACE clone-done\n");
      cow_trace_count++;
   }
   return child;

fail:
   spin_unlock(&userpd_lock);
   restoreflags(flags);
   userpd_free(child);
   return 0;
}

/* Resolve a present write-protection fault on a software-COW user leaf.
   Returns 1 when handled, 0 when the fault is not COW, and -1 on OOM. */
int userpd_handle_cow(u64 *pml4, unsigned long long vaddr, unsigned fault_info)
{
   const u64 phys_mask = 0x000FFFFFFFFF000ULL;
   u64 *pml4v, *pdpt, *pd, *pte, entry, oldphys, newphys;
   DWORD flags;
   unsigned refs;
   int result = 0;
   int trace_action = 0;

   if (!userpd_is_private(pml4) || vaddr >= 0x40000000ULL
       || (fault_info & 3) != 3)
      return 0;
   storeflags(&flags);
   stopints();
   spin_lock(&userpd_lock);
   pml4v = (u64 *)KDIRECT((u64)(uintptr)pml4 & phys_mask);
   entry = pml4v[(vaddr >> 39) & 0x1FF];
   if (!(entry & PG_PRESENT) || (entry & PG_PAGESIZE))
      goto out;
   pdpt = (u64 *)KDIRECT(entry & phys_mask);
   entry = pdpt[(vaddr >> 30) & 0x1FF];
   if (!(entry & PG_PRESENT) || (entry & PG_PAGESIZE))
      goto out;
   pd = (u64 *)KDIRECT(entry & phys_mask);
   entry = pd[(vaddr >> 21) & 0x1FF];
   if (!(entry & PG_PRESENT) || (entry & PG_PAGESIZE))
      goto out;
   pte = (u64 *)KDIRECT(entry & phys_mask);
   pte += (vaddr >> 12) & 0x1FF;
   entry = *pte;
   if (!(entry & PG_PRESENT) || !(entry & PG_COPYWRITE) || (entry & PG_WR))
      goto out;

   cow_faults++;
   oldphys = entry & phys_mask;
   refs = frame_refcount(oldphys);
   if (refs == 1) {
      *pte = (entry | PG_WR) & ~PG_COPYWRITE;
      cow_fastpaths++;
      trace_action = 1;
      result = 1;
      goto flush;
   }
   if (refs < 2)
      goto out;
   if (__sync_bool_compare_and_swap(&cow_fail_next,1,0)) {
      cow_oom++;
      serial_puts("COW_FAULT oom-injected\n");
      result = -1;
      goto out;
   }
   newphys = frame_alloc();
   if (!newphys) {
      cow_oom++;
      result = -1;
      goto out;
   }
   memcpy(KDIRECT(newphys), KDIRECT(oldphys), 0x1000);
   *pte = newphys | ((entry | PG_WR) & ~(phys_mask | PG_COPYWRITE));
   cow_copies++;
   trace_action = 2;
   result = 1;

flush:
   if (!smp_tlb_shootdown((u64)(uintptr)pml4))
      result = -1;
   else {
      if (trace_action == 2)
         frame_release(oldphys);
      cow_shootdowns++;
      if (cow_fault_trace_count < 16) {
         serial_puts(trace_action == 1 ? "COW_FAULT fast\n"
                                      : "COW_FAULT copy\n");
         cow_fault_trace_count++;
      }
   }
out:
   spin_unlock(&userpd_lock);
   restoreflags(flags);
   return result;
}

void userpd_cow_fail_next(void)
{
   cow_fail_next = 1;
}

void userpd_cow_stats(u64 *faults, u64 *copies, u64 *fastpaths,
                      u64 *shootdowns, u64 *oom)
{
   if (faults) *faults = cow_faults;
   if (copies) *copies = cow_copies;
   if (fastpaths) *fastpaths = cow_fastpaths;
   if (shootdowns) *shootdowns = cow_shootdowns;
   if (oom) *oom = cow_oom;
}

#ifdef __x86_64__
static u64 gpf_probe_va, gpf_probe_cr3, gpf_probe_rip;
static int gpf_probe_ready;

void gpf_probe_store(unsigned long va, unsigned long cr3, unsigned long rip)
{
   gpf_probe_va  = (u64)va;
   gpf_probe_cr3 = (u64)cr3;
   gpf_probe_rip = (u64)rip;
   gpf_probe_ready = 1;
}
#endif

/* Free a private PML4 by walking its page tables and returning every
    frame that was allocated for it to the global pool via frame_release().
   Only the private tables are freed: pml4, the private PDPT (pml4[0]),
   the private PD0 (pdpt[0]), any 4KiB PTE tables userpd_map_page() created
   in PD0, and the 4KiB leaf frames those PTE tables map.  The shared
   boot_pd1..3 (pdpt[1..3]) and the 2MiB PS identity pages in PD0 are NOT
   freed — they are the kernel's own mappings. */
void userpd_free(u64 *pml4)
 {
    u64 *pdpt, *pd0, *pte;
    u64 *pml4v, *pdptv, *pd0v, *ptev;
    unsigned long long cr3;
    int i, j, freed = 0;

    if (!pml4)
       return;

   pml4v = (u64 *)KDIRECT((u64)(uintptr)pml4 & 0x000FFFFFFFFF000ULL);

    /* Record for post-mortem correlation WITHOUT serial I/O (see ring note). */
    freed_pml4_record((u64)(uintptr)pml4 & 0x000FFFFFFFFF000ULL);

    /* Never free tables that are still installed as CR3. */
    __asm__ __volatile__("movq %%cr3, %0" : "=r"(cr3));
    if ((cr3 & ~0xFFFULL) == ((unsigned long long)(uintptr)pml4 & ~0xFFFULL)) {
       if (!pagedir1)
          return;   /* cannot switch away; abort to protect live tables */
       __asm__ __volatile__("movq %0, %%cr3" :: "r"((unsigned long)(uintptr)pagedir1) : "memory");
    }

#ifdef __x86_64__
    if (gpf_probe_ready && gpf_probe_cr3 == (u64)(uintptr)pml4) {
        unsigned long long va = (unsigned long long)gpf_probe_va;
        unsigned long long pmi = (va >> 39) & 511;
        unsigned long long pi  = (va >> 30) & 511;
        unsigned long long bi  = (va >> 21) & 511;
        unsigned long long p4  = (va >> 12) & 511;
        unsigned long long pml4e, pdpte, pde, pte;
        unsigned long long pdptaddr, pdaddr, ptaddr;
        u64 *pdptp, *pdp, *ptep;
        int walked = 0;

        printf("GPFPROBE: ready va=0x%llx rip=0x%llx pml4=0x%llx\n",
               va, (unsigned long long)gpf_probe_rip,
               (unsigned long long)(uintptr)pml4);
        pml4e = pml4v[pmi];
        printf("GPFPROBE: PML4[%llu]=0x%llx\n", pmi, pml4e);
        if (!(pml4e & 1)) {
           printf("GPFPROBE: PML4 absent\n");
           walked = 1;
        } else {
           pdptaddr = pml4e & 0x000FFFFFFFFF000ULL;
           if (pdptaddr == 0 || pdptaddr >= 0x100000000ULL) {
              printf("GPFPROBE: PML4 phys bad=0x%llx\n", pdptaddr);
              walked = 1;
           } else {
              pdptp = (u64 *)KDIRECT(pdptaddr);
              pdpte = pdptp[pi];
              printf("GPFPROBE: PDPT[%llu]=0x%llx\n", pi, pdpte);
              if (!(pdpte & 1)) {
                 printf("GPFPROBE: PDPT absent\n");
                 walked = 1;
              } else if (pdpte & 0x80) {
                 printf("GPFPROBE: PDPT 1GiB page\n");
                 walked = 1;
              } else {
                 pdaddr = pdpte & 0x000FFFFFFFFF000ULL;
                 if (pdaddr == 0 || pdaddr >= 0x100000000ULL) {
                    printf("GPFPROBE: PDPT phys bad=0x%llx\n", pdaddr);
                    walked = 1;
                 } else {
                    pdp = (u64 *)KDIRECT(pdaddr);
                    pde = pdp[bi];
                    printf("GPFPROBE: PD[%llu]=0x%llx\n", bi, pde);
                    if (!(pde & 1)) {
                       printf("GPFPROBE: PD absent\n");
                       walked = 1;
                    } else if (pde & 0x80) {
                       printf("GPFPROBE: PD 2MiB page\n");
                       walked = 1;
                    } else {
                       ptaddr = pde & 0x000FFFFFFFFF000ULL;
                       if (ptaddr == 0 || ptaddr >= 0x100000000ULL) {
                          printf("GPFPROBE: PD phys bad=0x%llx\n", ptaddr);
                          walked = 1;
                       } else {
                          ptep = (u64 *)KDIRECT(ptaddr);
                          pte = ptep[p4];
                          printf("GPFPROBE: PT[%llu]=0x%llx\n", p4, pte);
                          walked = 1;
                       }
                    }
                 }
              }
           }
        }
        if (walked)
           gpf_probe_ready = 0;
     }
 #endif

     /* pml4[0] is the only entry userpd_create() sets. */
     if (!(pml4v[0] & 1)) {
       frame_release((u64)(uintptr)pml4);
       return;
    }
    pdpt = (u64 *)(pml4v[0] & 0x000FFFFFFFFF000ULL);
    pdptv = (u64 *)KDIRECT((u64)(uintptr)pdpt);

    /* pdpt[0] is the private PD0; pdpt[1..3] are the shared boot_pd1..3. */
    pd0 = 0;
    if ((pdptv[0] & 1) && !(pdptv[0] & 0x80)) {
       pd0 = (u64 *)(pdptv[0] & 0x000FFFFFFFFF000ULL);
       pd0v = (u64 *)KDIRECT((u64)(uintptr)pd0);
    }

    /* Walk the private PD0. A present, non-PS entry is a 4KiB PTE table we
       allocated; free its leaf frames (present, non-PS) then the table.  A
       present PS entry is a shared 2MiB identity page — leave it alone. */
    if (pd0) {
       for (i = 0; i < 512; i++) {
          u64 de = pd0v[i];
          if ((de & 1) && !(de & 0x80)) {
             pte = (u64 *)(de & 0x000FFFFFFFFF000ULL);
             ptev = (u64 *)KDIRECT((u64)(uintptr)pte);
             for (j = 0; j < 512; j++) {
                u64 le = ptev[j];
                if ((le & 1) && !(le & 0x80)) {
                   frame_release(le & 0x000FFFFFFFFF000ULL);
                   freed++;
                }
             }
             frame_release((u64)(uintptr)pte);
             freed++;
          }
       }
       frame_release((u64)(uintptr)pd0);
       freed++;
    }
    frame_release((u64)(uintptr)pdpt);
    frame_release((u64)(uintptr)pml4);
    freed += 2;

    printf("userpd: freed %d frames, free %llu/%llu\n",
           freed,
           (unsigned long long)frame_free_count(),
           (unsigned long long)frame_total_count());
 }

void mem_init()
{
#ifdef __x86_64__
    extern u64 boot_pml4[];
    /* Long mode already has an identity map via boot_pml4; keep CR3. */
    pagedir1 = (DWORD *)(uintptr)boot_pml4;
    (void)pagedir1;
#else
    DWORD i;
    char temp[255];
    pagedir1=mempop(); //obtain the first pagedirectory

    clearpagetable(pagedir1);
    /* Identity-map low memory as writable. GDT/IDT, the free-page stack
       (0x200000) and the kernel image all live here; a read-only map
       page-faults on CPUs that honor CR0.WP. */
    for (i=0;i<0xB8000;i+=0x1000)
        maplineartophysical((DWORD*)pagedir1,i,i        /*,stackbase*/,1 | PG_WR );
    for (i=0xb8000;i<0x100000;i+=0x1000)
        maplineartophysical((DWORD*)pagedir1,i,i        /*,stackbase*/,1 | PG_USER | PG_WR );
    for (i=0x100000;i<0x1000000;i+=0x1000)
        maplineartophysical((DWORD*)pagedir1,i,i        /*,stackbase*/,1 | PG_WR );

    maplineartophysical((DWORD*)pagedir1,(DWORD)SYS_PAGEDIR_VIR,(DWORD)pagedir1    /*,stackbase*/,1);
    maplineartophysical((DWORD*)pagedir1,(DWORD)SYS_PAGEDIR2_VIR,
    (DWORD)pagedir1[SYS_PAGEDIR_VIR >> 22]&0xFFFFF000,1);
    maplineartophysical((DWORD*)pagedir1,(DWORD)SYS_PAGEDIR3_VIR,
    (DWORD)pagedir1[SYS_PAGEDIR_VIR >> 22]&0xFFFFF000,1);
    maplineartophysical((DWORD*)pagedir1,(DWORD)SYS_KERPDIR_VIR,(DWORD)pagedir1    /*,stackbase*/,1);

    setpagedir(pagedir1);
    enablepaging();
#endif
};

/*This function registers the memory amanger to the device manager so that it's
  interface could be used by other device drivers*/
void mem_register()
{
devmgr_mem memory_manager;

//Set up the usual things the device manager needs
memory_manager.hdr.size = sizeof(devmgr_mem);
strcpy(memory_manager.hdr.name,"mem_mgr");
strcpy(memory_manager.hdr.description,"DEX low-level memory manager");
memory_manager.hdr.type = DEVMGR_MEM;

/* Fill up the service functions that the device manager provices, this
   functions will be visible to other modules that use the mem_mgr interface*/
   
memory_manager.sbrk = sbrk;
memory_manager.mem_map = maplineartophysical2;
memory_manager.commit = commit;
memory_manager.freemultiple = freemultiple;

/*register myself to the device manager*/
devmgr_register( (devmgr_generic*) &memory_manager );

};

