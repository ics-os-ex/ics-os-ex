/*
 * Portions derived from Linux rtw88 (GPL-2.0 OR BSD-3-Clause), Realtek / Larry Finger et al.
 */
#ifndef ICSOS_RTW_DEV_H
#define ICSOS_RTW_DEV_H

#include "../../../types.h"
#include "rtw_reg.h"

#define RTW_FLAG_POWERON     0x01
#define RTW_FLAG_FW_RUNNING  0x02
#define RTW_FLAG_MAPPED      0x04
#define RTW_FLAG_PROBED      0x08
#define RTW_FLAG_MAC_READY   0x10
#define RTW_FLAG_RX_READY    0x20
#define RTW_FLAG_PHY_READY   0x40

#define RTW_WIFI_BSS_MAX     16
#define RTW_WIFI_SSID_MAX    32

struct rtw_bcn_ring {
    void *cpu_addr;
    void *alloc_base;
    u64 dma_addr;
    u32 length;
    u8 *payload;          /* TX desc + FW chunk */
    void *payload_base;
    u64 payload_dma;
    u32 payload_len;
};

struct rtw_rx_ring {
    void *desc_cpu;
    void *desc_alloc;
    u64 desc_dma;
    u32 desc_len;
    u8 *bufs[RTW_RX_RING_SIZE];
    void *buf_alloc[RTW_RX_RING_SIZE];
    u64 buf_dma[RTW_RX_RING_SIZE];
    u32 rp;
    u32 wp;
    u16 rx_tag;
    u32 rx_ok;
    u32 rx_beacon;
    u32 rx_c2h;
    u32 rx_err;
};

struct rtw_scan_bss {
    char ssid[RTW_WIFI_SSID_MAX + 1];
    u8 bssid[6];
    u8 channel;
    signed char rssi;
    u8 used;
};

struct rtw_dev {
    volatile u8 *mmio;
    u64 mmio_phys;
    u64 mmio_len;
    u8 bus, slot, fn;
    u8 cut_version;
    u8 flags;
    u8 sys_func_en;
    u8 crystal_cap;
    u8 rfe_option;     /* efuse rfe & 0x1f (RF front-end type, low 5 bits) */
    u8 pkg_type;       /* efuse rfe bit 5 ? 1 : 0 (package type for phy_cond) */
    u8 channel;
    u8 mac_addr[6];
    u8 *fw_data;
    u32 fw_len;
    int fw_ready;
    int present;
    int scanning;
    struct rtw_bcn_ring bcn;
    struct rtw_rx_ring rx;
    struct rtw_scan_bss bss[RTW_WIFI_BSS_MAX];
    u32 bss_count;
};

#endif
