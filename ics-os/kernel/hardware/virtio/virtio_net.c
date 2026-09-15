/*
 * virtio-net (OASIS virtio 1.2, modern PCI transport).
 *
 * RX queue 0 + TX queue 1, MSI-X, identity-mapped DMA buffers.
 * Feeds the in-tree IPv4 stack via netif.
 */
#include "virtio.h"
#include "virtio_net.h"
#include "../pci_scan.h"
#include "../../dextypes.h"
#include "../../cpu/lapic.h"
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
#include "../irq_lifecycle.h"

extern void *malloc(unsigned int);
extern void *memset(void *s, int c, unsigned int n);
extern void *memcpy(void *d, const void *s, unsigned int n);
extern int printf(const char *fmt, ...);
extern void *mmio_map(u64 phys, u64 len);
extern int serial_com1_present(void);
extern char kernel_cmdline[];
extern unsigned int ticks;

#define VNET_BUF_SIZE     (sizeof(struct virtio_net_hdr) + ETH_MAX_FRAME)
#define VNET_QUEUE_MAX    32
#define SYS_CODE_SEL      0x08
typedef char vnet_hdr_sz[(sizeof(struct virtio_net_hdr) == 12) ? 1 : -1];

typedef struct __attribute__((packed)) _idtentry_v {
   WORD lowphy;
   WORD selector;
   BYTE ist;
   BYTE attr;
   WORD midphy;
   DWORD highphy;
   DWORD reserved;
} idtentry_v;

extern idtentry_v *dex_idtbase;
extern void setinterruptvector(DWORD index, idtentry_v *t, unsigned char attr,
                               void (*handler)(int irq), WORD sel);
extern void virtio_net_msixwrapper(void);

#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC
#define PCI_CMD          0x04
#define PCI_STATUS       0x06
#define PCI_BAR0         0x10
#define PCI_CAP_PTR      0x34
#define PCI_CMD_MEMORY   0x0002
#define PCI_CMD_MASTER   0x0004
#define PCI_CMD_INTX_OFF 0x0400
#define PCI_STATUS_CAPS  0x0010

static inline void virt_mb(void)
{
   __asm__ volatile ("mfence" ::: "memory");
}

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

static inline u32 pci_addr(u8 bus, u8 dev, u8 fn, u8 off)
{
   return 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) |
          ((u32)fn << 8) | (off & 0xFC);
}

static u32 pci_read32(u8 bus, u8 dev, u8 fn, u8 off)
{
   pci_outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, fn, off));
   return pci_inl(PCI_CONFIG_DATA);
}

static void pci_write32(u8 bus, u8 dev, u8 fn, u8 off, u32 val)
{
   pci_outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, fn, off));
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

static u64 pci_read_bar(u8 bus, u8 dev, u8 fn, u8 bar)
{
   u32 lo, hi;
   u8 off = (u8)(PCI_BAR0 + bar * 4);
   lo = pci_read32(bus, dev, fn, off);
   if (lo & 1)
      return (u64)(lo & ~3u);
   if ((lo & 6) == 4) {
      hi = pci_read32(bus, dev, fn, (u8)(off + 4));
      return ((u64)hi << 32) | (lo & ~0xFu);
   }
   return (u64)(lo & ~0xFu);
}

static inline u8 mmio_r8(volatile u8 *p) { return *p; }
static inline u16 mmio_r16(volatile u16 *p) { return *p; }
static inline u32 mmio_r32(volatile u32 *p) { return *p; }

static inline void mmio_w8(volatile u8 *p, u8 v)
{
   *p = v;
   virt_mb();
}
static inline void mmio_w16(volatile u16 *p, u16 v)
{
   *p = v;
   virt_mb();
}
static inline void mmio_w32(volatile u32 *p, u32 v)
{
   *p = v;
   virt_mb();
}
static inline void mmio_w64(volatile u64 *p, u64 v)
{
   *p = v;
   virt_mb();
}

struct vnet_queue {
   volatile struct virtq_desc *desc;
   volatile struct virtq_avail *avail;
   volatile struct virtq_used *used;
   u16 qsize;
   u16 avail_idx;
   u16 last_used;
   u16 notify_off;
   u8 *bufs; /* qsize * VNET_BUF_SIZE */
   u8 free[VNET_QUEUE_MAX];
};

struct vnet_dev {
   u8 bus, dev, fn;
   volatile struct virtio_pci_common_cfg *common;
   volatile u8 *notify;
   volatile u8 *isr;
   volatile u8 *devcfg;
   u32 notify_off_mul;
   u64 features;
   int has_msix;
   u8 msix_cap;
   u8 irq_vector;
   volatile u32 *msix_entry;
   unsigned char mac[6];
   int has_status;
   struct vnet_queue rx;
   struct vnet_queue tx;
   spinlock_t lock;
   struct netdev ndev;
   struct netif nif;
};

static struct vnet_dev vnet;
static int vnet_ready;
static char vnet_irq_owner;

static void *zalloc_align(unsigned sz, unsigned align)
{
   uintptr p;
   unsigned extra = align + 32;
   p = (uintptr)malloc(sz + extra);
   if (!p)
      return 0;
   p = (p + (align - 1)) & ~(uintptr)(align - 1);
   memset((void *)p, 0, sz);
   return (void *)p;
}

static int virtio_find_net(u8 *bus, u8 *dev, u8 *fn)
{
   int b, d, f;
   if (!pci_scan_virtio_allowed(serial_com1_present())) {
      printf("virtio-net: skip pci scan (no COM1)\n");
      return 0;
   }
   for (b = 0; b < 8; b++) {
      for (d = 0; d < 32; d++) {
         u16 vend = pci_read16((u8)b, (u8)d, (u8)0, 0);
         unsigned int maxf;
         if (vend == 0xFFFF)
            continue;
         maxf = pci_slot_fn_count(vend, pci_read32((u8)b, (u8)d, 0, 0x0C));
         for (f = 0; f < (int)maxf; f++) {
            u16 did;
            if (f) {
               vend = pci_read16((u8)b, (u8)d, (u8)f, 0);
               if (vend == 0xFFFF)
                  continue;
            }
            if (vend != VIRTIO_VENDOR_ID)
               continue;
            did = pci_read16((u8)b, (u8)d, (u8)f, 2);
            if (did == VIRTIO_DEV_NET_TRANS || did == VIRTIO_DEV_NET_MODERN) {
               *bus = (u8)b;
               *dev = (u8)d;
               *fn = (u8)f;
               return 1;
            }
         }
      }
   }
   return 0;
}

static volatile u8 *map_cap(u8 bus, u8 dev, u8 fn, u8 bar, u32 offset, u32 length)
{
   u64 base;
   if (bar > 5 || length == 0)
      return 0;
   base = pci_read_bar(bus, dev, fn, bar);
   if (base < 0x100000ULL)
      return 0;
   return (volatile u8 *)mmio_map(base + offset, length ? length : 0x1000);
}

static int virtio_parse_caps(struct vnet_dev *d)
{
   u16 status = pci_read16(d->bus, d->dev, d->fn, PCI_STATUS);
   u8 ptr;
   if (!(status & PCI_STATUS_CAPS))
      return 0;
   ptr = pci_read8(d->bus, d->dev, d->fn, PCI_CAP_PTR);
   while (ptr >= 0x40) {
      u8 id = pci_read8(d->bus, d->dev, d->fn, ptr);
      u8 next = pci_read8(d->bus, d->dev, d->fn, (u8)(ptr + 1));
      if (id == VIRTIO_PCI_CAP_VNDR) {
         u8 cfg_type = pci_read8(d->bus, d->dev, d->fn, (u8)(ptr + 3));
         u8 bar = pci_read8(d->bus, d->dev, d->fn, (u8)(ptr + 4));
         u32 offset = pci_read32(d->bus, d->dev, d->fn, (u8)(ptr + 8));
         u32 length = pci_read32(d->bus, d->dev, d->fn, (u8)(ptr + 12));
         volatile u8 *p = map_cap(d->bus, d->dev, d->fn, bar, offset, length);
         if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG)
            d->common = (volatile struct virtio_pci_common_cfg *)p;
         else if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
            d->notify = p;
            d->notify_off_mul = pci_read32(d->bus, d->dev, d->fn, (u8)(ptr + 16));
         } else if (cfg_type == VIRTIO_PCI_CAP_ISR_CFG)
            d->isr = p;
         else if (cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG)
            d->devcfg = p;
      }
      ptr = next;
      if (ptr == 0)
         break;
   }
   return d->common != 0 && d->notify != 0 && d->devcfg != 0;
}

static int virtio_setup_msix(struct vnet_dev *d)
{
   u16 status = pci_read16(d->bus, d->dev, d->fn, PCI_STATUS);
   u8 ptr;
   u32 table_bir;
   u16 ctrl;
   u64 table_base;
   volatile u32 *entry;
   irq_msi_message message;
   u16 cmd;

   if (!(status & PCI_STATUS_CAPS))
      return 0;
   if (!serial_com1_present() || lapic_x2apic_enabled())
      return 0;
   ptr = pci_read8(d->bus, d->dev, d->fn, PCI_CAP_PTR);
   while (ptr >= 0x40) {
      u8 id = pci_read8(d->bus, d->dev, d->fn, ptr);
      u8 next = pci_read8(d->bus, d->dev, d->fn, (u8)(ptr + 1));
      if (id == PCI_CAP_ID_MSIX) {
         ctrl = pci_read16(d->bus, d->dev, d->fn, (u8)(ptr + 2));
         table_bir = pci_read32(d->bus, d->dev, d->fn, (u8)(ptr + 4));
         table_base = pci_read_bar(d->bus, d->dev, d->fn, (u8)(table_bir & 7));
         table_base += (table_bir & ~7u);
         entry = (volatile u32 *)mmio_map(table_base, 0x1000);
         if (!entry)
            return 0;
         if (!irq_vector_allocate(&vnet_irq_owner, &d->irq_vector)) {
            printf("virtio-net: no MSI-X vector available\n");
            return 0;
         }
         if (!irq_domain_compose_xapic_msi(&irq_device_domain,
                                           d->irq_vector, lapic_get_id(),
                                           &message)) {
            (void)irq_vector_release(d->irq_vector, &vnet_irq_owner);
            d->irq_vector = 0;
            return 0;
         }
         entry[0] = message.address_lo;
         entry[1] = message.address_hi;
         entry[2] = message.data;
         entry[3] = 0;
         virt_mb();
         setinterruptvector(d->irq_vector, dex_idtbase, 0x8E,
                            (void (*)(int))virtio_net_msixwrapper, SYS_CODE_SEL);
         cmd = pci_read16(d->bus, d->dev, d->fn, PCI_CMD);
         pci_write16(d->bus, d->dev, d->fn, PCI_CMD,
                     (u16)(cmd | PCI_CMD_INTX_OFF));
         pci_write16(d->bus, d->dev, d->fn, (u8)(ptr + 2),
                     (u16)(ctrl | 0x8000));
         d->has_msix = 1;
         d->msix_cap = ptr;
         d->msix_entry = entry;
         printf("virtio-net: MSI-X vector=%u\n", d->irq_vector);
         return 1;
      }
      ptr = next;
      if (ptr == 0)
         break;
   }
   return 0;
}

static u64 virtio_read_features(struct vnet_dev *d)
{
   u32 lo, hi;
   mmio_w32(&d->common->device_feature_select, 0);
   lo = mmio_r32(&d->common->device_feature);
   mmio_w32(&d->common->device_feature_select, 1);
   hi = mmio_r32(&d->common->device_feature);
   return ((u64)hi << 32) | lo;
}

static void virtio_write_features(struct vnet_dev *d, u64 f)
{
   mmio_w32(&d->common->driver_feature_select, 0);
   mmio_w32(&d->common->driver_feature, (u32)f);
   mmio_w32(&d->common->driver_feature_select, 1);
   mmio_w32(&d->common->driver_feature, (u32)(f >> 32));
}

static void vnet_notify(struct vnet_dev *d, struct vnet_queue *q)
{
   volatile u16 *p;
   u32 off = (u32)q->notify_off * d->notify_off_mul;
   p = (volatile u16 *)(d->notify + off);
   mmio_w16(p, 0);
}

static int vnet_setup_queue(struct vnet_dev *d, struct vnet_queue *q, u16 index)
{
   u16 qsz;
   u16 i;

   mmio_w16(&d->common->queue_select, index);
   qsz = mmio_r16(&d->common->queue_size);
   if (qsz == 0)
      return 0;
   if (qsz > VNET_QUEUE_MAX) {
      qsz = VNET_QUEUE_MAX;
      mmio_w16(&d->common->queue_size, qsz);
   }
   q->qsize = qsz;
   q->desc = zalloc_align(sizeof(struct virtq_desc) * qsz, 16);
   q->avail = zalloc_align(sizeof(struct virtq_avail), 2);
   q->used = zalloc_align(sizeof(struct virtq_used), 4);
   q->bufs = zalloc_align((unsigned)qsz * VNET_BUF_SIZE, 4096);
   if (!q->desc || !q->avail || !q->used || !q->bufs)
      return 0;
   for (i = 0; i < qsz; i++)
      q->free[i] = 1;
   q->avail_idx = 0;
   q->last_used = 0;

   mmio_w16(&d->common->queue_msix_vector,
            d->has_msix ? 0 : VIRTIO_MSI_NO_VECTOR);
   mmio_w16(&d->common->msix_config, VIRTIO_MSI_NO_VECTOR);
   mmio_w64(&d->common->queue_desc, (u64)(uintptr)q->desc);
   mmio_w64(&d->common->queue_driver, (u64)(uintptr)q->avail);
   mmio_w64(&d->common->queue_device, (u64)(uintptr)q->used);
   q->notify_off = mmio_r16(&d->common->queue_notify_off);
   mmio_w16(&d->common->queue_enable, 1);
   return 1;
}

static void vnet_rx_post(struct vnet_dev *d, u16 idx)
{
   struct vnet_queue *q = &d->rx;
   u8 *buf = q->bufs + (unsigned)idx * VNET_BUF_SIZE;

   q->free[idx] = 0;
   q->desc[idx].addr = (u64)(uintptr)buf;
   q->desc[idx].len = VNET_BUF_SIZE;
   q->desc[idx].flags = VIRTQ_DESC_F_WRITE;
   q->desc[idx].next = 0;
   virt_mb();
   q->avail->ring[q->avail_idx % q->qsize] = idx;
   virt_mb();
   q->avail_idx++;
   q->avail->idx = q->avail_idx;
   virt_mb();
}

static void vnet_rx_fill(struct vnet_dev *d)
{
   u16 i;
   for (i = 0; i < d->rx.qsize; i++) {
      if (d->rx.free[i])
         vnet_rx_post(d, i);
   }
   vnet_notify(d, &d->rx);
}

#define VNET_RX_PENDING_MAX 16

static struct pbuf *vnet_rx_pending[VNET_RX_PENDING_MAX];
static unsigned int vnet_rx_pending_count;

/* Called with lock held. Copies frames into pbufs; does not run the stack. */
static void vnet_harvest_rx(struct vnet_dev *d)
{
   struct vnet_queue *q = &d->rx;
   u16 used_idx;

   if (!q->used)
      return;
   virt_mb();
   used_idx = q->used->idx;
   while (q->last_used != used_idx) {
      struct virtq_used_elem *e;
      u16 head;
      u32 len;
      u8 *buf;
      struct pbuf *p;
      unsigned int frame_len;

      e = &q->used->ring[q->last_used % q->qsize];
      head = (u16)e->id;
      len = e->len;
      q->last_used++;
      if (head >= q->qsize)
         continue;
      buf = q->bufs + (unsigned)head * VNET_BUF_SIZE;
      if (len > sizeof(struct virtio_net_hdr) &&
          vnet_rx_pending_count < VNET_RX_PENDING_MAX) {
         frame_len = len - (unsigned)sizeof(struct virtio_net_hdr);
         if (frame_len > ETH_MAX_FRAME)
            frame_len = ETH_MAX_FRAME;
         p = pbuf_alloc((u16)frame_len);
         if (p) {
            memcpy(p->data, buf + sizeof(struct virtio_net_hdr), frame_len);
            vnet_rx_pending[vnet_rx_pending_count++] = p;
         }
      }
      vnet_rx_post(d, head);
   }
   vnet_notify(d, &d->rx);
}

static void vnet_steal_pending(struct vnet_dev *d, struct pbuf **local,
                               unsigned int *n_out)
{
   unsigned int i, n;

   n = vnet_rx_pending_count;
   for (i = 0; i < n; i++)
      local[i] = vnet_rx_pending[i];
   vnet_rx_pending_count = 0;
   *n_out = n;
}

static void vnet_deliver_pending(struct vnet_dev *d)
{
   unsigned int i, n;
   struct pbuf *local[VNET_RX_PENDING_MAX];
   spin_irq_flags_t flags;

   flags = spin_lock_irqsave(&d->lock);
   vnet_steal_pending(d, local, &n);
   spin_unlock_irqrestore(&d->lock, flags);
   if (!n)
      return;
   net_lock();
   for (i = 0; i < n; i++)
      netif_input(&d->nif, local[i]);
   net_unlock();
}

static void vnet_harvest_tx(struct vnet_dev *d)
{
   struct vnet_queue *q = &d->tx;
   u16 used_idx;

   if (!q->used)
      return;
   virt_mb();
   used_idx = q->used->idx;
   while (q->last_used != used_idx) {
      struct virtq_used_elem *e = &q->used->ring[q->last_used % q->qsize];
      u16 head = (u16)e->id;
      q->last_used++;
      if (head < q->qsize)
         q->free[head] = 1;
   }
}

static void vnet_poll_locked(struct vnet_dev *d)
{
   vnet_harvest_rx(d);
   vnet_harvest_tx(d);
}

static void vnet_poll_ops(void *drv)
{
   struct vnet_dev *d = (struct vnet_dev *)drv;
   spin_irq_flags_t flags;
   if (!d || !vnet_ready)
      return;
   flags = spin_lock_irqsave(&d->lock);
   vnet_poll_locked(d);
   spin_unlock_irqrestore(&d->lock, flags);
   vnet_deliver_pending(d);
}

static int vnet_transmit(void *drv, struct pbuf *p)
{
   struct vnet_dev *d = (struct vnet_dev *)drv;
   struct vnet_queue *q;
   spin_irq_flags_t flags;
   u16 i, idx;
   u8 *buf;
   unsigned int total;

   if (!d || !p || !vnet_ready)
      return -1;
   total = (unsigned)sizeof(struct virtio_net_hdr) + p->len;
   if (total > VNET_BUF_SIZE)
      return -1;

   flags = spin_lock_irqsave(&d->lock);
   vnet_harvest_tx(d);
   q = &d->tx;
   idx = 0xFFFF;
   for (i = 0; i < q->qsize; i++) {
      if (q->free[i]) {
         idx = i;
         break;
      }
   }
   if (idx == 0xFFFF) {
      spin_unlock_irqrestore(&d->lock, flags);
      return -1;
   }
   q->free[idx] = 0;
   buf = q->bufs + (unsigned)idx * VNET_BUF_SIZE;
   memset(buf, 0, sizeof(struct virtio_net_hdr));
   memcpy(buf + sizeof(struct virtio_net_hdr), p->data, p->len);
   q->desc[idx].addr = (u64)(uintptr)buf;
   q->desc[idx].len = total;
   q->desc[idx].flags = 0;
   q->desc[idx].next = 0;
   virt_mb();
   q->avail->ring[q->avail_idx % q->qsize] = idx;
   virt_mb();
   q->avail_idx++;
   q->avail->idx = q->avail_idx;
   virt_mb();
   vnet_notify(d, q);
   spin_unlock_irqrestore(&d->lock, flags);
   return 0;
}

static int vnet_link_up(void *drv)
{
   struct vnet_dev *d = (struct vnet_dev *)drv;
   u16 st;
   if (!d)
      return 0;
   if (!d->has_status)
      return 1;
   st = *(volatile u16 *)(d->devcfg + 6);
   return (st & VIRTIO_NET_S_LINK_UP) != 0;
}

static void vnet_get_mac(void *drv, unsigned char mac[ETH_ADDR_LEN])
{
   struct vnet_dev *d = (struct vnet_dev *)drv;
   int i;
   for (i = 0; i < ETH_ADDR_LEN; i++)
      mac[i] = d->mac[i];
}

static const struct netdev_ops vnet_ops = {
   .transmit = vnet_transmit,
   .link_up = vnet_link_up,
   .poll = vnet_poll_ops,
   .get_mac = vnet_get_mac,
};

void virtio_net_poll(void)
{
   vnet_poll_ops(&vnet);
}

int virtio_net_present(void)
{
   return vnet_ready;
}

void virtio_net_irq(void)
{
   int entered;
   entered = irq_vector_enter(vnet.irq_vector, &vnet_irq_owner);
   if (!entered) {
      lapic_eoi();
      return;
   }
   if (vnet.isr)
      (void)*vnet.isr;
   if (vnet_ready) {
      spin_lock(&vnet.lock);
      vnet_poll_locked(&vnet);
      spin_unlock(&vnet.lock);
      vnet_deliver_pending(&vnet);
   }
   irq_vector_exit(vnet.irq_vector, &vnet_irq_owner);
   lapic_eoi();
}

static void vnet_selftest(struct vnet_dev *d)
{
   struct inet_config cfg;
   unsigned int spins;
   int dhcp_ok = 0;

   inet_config_from_cmdline(&cfg, kernel_cmdline);

   /* Prefer status bit when available; do not depend on the timer yet
      (early boot may still be before a reliable tick source). */
   for (spins = 0; spins < 100000 && !vnet_link_up(d); spins++)
      __asm__ volatile ("pause");

   if (!vnet_link_up(d) && d->has_status) {
      printf("virtio-net: link status not up yet; continuing\n");
   }

   arp_init(&d->nif);
   tcp_init();

   /* Bring the interface up for TX/RX before DHCP (no IP yet). */
   d->nif.link_up = 1;
   if (dhcp_client(&d->nif, &cfg, 4000000) == 0) {
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

   netif_set_addr(&d->nif, cfg.ip, cfg.netmask, cfg.gateway);
   netif_set_up(&d->nif);
   printf("NETIF_UP ip=%u.%u.%u.%u gw=%u.%u.%u.%u dhcp=%d\n",
          (cfg.ip >> 24) & 0xFF, (cfg.ip >> 16) & 0xFF,
          (cfg.ip >> 8) & 0xFF, cfg.ip & 0xFF,
          (cfg.gateway >> 24) & 0xFF, (cfg.gateway >> 16) & 0xFF,
          (cfg.gateway >> 8) & 0xFF, cfg.gateway & 0xFF, dhcp_ok);

   if (dhcp_ok) {
      /* Timer FSM: force T1 due → renew; force T2 due → rebind. */
      dhcp_force_timer_due(1, 0);
      if (dhcp_service(&d->nif, &cfg, 1, 4000000) == 0)
         printf("NET_DHCP_RENEW_OK\n");
      else
         printf("NET_DHCP_RENEW_FAIL\n");

      dhcp_force_timer_due(1, 1);
      if (dhcp_service(&d->nif, &cfg, 1, 4000000) == 0)
         printf("NET_DHCP_REBIND_OK\n");
      else
         printf("NET_DHCP_REBIND_FAIL\n");
   }

   tcp_listen_echo(TCP_ECHO_PORT);

   if (icmp_ping(&d->nif, cfg.gateway, 300) == 0)
      printf("NET_PING_OK\n");
   else
      printf("NET_PING_FAIL\n");

   if (udp_echo_client(&d->nif, cfg.gateway, UDP_TEST_PORT, 2000000) == 0)
      printf("NET_UDP_OK\n");
   else
      printf("NET_UDP_FAIL\n");

   if (tcp_echo_client(&d->nif, cfg.gateway, TCP_TEST_PORT, 4000000) == 0)
      printf("NET_TCP_OK\n");
   else
      printf("NET_TCP_FAIL\n");

   if (tcp_echo_rexmit_selftest(&d->nif, cfg.gateway, TCP_TEST_PORT,
                                8000000) == 0)
      printf("NET_TCP_REXMIT_OK\n");
   else
      printf("NET_TCP_REXMIT_FAIL\n");

   {
      unsigned int dip = 0;
      if (dns_query_a(&d->nif, cfg.gateway, DNS_TEST_PORT, "icsos.test",
                      &dip, 4000000) == 0 && dip == 0x0A000202u)
         printf("NET_DNS_OK ip=%u.%u.%u.%u\n",
                (dip >> 24) & 0xFF, (dip >> 16) & 0xFF,
                (dip >> 8) & 0xFF, dip & 0xFF);
      else
         printf("NET_DNS_FAIL\n");
   }

   softnet_init();
   printf("NET_SOFTNET_OK\n");
}

void virtio_net_init(void)
{
   u64 offered, wanted;
   u8 st;
   int i;

   memset(&vnet, 0, sizeof(vnet));
   spin_init(&vnet.lock);
   pbuf_pool_init();

   if (!virtio_find_net(&vnet.bus, &vnet.dev, &vnet.fn)) {
      printf("virtio-net: none\n");
      return;
   }

   printf("virtio-net: pci %d:%d.%d\n", vnet.bus, vnet.dev, vnet.fn);
   pci_write16(vnet.bus, vnet.dev, vnet.fn, PCI_CMD,
               PCI_CMD_MEMORY | PCI_CMD_MASTER);

   if (!virtio_parse_caps(&vnet)) {
      printf("virtio-net: no modern virtio-pci caps\n");
      printf("VIRTIO_NET_FAIL\n");
      return;
   }

   mmio_w8(&vnet.common->device_status, 0);
   mmio_w8(&vnet.common->device_status, VIRTIO_STATUS_ACKNOWLEDGE);
   mmio_w8(&vnet.common->device_status,
           VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

   offered = virtio_read_features(&vnet);
   wanted = (1ULL << VIRTIO_F_VERSION_1);
   if (offered & (1ULL << VIRTIO_NET_F_MAC))
      wanted |= (1ULL << VIRTIO_NET_F_MAC);
   if (offered & (1ULL << VIRTIO_NET_F_STATUS)) {
      wanted |= (1ULL << VIRTIO_NET_F_STATUS);
      vnet.has_status = 1;
   }
   vnet.features = wanted;
   virtio_write_features(&vnet, wanted);

   st = (u8)(VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
             VIRTIO_STATUS_FEATURES_OK);
   mmio_w8(&vnet.common->device_status, st);
   if (!(mmio_r8(&vnet.common->device_status) & VIRTIO_STATUS_FEATURES_OK)) {
      printf("virtio-net: FEATURES_OK rejected\n");
      printf("VIRTIO_NET_FAIL\n");
      mmio_w8(&vnet.common->device_status, VIRTIO_STATUS_FAILED);
      return;
   }

   virtio_setup_msix(&vnet);
   if (!vnet_setup_queue(&vnet, &vnet.rx, 0) ||
       !vnet_setup_queue(&vnet, &vnet.tx, 1)) {
      printf("virtio-net: queue setup failed\n");
      printf("VIRTIO_NET_FAIL\n");
      mmio_w8(&vnet.common->device_status, VIRTIO_STATUS_FAILED);
      return;
   }

   for (i = 0; i < 6; i++)
      vnet.mac[i] = vnet.devcfg[i];

   mmio_w8(&vnet.common->device_status,
           (u8)(st | VIRTIO_STATUS_DRIVER_OK));

   vnet.ndev.drv = &vnet;
   vnet.ndev.ops = &vnet_ops;
   vnet.ndev.name[0] = 'e';
   vnet.ndev.name[1] = 't';
   vnet.ndev.name[2] = 'h';
   vnet.ndev.name[3] = '0';
   vnet.ndev.name[4] = 0;

   netif_init(&vnet.nif, &vnet.ndev);
   netif_set_default(&vnet.nif);

   vnet_rx_fill(&vnet);
   vnet_ready = 1;

   printf("virtio-net: mac=%02x:%02x:%02x:%02x:%02x:%02x msix=%d\n",
          vnet.mac[0], vnet.mac[1], vnet.mac[2],
          vnet.mac[3], vnet.mac[4], vnet.mac[5], vnet.has_msix);
   printf("VIRTIO_NET_OK\n");
   if (vnet.has_msix)
      printf("VIRTIO_NET_IRQ_OK\n");
   else
      printf("VIRTIO_NET_POLL_OK\n");

   vnet_selftest(&vnet);
}
