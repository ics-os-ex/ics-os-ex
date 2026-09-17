/*
 * Portions derived from Linux rtw88 (GPL-2.0 OR BSD-3-Clause), Realtek / Larry Finger et al.
 */
#ifndef ICSOS_RTW_MAC_H
#define ICSOS_RTW_MAC_H

#include "rtw_dev.h"

int rtw_mac_init_post_fw(struct rtw_dev *rtwdev);
int rtw_phy_basic_init(struct rtw_dev *rtwdev);
void rtw_mac_write_addr(struct rtw_dev *rtwdev, const u8 mac[6]);

#endif
