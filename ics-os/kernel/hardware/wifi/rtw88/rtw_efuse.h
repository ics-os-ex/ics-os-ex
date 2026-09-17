/*
 * Portions derived from Linux rtw88 (GPL-2.0 OR BSD-3-Clause), Realtek / Larry Finger et al.
 */
#ifndef ICSOS_RTW_EFUSE_H
#define ICSOS_RTW_EFUSE_H

#include "rtw_dev.h"

#define RTW_EFUSE_PHY_SIZE   512
#define RTW_EFUSE_LOG_SIZE   512
#define RTW_EFUSE_PROT_SIZE  96
#define RTW_EFUSE_MAC_OFF    0xd0  /* PCIE map->e.mac_addr */

int rtw_efuse_read_mac(struct rtw_dev *rtwdev, u8 mac[6]);

#endif
