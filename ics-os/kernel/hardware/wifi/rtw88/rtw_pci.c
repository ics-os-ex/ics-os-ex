/*
 * Portions derived from Linux rtw88 (GPL-2.0 OR BSD-3-Clause), Realtek / Larry Finger et al.
 */
#include "rtw_pci.h"
#include "rtw_io.h"
#include "../../pci_cfg.h"
#include "../../pci_scan.h"
#include "../../dma.h"
#include "../../../net/ieee80211.h"

extern void *malloc(unsigned int);
extern void free(void *p);
extern void *memset(void *s, int c, unsigned int n);
extern void *memcpy(void *d, const void *s, unsigned int n);
extern int memcmp(const void *a, const void *b, unsigned int n);
extern int printf(const char *fmt, ...);
extern int mmio_mark_uncacheable(u64 phys, u64 len);

#define PCI_CMD          0x04
#define PCI_CMD_MEM      0x0002
#define PCI_CMD_MASTER   0x0004
#define PCI_BAR2         0x18

static void pci_write32(u8 bus, u8 dev, u8 fn, u8 off, u32 val)
{
    pci_cfg_outl(PCI_CFG_ADDR, pci_cfg_addr(bus, dev, fn, off));
    pci_cfg_outl(PCI_CFG_DATA, val);
}

int rtw_pci_find(struct rtw_dev *rtwdev)
{
    unsigned char bus, slot, fn, fn_max;
    unsigned int id, vendor, device;

    for (bus = 0; bus < 8; bus++) {
        for (slot = 0; slot < 32; slot++) {
            id = pci_cfg_read32(bus, slot, 0, 0);
            if ((id & 0xFFFF) == 0xFFFF)
                continue;
            fn_max = (unsigned char)pci_slot_fn_count(id & 0xFFFF,
                        pci_cfg_read32(bus, slot, 0, 0x0C));
            for (fn = 0; fn < fn_max; fn++) {
                id = pci_cfg_read32(bus, slot, fn, 0);
                vendor = id & 0xFFFF;
                device = (id >> 16) & 0xFFFF;
                if (vendor != RTW_VENDOR_REALTEK)
                    continue;
                if (device != RTW_DEVICE_8821CE &&
                    device != RTW_DEVICE_8821CE_B)
                    continue;
                rtwdev->bus = bus;
                rtwdev->slot = slot;
                rtwdev->fn = fn;
                rtwdev->present = 1;
                printf("rtw88: found 10ec:%04x at %u:%u.%u\n",
                       device, bus, slot, fn);
                return 0;
            }
        }
    }
    return -1;
}

int rtw_pci_map_bar2(struct rtw_dev *rtwdev)
{
    u32 bar_lo, bar_hi, cmd;
    u64 bar, size;

    bar_lo = pci_cfg_read32(rtwdev->bus, rtwdev->slot, rtwdev->fn, PCI_BAR2);
    if ((bar_lo & 0x1) != 0) {
        printf("rtw88: BAR2 is I/O, expected mem64\n");
        return -1;
    }
    if (((bar_lo >> 1) & 0x3) != 0x2) {
        /* Still accept 32-bit mem BAR if present. */
        bar = (u64)(bar_lo & ~0xFu);
        size = 0x10000; /* probe size unused; use 64KiB window */
    } else {
        bar_hi = pci_cfg_read32(rtwdev->bus, rtwdev->slot, rtwdev->fn,
                                PCI_BAR2 + 4);
        bar = ((u64)bar_hi << 32) | (u64)(bar_lo & ~0xFu);
        size = 0x10000;
    }

    if (!bar || bar == 0xFFFFFFFFFFFFFFFFull) {
        printf("rtw88: invalid BAR2\n");
        return -1;
    }

    cmd = pci_cfg_read16(rtwdev->bus, rtwdev->slot, rtwdev->fn, PCI_CMD);
    cmd |= PCI_CMD_MEM | PCI_CMD_MASTER;
    pci_write32(rtwdev->bus, rtwdev->slot, rtwdev->fn, PCI_CMD, cmd);

    if (!mmio_mark_uncacheable(bar, size)) {
        printf("rtw88: mmio_mark_uncacheable failed for BAR2 0x%llx\n",
               (unsigned long long)bar);
        /* Continue anyway; identity map may already cover it. */
    }

    rtwdev->mmio_phys = bar;
    rtwdev->mmio_len = size;
    rtwdev->mmio = (volatile u8 *)(uintptr)bar;
    rtwdev->flags |= RTW_FLAG_MAPPED;
    printf("rtw88: BAR2 mmio=0x%llx\n", (unsigned long long)bar);
    return 0;
}

int rtw_pci_bcn_ring_init(struct rtw_dev *rtwdev)
{
    dma_region region;
    u32 desc_sz = RTW_TX_BUF_DESC_SZ;
    u32 len = desc_sz; /* single BCN BD entry */

    if (!dma_alloc_coherent(&region, len, 256, ~0ULL, malloc, free)) {
        printf("rtw88: BCN BD alloc failed\n");
        return -1;
    }
    rtwdev->bcn.cpu_addr = region.cpu_addr;
    rtwdev->bcn.alloc_base = region.allocation_base;
    rtwdev->bcn.dma_addr = region.dma_addr;
    rtwdev->bcn.length = len;
    memset(region.cpu_addr, 0, len);

    rtw_write32(rtwdev, RTK_PCI_TXBD_DESA_BCNQ, (u32)region.dma_addr);
    return 0;
}

static void rtw_pci_reset_rx_desc(struct rtw_dev *rtwdev, u32 idx)
{
    u8 *desc = (u8 *)rtwdev->rx.desc_cpu + idx * RTW_RX_BUF_DESC_SZ;
    u16 *d16 = (u16 *)desc;
    u32 *d32 = (u32 *)desc;

    memset(desc, 0, RTW_RX_BUF_DESC_SZ);
    d16[0] = (u16)RTW_RX_BUF_SIZE; /* buf_size */
    d16[1] = 0;                    /* total_pkt_size / rx_tag filled by HW */
    d32[1] = (u32)rtwdev->rx.buf_dma[idx];
}

int rtw_pci_rx_ring_init(struct rtw_dev *rtwdev)
{
    dma_region region;
    u32 ring_sz = RTW_RX_RING_SIZE * RTW_RX_BUF_DESC_SZ;
    u32 i;

    if (!dma_alloc_coherent(&region, ring_sz, 256, ~0ULL, malloc, free)) {
        printf("rtw88: RX BD alloc failed\n");
        return -1;
    }
    memset(region.cpu_addr, 0, ring_sz);
    rtwdev->rx.desc_cpu = region.cpu_addr;
    rtwdev->rx.desc_alloc = region.allocation_base;
    rtwdev->rx.desc_dma = region.dma_addr;
    rtwdev->rx.desc_len = ring_sz;
    rtwdev->rx.rp = 0;
    rtwdev->rx.wp = 0;
    rtwdev->rx.rx_tag = 0;

    for (i = 0; i < RTW_RX_RING_SIZE; i++) {
        dma_region buf;
        if (!dma_alloc_coherent(&buf, RTW_RX_BUF_SIZE, 256, ~0ULL,
                                malloc, free)) {
            printf("rtw88: RX buf[%u] alloc failed\n", i);
            return -1;
        }
        memset(buf.cpu_addr, 0, RTW_RX_BUF_SIZE);
        rtwdev->rx.bufs[i] = (u8 *)buf.cpu_addr;
        rtwdev->rx.buf_alloc[i] = buf.allocation_base;
        rtwdev->rx.buf_dma[i] = buf.dma_addr;
        rtw_pci_reset_rx_desc(rtwdev, i);
    }
    return 0;
}

void rtw_pci_rx_ring_enable(struct rtw_dev *rtwdev)
{
    if (!rtwdev->rx.desc_cpu)
        return;

    rtwdev->rx.rp = 0;
    rtwdev->rx.wp = 0;
    rtw_write16(rtwdev, RTK_PCI_RXBD_NUM_MPDUQ,
                (u16)(RTW_RX_RING_SIZE & TRX_BD_IDX_MASK));
    rtw_write32(rtwdev, RTK_PCI_RXBD_DESA_MPDUQ, (u32)rtwdev->rx.desc_dma);
    rtw_write32(rtwdev, RTK_PCI_TXBD_RWPTR_CLR, 0xffffffffu);
    rtw_write32_set(rtwdev, RTK_PCI_CTRL, BIT_RST_TRXDMA_INTF | BIT_RX_TAG_EN);
    rtwdev->rx.rx_tag = 0;
    rtw_write16(rtwdev, RTK_PCI_RXBD_IDX_MPDUQ, 0);
    rtwdev->flags |= RTW_FLAG_RX_READY;
    printf("RTL8821CE_RX_RING_OK entries=%u buf=%u\n",
           RTW_RX_RING_SIZE, RTW_RX_BUF_SIZE);
}

static u32 rtw_rx_desc_bits(const u8 *rx_desc, u32 word, u32 mask, u32 shift)
{
    u32 v = ((const u32 *)rx_desc)[word];
    return (v >> shift) & mask;
}

static int rtw_ssid_from_beacon(const u8 *frame, u32 len, char *ssid, u32 ssid_max,
                                u8 *bssid)
{
    u16 fc;
    u32 ie_off;
    u32 pos;
    u8 elen;

    if (!frame || len < 36 || !ssid || ssid_max < 2)
        return -1;
    fc = (u16)(frame[0] | ((u16)frame[1] << 8));
    if ((fc & 0x00fc) != IEEE80211_STYPE_BEACON &&
        (fc & 0x00fc) != IEEE80211_STYPE_PROBE_RESP)
        return -1;
    /* addr3 = BSSID at offset 16 */
    memcpy(bssid, frame + 16, 6);
    ie_off = 24 + 12; /* hdr + timestamp(8)+interval(2)+cap(2) */
    if (len < ie_off)
        return -1;
    pos = ie_off;
    while (pos + 2 <= len) {
        elen = frame[pos + 1];
        if (pos + 2 + elen > len)
            break;
        if (frame[pos] == 0) { /* SSID */
            u32 n = elen;
            if (n > ssid_max - 1)
                n = ssid_max - 1;
            if (n == 0) {
                ssid[0] = 0;
                return 0; /* hidden */
            }
            memcpy(ssid, frame + pos + 2, n);
            ssid[n] = 0;
            return 1;
        }
        pos += 2 + elen;
    }
    return -1;
}

static void rtw_scan_note_bss(struct rtw_dev *rtwdev, const char *ssid,
                              const u8 *bssid)
{
    u32 i;

    if (!ssid || !ssid[0])
        return;
    for (i = 0; i < rtwdev->bss_count; i++) {
        if (rtwdev->bss[i].used &&
            memcmp(rtwdev->bss[i].bssid, bssid, 6) == 0)
            return;
    }
    if (rtwdev->bss_count >= RTW_WIFI_BSS_MAX)
        return;
    i = rtwdev->bss_count++;
    memset(&rtwdev->bss[i], 0, sizeof(rtwdev->bss[i]));
    rtwdev->bss[i].used = 1;
    memcpy(rtwdev->bss[i].bssid, bssid, 6);
    {
        u32 n = 0;
        while (ssid[n] && n < RTW_WIFI_SSID_MAX) {
            rtwdev->bss[i].ssid[n] = ssid[n];
            n++;
        }
        rtwdev->bss[i].ssid[n] = 0;
    }
    printf("WIFI_BEACON ssid=\"%s\" bssid=%02x:%02x:%02x:%02x:%02x:%02x\n",
           rtwdev->bss[i].ssid,
           bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
}

u32 rtw_pci_rx_poll(struct rtw_dev *rtwdev, u32 limit)
{
    u32 tmp, cur_wp, count, done = 0;
    u32 cur_rp;
    char ssid[RTW_WIFI_SSID_MAX + 1];
    u8 bssid[6];

    if (!(rtwdev->flags & RTW_FLAG_RX_READY) || !rtwdev->rx.desc_cpu)
        return 0;

    tmp = rtw_read32(rtwdev, RTK_PCI_RXBD_IDX_MPDUQ);
    cur_wp = (tmp & TRX_BD_HW_IDX_MASK) >> TRX_BD_HW_IDX_SHIFT;
    if (cur_wp >= rtwdev->rx.wp)
        count = cur_wp - rtwdev->rx.wp;
    else
        count = RTW_RX_RING_SIZE - (rtwdev->rx.wp - cur_wp);
    if (count > limit)
        count = limit;

    cur_rp = rtwdev->rx.rp;
    while (count--) {
        u8 *rx_desc = rtwdev->rx.bufs[cur_rp];
        u32 pkt_len, drv_info_sz, shift, pkt_offset;
        u32 crc_err, icv_err, is_c2h, physt;
        u8 *frame;

        /* Consume RX tag from BD (optional timeout check). */
        {
            u8 *bd = (u8 *)rtwdev->rx.desc_cpu + cur_rp * RTW_RX_BUF_DESC_SZ;
            u16 total = ((u16 *)bd)[1];
            (void)total;
            rtwdev->rx.rx_tag = (u16)((rtwdev->rx.rx_tag + 1) % 8192);
        }

        pkt_len = rtw_rx_desc_bits(rx_desc, 0, 0x3fff, 0);
        drv_info_sz = rtw_rx_desc_bits(rx_desc, 0, 0xf, 16) * 8;
        shift = rtw_rx_desc_bits(rx_desc, 0, 0x3, 24);
        crc_err = rtw_rx_desc_bits(rx_desc, 0, 1, 14);
        icv_err = rtw_rx_desc_bits(rx_desc, 0, 1, 15);
        physt = rtw_rx_desc_bits(rx_desc, 0, 1, 26);
        is_c2h = rtw_rx_desc_bits(rx_desc, 2, 1, 28);
        (void)physt;

        pkt_offset = RTW_RX_PKT_DESC_SZ + drv_info_sz + shift;
        if (crc_err || icv_err || pkt_len == 0 ||
            pkt_offset + pkt_len > RTW_RX_BUF_SIZE) {
            rtwdev->rx.rx_err++;
            goto next;
        }

        if (is_c2h) {
            rtwdev->rx.rx_c2h++;
            goto next;
        }

        frame = rx_desc + pkt_offset;
        rtwdev->rx.rx_ok++;
        if (rtwdev->rx.rx_ok == 1)
            printf("RTL8821CE_RX_OK first_pkt_len=%u\n", pkt_len);

        if (rtw_ssid_from_beacon(frame, pkt_len, ssid, sizeof(ssid), bssid) > 0) {
            rtwdev->rx.rx_beacon++;
            rtw_scan_note_bss(rtwdev, ssid, bssid);
        }
        done++;

next:
        rtw_pci_reset_rx_desc(rtwdev, cur_rp);
        if (++cur_rp >= RTW_RX_RING_SIZE)
            cur_rp = 0;
    }

    rtwdev->rx.rp = cur_rp;
    rtwdev->rx.wp = cur_rp;
    rtw_write16(rtwdev, RTK_PCI_RXBD_IDX_MPDUQ, (u16)rtwdev->rx.rp);
    return done;
}

/*
 * Simplified reserved-page TX through the BCN queue (Linux
 * rtw_pci_write_data_rsvd_page / rtw_pci_tx_write_data subset).
 * TX descriptor words filled with the essential fields only.
 */
int rtw_pci_write_data_rsvd_page(struct rtw_dev *rtwdev, const u8 *buf,
                                 u32 size)
{
    u8 *pkt;
    u32 total;
    u32 psb_len;
    u16 *bd;
    u32 *bd32;
    dma_region region;
    u32 i;
    u16 chk;

    if (!rtwdev->bcn.cpu_addr)
        return -1;

    total = size + RTW_TX_PKT_DESC_SZ;
    if (!dma_alloc_coherent(&region, total, 256, ~0ULL, malloc, free))
        return -1;

    pkt = (u8 *)region.cpu_addr;
    memset(pkt, 0, RTW_TX_PKT_DESC_SZ);
    memcpy(pkt + RTW_TX_PKT_DESC_SZ, buf, size);

    /* Minimal TX desc (rtw_tx_fill_tx_desc essentials). */
    {
        u32 *w = (u32 *)pkt;
        w[0] = (size & 0xFFFF) | ((RTW_TX_PKT_DESC_SZ & 0xFF) << 16) |
               (1u << 26); /* LS */
        w[1] = (16u /* BEACON qsel */ << 8);
        w[3] = (1u << 8); /* USE_RATE */
        w[8] = (1u << 15); /* EN_HWSEQ */
        /* checksum over first 16 words (8821c) */
        chk = 0;
        for (i = 0; i < 16; i++)
            chk ^= (u16)(w[i] & 0xFFFF) ^ (u16)(w[i] >> 16);
        w[7] = (w[7] & 0xFFFF0000u) | chk;
    }

    bd = (u16 *)rtwdev->bcn.cpu_addr;
    bd32 = (u32 *)rtwdev->bcn.cpu_addr;
    memset(rtwdev->bcn.cpu_addr, 0, rtwdev->bcn.length);
    psb_len = (total - 1) / 128 + 1;
    psb_len |= 1u << RTK_PCI_TXBD_OWN_OFFSET;
    bd[0] = (u16)RTW_TX_PKT_DESC_SZ;      /* buf_size seg0 */
    bd[1] = (u16)psb_len;                 /* psb_len */
    bd32[1] = (u32)region.dma_addr;       /* dma seg0 */
    bd[4] = (u16)size;                    /* buf_size seg1 */
    bd[5] = 0;
    bd32[3] = (u32)(region.dma_addr + RTW_TX_PKT_DESC_SZ);

    /* Free previous payload if any */
    if (rtwdev->bcn.payload_base)
        free(rtwdev->bcn.payload_base);
    rtwdev->bcn.payload = pkt;
    rtwdev->bcn.payload_base = region.allocation_base;
    rtwdev->bcn.payload_dma = region.dma_addr;
    rtwdev->bcn.payload_len = total;

    rtw_write8(rtwdev, RTK_PCI_TXBD_BCN_WORK,
               (u8)(rtw_read8(rtwdev, RTK_PCI_TXBD_BCN_WORK) | BIT_PCI_BCNQ_FLAG));

    /* Brief settle for DMA. */
    rtw_mdelay(1);
    return 0;
}
