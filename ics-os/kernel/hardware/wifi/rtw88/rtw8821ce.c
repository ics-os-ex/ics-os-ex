/*
 * Portions derived from Linux rtw88 (GPL-2.0 OR BSD-3-Clause), Realtek / Larry Finger et al.
 * RTL8821CE probe / power-on / firmware / efuse / RX bring-up.
 */
#include "rtw8821ce.h"
#include "rtw_dev.h"
#include "rtw_io.h"
#include "rtw_pci.h"
#include "rtw_pwr.h"
#include "rtw_fw.h"
#include "rtw_fw_hdr.h"
#include "rtw_efuse.h"
#include "rtw_mac.h"
#include "rtw_phy.h"
#include "../../../net/wifi.h"
#include "../../../net/softnet.h"

extern int printf(const char *fmt, ...);
extern void *memset(void *s, int c, unsigned int n);
extern void *memcpy(void *d, const void *s, unsigned int n);
extern int strcmp(const char *a, const char *b);
extern void delay(unsigned int w);

static struct rtw_dev g_rtw;
static struct wifi_dev g_wlan0;
static int g_inited;

static const char *fw_paths[] = {
    "/icsos/firmware/rtw88/rtw8821c_fw.bin",
    "/icsos/firmware/rtw88/RTW882~1.BIN",
    "/firmware/rtw88/rtw8821c_fw.bin",
    "firmware/rtw88/rtw8821c_fw.bin",
    0
};

int rtw8821ce_present(void)
{
    return g_rtw.present != 0;
}

int rtw8821ce_fw_ready(void)
{
    return g_rtw.fw_ready != 0;
}

void rtw8821ce_poll(void)
{
    if (!(g_rtw.flags & RTW_FLAG_RX_READY))
        return;
    (void)rtw_pci_rx_poll(&g_rtw, 16);
}

void rtw8821ce_status(void)
{
    u32 cfg1, cr, mcu, rxidx;

    printf("rtw8821ce: present=%d mapped=%d power=%d fw=%d mac=%d rx=%d phy=%d ch=%u\n",
           g_rtw.present,
           (g_rtw.flags & RTW_FLAG_MAPPED) ? 1 : 0,
           (g_rtw.flags & RTW_FLAG_POWERON) ? 1 : 0,
           g_rtw.fw_ready,
           (g_rtw.flags & RTW_FLAG_MAC_READY) ? 1 : 0,
           (g_rtw.flags & RTW_FLAG_RX_READY) ? 1 : 0,
           (g_rtw.flags & RTW_FLAG_PHY_READY) ? 1 : 0,
           g_rtw.channel);
    if (!(g_rtw.flags & RTW_FLAG_MAPPED) || !g_rtw.mmio)
        return;
    cfg1 = rtw_read32(&g_rtw, REG_SYS_CFG1);
    cr = rtw_read32(&g_rtw, REG_CR);
    mcu = rtw_read32(&g_rtw, REG_MCUFW_CTRL);
    rxidx = rtw_read32(&g_rtw, RTK_PCI_RXBD_IDX_MPDUQ);
    printf("rtw8821ce: PCI %u:%u.%u BAR2=0x%llx SYS_CFG1=0x%x cut=%u "
           "CR=0x%x MCUFW=0x%x RXBD_IDX=0x%x\n",
           g_rtw.bus, g_rtw.slot, g_rtw.fn,
           (unsigned long long)g_rtw.mmio_phys, cfg1, g_rtw.cut_version,
           cr, mcu, rxidx);
    printf("rtw8821ce: rx_ok=%u rx_beacon=%u rx_c2h=%u rx_err=%u bss=%u\n",
           g_rtw.rx.rx_ok, g_rtw.rx.rx_beacon, g_rtw.rx.rx_c2h,
           g_rtw.rx.rx_err, g_rtw.bss_count);
    wifi_dump();
}

void rtw8821ce_scan_dump(void)
{
    u32 i;
    if (!g_rtw.bss_count) {
        printf("wifiscan: no BSS yet (phy=%d ch=%u)\n",
               (g_rtw.flags & RTW_FLAG_PHY_READY) ? 1 : 0, g_rtw.channel);
        return;
    }
    for (i = 0; i < g_rtw.bss_count; i++) {
        printf("  [%u] \"%s\" %02x:%02x:%02x:%02x:%02x:%02x\n",
               i, g_rtw.bss[i].ssid,
               g_rtw.bss[i].bssid[0], g_rtw.bss[i].bssid[1],
               g_rtw.bss[i].bssid[2], g_rtw.bss[i].bssid[3],
               g_rtw.bss[i].bssid[4], g_rtw.bss[i].bssid[5]);
    }
}

/*
 * Read-only RX diagnostic. Dumps the full RX-path register set, then samples
 * the HW RXBD write pointer (RTK_PCI_RXBD_IDX_MPDUQ bits [27:16]) across a
 * beacon-rich dwell WITHOUT consuming any frames. The HW pointer only advances
 * when the radio actually delivers RX descriptors, so:
 *   delta > 0  -> frames are arriving (analog/RX-DMA path works); a bss=0
 *                 result then points at the MAC filter / RXBD consumption.
 *   delta == 0 -> nothing is arriving; the analog/RF/RX-DMA/channel path is
 *                 not receiving beacons at all.
 * This is the decisive split for the "PHY_OK but bss=0" symptom.
 */
void rtw8821ce_wifidbg(void)
{
    u32 i, raw, hw, first = 0, last = 0, mn = 0, mx = 0, distinct = 0;
    u32 prev;

    if (!(g_rtw.flags & RTW_FLAG_MAPPED) || !g_rtw.mmio) {
        printf("wifidbg: device not mapped\n");
        return;
    }
    printf("WIFI_DBG_BEGIN ch=%u phy=%d\n",
           g_rtw.channel, (g_rtw.flags & RTW_FLAG_PHY_READY) ? 1 : 0);
    printf("wifidbg CR=0x%x RCR=0x%x RXFLTMAP0=0x%x RXFLTMAP2=0x%x\n",
           rtw_read32(&g_rtw, REG_CR), rtw_read32(&g_rtw, REG_RCR),
           rtw_read32(&g_rtw, REG_RXFLTMAP0), rtw_read32(&g_rtw, REG_RXFLTMAP2));
    printf("wifidbg MACID=%02x:%02x:%02x:%02x:%02x:%02x RXFF_BNDY=0x%x "
           "RXDRVINFO=0x%x\n",
           rtw_read8(&g_rtw, REG_MACID), rtw_read8(&g_rtw, REG_MACID + 1),
           rtw_read8(&g_rtw, REG_MACID + 2), rtw_read8(&g_rtw, REG_MACID + 3),
           rtw_read8(&g_rtw, REG_MACID + 4), rtw_read8(&g_rtw, REG_MACID + 5),
           rtw_read32(&g_rtw, REG_RXFF_BNDY),
           rtw_read8(&g_rtw, REG_RX_DRVINFO_SZ));
    printf("wifidbg RXBD_NUM=0x%x RXBD_DESA=0x%x RXBD_IDX=0x%x "
           "TXDMA_STATUS=0x%x\n",
           rtw_read32(&g_rtw, RTK_PCI_RXBD_NUM_MPDUQ),
           rtw_read32(&g_rtw, RTK_PCI_RXBD_DESA_MPDUQ),
           rtw_read32(&g_rtw, RTK_PCI_RXBD_IDX_MPDUQ),
           rtw_read32(&g_rtw, REG_TXDMA_STATUS));
    printf("wifidbg SYS_FUNC_EN=0x%x RF_CTRL=0x%x WLRF1=0x%x RXPSEL=0x%x\n",
           rtw_read32(&g_rtw, REG_SYS_FUNC_EN), rtw_read32(&g_rtw, REG_RF_CTRL),
           rtw_read32(&g_rtw, REG_WLRF1), rtw_read32(&g_rtw, REG_RXPSEL));
    printf("wifidbg MCUFW=0x%x CR_EXT=0x%x FIFOPAGE_INFO1..5=0x%x,0x%x,0x%x,0x%x,0x%x "
           "RQPN_CTRL2=0x%x\n",
           rtw_read32(&g_rtw, REG_MCUFW_CTRL), rtw_read32(&g_rtw, REG_CR_EXT),
           rtw_read32(&g_rtw, REG_FIFOPAGE_INFO_1), rtw_read32(&g_rtw, REG_FIFOPAGE_INFO_2),
           rtw_read32(&g_rtw, REG_FIFOPAGE_INFO_3), rtw_read32(&g_rtw, REG_FIFOPAGE_INFO_4),
           rtw_read32(&g_rtw, REG_FIFOPAGE_INFO_5), rtw_read32(&g_rtw, REG_RQPN_CTRL_2));

    /* Dwell on the current channel (default 1) and watch the HW write pointer. */
    if (g_rtw.flags & RTW_FLAG_PHY_READY)
        rtw8821c_phy_set_channel(&g_rtw, (u8)(g_rtw.channel ? g_rtw.channel : 1));
    raw = rtw_read32(&g_rtw, RTK_PCI_RXBD_IDX_MPDUQ);
    first = (raw & TRX_BD_HW_IDX_MASK) >> TRX_BD_HW_IDX_SHIFT;
    last = first;
    mn = first;
    mx = first;
    prev = first;
    for (i = 0; i < 40; i++) {
        rtw_mdelay(50);
        raw = rtw_read32(&g_rtw, RTK_PCI_RXBD_IDX_MPDUQ);
        hw = (raw & TRX_BD_HW_IDX_MASK) >> TRX_BD_HW_IDX_SHIFT;
        if (hw != prev) {
            distinct++;
            prev = hw;
        }
        last = hw;
        if (hw < mn) mn = hw;
        if (hw > mx) mx = hw;
    }
    printf("WIFI_DBG dwell ch=%u samples=40 hw_wp first=%u last=%u min=%u max=%u "
           "distinct=%u delta=%u rx_ok=%u rx_err=%u rx_beacon=%u\n",
           g_rtw.channel ? g_rtw.channel : 1,
           first, last, mn, mx, distinct,
           (last > first) ? (last - first) : 0,
           g_rtw.rx.rx_ok, g_rtw.rx.rx_err, g_rtw.rx.rx_beacon);
    printf("WIFI_DBG_END\n");
}

static int rtw8821ce_wifi_start(struct wifi_dev *wdev)
{
    (void)wdev;
    if (!g_rtw.fw_ready)
        return -1;
    wdev->state = WIFI_STATE_INIT;
    return 0;
}

static int rtw8821ce_wifi_stop(struct wifi_dev *wdev)
{
    if (wdev)
        wdev->state = WIFI_STATE_DOWN;
    g_rtw.scanning = 0;
    return 0;
}

static int rtw8821ce_wifi_scan(struct wifi_dev *wdev)
{
    u32 i, ch;

    if (!g_rtw.fw_ready || !(g_rtw.flags & RTW_FLAG_RX_READY)) {
        printf("rtw8821ce: scan requires FW+RX ring\n");
        return -1;
    }
    if (wdev)
        wdev->state = WIFI_STATE_SCANNING;
    g_rtw.scanning = 1;
    g_rtw.bss_count = 0;
    memset(g_rtw.bss, 0, sizeof(g_rtw.bss));
    printf("WIFI_SCAN_START\n");

    if (g_rtw.flags & RTW_FLAG_PHY_READY) {
        /* Full 2.4GHz sweep: dwell per channel, RX poll during dwell. */
        for (ch = 1; ch <= 14; ch++) {
            rtw8821c_phy_set_channel(&g_rtw, (u8)ch);
            for (i = 0; i < 40; i++) {
                rtw_pci_rx_poll(&g_rtw, 32);
                delay(10);
            }
        }
        rtw8821c_phy_set_channel(&g_rtw, 1);
    } else {
        /* No PHY tables: single dwell on the default channel. */
        for (i = 0; i < 200; i++) {
            rtw_pci_rx_poll(&g_rtw, 32);
            delay(10);
        }
    }
    g_rtw.scanning = 0;
    if (wdev)
        wdev->state = WIFI_STATE_INIT;
    printf("WIFI_SCAN_DONE bss=%u rx_ok=%u\n",
           g_rtw.bss_count, g_rtw.rx.rx_ok);
    rtw8821ce_scan_dump();
    return g_rtw.bss_count > 0 ? 0 : -1;
}

static int rtw8821ce_wifi_connect(struct wifi_dev *wdev, const char *ssid)
{
    (void)wdev;
    (void)ssid;
    printf("rtw8821ce: connect not implemented yet\n");
    return -1;
}

static const struct wifi_ops rtw8821ce_wifi_ops = {
    rtw8821ce_wifi_start,
    rtw8821ce_wifi_stop,
    rtw8821ce_wifi_scan,
    rtw8821ce_wifi_connect
};

void rtw8821ce_init(void)
{
    u32 cfg1;
    int i, ret;
    u8 mac[6];

    if (g_inited)
        return;
    g_inited = 1;
    memset(&g_rtw, 0, sizeof(g_rtw));
    g_rtw.sys_func_en = RTW_8821C_SYS_FUNC_EN;

    if (rtw_pci_find(&g_rtw) != 0) {
        printf("rtw88: no RTL8821CE present\n");
        return;
    }
    g_rtw.flags |= RTW_FLAG_PROBED;

    if (rtw_pci_map_bar2(&g_rtw) != 0)
        return;

    cfg1 = rtw_read32(&g_rtw, REG_SYS_CFG1);
    if (cfg1 == 0xFFFFFFFFu) {
        printf("rtw88: SYS_CFG1 read failed (MMIO dead)\n");
        return;
    }
    g_rtw.cut_version = (u8)BIT_GET_CHIP_VER(cfg1);
    printf("RTL8821CE_PROBE_OK PCI %u:%u.%u SYS_CFG1=0x%x cut=%u\n",
           g_rtw.bus, g_rtw.slot, g_rtw.fn, cfg1, g_rtw.cut_version);

    if (rtw_pci_bcn_ring_init(&g_rtw) != 0)
        printf("rtw88: BCN ring init failed (FW may fail)\n");
    if (rtw_pci_rx_ring_init(&g_rtw) != 0)
        printf("rtw88: RX ring init failed\n");

    if (rtw_mac_power_on(&g_rtw) != 0) {
        printf("RTL8821CE_POWER_FAIL\n");
        return;
    }
    printf("RTL8821CE_POWER_OK CR=0x%x\n", rtw_read32(&g_rtw, REG_CR));

    if (rtw_efuse_read_mac(&g_rtw, mac) == 0) {
        memcpy(g_rtw.mac_addr, mac, 6);
        printf("RTL8821CE_EFUSE_OK mac=%02x:%02x:%02x:%02x:%02x:%02x "
               "xtal=0x%x rfe=%u\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
               g_rtw.crystal_cap, g_rtw.rfe_option);
    } else {
        printf("RTL8821CE_EFUSE_FAIL\n");
        mac[0] = 0x02;
        mac[1] = 0x00;
        mac[2] = 0x00;
        mac[3] = 0x88;
        mac[4] = 0x21;
        mac[5] = 0xCE;
        memcpy(g_rtw.mac_addr, mac, 6);
    }

    ret = -1;
    for (i = 0; fw_paths[i]; i++) {
        if (rtw_fw_load_file(&g_rtw, fw_paths[i]) == 0) {
            ret = 0;
            break;
        }
    }
    if (ret != 0) {
        printf("RTL8821CE_FW_MISSING\n");
        goto register_dev;
    }

    if (rtw_download_firmware(&g_rtw) != 0) {
        printf("RTL8821CE_FW_FAIL\n");
        goto register_dev;
    }
    printf("RTL8821CE_FW_OK\n");

    if (rtw_mac_init_post_fw(&g_rtw) != 0)
        goto register_dev;
    rtw_mac_write_addr(&g_rtw, g_rtw.mac_addr);
    (void)rtw_phy_basic_init(&g_rtw);
    rtw_phy_init(&g_rtw);
    if (g_rtw.rx.desc_cpu)
        rtw_pci_rx_ring_enable(&g_rtw);
    /* Ensure softnet tick runs even without virtio-net/rtl8139. */
    softnet_init();

register_dev:
    memset(&g_wlan0, 0, sizeof(g_wlan0));
    memcpy(g_wlan0.name, "wlan0", 6);
    g_wlan0.priv = &g_rtw;
    g_wlan0.ops = &rtw8821ce_wifi_ops;
    g_wlan0.state = g_rtw.fw_ready ? WIFI_STATE_INIT : WIFI_STATE_ERROR;
    memcpy(g_wlan0.addr, g_rtw.mac_addr, 6);
    wifi_register(&g_wlan0);
}
