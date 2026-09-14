#ifndef ICSOS_LAPIC_H
#define ICSOS_LAPIC_H

#include "../types.h"

#define LAPIC_ID          0x020
#define LAPIC_VER         0x030
#define LAPIC_TPR         0x080
#define LAPIC_EOI         0x0B0
#define LAPIC_SVR         0x0F0
#define LAPIC_ICR_LOW     0x300
#define LAPIC_ICR_HIGH    0x310
#define LAPIC_LVT_TIMER   0x320
#define LAPIC_TIMER_INIT  0x380
#define LAPIC_TIMER_CUR   0x390
#define LAPIC_TIMER_DIV   0x3E0

#define LAPIC_SVR_ENABLE  0x100
#define LAPIC_TIMER_PERIODIC 0x20000
/* Prefer a dedicated vector so AP timer IRQs do not share PIT 0x20. */
#define LAPIC_TIMER_VECTOR   0x41

void lapic_init(void);
void lapic_eoi(void);
u32  lapic_read(u32 reg);
void lapic_write(u32 reg, u32 val);
u32  lapic_get_id(void);
void lapic_timer_init(u32 hz);
int  lapic_send_ipi(u32 apic_id, u32 vector);
int  lapic_send_init(u32 apic_id);
int  lapic_send_sipi(u32 apic_id, u32 vector);

extern volatile u32 *lapic_mmio;
extern volatile u64 lapic_expected_base;
int lapic_present(void);

#endif
