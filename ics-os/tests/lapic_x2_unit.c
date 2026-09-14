/*
  Host TAP for x2APIC MSR numbers (MMIO offset / 16 + 0x800).
*/
#include <stdio.h>

#include "kernel/cpu/lapic_x2.h"

static int check(const char *name, int condition)
{
    if (!condition) {
        printf("not ok - %s\n", name);
        return 0;
    }
    printf("ok - %s\n", name);
    return 1;
}

int main(void)
{
    int ok = 1;

    printf("TAP version 13\n1..6\n");

    ok &= check("ID 0x20 is MSR 0x802",
                lapic_x2apic_msr(0x020) == 0x802u);
    ok &= check("TPR 0x80 is MSR 0x808",
                lapic_x2apic_msr(0x080) == 0x808u);
    ok &= check("EOI 0xB0 is MSR 0x80B",
                lapic_x2apic_msr(0x0B0) == 0x80Bu);
    ok &= check("SVR 0xF0 is MSR 0x80F",
                lapic_x2apic_msr(0x0F0) == 0x80Fu);
    ok &= check("ICR 0x300 is MSR 0x830 (no ICR_HIGH)",
                lapic_x2apic_msr(0x300) == 0x830u);
    ok &= check("timer init 0x380 is MSR 0x838",
                lapic_x2apic_msr(0x380) == 0x838u);

    return ok ? 0 : 1;
}
