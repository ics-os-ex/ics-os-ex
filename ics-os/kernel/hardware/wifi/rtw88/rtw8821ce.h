/*
 * Portions derived from Linux rtw88 (GPL-2.0 OR BSD-3-Clause), Realtek / Larry Finger et al.
 * RTL8821CE (PCI 10ec:c821) bring-up for ICS-OS.
 */
#ifndef ICSOS_RTW8821CE_H
#define ICSOS_RTW8821CE_H

void rtw8821ce_init(void);
int  rtw8821ce_present(void);
int  rtw8821ce_fw_ready(void);
void rtw8821ce_poll(void);
void rtw8821ce_status(void);
void rtw8821ce_scan_dump(void);
void rtw8821ce_wifidbg(void);

#endif
