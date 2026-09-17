/*
 * Portions derived from Linux rtw88 (GPL-2.0 OR BSD-3-Clause), Realtek / Larry Finger et al.
 * Firmware download via reserved-page TX + DDMA (mac.c subset).
 */
#ifndef ICSOS_RTW_FW_H
#define ICSOS_RTW_FW_H

#include "rtw_dev.h"

/* Load firmware file into rtwdev->fw_data. Returns 0 on success. */
int rtw_fw_load_file(struct rtw_dev *rtwdev, const char *path);

/* Download loaded firmware into chip MCU. Returns 0 if FW_READY. */
int rtw_download_firmware(struct rtw_dev *rtwdev);

void rtw_fw_free(struct rtw_dev *rtwdev);

#endif
