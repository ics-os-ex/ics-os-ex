#ifndef ICSOS_LAPIC_X2_H
#define ICSOS_LAPIC_X2_H

/* IA32_APIC_BASE (0x1B): bit 10 is EXTD (x2APIC). */
#define LAPIC_APIC_BASE_EXTD (1ULL << 10)
#define LAPIC_APIC_BASE_EN   (1ULL << 11)

/* x2APIC MSRs are 0x800 + (MMIO offset / 16). ICR is one 64-bit MSR
   at 0x830; there is no separate ICR_HIGH. */
static inline unsigned int lapic_x2apic_msr(unsigned int mmio_off)
{
    return 0x800u + (mmio_off >> 4);
}

#endif
