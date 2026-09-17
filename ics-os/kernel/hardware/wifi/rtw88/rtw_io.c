/*
 * Portions derived from Linux rtw88 (GPL-2.0 OR BSD-3-Clause), Realtek / Larry Finger et al.
 */
#include "rtw_io.h"
#include "rtw_mask.h"

extern void taskswitch(void);

u8 rtw_read8(struct rtw_dev *d, u32 addr)
{
    return d->mmio[addr];
}

u16 rtw_read16(struct rtw_dev *d, u32 addr)
{
    return *(volatile u16 *)(d->mmio + addr);
}

u32 rtw_read32(struct rtw_dev *d, u32 addr)
{
    return *(volatile u32 *)(d->mmio + addr);
}

void rtw_write8(struct rtw_dev *d, u32 addr, u8 v)
{
    d->mmio[addr] = v;
}

void rtw_write16(struct rtw_dev *d, u32 addr, u16 v)
{
    *(volatile u16 *)(d->mmio + addr) = v;
}

void rtw_write32(struct rtw_dev *d, u32 addr, u32 v)
{
    *(volatile u32 *)(d->mmio + addr) = v;
}

/* Linux rtw88 semantics: data is an unshifted field value. */
void rtw_write32_mask(struct rtw_dev *d, u32 addr, u32 mask, u32 data)
{
    if (!mask)
        return;
    rtw_write32(d, addr, rtw_mask_merge(rtw_read32(d, addr), mask, data));
}

/* Raw 32-bit read masked by the caller's field (no shift; the caller
 * pre-shifted data on write, so it pre-shifts the expected value on
 * compare/poll).
 */
u32 rtw_read32_mask(struct rtw_dev *d, u32 addr, u32 mask)
{
    u32 v;

    if (!mask)
        return 0;
    v = rtw_read32(d, addr);
    return v & mask;
}

void rtw_write8_set(struct rtw_dev *d, u32 addr, u8 bits)
{
    rtw_write8(d, addr, (u8)(rtw_read8(d, addr) | bits));
}

void rtw_write8_clr(struct rtw_dev *d, u32 addr, u8 bits)
{
    rtw_write8(d, addr, (u8)(rtw_read8(d, addr) & ~bits));
}

void rtw_write16_set(struct rtw_dev *d, u32 addr, u16 bits)
{
    rtw_write16(d, addr, (u16)(rtw_read16(d, addr) | bits));
}

void rtw_write16_clr(struct rtw_dev *d, u32 addr, u16 bits)
{
    rtw_write16(d, addr, (u16)(rtw_read16(d, addr) & ~bits));
}

void rtw_write32_set(struct rtw_dev *d, u32 addr, u32 bits)
{
    rtw_write32(d, addr, rtw_read32(d, addr) | bits);
}

void rtw_write32_clr(struct rtw_dev *d, u32 addr, u32 bits)
{
    rtw_write32(d, addr, rtw_read32(d, addr) & ~bits);
}

void rtw_udelay(u32 us)
{
    volatile u32 i;
    /* Rough busy-wait; calibrated loosely for ~GHz cores. */
    while (us--) {
        for (i = 0; i < 200; i++)
            ;
    }
}

void rtw_mdelay(u32 ms)
{
    while (ms--) {
        rtw_udelay(1000);
        taskswitch();
    }
}

int rtw_check_hw_ready(struct rtw_dev *d, u32 addr, u32 mask, u32 target)
{
    u32 i;
    target &= mask;
    for (i = 0; i < 10000; i++) {
        if ((rtw_read32(d, addr) & mask) == target)
            return 1;
        rtw_udelay(50);
        if ((i & 0x1F) == 0)
            taskswitch();
    }
    return 0;
}
