/*
  Name: kernel heap management module (high level memory management)
  Copyright: 
  Author: Joseph Emmanuel DL Dayo
  Date: 05/03/04 06:54
  Description: This module handles calls to malloc, free and realloc, it does not
  actually does the memory allocation but serves as a bridge to the custom
  malloc function that the user wants to use.
*/

/* dlmalloc is not SMP-safe.  The old stopints()/restoreflags() pair only
   disabled interrupts on the local CPU, so two CPUs could enter dlmalloc at
   the same time and corrupt the free-list.  This crit is acquired with IRQs
   disabled and held that way, which both serializes the allocator across CPUs
   and prevents the owning CPU from being preempted in the middle of an
   allocation. */
sync_sharedvar kheap_crit;

extern int smp_cpu_id(void);
extern DWORD getprocessid(void);

volatile int kheap_diag_op[8];
volatile int kheap_diag_state[8];
volatile int kheap_diag_pid[8];
volatile unsigned int kheap_diag_size[8];
volatile unsigned long kheap_free_rip[8];
volatile unsigned long kheap_malloc_rip[8];
volatile unsigned long kheap_realloc_rip[8];

volatile unsigned long kheap_ev_ptr[128];
volatile unsigned long kheap_ev_rip[128];
volatile unsigned long kheap_ev_size[128];
volatile int kheap_ev_type[128];
volatile int kheap_ev_cpu[128];
volatile int kheap_ev_pid[128];
volatile unsigned long kheap_ev_seq;

static unsigned long kheap_chunk_size(unsigned long p)
{
    if (p >= 0x02040000UL && p < 0x06000000UL)
      {
       unsigned long s = ((volatile unsigned long *)(p - 8))[0] & ~7UL;
       if (s >= 16 && s < 0x04000000UL)
          return s;
      }
    return 0;
};

static void kheap_event(int type, unsigned long ptr, unsigned long size, unsigned long rip)
{
    unsigned long seq = ++kheap_ev_seq;
    unsigned int idx = (unsigned int)(seq & 127UL);
    kheap_ev_ptr[idx] = ptr;
    kheap_ev_rip[idx] = rip;
    kheap_ev_size[idx] = size;
    kheap_ev_type[idx] = type;
    kheap_ev_cpu[idx] = smp_cpu_id();
    kheap_ev_pid[idx] = (int)getprocessid();
};

static void kheap_diag(int op, int state, unsigned int size)
{
    int me = smp_cpu_id();
    if (me >= 0 && me < 8)
      {
       kheap_diag_op[me] = op;
       kheap_diag_state[me] = state;
       kheap_diag_pid[me] = (int)getprocessid();
       kheap_diag_size[me] = size;
      }
};

/*This functions serves as a brdige to the malloc function, it picks the current
  malloc function being used and then diverts to it, for the actual malloc function
  you have to refer to the module it calls*/
void *malloc(unsigned int size)
{
    unsigned long flags;
    void *val;
    devmgr_malloc_extension *dev_malloc;

    kheap_diag(1, 1, size);
    {
        int dme = smp_cpu_id();
        if (dme >= 0 && dme < 8)
            kheap_malloc_rip[dme] = (unsigned long)(char *)__builtin_return_address(0);
    }
    flags = sync_entercrit_irqsave(&kheap_crit);
    kheap_diag(1, 2, size);

    //Use the default malloc function if there is no other malloc functio available

    if (auxillary_malloc_base == 0)
    {

        val = (void *)dlmalloc(size);

    }
    else
    {
        dev_malloc = (devmgr_malloc_extension*)extension_table[CURRENT_MALLOC].iface;
        val = (void *)bridges_call(dev_malloc,&dev_malloc->malloc,size);
    };

    if (val)
       kheap_event(1, (unsigned long)val, size,
                   kheap_malloc_rip[(smp_cpu_id() >= 0 && smp_cpu_id() < 8) ? smp_cpu_id() : 0]);
    kheap_diag(1, 3, size);
    sync_leavecrit_irqrestore(&kheap_crit, flags);
    kheap_diag(1, 0, 0);

    return val;
};

/*This functions serves as a brdige to the realloc function, it picks the current
  realloc function being used and then diverts to it, for the actual malloc function
  you have to refer to the module it calls*/
void *realloc(void *ptr,unsigned int size)
{
    unsigned long flags;
    void *val;
    devmgr_malloc_extension *dev_malloc;

    kheap_diag(2, 1, size);
    {
        int dme = smp_cpu_id();
        if (dme >= 0 && dme < 8)
            kheap_realloc_rip[dme] = (unsigned long)(char *)__builtin_return_address(0);
    }
    flags = sync_entercrit_irqsave(&kheap_crit);
    kheap_diag(2, 2, size);

    //Use the default realloc function if there is no other malloc module installed.
    if (auxillary_malloc_base == 0)
        val = (void *)dlrealloc(ptr,size);
    else
    if (ptr < auxillary_malloc_base)
    {
        void *memptr;
    /* The old realloc was called when there is another malloc module installed.
        The solution to this is to allocate the new size of memory in the new malloc module,
        copy the data. Unfortunately, if the new size is greater than the size
        currently used, we have a big problem since we do not know how large or how
        small the old data is, memcpy will most likely overstep the bounds of the
        old data. The solution to this of course, is to not use realloc anywhere
        in the kernel :)*/


        dev_malloc = (devmgr_malloc_extension*)extension_table[CURRENT_MALLOC].iface;
        memptr = (void*) bridges_call(dev_malloc,&dev_malloc->malloc,size);
        memcpy(memptr, ptr, size);

        //use the old free
        dlfree(ptr);
        val = memptr;
    }
    else
    {
        dev_malloc = (devmgr_malloc_extension*)extension_table[CURRENT_MALLOC].iface;
        val = (void *)bridges_call(dev_malloc,&dev_malloc->realloc,ptr,size);
    };

   if (val)
       kheap_event(2, (unsigned long)val, size,
                   kheap_realloc_rip[(smp_cpu_id() >= 0 && smp_cpu_id() < 8) ? smp_cpu_id() : 0]);
    kheap_diag(2, 3, size);
    sync_leavecrit_irqrestore(&kheap_crit, flags);
    kheap_diag(2, 0, 0);
    return val;
};

/*This functions serves as a brdige to the free function, it picks the current
  free function being used and then diverts to it, for the actual free function
  you have to refer to the module it calls*/
void free(void *ptr)
{
    unsigned long flags;
    devmgr_malloc_extension *dev_malloc;

    kheap_diag(3, 1, 0);
    {
        int dme = smp_cpu_id();
        if (dme >= 0 && dme < 8)
            kheap_free_rip[dme] = (unsigned long)(char *)__builtin_return_address(0);
    }
    flags = sync_entercrit_irqsave(&kheap_crit);
    kheap_diag(3, 2, 0);
    {
       int fme = smp_cpu_id();
       kheap_event(3, (unsigned long)ptr,
                   kheap_chunk_size((unsigned long)ptr),
                   (fme >= 0 && fme < 8) ? kheap_free_rip[fme] : 0);
    }

    //Use the default realloc function if there is no other malloc module installed.
    if (auxillary_malloc_base == 0 || (uintptr)ptr < auxillary_malloc_base)
    {
        dlfree(ptr);
    }
    else
    {
        dev_malloc = (devmgr_malloc_extension*)extension_table[CURRENT_MALLOC].iface;
        bridges_call(dev_malloc,&dev_malloc->free,ptr);
    };

   kheap_diag(3, 3, 0);
    sync_leavecrit_irqrestore(&kheap_crit, flags);
    kheap_diag(3, 0, 0);
};

void alloc_init(const char *alloc_name)
{
int mydevid;

//Register myself to the extension manager
extension_override(devmgr_getdevicebyname(alloc_name),0);

auxillary_malloc_base = 0;
alloc_ready = 1;
};


