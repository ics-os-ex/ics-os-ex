/*
 * Portions derived from Linux rtw88 (GPL-2.0 OR BSD-3-Clause), Realtek / Larry Finger et al.
 */
#ifndef ICSOS_RTW_PCI_H
#define ICSOS_RTW_PCI_H

#include "rtw_dev.h"

#define RTW_VENDOR_REALTEK  0x10EC
#define RTW_DEVICE_8821CE   0xC821
#define RTW_DEVICE_8821CE_B 0xB821

int rtw_pci_find(struct rtw_dev *rtwdev);
int rtw_pci_map_bar2(struct rtw_dev *rtwdev);
int rtw_pci_bcn_ring_init(struct rtw_dev *rtwdev);
int rtw_pci_rx_ring_init(struct rtw_dev *rtwdev);
void rtw_pci_rx_ring_enable(struct rtw_dev *rtwdev);
u32 rtw_pci_rx_poll(struct rtw_dev *rtwdev, u32 limit);
int rtw_pci_write_data_rsvd_page(struct rtw_dev *rtwdev, const u8 *buf,
                                 u32 size);

#endif
