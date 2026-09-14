#include "lapic.h"
#include "lapic_x2.h"

/* Minimal console output without pulling stdlib.h (size_t). */
extern int printf(const char *fmt, ...);

volatile u32 *lapic_mmio = 0;
volatile u64 lapic_expected_base = 0;
static int lapic_x2apic;

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

int lapic_present(void)
{
    return lapic_x2apic || lapic_mmio != 0;
}

u32 lapic_read(u32 reg) {
    if (lapic_x2apic)
        return (u32)rdmsr(lapic_x2apic_msr(reg));
    if (!lapic_mmio) return 0;
    return lapic_mmio[reg / 4];
}

void lapic_write(u32 reg, u32 val) {
    if (lapic_x2apic) {
        wrmsr(lapic_x2apic_msr(reg), val);
        return;
    }
    if (!lapic_mmio) return;
    lapic_mmio[reg / 4] = val;
}

void lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

u32 lapic_get_id(void) {
    u32 v = lapic_read(LAPIC_ID);
    if (lapic_x2apic)
        return v;
    return (v >> 24) & 0xFF;
}

void lapic_init(void) {
    u64 apic_base = rdmsr(0x1B);
    u64 base = apic_base & 0xFFFFF000ULL;

    /* Ensure APIC global enable */
    if (!(apic_base & LAPIC_APIC_BASE_EN)) {
        wrmsr(0x1B, apic_base | LAPIC_APIC_BASE_EN);
        apic_base = rdmsr(0x1B);
        base = apic_base & 0xFFFFF000ULL;
    }

    /* UEFI on Alder Lake enables x2APIC (EXTD). MMIO at FEE00000 is
       then undefined and hangs the N150. Use MSRs instead. */
    lapic_x2apic = (apic_base & LAPIC_APIC_BASE_EXTD) ? 1 : 0;
    if (lapic_x2apic) {
        lapic_mmio = 0;
        lapic_expected_base = base;
        lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | 0xFF);
        lapic_write(LAPIC_TPR, 0);
        printf("LAPIC: x2APIC id=%d\n", lapic_get_id());
        return;
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
    if (!lapic_present() || hz == 0) return;

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
    /* x2APIC ICR has no delivery-pending bit; the WRMSR is synchronous. */
    if (lapic_x2apic)
        return 1;
    for (spins=0;spins<1000000u;spins++) {
        if (!(lapic_read(LAPIC_ICR_LOW)&(1u<<12)))
            return 1;
        __asm__ __volatile__("pause");
    }
    printf("LAPIC: ICR delivery timeout cpu=%d\n",lapic_get_id());
    return 0;
}

static void lapic_icr_write(u32 dest, u32 low)
{
    if (lapic_x2apic) {
        wrmsr(lapic_x2apic_msr(LAPIC_ICR_LOW), ((u64)dest << 32) | low);
        return;
    }
    lapic_write(LAPIC_ICR_HIGH, dest << 24);
    lapic_write(LAPIC_ICR_LOW, low);
}

int lapic_send_ipi(u32 apic_id, u32 vector) {
    if (!lapic_wait_icr_idle()) return 0;
    lapic_icr_write(apic_id, vector);
    return lapic_wait_icr_idle();
}

int lapic_send_init(u32 apic_id) {
    if (!lapic_wait_icr_idle()) return 0;
    lapic_icr_write(apic_id, 0x4500); /* INIT */
    if (!lapic_wait_icr_idle()) return 0;
    if (lapic_x2apic)
        return 1;
    {
        volatile u32 i;
        for (i = 0; i < 100000; i++)
            __asm__ __volatile__("pause");
    }
    lapic_icr_write(apic_id, 0x0500); /* INIT deassert (xAPIC only) */
    return lapic_wait_icr_idle();
}

int lapic_send_sipi(u32 apic_id, u32 vector) {
    if (!lapic_wait_icr_idle()) return 0;
    lapic_icr_write(apic_id, 0x4600 | (vector & 0xFF));
    return lapic_wait_icr_idle();
}
