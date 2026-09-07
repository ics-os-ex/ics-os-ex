#include "lapic.h"

/* Minimal console output without pulling stdlib.h (size_t). */
extern int printf(const char *fmt, ...);

volatile u32 *lapic_mmio = 0;
volatile u64 lapic_expected_base = 0;

static u64 rdmsr(u32 msr) {
    u32 lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

static void wrmsr(u32 msr, u64 val) {
    u32 lo = (u32)val;
    u32 hi = (u32)(val >> 32);
    __asm__ __volatile__("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

u32 lapic_read(u32 reg) {
    if (!lapic_mmio) return 0;
    return lapic_mmio[reg / 4];
}

void lapic_write(u32 reg, u32 val) {
    if (!lapic_mmio) return;
    lapic_mmio[reg / 4] = val;
}

void lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

u32 lapic_get_id(void) {
    return (lapic_read(LAPIC_ID) >> 24) & 0xFF;
}

void lapic_init(void) {
    u64 apic_base = rdmsr(0x1B);
    u64 base = apic_base & 0xFFFFF000ULL;

    /* Ensure APIC global enable */
    if (!(apic_base & (1ULL << 11))) {
        wrmsr(0x1B, apic_base | (1ULL << 11));
        apic_base = rdmsr(0x1B);
        base = apic_base & 0xFFFFF000ULL;
    }

    lapic_mmio = (volatile u32 *)(uintptr)base;
    lapic_expected_base = base;

    /* Spurious interrupt vector + enable */
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | 0xFF);
    lapic_write(LAPIC_TPR, 0);
    printf("LAPIC: mmio=0x%X id=%d\n", (u32)base, lapic_get_id());
}

void lapic_timer_init(u32 hz) {
    u32 ticks;
    if (!lapic_mmio || hz == 0) return;

    lapic_write(LAPIC_TIMER_DIV, 0x3); /* divide by 16 */
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR);
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);

    /* Rough calibrate against PIT-ish busy wait (~10ms) */
    {
        volatile u32 i;
        for (i = 0; i < 1000000; i++)
            __asm__ __volatile__("pause");
    }
    ticks = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CUR);
    if (ticks < 1000) ticks = 100000;
    ticks = ticks / 10; /* per ~10ms -> scale to hz */
    if (hz != 100)
        ticks = (ticks * 100) / hz;
    if (ticks < 1000) ticks = 10000;

    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_PERIODIC | LAPIC_TIMER_VECTOR);
    lapic_write(LAPIC_TIMER_DIV, 0x3);
    lapic_write(LAPIC_TIMER_INIT, ticks);
    printf("LAPIC: timer hz=%d init=%d\n", hz, ticks);
}

static int lapic_wait_icr_idle(void) {
    volatile u32 spins;
    for (spins=0;spins<1000000u;spins++) {
        if (!(lapic_read(LAPIC_ICR_LOW)&(1u<<12)))
            return 1;
        __asm__ __volatile__("pause");
    }
    printf("LAPIC: ICR delivery timeout cpu=%d\n",lapic_get_id());
    return 0;
}

int lapic_send_ipi(u32 apic_id, u32 vector) {
    if (!lapic_wait_icr_idle()) return 0;
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, vector);
    return lapic_wait_icr_idle();
}

int lapic_send_init(u32 apic_id) {
    if (!lapic_wait_icr_idle()) return 0;
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, 0x4500); /* INIT level assert */
    if (!lapic_wait_icr_idle()) return 0;
    {
        volatile u32 i;
        for (i = 0; i < 100000; i++)
            __asm__ __volatile__("pause");
    }
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, 0x0500); /* INIT deassert */
    return lapic_wait_icr_idle();
}

int lapic_send_sipi(u32 apic_id, u32 vector) {
    if (!lapic_wait_icr_idle()) return 0;
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, 0x4600 | (vector & 0xFF));
    return lapic_wait_icr_idle();
}
