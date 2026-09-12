#include "smp.h"
#include "lapic.h"
#include "../process/process.h"
#include "../cpu/context.h"

extern int printf(const char *fmt, ...);
extern int sprintf(char *str, const char *fmt, ...);
extern void serial_puts(const char *s);
extern void *memset(void *s, int c, unsigned long n);
extern void *memcpy(void *d, const void *s, unsigned long n);
extern char *strcpy(char *d, const char *s);
extern void storeflags(unsigned int *flags);
extern void restoreflags(unsigned int flags);
extern void stopints(void);
extern void smp_install_trampoline(void);
extern void loadregisters(void);
extern void ap_load_kernel_gdt(void);
extern volatile u64 ap_pml4;
extern volatile u64 ap_stack_top;
extern volatile u64 ap_entry;
extern u64 boot_pml4;
extern DWORD *pagedir1;

cpu_local cpus[MAX_CPUS];
int cpu_count = 1;
spinlock_t sched_lock;
u8 smp_fpu_scratch[MAX_CPUS][512] __attribute__((aligned(16)));

static u8 ap_stacks[MAX_CPUS][65536] __attribute__((aligned(16)));
static PCB386 ap_idle_pcb[MAX_CPUS];
static u8 ap_idle_stacks[MAX_CPUS][8192] __attribute__((aligned(16)));
static volatile int ap_claimed = 0;
static volatile int ap_boot_state = 0;
static volatile int ap_boot_apic = -1;
volatile int smp_sched_enabled = 0;
static volatile u64 tlb_shootdown_cr3[MAX_CPUS];
static volatile u32 tlb_shootdown_targets[MAX_CPUS];
static volatile u32 tlb_shootdown_ack[MAX_CPUS];

/* IA32_TSC_AUX: logical CPU id independent of CR3 and LAPIC MMIO.  After a
   user process loads a private PML4, 0xFEE00000 may not be readable, so
   lapic_get_id() returns 0 and the old smp_cpu_id() treated every AP as the
   BSP.  That made sync_entercrit use the BSP process token (pc_busy
   non-owner leave under make -j4).  RDTSCP reads TSC_AUX without a memory
   walk.  GS cannot hold this: startup/kexec reload %gs from the GDT. */
#define MSR_IA32_TSC_AUX 0xC0000103u

static void smp_publish_cpu_id(int id)
{
    u32 lo = (u32)id;
    u32 hi = 0;
    __asm__ __volatile__("wrmsr"
                         :
                         : "c"(MSR_IA32_TSC_AUX), "a"(lo), "d"(hi)
                         : "memory");
}

static int smp_cpu_id_from_lapic(void)
{
    u32 apic;
    int i;
    if (!lapic_mmio)
        return -1;
    apic = lapic_get_id();
    for (i = 0; i < MAX_CPUS; i++) {
        if (cpus[i].cpu_id == i && cpus[i].apic_id == apic)
            return i;
    }
    return -1;
}

static int smp_have_rdtscp = -1;

static int smp_rdtscp_available(void)
{
    u32 a, b, c, d, maxext;
    if (smp_have_rdtscp >= 0)
        return smp_have_rdtscp;
    __asm__ __volatile__("cpuid"
                         : "=a"(maxext), "=b"(b), "=c"(c), "=d"(d)
                         : "a"(0x80000000u)
                         : "memory");
    if (maxext < 0x80000001u) {
        smp_have_rdtscp = 0;
        return 0;
    }
    __asm__ __volatile__("cpuid"
                         : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                         : "a"(0x80000001u)
                         : "memory");
    smp_have_rdtscp = (d & (1u << 27)) ? 1 : 0;
    return smp_have_rdtscp;
}

int smp_cpu_id(void) {
    int id;
    if (smp_rdtscp_available()) {
        u32 aux, lo, hi;
        __asm__ __volatile__("rdtscp"
                             : "=a"(lo), "=d"(hi), "=c"(aux)
                             :
                             : "memory");
        if (aux < (u32)MAX_CPUS)
            return (int)aux;
    }
    id = smp_cpu_id_from_lapic();
    if (id >= 0)
        return id;
    return 0;
}

static void smp_tlb_process_requests(int cpu)
{
    unsigned long cr3;
    int sender;
    __asm__ __volatile__("movq %%cr3, %0" : "=r"(cr3));
    for (sender = 0; sender < MAX_CPUS; sender++) {
        u32 bit = 1u << cpu;
        u64 requested_cr3;
        if (!(tlb_shootdown_targets[sender] & bit) ||
            (tlb_shootdown_ack[sender] & bit))
            continue;
        requested_cr3 = tlb_shootdown_cr3[sender];
        if (!requested_cr3 ||
            (cr3 & ~0xFFFUL) == (requested_cr3 & ~0xFFFULL))
            __asm__ __volatile__("movq %0, %%cr3" :: "r"(cr3) : "memory");
        __sync_fetch_and_or(&tlb_shootdown_ack[sender], bit);
    }
}

cpu_local *smp_this_cpu(void) {
    return &cpus[smp_cpu_id()];
}

void smp_enable_scheduling(void) {
    smp_sched_enabled = 1;
    /* Give APs time to leave the park loop and enter idle, then nudge. */
    {
        volatile u32 t;
        for (t = 0; t < 1000000; t++)
            __asm__ __volatile__("pause");
    }
    smp_reschedule_others();
}

void smp_cpu_idle(void) {
    /* Arm LAPIC timer only once we are on the idle stack with kernel GDT. */
    if (smp_cpu_id() != 0)
        smp_ap_enable_timer();
    for (;;) {
        __asm__ __volatile__("sti; hlt");
    }
}

/* IPI handler: wake idle/kernel threads so they can claim newly published
   work.  Do not software-context-switch a USER process from this wrapper.
   Stage-1 cooperative certs suppress timer preemption of tools; an IPI
   that still called taskswitch() preempted gcc mid-syscall, resumed with
   IF=1 before iretq (context_switch forces IF on save), and GPF'd in
   reschedwrapper (err=0xe470).  Idle and ACCESS_SYS threads may still
   switch: test-smp work-steal depends on that. */
void smp_reschedule_ipi(void) {
    lapic_eoi();
    if (!smp_sched_enabled)
        return;
    if (current_process && current_process->accesslevel == ACCESS_USER)
        return;
    taskswitch();
}

void smp_tlb_shootdown_ipi(void)
{
    smp_tlb_process_requests(smp_cpu_id());
    lapic_eoi();
}

int smp_tlb_shootdown(u64 cr3)
{
    u32 targets = 0;
    unsigned long local_cr3;
    int cpu, me = smp_cpu_id();
    volatile u32 spins;

    __asm__ __volatile__("movq %%cr3, %0" : "=r"(local_cr3));
    if (!cr3 || (local_cr3 & ~0xFFFUL) == (cr3 & ~0xFFFULL))
        __asm__ __volatile__("movq %0, %%cr3" :: "r"(local_cr3) : "memory");
    if (!lapic_mmio || cpu_count < 2 || (!cr3 && !smp_sched_enabled))
        return 1;
    if (tlb_shootdown_targets[me])
        return 0;
    for (cpu = 0; cpu < cpu_count; cpu++) {
        PCB386 *running;
        if (cpu == me || !cpus[cpu].online)
            continue;
        running = cpus[cpu].current;
        if (cr3 && (!running || running->on_cpu != cpu ||
            (u64)(uintptr)running->pagedirloc != cr3))
            continue;
        targets |= 1u << cpu;
    }
    tlb_shootdown_cr3[me] = cr3;
    tlb_shootdown_ack[me] = 1u << me;
    __sync_synchronize();
    tlb_shootdown_targets[me] = targets;
    __sync_synchronize();
    for (cpu = 0; cpu < cpu_count; cpu++) {
        if (!(targets & (1u << cpu)))
            continue;
        if (!lapic_send_ipi(cpus[cpu].apic_id, IPI_TLB_SHOOTDOWN)) {
            tlb_shootdown_targets[me] = 0;
            return 0;
        }
    }
    for (spins = 0; spins < 10000000u; spins++) {
        if ((tlb_shootdown_ack[me] & targets) == targets) {
            tlb_shootdown_targets[me] = 0;
            return 1;
        }
        smp_tlb_process_requests(me);
        __asm__ __volatile__("pause");
    }
    printf("SMP: TLB shootdown timeout target=%x ack=%x cr3=%llx\n",
            targets, tlb_shootdown_ack[me], (unsigned long long)cr3);
        tlb_shootdown_targets[me] = 0;
    return 0;
}

int smp_tlb_shootdown_all(void)
{
    return smp_tlb_shootdown(0);
}

static volatile u32 ap_work_mask = 0;

static void ap_work_smoke(void) {
    int cpu=smp_cpu_id();
    char record[48];
    sprintf(record,"\nSMP_RESULT cpuid cpu=%d\n",cpu);
    serial_puts(record);
    if (cpu>0 && cpu<MAX_CPUS)
        __sync_fetch_and_or(&ap_work_mask,1u<<cpu);
    sched_block_process(current_process,0);
    taskswitch();
    for (;;)
        __asm__ __volatile__("sti; hlt");
}

void smp_start_ap_work_smoke(void) {
    DWORD wid;
    u32 expected;
    int cpu,failed=0;
    if (cpu_count < 2)
        return;
    ap_work_mask = 0;
    expected=(1u<<cpu_count)-2u;
    for (cpu=1;cpu<cpu_count;cpu++) {
        wid=createkthread_on_cpu((void *)ap_work_smoke,"ap_work",8192,cpu);
        if (!wid) {
            failed=1;
            break;
        }
        {
            int retry;
            for (retry=0;retry<32 && !(ap_work_mask&(1u<<cpu));retry++) {
                volatile u32 t;
                lapic_send_ipi(cpus[cpu].apic_id,IPI_RESCHEDULE);
                for (t=0;t<200000u && !(ap_work_mask&(1u<<cpu));t++)
                    __asm__ __volatile__("pause");
            }
            if (!(ap_work_mask&(1u<<cpu))) {
                printf("SMP: AP work-steal TIMEOUT cpu=%d mask=%x\n",
                       cpu,ap_work_mask);
                return;
            }
        }
    }
    if (failed) {
        printf("SMP: AP work-steal CREATE FAIL cpu=%d\n",cpu);
        return;
    }
    if (ap_work_mask!=expected)
        printf("SMP: AP work-steal TIMEOUT mask=%x expected=%x\n",
               ap_work_mask,expected);
    else {
        char record[80];
        sprintf(record,"\nSMP_RESULT work-steal=ok cpus=%d mask=%x\n",
                cpu_count,ap_work_mask);
        serial_puts(record);
        printf("SMP: AP work-steal ALL OK cpus=%d mask=%x\n",
               cpu_count,ap_work_mask);
    }
}

void smp_ap_enable_timer(void) {
    /* Local APIC already mapped; enable SVR + periodic timer on this CPU. */
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | 0xFF);
    lapic_write(LAPIC_TPR, 0);
    lapic_timer_init(100);
}

void smp_reschedule_others(void) {
    int i, me;
    if (!smp_sched_enabled || cpu_count < 2 || !lapic_mmio)
        return;
    me = smp_cpu_id();
    for (i = 0; i < cpu_count; i++) {
        if (i == me || !cpus[i].online)
            continue;
        lapic_send_ipi(cpus[i].apic_id, IPI_RESCHEDULE);
    }
}

static void ap_prepare_idle(int id) {
    PCB386 *idle = &ap_idle_pcb[id];
    memset(idle, 0, sizeof(*idle));
    idle->processid = 0xFFFF0000u | (DWORD)id;
    idle->accesslevel = ACCESS_SYS;
    idle->status = PS_ATTB_LOCKED | PS_ATTB_UNLOADABLE;
    idle->priority = 0;
    idle->pagedirloc = pagedir1;
    strcpy(idle->name, "cpu_idle");
    idle->regs.EIP = (DWORD)(uintptr)smp_cpu_idle;
    idle->regs.ESP = (DWORD)(uintptr)&ap_idle_stacks[id][8192];
    idle->regs.EFLAGS = 0x202;
    idle->regs.CS = 0x08; /* SYS_CODE_SEL */
    idle->regs.SS = 0x10; /* SYS_DATA_SEL */
    idle->regs.DS = 0x10;
    idle->ctx.rip = (u64)(uintptr)smp_cpu_idle;
    {
        uintptr top = (uintptr)&ap_idle_stacks[id][8192];
        top &= ~(uintptr)15;
        top -= 8;
        idle->regs.ESP = (DWORD)top;
        idle->ctx.rsp = (u64)top;
    }
    idle->ctx.rflags = 0x202;
    idle->ctx.cs = 0x08;
    idle->ctx.ss = 0x10;
    idle->ctx.cr3 = (u64)(uintptr)pagedir1;
    fpu_init_default(&idle->fpu);
    cpus[id].idle = idle;
    cpus[id].current = idle;
    idle->cpu_affinity = id;
    idle->on_cpu = id;
    ps_enqueue(idle);
}

void ap_main(void) {
    int id;
    int apic;

    /* Leave the trampoline GDT; IDT gates use kernel CS selectors. */
    ap_load_kernel_gdt();
    loadregisters(); /* same IDT as BSP */

    apic=(int)lapic_get_id();
    if (ap_boot_state!=1 || ap_boot_apic!=apic) {
        for (;;)
            __asm__ __volatile__("cli; hlt");
    }
    id = __sync_fetch_and_add(&ap_claimed, 1);
    if (id<=0 || id>=MAX_CPUS) {
        printf("SMP: rejected APIC %d; CPU limit %d\n",
               lapic_get_id(),MAX_CPUS);
        for (;;)
            __asm__ __volatile__("cli; hlt");
    }
    smp_publish_cpu_id(id);
    cpus[id].cpu_id = id;
    cpus[id].apic_id = (u32)apic;
    cpus[id].online = 0;
    cpus[id].kernel_stack = &ap_stacks[id][65536];

    /* Enable APIC software enable + spurious vector before parking. */
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | 0xFF);
    lapic_write(LAPIC_TPR, 0);

    ap_prepare_idle(id);

     /* Commit only after the per-CPU and idle scheduler state is complete. A
         BSP timeout changes state 1 to rejected state 3, permanently parking a
         late AP before it can enter scheduling or be mistaken for CPU 0. */
     cpus[id].online=1;
     __sync_synchronize();
     if (!__sync_bool_compare_and_swap(&ap_boot_state,1,2)) {
          cpus[id].online=0;
          ps_dequeue(&ap_idle_pcb[id]);
          for (;;)
                __asm__ __volatile__("cli; hlt");
     }

     printf("SMP: CPU %d (APIC %d) online (parked)\n", id, cpus[id].apic_id);

    /* Wait until BSP enables multi-core scheduling. */
    while (!smp_sched_enabled)
        __asm__ __volatile__("pause");

    /* Enter idle with IF clear; idle arms the timer then sti/hlt. */
    fpu_restore(&ap_idle_pcb[id].fpu);
    __asm__ __volatile__("cli");
    context_load(&ap_idle_pcb[id].ctx);
    /* not reached */
    smp_cpu_idle();
}

void smp_init(void) {
    spin_init(&sched_lock);
    memset(cpus, 0, sizeof(cpus));
    cpus[0].cpu_id = 0;
    cpus[0].apic_id = lapic_get_id();
    cpus[0].online = 1;
    smp_publish_cpu_id(0);
    /* The BSP needs its own per-CPU kernel stack for int 0x30 / timer
       entry: user ELFs run in kernel CS (CPL 0), so those traps are
       same-privilege and never load a TSS RSP0.  Without this, the
       kernel's full call depth runs on the current task's 1 MiB user
       stack and deep syscall chains overflow it (SMP=4 cert PF64 in
       sync_entercrit, orphaned io_devlock).  ap_stacks[0] is never
       used by an AP (APs claim ids 1..MAX_CPUS-1), so it is free. */
    cpus[0].kernel_stack = &ap_stacks[0][65536];
    /* BSP current is set by main/process_init; keep slot ready. */
    {
        extern PCB386 sPCB;
        cpus[0].current = &sPCB;
    }
    cpu_count = 1;
    ap_claimed = 1; /* BSP occupies slot 0 */
    ap_boot_state = 0;
    ap_boot_apic = -1;
    printf("SMP: BSP APIC id=%d\n", cpus[0].apic_id);
}

void smp_start_aps(void) {
    int i;
    unsigned int flags;

    storeflags(&flags);
    stopints();

    smp_install_trampoline();
    ap_pml4 = (u64)(uintptr)&boot_pml4;
    ap_entry = (u64)(uintptr)ap_main;
    *(volatile u64 *)0x8F10 = ap_pml4;
    *(volatile u64 *)0x8F20 = ap_entry;

    for (i = 1; i < MAX_CPUS; i++) {
        int started=0;
        ap_boot_apic=i;
        ap_boot_state=1;
        __sync_synchronize();
        ap_stack_top = (u64)(uintptr)&ap_stacks[i][65536];
        *(volatile u64 *)0x8F18 = ap_stack_top;
        cpus[i].kernel_stack = &ap_stacks[i][65536];

        if (!lapic_send_init((u32)i)) {
            printf("SMP: APIC command timeout target=%d phase=INIT\n",i);
            ap_boot_state=3;
            break;
        }
        {
            volatile u32 t;
            for (t = 0; t < 1000000; t++)
                __asm__ __volatile__("pause");
        }
        if (!lapic_send_sipi((u32)i,0x08)) {
            printf("SMP: APIC command timeout target=%d phase=SIPI\n",i);
            ap_boot_state=3;
            break;
        }
        {
            volatile u32 t;
            for (t=0;t<2000000u && ap_boot_state==1;t++)
                __asm__ __volatile__("pause");
        }
        /* Intel permits a second SIPI when the first did not produce a
           startup acknowledgement. Keep waiting bounded per AP. */
        if (ap_boot_state==1) {
            if (!lapic_send_sipi((u32)i,0x08)) {
                printf("SMP: APIC command timeout target=%d phase=SIPI2\n",i);
                ap_boot_state=3;
                break;
            }
            {
                volatile u32 t;
                for (t=0;t<16000000u && ap_boot_state==1;t++)
                    __asm__ __volatile__("pause");
            }
        }
        if (ap_boot_state==2) {
            started=1;
            cpu_count++;
            printf("SMP: APIC %d started (cpu_count=%d)\n", i, cpu_count);
        } else {
            (void)__sync_bool_compare_and_swap(&ap_boot_state,1,3);
            break;
        }
        if (started) {
            ap_boot_state=0;
            ap_boot_apic=-1;
        }
    }
    {
        char record[48];
        sprintf(record,"\nSMP_RESULT online=%d\n",cpu_count);
        serial_puts(record);
    }
    printf("SMP: %d CPUs online\n", cpu_count);
    restoreflags(flags);
}

/* Reset every online AP with an INIT pulse so it sits in the reset state
   (waiting for SIPI at 0x46000). The kexec trampoline overwrites the running
   kernel image in place and does NOT reset secondary cores, so any AP still
   executing old-kernel code would fault on the clobbered text/data. After the
   INIT the AP runs nothing until the NEW kernel re-STARTs it, so parking here
   is safe and required before a self-host kexec. */
void smp_park_aps(void) {
    int me = smp_cpu_id();
    int i;
    unsigned int flags;

    if (cpu_count < 2 || !lapic_mmio)
        return;

    storeflags(&flags);
    stopints();

    for (i = 1; i < cpu_count; i++) {
        if (!cpus[i].online || i == me)
            continue;
        (void)lapic_send_init((u32)cpus[i].apic_id);
    }

    /* Give the INIT pulses time to land before dropping the AP state. */
    {
        volatile u32 t;
        for (t = 0; t < 1000000; t++)
            __asm__ __volatile__("pause");
    }

    for (i = 1; i < cpu_count; i++) {
        cpus[i].online = 0;
        cpus[i].current = 0;
    }
    smp_sched_enabled = 0;
    cpu_count = 1;

    restoreflags(flags);
    serial_puts("SMP_RESULT park_aps=ok\n");
}
