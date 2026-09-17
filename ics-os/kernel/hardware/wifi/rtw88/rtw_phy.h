/*
 * Portions derived from Linux rtw88 (GPL-2.0 OR BSD-3-Clause), Realtek / Larry Finger et al.
 * File-backed BB/RF/AGC/MAC PHY tables for RTL8821CE (air RX enable).
 *
 * Table format (linux-firmware rtw8821c_*.bin):
 *   Raw flat u32 word array; each entry is a pair { u32 addr, u32 data }.
 *   NO header.  Entries may carry conditional-branch tokens (bit 31 = pos
 *   branch header, bit 30 = neg condition test) and per-table delay tokens
 *   in addition to plain register writes.
 *
 * Per-table writer (matches Linux rtw8821c_table.c RTW_DECL_TABLE_* wiring):
 *   mac  -> 8-bit MMIO (rtw_write8)
 *   agc  -> 32-bit MMIO (rtw_write32)
 *   bb   -> 32-bit MMIO + delay tokens 0xf9..0xfe
 *   rf_a -> SIPI RF write to 0xc90 (path A) + delay tokens 0xfe/0xffe
 * The conditional state machine (IF/ELIF/ELSE/ENDIF) gates each write on
 * the driver's (cut, pkg, intf, rfe) condition.
 */
#ifndef ICSOS_RTW_PHY_H
#define ICSOS_RTW_PHY_H

#include "rtw_dev.h"

struct rtw_phy_tbl {
    u8 *data;
    u32 size;
    int loaded;
};

int  rtw_phy_tbl_load_file(struct rtw_dev *rtwdev, const char *path,
                           struct rtw_phy_tbl *t);
void rtw_phy_tbl_free(struct rtw_phy_tbl *t);
int  rtw_phy_tbl_apply_bb(struct rtw_dev *rtwdev, struct rtw_phy_tbl *t);
int  rtw_phy_tbl_apply_rf(struct rtw_dev *rtwdev, struct rtw_phy_tbl *t);
int  rtw_phy_tbl_apply_agc(struct rtw_dev *rtwdev, struct rtw_phy_tbl *t);
int  rtw_phy_tbl_apply_mac(struct rtw_dev *rtwdev, struct rtw_phy_tbl *t);

int  rtw_phy_init(struct rtw_dev *rtwdev);
int  rtw8821c_phy_set_channel(struct rtw_dev *rtwdev, u8 channel);

#endif
