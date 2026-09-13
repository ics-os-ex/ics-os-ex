#ifndef ICSOS_SMP_H
#define ICSOS_SMP_H

#include "../types.h"
#include "spinlock.h"

#define MAX_CPUS 8
#define IPI_RESCHEDULE 0xFC
#define IPI_TLB_SHOOTDOWN 0xFB

struct _PCB386;

typedef struct cpu_local {
    int cpu_id;
    u32 apic_id;
    int online;
    struct _PCB386 *current;
    struct _PCB386 *idle;
    void *kernel_stack;
    u64 ticks;
} cpu_local;

extern cpu_local cpus[MAX_CPUS];
extern int cpu_count;
extern int smp_have_rdtscp;
/* Per-CPU IRQ stack (MEM_CPUIRQ_*).  FOREIGN user IRQs land here so
   kernel C never runs on another CPU's user stack or process kstack.
   ACCESS_SYS stays on its own stack (continuation across context_switch).
   Nested IRQs must not reset RSP to the top. */
extern u64 irq_safe_stack_top[MAX_CPUS];
extern u64 irq_safe_stack_base[MAX_CPUS];
void smp_gs_publish(int id);
extern spinlock_t sched_lock;

void smp_init(void);
 void smp_start_aps(void);
 void smp_park_aps(void);
int  smp_cpu_id(void);
cpu_local *smp_this_cpu(void);
void smp_cpu_idle(void);
void smp_repair_stale_current(void);
void smp_reschedule_others(void);
int smp_tlb_shootdown(u64 cr3);
int smp_tlb_shootdown_all(void);
void smp_tlb_shootdown_ipi(void);
void smp_ap_enable_timer(void);
void smp_enable_scheduling(void);
void smp_start_ap_work_smoke(void);
extern volatile int smp_sched_enabled;

#endif
