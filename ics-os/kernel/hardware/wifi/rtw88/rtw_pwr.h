/*
 * Portions derived from Linux rtw88 (GPL-2.0 OR BSD-3-Clause), Realtek / Larry Finger et al.
 */
#ifndef ICSOS_RTW_PWR_H
#define ICSOS_RTW_PWR_H

#include "rtw_dev.h"

#define RTW_PWR_CMD_READ    0x00
#define RTW_PWR_CMD_WRITE   0x01
#define RTW_PWR_CMD_POLLING 0x02
#define RTW_PWR_CMD_DELAY   0x03
#define RTW_PWR_CMD_END     0x04

#define RTW_PWR_ADDR_MAC    0x00
#define RTW_PWR_ADDR_SDIO   0x03

#define RTW_PWR_INTF_SDIO_MSK BIT(0)
#define RTW_PWR_INTF_USB_MSK  BIT(1)
#define RTW_PWR_INTF_PCI_MSK  BIT(2)
#define RTW_PWR_INTF_ALL_MSK  (BIT(0)|BIT(1)|BIT(2)|BIT(3))

#define RTW_PWR_CUT_ALL_MSK   0xFF

#define RTW_PWR_DELAY_US      0
#define RTW_PWR_DELAY_MS      1

#define cut_version_to_mask(cut) (0x1u << ((cut) + 1))

struct rtw_pwr_seq_cmd {
    u32 offset;
    u8  cut_mask;
    u8  intf_mask;
    u8  base;
    u8  cmd;
    u8  mask;
    u8  value;
};

int rtw_mac_power_on(struct rtw_dev *rtwdev);

#endif
