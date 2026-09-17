/*
 * Portions derived from Linux rtw88 (GPL-2.0 OR BSD-3-Clause), Realtek / Larry Finger et al.
 */
#ifndef ICSOS_RTW_IO_H
#define ICSOS_RTW_IO_H

#include "rtw_dev.h"

u8  rtw_read8(struct rtw_dev *d, u32 addr);
u16 rtw_read16(struct rtw_dev *d, u32 addr);
u32 rtw_read32(struct rtw_dev *d, u32 addr);
void rtw_write8(struct rtw_dev *d, u32 addr, u8 v);
void rtw_write16(struct rtw_dev *d, u32 addr, u16 v);
void rtw_write32(struct rtw_dev *d, u32 addr, u32 v);
void rtw_write32_mask(struct rtw_dev *d, u32 addr, u32 mask, u32 data);
u32  rtw_read32_mask(struct rtw_dev *d, u32 addr, u32 mask);
void rtw_write8_set(struct rtw_dev *d, u32 addr, u8 bits);
void rtw_write8_clr(struct rtw_dev *d, u32 addr, u8 bits);
void rtw_write16_set(struct rtw_dev *d, u32 addr, u16 bits);
void rtw_write16_clr(struct rtw_dev *d, u32 addr, u16 bits);
void rtw_write32_set(struct rtw_dev *d, u32 addr, u32 bits);
void rtw_write32_clr(struct rtw_dev *d, u32 addr, u32 bits);
void rtw_udelay(u32 us);
void rtw_mdelay(u32 ms);
int rtw_check_hw_ready(struct rtw_dev *d, u32 addr, u32 mask, u32 target);

#endif
