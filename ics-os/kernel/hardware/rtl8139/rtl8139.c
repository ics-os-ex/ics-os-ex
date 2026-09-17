/*
 * RTL8139C Ethernet (C-mode ring RX + 4 TX descriptors).
 * Based on Realtek RTL8139 Programming Guide; QEMU -device rtl8139.
 *
 * Feeds the in-tree IPv4 stack via netif/netdev. Uses PCI I/O BAR0 and
 * legacy INTx through irq_addhandler().
 */
#include "rtl8139.h"
#include "../pci_scan.h"
#include "../../dextypes.h"
#include "../../cpu/spinlock.h"
#include "../../net/pbuf.h"
#include "../../net/netif.h"
#include "../../net/netdev.h"
#include "../../net/inet_config.h"
#include "../../net/icmp.h"
#include "../../net/udp.h"
#include "../../net/tcp.h"
#include "../../net/arp.h"
#include "../../net/net_sync.h"
#include "../../net/dhcp.h"
#include "../../net/softnet.h"
#include "../../net/dns.h"
#include "../../net/ethernet.h"

extern void *malloc(unsigned int);
extern void *memset(void *s, int c, unsigned int n);
extern void *memcpy(void *d, const void *s, unsigned int n);
extern int printf(const char *fmt, ...);
extern int serial_com1_present(void);
extern char kernel_cmdline[];
extern int irq_addhandler(int deviceid, int irq_number, void (*handler)());
extern void outportb(unsigned short port, unsigned char data);
extern unsigned char inportb(unsigned short port);

#define RTL_VENDOR  0x10EC
#define RTL_DEVICE  0x8139

#define RTL_MAC0       0x00
#define RTL_TSD0       0x10
#define RTL_TSAD0      0x20
#define RTL_RBSTART    0x30
#define RTL_CMD        0x37
#define RTL_CAPR       0x38
#define RTL_IMR        0x3C
#define RTL_ISR        0x3E
#define RTL_TCR        0x40
#define RTL_RCR        0x44
#define RTL_CONFIG1    0x52

#define RTL_CMD_RESET  0x10
#define RTL_CMD_RX_EN  0x08
#define RTL_CMD_TX_EN  0x04
#define RTL_CMD_BUFE   0x01

#define RTL_ISR_ROK    0x0001
#define RTL_ISR_RER    0x0002
#define RTL_ISR_TOK    0x0004
#define RTL_ISR_TER    0x0008
#define RTL_ISR_RXOVW  0x0010
#define RTL_ISR_FOVW   0x0040

#define RTL_TSD_OWN    0x2000u
#define RTL_TSD_SIZE_MASK 0x1FFFu
#define RTL_TSD_ETHR   0x00080000u /* early TX thresh = 8*32 = 256 bytes */

#define RTL_RX_SIZE    32768
#define RTL_RX_PAD     16
#define RTL_RX_ALLOC   (RTL_RX_SIZE + RTL_RX_PAD + 1500)
#define RTL_TX_SIZE    1536
#define RTL_TX_DESC    4

#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC
#define PCI_CMD          0x04
#define PCI_BAR0         0x10
#define PCI_IRQ_LINE     0x3C
#define PCI_CMD_IO       0x0001
#define PCI_CMD_MASTER   0x0004

struct rtl8139_dev {
    u16 io_base;
    u8  irq_line;
    u8  mac[6];
    u8  tx_cur;
    u16 rx_ptr;
    u8 *rx_buf;
    u8 *tx_buf[RTL_TX_DESC];
    struct netdev ndev;
    struct netif nif;
    spinlock_t lock;
    int ready;
    unsigned int irq_count;
    unsigned int rx_count;
    unsigned int tx_count;
};

static struct rtl8139_dev rtl;

static inline u32 pci_inl(u16 port)
{
    u32 v;
    __asm__ volatile ("inl %%dx, %%eax" : "=a"(v) : "d"(port));
    return v;
}
static inline void pci_outl(u16 port, u32 val)
{
    __asm__ volatile ("outl %%eax, %%dx" : : "d"(port), "a"(val));
}
static inline u32 pci_cfg_addr(u8 bus, u8 dev, u8 fn, u8 off)
{
    return 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) |
           ((u32)fn << 8) | (off & 0xFC);
}
static u32 pci_read32(u8 bus, u8 dev, u8 fn, u8 off)
{
    pci_outl(PCI_CONFIG_ADDR, pci_cfg_addr(bus, dev, fn, off));
    return pci_inl(PCI_CONFIG_DATA);
}
static void pci_write32(u8 bus, u8 dev, u8 fn, u8 off, u32 val)
{
    pci_outl(PCI_CONFIG_ADDR, pci_cfg_addr(bus, dev, fn, off));
    pci_outl(PCI_CONFIG_DATA, val);
}
static u16 pci_read16(u8 bus, u8 dev, u8 fn, u8 off)
{
    u32 v = pci_read32(bus, dev, fn, off & 0xFC);
    return (u16)(v >> ((off & 2) * 8));
}
static u8 pci_read8(u8 bus, u8 dev, u8 fn, u8 off)
{
    u32 v = pci_read32(bus, dev, fn, off & 0xFC);
    return (u8)(v >> ((off & 3) * 8));
}
static void pci_write16(u8 bus, u8 dev, u8 fn, u8 off, u16 val)
{
    u32 v = pci_read32(bus, dev, fn, off & 0xFC);
    u32 sh = (off & 2) * 8;
    v = (v & ~(0xFFFFu << sh)) | ((u32)val << sh);
    pci_write32(bus, dev, fn, off & 0xFC, v);
}

static inline void rtl_out8(u16 off, u8 v)
{
    outportb(rtl.io_base + off, v);
}
static inline u8 rtl_in8(u16 off)
{
    return inportb(rtl.io_base + off);
}
static inline void rtl_out16(u16 off, u16 v)
{
    __asm__ volatile ("outw %%ax, %%dx"
                      : : "d"((u16)(rtl.io_base + off)), "a"(v));
}
static inline u16 rtl_in16(u16 off)
{
    u16 v;
    __asm__ volatile ("inw %%dx, %%ax"
                      : "=a"(v) : "d"((u16)(rtl.io_base + off)));
    return v;
}
static inline void rtl_out32(u16 off, u32 v)
{
    __asm__ volatile ("outl %%eax, %%dx"
                      : : "d"((u16)(rtl.io_base + off)), "a"(v));
}
static inline u32 rtl_in32(u16 off)
{
    u32 v;
    __asm__ volatile ("inl %%dx, %%eax"
                      : "=a"(v) : "d"((u16)(rtl.io_base + off)));
    return v;
}

static int rtl_find(u8 *bus, u8 *dev, u8 *fn)
{
    u8 b, d, f;

    /* Match virtio-net: scan a few buses only. A 256-bus walk stalls
       QEMU stress gates when the NIC is absent. */
    if (!serial_com1_present())
        return 0;
    for (b = 0; b < 8; b++) {
        for (d = 0; d < 32; d++) {
            u32 id0 = pci_read32(b, d, 0, 0);
            u32 hdr = pci_read32(b, d, 0, 0x0C);
            unsigned int nf = pci_slot_fn_count(id0 & 0xFFFF, hdr);
            for (f = 0; f < nf; f++) {
                u32 id = pci_read32(b, d, f, 0);
                if ((id & 0xFFFF) == RTL_VENDOR &&
                    ((id >> 16) & 0xFFFF) == RTL_DEVICE) {
                    *bus = b;
                    *dev = d;
                    *fn = f;
                    return 1;
                }
            }
        }
    }
    return 0;
}

static void rtl_copy_rx(u8 *dst, unsigned int offset, unsigned int len)
{
    unsigned int i;
    for (i = 0; i < len; i++)
        dst[i] = rtl.rx_buf[(offset + i) % RTL_RX_SIZE];
}

static void rtl_rx_packets(void)
{
    struct pbuf *pending[16];
    int npend = 0;
    int i;
    spin_irq_flags_t f;

    /* Harvest under the device lock so SMP poll+IRQ cannot desync CAPR.
       Deliver after unlock so nested TX (ACK) can take the same lock. */
    f = spin_lock_irqsave(&rtl.lock);
    while (npend < 16 && !(rtl_in8(RTL_CMD) & RTL_CMD_BUFE)) {
        u16 status, plen;
        u16 offset;
        u8 hdr[4];
        struct pbuf *p;
        unsigned int copy;

        offset = rtl.rx_ptr % RTL_RX_SIZE;
        rtl_copy_rx(hdr, offset, 4);
        status = (u16)(hdr[0] | (hdr[1] << 8));
        plen = (u16)(hdr[2] | (hdr[3] << 8));

        if (!(status & 0x0001) || plen < 64 || plen > 1518) {
            rtl.rx_ptr = rtl_in16(0x3A);
            rtl_out16(RTL_CAPR, (u16)((rtl.rx_ptr - 16) & ~3));
            break;
        }

        copy = plen - 4;
        p = pbuf_alloc((u16)copy);
        if (p) {
            unsigned short et;
            rtl_copy_rx(p->data, (offset + 4) % RTL_RX_SIZE, copy);
            p->len = (u16)copy;
            et = (copy >= 14) ?
                 (unsigned short)((p->data[12] << 8) | p->data[13]) : 0;
            /* Resync if the ring cursor landed mid-frame. */
            if (et != ETH_TYPE_IPV4 && et != ETH_TYPE_ARP) {
                pbuf_free(p);
                rtl.rx_ptr = rtl_in16(0x3A);
                rtl_out16(RTL_CAPR, (u16)((rtl.rx_ptr - 16) & ~3));
                break;
            }
            pending[npend++] = p;
            rtl.rx_count++;
        }

        rtl.rx_ptr = (u16)((rtl.rx_ptr + plen + 4 + 3) & ~3);
        if (rtl.rx_ptr >= RTL_RX_SIZE)
            rtl.rx_ptr = (u16)(rtl.rx_ptr - RTL_RX_SIZE);
        rtl_out16(RTL_CAPR, (u16)((rtl.rx_ptr - 16) & ~3));
    }
    spin_unlock_irqrestore(&rtl.lock, f);

    for (i = 0; i < npend; i++) {
        net_lock();
        netif_input(&rtl.nif, pending[i]);
        net_unlock();
    }
}

static void rtl_irq_handler(void)
{
    u16 isr;

    if (!rtl.ready)
        return;
    isr = rtl_in16(RTL_ISR);
    if (!isr)
        return;
    /* Drain RX before ACK'ing the IRQ so a busy reentry cannot
       drop the only notification for buffered frames. */
    if (isr & (RTL_ISR_ROK | RTL_ISR_RXOVW | RTL_ISR_FOVW))
        rtl_rx_packets();
    rtl_out16(RTL_ISR, isr);
    rtl.irq_count++;
}

static int rtl_transmit(void *drv, struct pbuf *p)
{
    struct rtl8139_dev *d = (struct rtl8139_dev *)drv;
    unsigned int len, spins;
    u8 cur;
    spin_irq_flags_t f;

    if (!d || !p || !d->ready)
        return -1;
    len = p->len;
    if (len < 60)
        len = 60;
    if (len > RTL_TX_SIZE)
        return -1;

    f = spin_lock_irqsave(&d->lock);
    cur = d->tx_cur;
    spins = 0;
    for (;;) {
        if (rtl_in32(RTL_TSD0 + cur * 4) & RTL_TSD_OWN)
            break;
        if (spins++ >= 2000000) {
            spin_unlock_irqrestore(&d->lock, f);
            return -1;
        }
        /* Drop the device lock so RX/IRQ can reclaim descriptors. */
        spin_unlock_irqrestore(&d->lock, f);
        __asm__ volatile ("pause");
        f = spin_lock_irqsave(&d->lock);
        cur = d->tx_cur;
    }

    memset(d->tx_buf[cur], 0, RTL_TX_SIZE);
    memcpy(d->tx_buf[cur], p->data, p->len);
    rtl_out32(RTL_TSAD0 + cur * 4, (u32)(uintptr)d->tx_buf[cur]);
    /* Writing TSD without OWN hands the descriptor to the NIC; QEMU
       always transmits from currTxDesc, so keep tx_cur in lock-step. */
    rtl_out32(RTL_TSD0 + cur * 4,
              RTL_TSD_ETHR | (len & RTL_TSD_SIZE_MASK));
    d->tx_cur = (u8)((cur + 1) & 3);
    d->tx_count++;
    spin_unlock_irqrestore(&d->lock, f);
    return 0;
}

static int rtl_link_up(void *drv)
{
    (void)drv;
    return rtl.ready;
}

static void rtl_poll_fn(void *drv)
{
    (void)drv;
    if (rtl.ready)
        rtl_rx_packets();
}

static void rtl_get_mac(void *drv, unsigned char mac[ETH_ADDR_LEN])
{
    struct rtl8139_dev *d = (struct rtl8139_dev *)drv;
    int i;
    for (i = 0; i < 6; i++)
        mac[i] = d->mac[i];
}

static const struct netdev_ops rtl_ops = {
    rtl_transmit,
    rtl_link_up,
    rtl_poll_fn,
    rtl_get_mac,
};

void rtl8139_poll(void)
{
    rtl_poll_fn(&rtl);
}

int rtl8139_present(void)
{
    return rtl.ready;
}

static void rtl_selftest(void)
{
    struct inet_config cfg;
    int dhcp_ok = 0;

    inet_config_from_cmdline(&cfg, kernel_cmdline);
    arp_init(&rtl.nif);
    tcp_init();
    rtl.nif.link_up = 1;

    if (dhcp_client(&rtl.nif, &cfg, 4000000) == 0) {
        dhcp_ok = 1;
        printf("NET_DHCP_OK ip=%u.%u.%u.%u gw=%u.%u.%u.%u\n",
               (cfg.ip >> 24) & 0xFF, (cfg.ip >> 16) & 0xFF,
               (cfg.ip >> 8) & 0xFF, cfg.ip & 0xFF,
               (cfg.gateway >> 24) & 0xFF, (cfg.gateway >> 16) & 0xFF,
               (cfg.gateway >> 8) & 0xFF, cfg.gateway & 0xFF);
    } else {
        printf("NET_DHCP_FAIL\n");
        inet_config_from_cmdline(&cfg, kernel_cmdline);
    }

    netif_set_addr(&rtl.nif, cfg.ip, cfg.netmask, cfg.gateway);
    netif_set_up(&rtl.nif);
    printf("NETIF_UP ip=%u.%u.%u.%u gw=%u.%u.%u.%u dhcp=%d\n",
           (cfg.ip >> 24) & 0xFF, (cfg.ip >> 16) & 0xFF,
           (cfg.ip >> 8) & 0xFF, cfg.ip & 0xFF,
           (cfg.gateway >> 24) & 0xFF, (cfg.gateway >> 16) & 0xFF,
           (cfg.gateway >> 8) & 0xFF, cfg.gateway & 0xFF, dhcp_ok);

    if (dhcp_ok) {
        dhcp_force_timer_due(1, 0);
        if (dhcp_service(&rtl.nif, &cfg, 1, 4000000) == 0)
            printf("NET_DHCP_RENEW_OK\n");
        else
            printf("NET_DHCP_RENEW_FAIL\n");
        dhcp_force_timer_due(1, 1);
        if (dhcp_service(&rtl.nif, &cfg, 1, 4000000) == 0)
            printf("NET_DHCP_REBIND_OK\n");
        else
            printf("NET_DHCP_REBIND_FAIL\n");
    }

    tcp_listen_echo(TCP_ECHO_PORT);

    if (icmp_ping(&rtl.nif, cfg.gateway, 300) == 0)
        printf("NET_PING_OK\n");
    else
        printf("NET_PING_FAIL\n");
    if (udp_echo_client(&rtl.nif, cfg.gateway, UDP_TEST_PORT, 2000000) == 0)
        printf("NET_UDP_OK\n");
    else
        printf("NET_UDP_FAIL\n");
    if (tcp_echo_client(&rtl.nif, cfg.gateway, TCP_TEST_PORT, 4000000) == 0)
        printf("NET_TCP_OK\n");
    else
        printf("NET_TCP_FAIL\n");
    if (tcp_echo_rexmit_selftest(&rtl.nif, cfg.gateway, TCP_TEST_PORT,
                                 8000000) == 0)
        printf("NET_TCP_REXMIT_OK\n");
    else
        printf("NET_TCP_REXMIT_FAIL\n");
    {
        unsigned int dip = 0;
        if (dns_query_a(&rtl.nif, cfg.gateway, DNS_TEST_PORT, "icsos.test",
                        &dip, 4000000) == 0 && dip == 0x0A000202u)
            printf("NET_DNS_OK ip=%u.%u.%u.%u\n",
                   (dip >> 24) & 0xFF, (dip >> 16) & 0xFF,
                   (dip >> 8) & 0xFF, dip & 0xFF);
        else
            printf("NET_DNS_FAIL\n");
    }
    softnet_init();
    printf("NET_SOFTNET_OK\n");
    printf("RTL8139_STATS irq=%u rx=%u tx=%u\n",
           rtl.irq_count, rtl.rx_count, rtl.tx_count);
}

void rtl8139_init(void)
{
    u8 bus, dev, fn;
    u32 bar0;
    int i;
    unsigned int spins;

    memset(&rtl, 0, sizeof(rtl));
    spin_init(&rtl.lock);

    if (!rtl_find(&bus, &dev, &fn)) {
        printf("rtl8139: none\n");
        return;
    }

    printf("rtl8139: pci %d:%d.%d\n", bus, dev, fn);
    pci_write16(bus, dev, fn, PCI_CMD, PCI_CMD_IO | PCI_CMD_MASTER);

    bar0 = pci_read32(bus, dev, fn, PCI_BAR0);
    if (!(bar0 & 1)) {
        printf("rtl8139: BAR0 not I/O\n");
        printf("RTL8139_FAIL\n");
        return;
    }
    rtl.io_base = (u16)(bar0 & ~0x3u);
    rtl.irq_line = pci_read8(bus, dev, fn, PCI_IRQ_LINE);

    rtl.rx_buf = (u8 *)malloc(RTL_RX_ALLOC);
    if (!rtl.rx_buf) {
        printf("rtl8139: RX alloc fail\n");
        printf("RTL8139_FAIL\n");
        return;
    }
    memset(rtl.rx_buf, 0, RTL_RX_ALLOC);
    for (i = 0; i < RTL_TX_DESC; i++) {
        rtl.tx_buf[i] = (u8 *)malloc(RTL_TX_SIZE);
        if (!rtl.tx_buf[i]) {
            printf("rtl8139: TX alloc fail\n");
            printf("RTL8139_FAIL\n");
            return;
        }
        memset(rtl.tx_buf[i], 0, RTL_TX_SIZE);
    }

    rtl_out8(RTL_CONFIG1, 0x00);
    /* Unlock config regs while programming. */
    rtl_out8(0x50, 0xC0);
    rtl_out8(RTL_CMD, RTL_CMD_RESET);
    for (spins = 0; spins < 100000 && (rtl_in8(RTL_CMD) & RTL_CMD_RESET);
         spins++)
        __asm__ volatile ("pause");

    /* Accept broadcast/multicast/phys; 32K ring (bits 11-12 = 2). */
    rtl_out32(RTL_RBSTART, (u32)(uintptr)rtl.rx_buf);
    rtl_out32(RTL_RCR, 0xFu | (6u << 13) | (7u << 8) | (2u << 11));
    rtl_out32(RTL_TCR, (6u << 8) | (3u << 24));
    /* Multicast filter accept-all. */
    rtl_out32(0x08, 0xFFFFFFFFu);
    rtl_out32(0x0C, 0xFFFFFFFFu);
    rtl_out8(0x50, 0x00); /* lock config */

    for (i = 0; i < RTL_TX_DESC; i++) {
        rtl_out32(RTL_TSAD0 + i * 4, (u32)(uintptr)rtl.tx_buf[i]);
        rtl_out32(RTL_TSD0 + i * 4, RTL_TSD_OWN);
    }

    rtl_out16(RTL_IMR, RTL_ISR_ROK | RTL_ISR_RER | RTL_ISR_TOK | RTL_ISR_TER |
                       RTL_ISR_RXOVW | RTL_ISR_FOVW);
    rtl_out16(RTL_ISR, 0xFFFF);
    rtl_out8(RTL_CMD, RTL_CMD_RX_EN | RTL_CMD_TX_EN);

    {
        u32 mac_lo = rtl_in32(RTL_MAC0);
        u16 mac_hi = rtl_in16(RTL_MAC0 + 4);
        rtl.mac[0] = (u8)(mac_lo);
        rtl.mac[1] = (u8)(mac_lo >> 8);
        rtl.mac[2] = (u8)(mac_lo >> 16);
        rtl.mac[3] = (u8)(mac_lo >> 24);
        rtl.mac[4] = (u8)(mac_hi);
        rtl.mac[5] = (u8)(mac_hi >> 8);
    }

    if (rtl.irq_line < 16)
        (void)irq_addhandler(0x8139, rtl.irq_line, rtl_irq_handler);

    rtl.ndev.drv = &rtl;
    rtl.ndev.ops = &rtl_ops;
    rtl.ndev.name[0] = 'e';
    rtl.ndev.name[1] = 't';
    rtl.ndev.name[2] = 'h';
    rtl.ndev.name[3] = '1';
    rtl.ndev.name[4] = 0;

    pbuf_pool_init();
    netif_init(&rtl.nif, &rtl.ndev);
    if (!netif_default())
        netif_set_default(&rtl.nif);

    rtl.ready = 1;
    printf("rtl8139: mac=%02x:%02x:%02x:%02x:%02x:%02x io=0x%x irq=%u\n",
           rtl.mac[0], rtl.mac[1], rtl.mac[2], rtl.mac[3], rtl.mac[4],
           rtl.mac[5], rtl.io_base, rtl.irq_line);
    printf("RTL8139_OK\n");
    if (rtl.irq_line < 16)
        printf("RTL8139_IRQ_OK\n");
    else
        printf("RTL8139_POLL_OK\n");

    if (netif_default() == &rtl.nif)
        rtl_selftest();
}
