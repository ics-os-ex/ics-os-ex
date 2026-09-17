/* Minimal xHCI backend for the legacy USB MSC/BOT layer. */

#include "../irq_lifecycle.h"
#include "xhci_policy.h"
#include "cpu/lapic.h"

extern void taskswitch(void);

#define XHCI_TRBS             256
#define XHCI_EVENT_TRBS       256
#define XHCI_MAX_SLOTS        8
#define XHCI_MAX_HCDS         8
#define XHCI_MAX_USBDEVS      2
#define XHCI_TIMEOUT          4000000

static int xhci_no_recover;
static DWORD xhci_last_remain;

#define XHCI_CMD_RUN          (1u << 0)
#define XHCI_CMD_RESET        (1u << 1)
#define XHCI_CMD_INTE         (1u << 2)
#define XHCI_STS_HALTED       (1u << 0)
#define XHCI_STS_CNR          (1u << 11)
#define XHCI_PORT_CCS         (1u << 0)
#define XHCI_PORT_PED         (1u << 1)
#define XHCI_PORT_RESET       (1u << 4)
#define XHCI_PORT_SPEED(v)    (((v) >> 10) & 0xF)
#define XHCI_PORT_CHANGE      ((1u << 17) | (1u << 18) | (1u << 20) | \
                               (1u << 21) | (1u << 22))

#define XHCI_TRB_CYCLE        (1u << 0)
#define XHCI_TRB_CHAIN        (1u << 4)
#define XHCI_TRB_IOC          (1u << 5)
#define XHCI_TRB_IDT          (1u << 6)
#define XHCI_TRB_DIR_IN       (1u << 16)
#define XHCI_TRB_TYPE(n)      ((DWORD)(n) << 10)
#define XHCI_TRB_NORMAL       XHCI_TRB_TYPE(1)
#define XHCI_TRB_SETUP        XHCI_TRB_TYPE(2)
#define XHCI_TRB_DATA         XHCI_TRB_TYPE(3)
#define XHCI_TRB_STATUS       XHCI_TRB_TYPE(4)
#define XHCI_TRB_LINK         XHCI_TRB_TYPE(6)
#define XHCI_TRB_ENABLE_SLOT  XHCI_TRB_TYPE(9)
#define XHCI_TRB_DISABLE_SLOT XHCI_TRB_TYPE(10)
#define XHCI_TRB_ADDRESS      XHCI_TRB_TYPE(11)
#define XHCI_TRB_CONFIG_EP    XHCI_TRB_TYPE(12)
#define XHCI_TRB_EVALUATE     XHCI_TRB_TYPE(13)
#define XHCI_TRB_RESET_EP     XHCI_TRB_TYPE(14)
#define XHCI_TRB_STOP_EP      XHCI_TRB_TYPE(15)
#define XHCI_TRB_SET_DEQUEUE  XHCI_TRB_TYPE(16)
#define XHCI_TRB_TRANSFER_EV  32
#define XHCI_TRB_COMMAND_EV   33
#define XHCI_CC_SUCCESS       1
#define XHCI_CC_STALL         6
#define XHCI_CC_SHORT_PACKET  13

typedef struct __attribute__((packed, aligned(16))) {
    volatile u64 parameter;
    volatile DWORD status;
    volatile DWORD control;
} xhci_trb;

typedef struct __attribute__((packed, aligned(16))) {
    u64 address;
    DWORD size;
    DWORD reserved;
} xhci_erst_entry;

typedef struct {
    xhci_trb *trbs;
    dma_region dma;
    DWORD enqueue;
    DWORD cycle;
    DWORD link_chain;
} xhci_ring;

/* Per-device port/slot state. Device 0 is the enumerated primary (normally
   the MSC root); device 1 is the optional secondary (CDC-ACM console). */
typedef struct {
    DWORD port;
    DWORD speed;
    DWORD slot;
    DWORD ep0_mps;
} xhci_usbdev;

typedef struct {
    u64 *dcbaa;
    xhci_trb *event_trbs;
    xhci_erst_entry *erst;
    BYTE *input_ctx;
    BYTE *device_ctx;
    volatile BYTE *mmio;
    volatile BYTE *op;
    volatile BYTE *runtime;
    volatile DWORD *doorbell;
    DWORD context_size;
    DWORD max_ports;
    xhci_usbdev usbdevs[XHCI_MAX_USBDEVS];
    DWORD usbdev_count;
    DWORD event_dequeue;
    DWORD event_cycle;
    xhci_ring cmd_ring;
    xhci_ring ep0_ring;
    xhci_ring ep_in_ring;
    xhci_ring ep_out_ring;
    xhci_ring cdc_ep0_ring;
    xhci_ring cdc_ep_in_ring;
    xhci_ring cdc_ep_out_ring;
    dma_region dcbaa_dma;
    dma_region event_dma;
    dma_region erst_dma;
    dma_region input_ctx_dma;
    dma_region device_ctx_dma;
    dma_region cdc_input_ctx_dma;
    dma_region cdc_device_ctx_dma;
    dma_region cdc_ep0_dma;
    dma_region cdc_ep_out_dma;
    BYTE *cdc_input_ctx;
    BYTE *cdc_device_ctx;
    dma_region scratchpad_array_dma;
    dma_region scratchpad_pages_dma;
    dma_device stream_dma;
    DWORD scratchpad_count;
    volatile int recovery_needed;
    int enumerating;
    volatile int fault_drop_next;
    volatile int fault_fail_init;
    volatile int fault_disconnect_inflight;
    volatile int connection_lost;
    DWORD stalled_endpoints;
    volatile DWORD irq_count;
    volatile DWORD irq_wait_count;
    int has_msix;
    int msix_qualified_reported;
    int sg_qualified_reported;
    int bounce_to_device_qualified;
    int bounce_from_device_qualified;
    int bounce_qualified_reported;
    BYTE pci_bus;
    BYTE pci_slot;
    BYTE pci_func;
    BYTE msix_cap;
    int pci_discovered;
    volatile DWORD *msix_entry;
    u8 irq_vector;
    int irq_route;
    char irq_owner;
} xhci_hcd;

static xhci_hcd xhci_primary_hcd;
static xhci_hcd xhci_secondary_hcds[XHCI_MAX_HCDS - 1];
static xhci_hcd *xhci_hcds[XHCI_MAX_HCDS] = {
    &xhci_primary_hcd,
    &xhci_secondary_hcds[0], &xhci_secondary_hcds[1],
    &xhci_secondary_hcds[2], &xhci_secondary_hcds[3],
    &xhci_secondary_hcds[4], &xhci_secondary_hcds[5],
    &xhci_secondary_hcds[6]
};
static DWORD xhci_hcd_count;
static xhci_hcd *xhci_irq_routes[XHCI_MAX_HCDS];
static char xhci_test_reservation_owner;
static int xhci_test_vector_reserved;

/* Leave a CDC bulk IN TRB posted across empty polls. Stop+SetDequeue on
   every timeout wedged Intel xHCI after the first USB_CDC_RX. MSC waits
   stash CDC IN completions (see xhci_next_event); do not Stop at BOT. */
static int xhci_cdc_in_pending;
static xhci_hcd *xhci_cdc_in_hcd;
static DWORD xhci_cdc_in_dev;
static DWORD xhci_cdc_in_epid;
static DWORD xhci_cdc_in_slot;
static xhci_ring *xhci_cdc_in_ring;
static dma_sg_mapping xhci_cdc_in_map;
static DWORD xhci_cdc_in_first;
static int xhci_cdc_in_len;
static BYTE *xhci_cdc_in_cpu;
static int xhci_cdc_in_done;
static DWORD xhci_cdc_in_cc;
static DWORD xhci_cdc_in_remain;

static int xhci_stop_endpoint(xhci_hcd *hcd, DWORD dev, DWORD endpoint,
                              xhci_ring *ring);
static void xhci_cdc_in_drop(xhci_hcd *hcd);
static int xhci_cdc_in_take(int *got, int len);
static void xhci_cdc_quiesce_for_msc(xhci_hcd *hcd);

extern void *mmio_map(u64 phys, u64 len);
extern int serial_com1_present(void);
extern void *malloc(unsigned int size);
extern void free(void *ptr);
extern void xhci_msixwrapper0(void);
extern void xhci_msixwrapper1(void);
extern void xhci_msixwrapper2(void);
extern void xhci_msixwrapper3(void);
extern void xhci_msixwrapper4(void);
extern void xhci_msixwrapper5(void);
extern void xhci_msixwrapper6(void);
extern void xhci_msixwrapper7(void);
extern void lapic_eoi(void);
extern char kernel_cmdline[];

#define SYS_CODE_SEL 0x08

static void (*xhci_irq_wrappers[XHCI_MAX_HCDS])(void) = {
    xhci_msixwrapper0, xhci_msixwrapper1,
    xhci_msixwrapper2, xhci_msixwrapper3,
    xhci_msixwrapper4, xhci_msixwrapper5,
    xhci_msixwrapper6, xhci_msixwrapper7
};

static int xhci_irq_route_bind(xhci_hcd *hcd)
{
    int route;
    for (route = 0; route < XHCI_MAX_HCDS; route++) {
        if (__sync_bool_compare_and_swap(&xhci_irq_routes[route], 0, hcd)) {
            hcd->irq_route = route;
            printf("xhci: IRQ route=%d bound\n", route);
            return 1;
        }
    }
    return 0;
}

static void xhci_irq_route_unbind(xhci_hcd *hcd)
{
    int route = hcd->irq_route;
    if (route >= 0 && route < XHCI_MAX_HCDS)
        (void)__sync_bool_compare_and_swap(&xhci_irq_routes[route], hcd, 0);
    printf("xhci: IRQ route=%d unbound\n", route);
    hcd->irq_route = -1;
}

static void xhci_mb(void)
{
    asm volatile ("mfence" ::: "memory");
}

static DWORD xhci_r32(volatile BYTE *base, DWORD off)
{
    return *(volatile DWORD *)(base + off);
}

static u64 xhci_r64(volatile BYTE *base, DWORD off)
{
    return *(volatile u64 *)(base + off);
}

static void xhci_w32(volatile BYTE *base, DWORD off, DWORD value)
{
    *(volatile DWORD *)(base + off) = value;
    xhci_mb();
}

static void xhci_w64(volatile BYTE *base, DWORD off, u64 value)
{
    *(volatile u64 *)(base + off) = value;
    xhci_mb();
}

static int xhci_wait32(volatile BYTE *base, DWORD off, DWORD mask,
                       DWORD expected)
{
    DWORD spins;
    for (spins = 0; spins < XHCI_TIMEOUT; spins++) {
        if ((xhci_r32(base, off) & mask) == expected)
            return 1;
        if ((spins & 0x7F) == 0x7F)
            taskswitch();
        usb_io_delay();
    }
    return 0;
}

static DWORD xhci_port_ccs_mask(xhci_hcd *hcd)
{
    DWORD mask = 0;
    DWORD port;
    if (!hcd || !hcd->op || !hcd->max_ports)
        return 0;
    for (port = 1; port <= hcd->max_ports && port <= 32; port++)
        if (xhci_r32(hcd->op, 0x400 + (port - 1) * 0x10) & XHCI_PORT_CCS)
            mask |= xhci_ccs_bit(port);
    return mask;
}

static DWORD xhci_wait_connected_ports(xhci_hcd *hcd)
{
    DWORD spins;
    DWORD mask = 0;
    for (spins = 0; spins < XHCI_PORT_SETTLE_SPINS; spins++) {
        mask = xhci_port_ccs_mask(hcd);
        if (mask)
            return mask;
        usb_io_delay();
    }
    return xhci_port_ccs_mask(hcd);
}

static int xhci_device_attached(xhci_hcd *hcd)
{
   return xhci_port_ccs_mask(hcd) != 0;
}

/* Per-device accessors. Device 0 (the MSC root) always exists once the HCD
  has a primary; device 1 is the optional CDC-ACM console. */
static xhci_usbdev *xhci_ud(xhci_hcd *hcd, DWORD index)
{
   if (!hcd || index >= XHCI_MAX_USBDEVS)
       return 0;
   return &hcd->usbdevs[index];
}

static xhci_ring *xhci_ep0_ring_for(xhci_hcd *hcd, DWORD dev)
{
   return dev ? &hcd->cdc_ep0_ring : &hcd->ep0_ring;
}

static xhci_ring *xhci_ep_out_ring_for(xhci_hcd *hcd, DWORD dev)
{
   return dev ? &hcd->cdc_ep_out_ring : &hcd->ep_out_ring;
}

static xhci_ring *xhci_ep_in_ring_for(xhci_hcd *hcd, DWORD dev)
{
   return dev ? &hcd->cdc_ep_in_ring : &hcd->ep_in_ring;
}

static BYTE *xhci_input_ctx_base(xhci_hcd *hcd, DWORD dev)
{
   return dev ? hcd->cdc_input_ctx : hcd->input_ctx;
}

static DWORD *xhci_input_context_dev(xhci_hcd *hcd, DWORD dev, DWORD index)
{
   return (DWORD *)(xhci_input_ctx_base(hcd, dev) +
                    index * hcd->context_size);
}

static void xhci_fill_slot_context_dev(xhci_hcd *hcd, DWORD dev, DWORD *slot,
                                      DWORD entries)
{
   xhci_usbdev *ud = xhci_ud(hcd, dev);
   if (!ud)
       return;
   memset(slot, 0, hcd->context_size);
   slot[0] = (ud->speed << 20) | (entries << 27);
   slot[1] = ud->port << 16;
}

static int xhci_usbdev_connected(xhci_hcd *hcd, DWORD dev)
{
   xhci_usbdev *ud = xhci_ud(hcd, dev);
   if (!hcd->op || !ud || !ud->port)
       return 0;
   return (xhci_r32(hcd->op, 0x400 + (ud->port - 1) * 0x10) &
           XHCI_PORT_CCS) != 0;
}

/* Any active device still connected (used to gate command waits). */
static int xhci_device_connected(xhci_hcd *hcd)
{
   DWORD dev;
   if (!hcd || !hcd->op)
       return 0;
   for (dev = 0; dev < hcd->usbdev_count; dev++)
       if (xhci_usbdev_connected(hcd, dev))
           return 1;
   return 0;
}

static int xhci_ring_is_cdc(xhci_hcd *hcd, xhci_ring *ring)
{
    if (!hcd || !ring)
        return 0;
    return ring == &hcd->cdc_ep0_ring || ring == &hcd->cdc_ep_in_ring ||
           ring == &hcd->cdc_ep_out_ring;
}

static int xhci_ring_owner_connected(xhci_hcd *hcd, xhci_ring *ring)
{
    DWORD dev = xhci_ring_is_cdc(hcd, ring) ? 1 : 0;
    if (!hcd->usbdevs[dev].port)
        return xhci_device_connected(hcd);
    return xhci_usbdev_connected(hcd, dev);
}

static DWORD xhci_discover_hcds(void)
{
    BYTE b, s, f;
    DWORD count = 0;
    if (xhci_hcd_count)
        return xhci_hcd_count;
    for (b = 0; b < 8; b++) {
        for (s = 0; s < 32; s++) {
            WORD vendor = pci_read16(b, s, 0, 0);
            BYTE maxf = 1;
            if (vendor == 0xFFFF)
                continue;
            if (pci_read32(b, s, 0, 0x0C) & 0x800000)
                maxf = 8;
            for (f = 0; f < maxf; f++) {
                DWORD classreg = pci_read32(b, s, f, 0x08);
                if ((classreg >> 8) == 0x0C0330) {
                    xhci_hcd *hcd;
                    if (count == XHCI_MAX_HCDS) {
                        printf("xhci: controller limit reached max=%u\n",
                               XHCI_MAX_HCDS);
                        continue;
                    }
                    hcd = xhci_hcds[count++];
                    hcd->pci_bus = b;
                    hcd->pci_slot = s;
                    hcd->pci_func = f;
                    hcd->pci_discovered = 1;
                    hcd->irq_route = -1;
                    printf("xhci: discovered hcd=%u PCI %u:%u.%u\n",
                           count - 1, b, s, f);
                }
            }
        }
    }
    xhci_hcd_count = count;
    printf("xhci: discovered controllers=%u\n", count);
    return count;
}

static BYTE xhci_pci_read8(BYTE bus, BYTE slot, BYTE func, BYTE off)
{
    DWORD value = pci_read32(bus, slot, func, off & 0xFC);
    return (BYTE)(value >> ((off & 3) * 8));
}

static int xhci_current_bar(BYTE bus, BYTE slot, BYTE func, DWORD bir,
                            u64 *base)
{
    BYTE off;
    DWORD lo, hi = 0;
    if (bir > 5 || !base)
        return 0;
    off = (BYTE)(0x10 + bir * 4);
    lo = pci_read32(bus, slot, func, off);
    if (lo & 1)
        return 0;
    if ((lo & 6) == 4) {
        if (bir == 5)
            return 0;
        hi = pci_read32(bus, slot, func, off + 4);
    }
    *base = ((u64)hi << 32) | (lo & ~0xFu);
    return *base != 0;
}

static int xhci_setup_msix(xhci_hcd *hcd, BYTE bus, BYTE slot, BYTE func)
{
    BYTE ptr;
    DWORD guard = 48;
    WORD status = pci_read16(bus, slot, func, 0x06);

    hcd->has_msix = 0;
    if (!(status & 0x10))
        return 0;
    ptr = xhci_pci_read8(bus, slot, func, 0x34);
    while (ptr >= 0x40 && guard--) {
        BYTE id = xhci_pci_read8(bus, slot, func, ptr);
        BYTE next = xhci_pci_read8(bus, slot, func, ptr + 1);
        if (id == 0x11) {
            WORD control = pci_read16(bus, slot, func, ptr + 2);
            DWORD table = pci_read32(bus, slot, func, ptr + 4);
            DWORD bir = table & 7;
            u64 bar;
            volatile DWORD *entry;
            WORD command;
            irq_msi_message message;
            if (!xhci_current_bar(bus, slot, func, bir, &bar) ||
                bar + (table & ~7u) < bar)
                return 0;
            entry = (volatile DWORD *)mmio_map(bar + (table & ~7u), 16);
            if (!entry)
                return 0;
            if (!irq_vector_allocate(&hcd->irq_owner, &hcd->irq_vector)) {
                printf("xhci: no MSI-X vector available\n");
                return 0;
            }
            if (!irq_domain_compose_xapic_msi(&irq_device_domain,
                                              hcd->irq_vector,
                                              lapic_get_id(), &message)) {
                (void)irq_vector_release(hcd->irq_vector, &hcd->irq_owner);
                hcd->irq_vector = 0;
                printf("xhci: cannot compose MSI-X message\n");
                return 0;
            }
            hcd->irq_route = -1;
            if (!xhci_irq_route_bind(hcd)) {
                (void)irq_vector_release(hcd->irq_vector, &hcd->irq_owner);
                hcd->irq_vector = 0;
                printf("xhci: no IRQ dispatch route available\n");
                return 0;
            }
            entry[3] = 1;
            xhci_mb();
            entry[0] = message.address_lo;
            entry[1] = message.address_hi;
            entry[2] = message.data;
            setinterruptvector(hcd->irq_vector, dex_idtbase, 0x8E,
                               (void (*)(int))xhci_irq_wrappers[hcd->irq_route],
                               SYS_CODE_SEL);
            command = pci_read16(bus, slot, func, 0x04);
            pci_write16(bus, slot, func, 0x04, command | 0x0400);
            pci_write16(bus, slot, func, ptr + 2,
                        (WORD)((control | 0x8000) & ~0x4000));
            entry[3] = 0;
            xhci_mb();
            hcd->has_msix = 1;
            hcd->pci_bus = bus;
            hcd->pci_slot = slot;
            hcd->pci_func = func;
            hcd->msix_cap = ptr;
            hcd->msix_entry = entry;
            printf("xhci: MSI-X vector=%u\n", hcd->irq_vector);
            return 1;
        }
        if (!next || next == ptr)
            break;
        ptr = next;
    }
    return 0;
}

static int xhci_pci_bar(BYTE bus, BYTE slot, BYTE func,
                        u64 *base, u64 *size)
{
    WORD command = pci_read16(bus, slot, func, 0x04);
    DWORD lo = pci_read32(bus, slot, func, 0x10);
    DWORD hi = 0, mask_lo, mask_hi = 0;
    int is_64 = (lo & 6) == 4;
    if (lo & 1)
        return 0;
    if (is_64)
        hi = pci_read32(bus, slot, func, 0x14);
    pci_write16(bus, slot, func, 0x04, command & (WORD)~0x06);
    pci_write32(bus, slot, func, 0x10, 0xFFFFFFFFu);
    if (is_64)
        pci_write32(bus, slot, func, 0x14, 0xFFFFFFFFu);
    mask_lo = pci_read32(bus, slot, func, 0x10);
    if (is_64)
        mask_hi = pci_read32(bus, slot, func, 0x14);
    pci_write32(bus, slot, func, 0x10, lo);
    if (is_64)
        pci_write32(bus, slot, func, 0x14, hi);
    pci_write16(bus, slot, func, 0x04, command);
    *base = ((u64)hi << 32) | (lo & ~0xFu);
    if (is_64) {
        u64 mask = ((u64)mask_hi << 32) | (mask_lo & ~0xFu);
        *size = (~mask) + 1;
    } else {
        *size = (u64)((~(mask_lo & ~0xFu)) + 1);
    }
    if (strstr(kernel_cmdline, "xhci-high-bar-test")) {
        DWORD id = pci_read32(bus, slot, func, 0x00);
        if (!is_64 || id != 0x000D1B36u) {
            printf("xhci: high BAR test requires QEMU qemu-xhci\n");
            return 0;
        }
        pci_write16(bus, slot, func, 0x04, command & (WORD)~0x06);
        pci_write32(bus, slot, func, 0x10, lo & 0xFu);
        pci_write32(bus, slot, func, 0x14, 1);
        pci_write16(bus, slot, func, 0x04, command);
        *base = ((u64)pci_read32(bus, slot, func, 0x14) << 32) |
                (pci_read32(bus, slot, func, 0x10) & ~0xFu);
        if (*base != 0x100000000ULL) {
            printf("xhci: high BAR test relocation failed base=0x%llx\n",
                   (unsigned long long)*base);
            return 0;
        }
        printf("xhci: test BAR relocated above 4 GiB\n");
    }
    return *base != 0 && *size != 0;
}

static int xhci_ring_init(xhci_ring *ring, DWORD link_chain)
{
    xhci_trb *trbs = (xhci_trb *)ring->dma.cpu_addr;
    if (!trbs || ring->dma.length != sizeof(xhci_trb) * XHCI_TRBS)
        return 0;
    memset(trbs, 0, sizeof(xhci_trb) * XHCI_TRBS);
    ring->trbs = trbs;
    ring->enqueue = 0;
    ring->cycle = 1;
    ring->link_chain = link_chain;
    trbs[XHCI_TRBS - 1].parameter = ring->dma.dma_addr;
    trbs[XHCI_TRBS - 1].control = XHCI_TRB_LINK | (1u << 1) |
                                    link_chain | XHCI_TRB_CYCLE;
    return 1;
}

static xhci_trb *xhci_ring_put_cycle(xhci_ring *ring, u64 parameter,
                                     DWORD status, DWORD control, DWORD cycle)
{
    xhci_trb *trb = &ring->trbs[ring->enqueue];
    trb->parameter = parameter;
    trb->status = status;
    xhci_mb();
    trb->control = (control & ~XHCI_TRB_CYCLE) | (cycle & XHCI_TRB_CYCLE);
    xhci_mb();
    ring->enqueue++;
    if (ring->enqueue == XHCI_TRBS - 1) {
        ring->trbs[XHCI_TRBS - 1].control = XHCI_TRB_LINK | (1u << 1) |
                                            ring->link_chain | ring->cycle;
        ring->enqueue = 0;
        ring->cycle ^= 1;
    }
    return trb;
}

static xhci_trb *xhci_ring_put(xhci_ring *ring, u64 parameter, DWORD status,
                               DWORD control)
{
    return xhci_ring_put_cycle(ring, parameter, status, control, ring->cycle);
}

static xhci_trb *xhci_ring_buffer(xhci_ring *ring, u64 phys, DWORD len,
                                  DWORD control, DWORD final_control)
{
    xhci_trb *last = 0;
    while (len) {
        DWORD chunk = 0x10000u - (DWORD)(phys & 0xFFFF);
        DWORD trb_control = control;
        if (chunk > len)
            chunk = len;
        if (chunk < len)
            trb_control |= XHCI_TRB_CHAIN;
        else
            trb_control |= final_control;
        last = xhci_ring_put(ring, phys, chunk, trb_control);
        phys += chunk;
        len -= chunk;
    }
    return last;
}

static int xhci_trb_in_td(xhci_ring *ring, DWORD first, u64 pointer)
{
    u64 base;
    u64 delta;
    DWORD index;
    DWORD end;
    if (!ring)
        return 0;
    base = ring->dma.dma_addr;
    delta = pointer - base;
    index = (DWORD)(delta / sizeof(xhci_trb));
    end = ring->enqueue;
    return pointer >= base && !(delta & 0xF) && index < XHCI_TRBS - 1 &&
           ((first < end && index >= first && index < end) ||
            (first > end && (index >= first || index < end)));
}

static int xhci_stash_cdc_in_event(DWORD type, DWORD event_endpoint,
                                   DWORD event_slot, u64 pointer, DWORD cc,
                                   DWORD remain)
{
    if (!xhci_cdc_in_pending || xhci_cdc_in_done || !xhci_cdc_in_ring)
        return 0;
    if (type != XHCI_TRB_TRANSFER_EV || event_endpoint != xhci_cdc_in_epid)
        return 0;
    if (xhci_cdc_in_slot && event_slot != xhci_cdc_in_slot)
        return 0;
    /* Stash on slot+epid even when in_td misses. Orphaning a completed
       CDC IN (pending&&!done, no TRB armed) kills Pico RPC after MSC. */
    (void)pointer;
    xhci_cdc_in_done = 1;
    xhci_cdc_in_cc = cc;
    xhci_cdc_in_remain = remain;
    return 1;
}

/* Transfer events are matched against wanted_ring by TRB pointer (in_td) and
   by endpoint. A posted CDC IN completion is stashed instead of dropped so
   MSC/command waits do not steal it and do not need Stop Endpoint. */
static int xhci_next_event(xhci_hcd *hcd, DWORD wanted_type, u64 wanted_ptr,
                           xhci_ring *wanted_ring, DWORD first,
                           DWORD wanted_endpoint,
                           DWORD *completion, DWORD *slot, DWORD max_spins)
{
    DWORD spins;
    DWORD flags;
    volatile BYTE *intr = hcd->runtime + 0x20;
    if (!max_spins)
        max_spins = XHCI_TIMEOUT;
    for (spins = 0; spins < max_spins; spins++) {
        /* ELF/MSC load set usb_cdc_bulk_io_quiesced so a stuck CDC OUT
           must drop usb_io_lock; otherwise openfilex waits forever. */
        if (wanted_ring && xhci_ring_is_cdc(hcd, wanted_ring) &&
            usb_cdc_bulk_io_quiesced)
            return 0;
        /* Yield periodically. The CDC-ring wait needs it so a same-CPU
           waiter on usb_io_lock can run; the command-ring and MSC waits
           need it for the same reason during an xHCI rebind (hotplug
           reconnect), otherwise the console thread is wedged for the
           whole rebind duration and an ELF stream load reads garbage. */
        if ((spins & 0x7F) == 0x7F)
            taskswitch();
        if (wanted_ring) {
            if (hcd->usbdev_count &&
                !xhci_ring_owner_connected(hcd, wanted_ring)) {
                if (!xhci_ring_is_cdc(hcd, wanted_ring))
                    hcd->connection_lost = 1;
                return 0;
            }
        } else if (hcd->usbdev_count && !xhci_device_connected(hcd)) {
            hcd->connection_lost = 1;
            return 0;
        }
        xhci_trb *event = &hcd->event_trbs[hcd->event_dequeue];
        DWORD control = event->control;
        if ((control & 1) == hcd->event_cycle) {
            DWORD type = (control >> 10) & 0x3F;
            DWORD event_endpoint = (control >> 16) & 0x1F;
            u64 pointer = event->parameter & ~(u64)0xF;
            DWORD cc = event->status >> 24;
            DWORD event_slot = control >> 24;
            DWORD remain = event->status & 0xFFFFFFu;
            int cdc_in;
            hcd->event_dequeue++;
            if (hcd->event_dequeue == XHCI_EVENT_TRBS) {
                hcd->event_dequeue = 0;
                hcd->event_cycle ^= 1;
            }
            xhci_w64(intr, 0x18,
                     hcd->event_dma.dma_addr +
                     hcd->event_dequeue * sizeof(xhci_trb) | 8);
            cdc_in = xhci_stash_cdc_in_event(type, event_endpoint, event_slot,
                                             pointer, cc, remain);
            if (type == wanted_type && wanted_ring) {
                int in_td = xhci_trb_in_td(wanted_ring, first, pointer);
                if (!in_td || event_endpoint != wanted_endpoint)
                    continue;
            } else if (type != wanted_type ||
                       (wanted_ptr && pointer != wanted_ptr)) {
                continue;
            }
            if (type == wanted_type) {
                xhci_last_remain = remain;
                if (completion)
                    *completion = cc;
                if (slot)
                    *slot = event_slot;
                if (cdc_in && wanted_ring == xhci_cdc_in_ring)
                    xhci_cdc_in_done = 0;
                return 1;
            }
        } else {
            storeflags(&flags);
            if (hcd->has_msix && (flags & 0x200) &&
                !(spins & 0xFFFF)) {
                DWORD irq_before = hcd->irq_count;
                hcd->irq_wait_count++;
                asm volatile ("hlt" ::: "memory");
                if (!hcd->msix_qualified_reported &&
                    hcd->irq_count != irq_before &&
                    strstr(kernel_cmdline, "xhci-msix-test")) {
                    hcd->msix_qualified_reported = 1;
                    serial_puts("XHCI_MSIX_OK\n");
                }
            } else {
                usb_io_delay();
            }
        }
    }
    return 0;
}

static int xhci_command(xhci_hcd *hcd, u64 parameter, DWORD status,
                        DWORD control,
                        DWORD *slot)
{
    DWORD cc = 0;
    unsigned long long trb_dma;
    xhci_trb *trb;
    trb = xhci_ring_put(&hcd->cmd_ring, parameter, status, control);
    if (!dma_region_map(&hcd->cmd_ring.dma, trb, sizeof(*trb), &trb_dma))
        return 0;
    xhci_mb();
    hcd->doorbell[0] = 0;
    xhci_mb();
    if (!xhci_next_event(hcd, XHCI_TRB_COMMAND_EV, trb_dma, 0, 0, 0,
                         &cc, slot, XHCI_TIMEOUT)) {
        /* command events match by TRB pointer; no ring/slot filter needed */

        if (hcd->connection_lost)
            return 0;
        printf("xhci: command timeout type=%u\n", (control >> 10) & 0x3F);
        if (!xhci_no_recover)
            hcd->recovery_needed = 1;
        return 0;
    }
    if (cc != XHCI_CC_SUCCESS) {
        printf("xhci: command failed type=%u cc=%u\n",
               (control >> 10) & 0x3F, cc);
        if (!xhci_no_recover)
            hcd->recovery_needed = 1;
        return 0;
    }
    return 1;
}

static DWORD *xhci_input_context(xhci_hcd *hcd, DWORD index)
{
    return (DWORD *)(hcd->input_ctx + index * hcd->context_size);
}

static void xhci_free_dma_storage(xhci_hcd *hcd)
{
    dma_free_coherent(&hcd->cmd_ring.dma, free);
    dma_free_coherent(&hcd->event_dma, free);
    dma_free_coherent(&hcd->ep0_ring.dma, free);
    dma_free_coherent(&hcd->ep_in_ring.dma, free);
    dma_free_coherent(&hcd->ep_out_ring.dma, free);
    dma_free_coherent(&hcd->cdc_ep0_ring.dma, free);
    dma_free_coherent(&hcd->cdc_ep_in_ring.dma, free);
    dma_free_coherent(&hcd->cdc_ep_out_ring.dma, free);
    dma_free_coherent(&hcd->cdc_input_ctx_dma, free);
    dma_free_coherent(&hcd->cdc_device_ctx_dma, free);
    dma_free_coherent(&hcd->dcbaa_dma, free);
    dma_free_coherent(&hcd->erst_dma, free);
    dma_free_coherent(&hcd->input_ctx_dma, free);
    dma_free_coherent(&hcd->device_ctx_dma, free);
    hcd->dcbaa = 0;
    hcd->event_trbs = 0;
    hcd->erst = 0;
    hcd->input_ctx = 0;
    hcd->device_ctx = 0;
}

static int xhci_alloc_dma_storage(xhci_hcd *hcd)
{
    if (hcd->dcbaa_dma.cpu_addr)
        return 1;
    if (!dma_alloc_coherent(&hcd->cmd_ring.dma,
                            sizeof(xhci_trb) * XHCI_TRBS, 64,
                            0xFFFFFFFFULL, malloc, free) ||
        !dma_alloc_coherent(&hcd->event_dma,
                            sizeof(xhci_trb) * XHCI_EVENT_TRBS, 64,
                            0xFFFFFFFFULL, malloc, free) ||
        !dma_alloc_coherent(&hcd->ep0_ring.dma,
                            sizeof(xhci_trb) * XHCI_TRBS, 64,
                            0xFFFFFFFFULL, malloc, free) ||
        !dma_alloc_coherent(&hcd->ep_in_ring.dma,
                            sizeof(xhci_trb) * XHCI_TRBS, 64,
                            0xFFFFFFFFULL, malloc, free) ||
        !dma_alloc_coherent(&hcd->ep_out_ring.dma,
                            sizeof(xhci_trb) * XHCI_TRBS, 64,
                            0xFFFFFFFFULL, malloc, free) ||
        !dma_alloc_coherent(&hcd->dcbaa_dma, 256 * sizeof(u64), 64,
                            0xFFFFFFFFULL, malloc, free) ||
        !dma_alloc_coherent(&hcd->erst_dma, sizeof(xhci_erst_entry), 64,
                            0xFFFFFFFFULL, malloc, free) ||
        !dma_alloc_coherent(&hcd->input_ctx_dma, 33 * 64, 64,
                            0xFFFFFFFFULL, malloc, free) ||
        !dma_alloc_coherent(&hcd->device_ctx_dma, 32 * 64, 64,
                            0xFFFFFFFFULL, malloc, free) ||
        !dma_alloc_coherent(&hcd->cdc_input_ctx_dma, 33 * 64, 64,
                            0xFFFFFFFFULL, malloc, free) ||
        !dma_alloc_coherent(&hcd->cdc_device_ctx_dma, 32 * 64, 64,
                            0xFFFFFFFFULL, malloc, free) ||
        !dma_alloc_coherent(&hcd->cdc_ep0_ring.dma,
                            sizeof(xhci_trb) * XHCI_TRBS, 64,
                            0xFFFFFFFFULL, malloc, free) ||
        !dma_alloc_coherent(&hcd->cdc_ep_in_ring.dma,
                            sizeof(xhci_trb) * XHCI_TRBS, 64,
                            0xFFFFFFFFULL, malloc, free) ||
        !dma_alloc_coherent(&hcd->cdc_ep_out_ring.dma,
                            sizeof(xhci_trb) * XHCI_TRBS, 64,
                            0xFFFFFFFFULL, malloc, free)) {
        xhci_free_dma_storage(hcd);
        return 0;
    }
    hcd->dcbaa = (u64 *)hcd->dcbaa_dma.cpu_addr;
    hcd->event_trbs = (xhci_trb *)hcd->event_dma.cpu_addr;
    hcd->erst = (xhci_erst_entry *)hcd->erst_dma.cpu_addr;
    hcd->input_ctx = (BYTE *)hcd->input_ctx_dma.cpu_addr;
    hcd->device_ctx = (BYTE *)hcd->device_ctx_dma.cpu_addr;
    hcd->cdc_input_ctx = (BYTE *)hcd->cdc_input_ctx_dma.cpu_addr;
    hcd->cdc_device_ctx = (BYTE *)hcd->cdc_device_ctx_dma.cpu_addr;
    return 1;
}

static void xhci_fill_slot_context(xhci_hcd *hcd, DWORD dev, DWORD *slot,
                                   DWORD entries)
{
    xhci_fill_slot_context_dev(hcd, dev, slot, entries);
}

static void xhci_fill_ep_context(xhci_hcd *hcd, DWORD *ep, DWORD type,
                                 DWORD max_packet, DWORD max_burst,
                                 xhci_ring *ring, DWORD average)
{
    u64 dequeue = ring->dma.dma_addr +
                  (u64)ring->enqueue * sizeof(xhci_trb);
    dequeue |= ring->cycle & 1u;
    memset(ep, 0, hcd->context_size);
        ep[1] = (3u << 1) | (type << 3) | (max_burst << 8) |
            (max_packet << 16);
    ep[2] = (DWORD)dequeue;
    ep[3] = (DWORD)(dequeue >> 32);
    ep[4] = average;
}

static int xhci_setup_scratchpads(xhci_hcd *hcd, DWORD hcs2)
{
    DWORD high = (hcs2 >> 21) & 0x1F;
    DWORD low = (hcs2 >> 27) & 0x1F;
    DWORD count = (high << 5) | low;
    u64 *array;
    BYTE *pages;
    DWORD i;
    if (!count)
        return 1;
    if (count > 256) {
        printf("xhci: unsupported scratchpad count=%u\n", count);
        return 0;
    }
    if (!hcd->scratchpad_array_dma.cpu_addr ||
        !hcd->scratchpad_pages_dma.cpu_addr) {
        if (!dma_alloc_coherent(&hcd->scratchpad_array_dma,
                                count * sizeof(u64), 64, 0xFFFFFFFFULL,
                                malloc, free) ||
            !dma_alloc_coherent(&hcd->scratchpad_pages_dma,
                                count * 4096, 4096, 0xFFFFFFFFULL,
                                malloc, free)) {
            dma_free_coherent(&hcd->scratchpad_array_dma, free);
            dma_free_coherent(&hcd->scratchpad_pages_dma, free);
            printf("xhci: scratchpad allocation failed count=%u\n", count);
            return 0;
        }
        hcd->scratchpad_count = count;
    } else if (count != hcd->scratchpad_count) {
        printf("xhci: scratchpad count changed old=%u new=%u\n",
               hcd->scratchpad_count, count);
        return 0;
    }
    array = (u64 *)hcd->scratchpad_array_dma.cpu_addr;
    pages = (BYTE *)hcd->scratchpad_pages_dma.cpu_addr;
    memset(array, 0, count * sizeof(u64));
    memset(pages, 0, count * 4096);
    for (i = 0; i < count; i++)
        array[i] = hcd->scratchpad_pages_dma.dma_addr + i * 4096;
    hcd->dcbaa[0] = hcd->scratchpad_array_dma.dma_addr;
    printf("xhci: scratchpads=%u\n", count);
    return 1;
}

static int xhci_address_device(xhci_hcd *hcd, DWORD dev,
                               int block_set_address)
{
    DWORD *control;
    DWORD *slot;
    DWORD *ep0;
    xhci_usbdev *ud = xhci_ud(hcd, dev);
    xhci_ring *ep0_ring;
    if (!ud)
        return 0;
    ep0_ring = xhci_ep0_ring_for(hcd, dev);
    memset(xhci_input_ctx_base(hcd, dev), 0,
           dev ? hcd->cdc_input_ctx_dma.length
               : hcd->input_ctx_dma.length);
    control = xhci_input_context_dev(hcd, dev, 0);
    slot = xhci_input_context_dev(hcd, dev, 1);
    ep0 = xhci_input_context_dev(hcd, dev, 2);
    control[1] = 3;
    xhci_fill_slot_context(hcd, dev, slot, 1);
    xhci_fill_ep_context(hcd, ep0, 4, ud->ep0_mps, 0, ep0_ring, 8);
    return xhci_command(hcd,
                        dev ? hcd->cdc_input_ctx_dma.dma_addr
                            : hcd->input_ctx_dma.dma_addr, 0,
                        XHCI_TRB_ADDRESS | (block_set_address ? (1u << 9) : 0) |
                        (ud->slot << 24), 0);
}

static int xhci_evaluate_ep0(xhci_hcd *hcd, DWORD dev)
{
    DWORD *control;
    xhci_usbdev *ud = xhci_ud(hcd, dev);
    xhci_ring *ep0_ring;
    if (!ud)
        return 0;
    ep0_ring = xhci_ep0_ring_for(hcd, dev);
    memset(xhci_input_ctx_base(hcd, dev), 0,
           dev ? hcd->cdc_input_ctx_dma.length
               : hcd->input_ctx_dma.length);
    control = xhci_input_context_dev(hcd, dev, 0);
    control[1] = 3;
    xhci_fill_slot_context(hcd, dev, xhci_input_context_dev(hcd, dev, 1), 1);
    xhci_fill_ep_context(hcd, xhci_input_context_dev(hcd, dev, 2),
                         4, ud->ep0_mps, 0, ep0_ring, 8);
    return xhci_command(hcd,
                        dev ? hcd->cdc_input_ctx_dma.dma_addr
                            : hcd->input_ctx_dma.dma_addr, 0,
                        XHCI_TRB_EVALUATE | (ud->slot << 24), 0);
}

static int xhci_set_ep0_packet_size(xhci_hcd *hcd, DWORD dev, BYTE encoded)
{
    xhci_usbdev *ud = xhci_ud(hcd, dev);
    DWORD size = (ud && ud->speed >= 4) ? (1u << encoded) : encoded;
    DWORD old;
    if (!ud ||
        (size != 8 && size != 16 && size != 32 && size != 64 && size != 512)) {
        printf("xhci: invalid EP0 packet size=%u encoded=%u\n", size, encoded);
        return 0;
    }
    old = ud->ep0_mps;
    ud->ep0_mps = size;
    if (size == old)
        return 1;
    return xhci_evaluate_ep0(hcd, dev);
}

static DWORD xhci_buffer_trb_count(u64 dma_addr, DWORD len)
{
    DWORD count = 0;
    while (len) {
        DWORD chunk = 0x10000u - (DWORD)(dma_addr & 0xFFFF);
        if (chunk > len)
            chunk = len;
        dma_addr += chunk;
        len -= chunk;
        count++;
    }
    return count;
}

static int xhci_transfer_sg(xhci_hcd *hcd, DWORD dev, xhci_ring *ring,
                            DWORD endpoint, const dma_segment *segments,
                            DWORD segment_count, DWORD control,
                            DWORD direction)
{
    xhci_usbdev *ud = xhci_ud(hcd, dev);
    DWORD cc = 0;
    DWORD first = ring->enqueue;
    DWORD index;
    DWORD len = 0;
    DWORD trb_count = 0;
    int drop = hcd->fault_drop_next;
    int result = 0;
    dma_sg_mapping mapping = { { { 0 } }, 0, 0, 0 };
    xhci_trb *last;
    if (!ud)
        return 0;
    if (ud->slot && !xhci_usbdev_connected(hcd, dev)) {
        if (dev == 0)
            hcd->connection_lost = 1;
        return 0;
    }
    if (!dma_map_sg_device(&mapping, segments, segment_count, direction,
                           &hcd->stream_dma))
        return 0;
    for (index = 0; index < mapping.count; index++) {
        trb_count += xhci_buffer_trb_count(mapping.mappings[index].dma_addr,
                                           mapping.mappings[index].length);
        len += mapping.mappings[index].length;
        if (len < mapping.mappings[index].length ||
            trb_count >= XHCI_TRBS - 1)
            goto out;
    }
    for (index = 0; index < mapping.count; index++) {
        last = xhci_ring_buffer(ring, mapping.mappings[index].dma_addr,
                                mapping.mappings[index].length, control,
                                index + 1 == mapping.count ? XHCI_TRB_IOC :
                                                            XHCI_TRB_CHAIN);
        if (!last)
            goto out;
    }
    xhci_mb();
    if (drop) {
        hcd->fault_drop_next = 0;
        printf("xhci: test dropping bulk doorbell ep=%u\n", endpoint);
    } else {
        hcd->doorbell[ud->slot] = endpoint;
        xhci_mb();
    }
    if (hcd->fault_disconnect_inflight) {
        DWORD waits;
        hcd->fault_disconnect_inflight = 0;
        printf("XHCI_DISCONNECT_INFLIGHT\n");
           for (waits = 0; waits < XHCI_TIMEOUT &&
               xhci_usbdev_connected(hcd, dev); waits++)
            usb_io_delay();
    }
    if (!xhci_next_event(hcd, XHCI_TRB_TRANSFER_EV, 0,
                         ring, first, endpoint, &cc, 0,
                         xhci_ring_is_cdc(hcd, ring) ? XHCI_CDC_IN_SPINS
                                                     : XHCI_TIMEOUT)) {
        if (hcd->connection_lost)
            goto out;
        printf("xhci: transfer timeout ep=%u len=%u\n", endpoint, len);
        if (dev == 0)
            hcd->recovery_needed = 1;
        goto out;
    }
    xhci_mb();
    if (cc != XHCI_CC_SUCCESS && cc != XHCI_CC_SHORT_PACKET) {
        printf("xhci: transfer failed ep=%u cc=%u\n", endpoint, cc);
        if (cc == XHCI_CC_STALL && endpoint < 32)
            hcd->stalled_endpoints |= 1u << endpoint;
        else if (dev == 0)
            hcd->recovery_needed = 1;
        goto out;
    }
    result = 1;
    if (hcd->stream_dma.force_bounce) {
        if (direction == DMA_TO_DEVICE || direction == DMA_BIDIRECTIONAL)
            hcd->bounce_to_device_qualified = 1;
        if (direction == DMA_FROM_DEVICE || direction == DMA_BIDIRECTIONAL)
            hcd->bounce_from_device_qualified = 1;
        if (hcd->bounce_to_device_qualified &&
            hcd->bounce_from_device_qualified &&
            !hcd->bounce_qualified_reported) {
            hcd->bounce_qualified_reported = 1;
            serial_puts("XHCI_BOUNCE_OK\n");
        }
    }
out:
    dma_unmap_sg(&mapping);
    return result;
}

static int xhci_transfer(xhci_hcd *hcd, DWORD dev, xhci_ring *ring,
                         DWORD endpoint, BYTE *data, DWORD len, DWORD control,
                         DWORD direction)
{
    dma_segment segment;
    segment.cpu_addr = data;
    segment.length = len;
    return xhci_transfer_sg(hcd, dev, ring, endpoint, &segment, 1, control,
                            direction);
}

static int xhci_control(xhci_hcd *hcd, DWORD dev, usb_setup *setup,
                        void *data, int len)
{
    xhci_usbdev *ud = xhci_ud(hcd, dev);
    xhci_ring *ep0;
    xhci_trb *first_trb;
    DWORD first;
    DWORD direction;
    DWORD setup_flags;
    DWORD start_cycle;
    u64 setup_value = 0;
    int result = 0;
    int dir_in;
    dma_mapping mapping = {0};
    DWORD cc = 0;
    if (!ud)
        return 0;
    ep0 = xhci_ep0_ring_for(hcd, dev);
    first = ep0->enqueue;
    dir_in = (setup->bmRequestType & 0x80) != 0;
    direction = dir_in ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
    if (len && !dma_map_single_device(&mapping, data, (DWORD)len, direction,
                                      &hcd->stream_dma))
        return 0;
    memcpy(&setup_value, setup, 8);
    setup_flags = xhci_ctrl_setup_flags(len != 0, dir_in);
    start_cycle = ep0->cycle;
    /* Invert the Setup cycle until Data/Status are posted so the xHC
       cannot fetch an incomplete control TD. */
    first_trb = xhci_ring_put_cycle(ep0, setup_value, 8, setup_flags,
                                    start_cycle ^ 1);
    if (len) {
        if (!xhci_ring_buffer(ep0, mapping.dma_addr, (DWORD)len,
                      xhci_ctrl_data_flags(dir_in), 0))
            goto out;
    }
    xhci_ring_put(ep0, 0, 0, xhci_ctrl_status_flags(len != 0, dir_in));
    xhci_mb();
    first_trb->control = setup_flags | start_cycle;
    xhci_mb();
    hcd->doorbell[ud->slot] = 1;
    xhci_mb();
    if (!xhci_next_event(hcd, XHCI_TRB_TRANSFER_EV, 0,
                         ep0, first, 1, &cc, 0, XHCI_TIMEOUT)) {
        if (hcd->connection_lost)
            goto out;
        printf("xhci: control timeout request=%u len=%d\n",
               setup->bRequest, len);
        if (!hcd->enumerating)
            hcd->recovery_needed = 1;
        goto out;
    }
    xhci_mb();
    if (cc != XHCI_CC_SUCCESS && cc != XHCI_CC_SHORT_PACKET) {
        printf("xhci: control failed request=%u cc=%u%s\n",
               setup->bRequest, cc,
               cc == XHCI_CC_STALL ? " (stall)" : "");
        if (!hcd->enumerating)
            hcd->recovery_needed = 1;
        goto out;
    }
    result = 1;
out:
    if (mapping.active)
        dma_unmap_single(&mapping);
    return result;
}

static DWORD xhci_endpoint_id(BYTE endpoint, int in)
{
    return (DWORD)endpoint * 2 + (in ? 1 : 0);
}

static int xhci_bulk(xhci_hcd *hcd, DWORD dev, BYTE endpoint, int in,
                     BYTE *data, int len)
{
    DWORD endpoint_id = xhci_endpoint_id(endpoint, in);
    xhci_ring *ring = in ? xhci_ep_in_ring_for(hcd, dev)
                         : xhci_ep_out_ring_for(hcd, dev);
    if (!ring)
        return 0;
    /* MSC is usbdevs[0]; CDC is usbdevs[1]. Take a completed CDC IN so
       BOT does not race a late take; never Stop (policy). */
    if (dev == 0)
        xhci_cdc_quiesce_for_msc(hcd);
    if (len > 1 && strstr(kernel_cmdline, "xhci-sg-test")) {
        dma_segment segments[2];
        int result;
        segments[0].cpu_addr = data;
        segments[0].length = (DWORD)len / 2;
        segments[1].cpu_addr = data + segments[0].length;
        segments[1].length = (DWORD)len - segments[0].length;
        result = xhci_transfer_sg(hcd, dev, ring, endpoint_id, segments, 2,
                                  XHCI_TRB_NORMAL,
                                  in ? DMA_FROM_DEVICE : DMA_TO_DEVICE);
        if (result && !hcd->sg_qualified_reported) {
            hcd->sg_qualified_reported = 1;
            serial_puts("XHCI_SG_OK\n");
        }
        return result;
    }
    return xhci_transfer(hcd, dev, ring, endpoint_id, data, (DWORD)len,
                         XHCI_TRB_NORMAL,
                         in ? DMA_FROM_DEVICE : DMA_TO_DEVICE);
}

static int xhci_stop_endpoint(xhci_hcd *hcd, DWORD dev, DWORD endpoint,
                              xhci_ring *ring)
{
    xhci_usbdev *ud = xhci_ud(hcd, dev);
    u64 dequeue;
    unsigned long long trb_dma;
    int saved = xhci_no_recover;
    if (!ud || !ring)
        return 0;
    xhci_no_recover = 1;
    if (!xhci_command(hcd, 0, 0, XHCI_TRB_STOP_EP |
                      (endpoint << 16) | (ud->slot << 24), 0)) {
        xhci_no_recover = saved;
        return 0;
    }
    if (!dma_region_map(&ring->dma, &ring->trbs[ring->enqueue],
                        sizeof(xhci_trb), &trb_dma)) {
        xhci_no_recover = saved;
        return 0;
    }
    dequeue = trb_dma | ring->cycle;
    if (!xhci_command(hcd, dequeue, 0, XHCI_TRB_SET_DEQUEUE |
                      (endpoint << 16) | (ud->slot << 24), 0)) {
        xhci_no_recover = saved;
        return 0;
    }
    xhci_no_recover = saved;
    return 1;
}

static void xhci_cdc_in_drop(xhci_hcd *hcd)
{
    if (!xhci_cdc_in_pending)
        return;
    if (hcd && hcd == xhci_cdc_in_hcd && xhci_cdc_in_ring)
        (void)xhci_stop_endpoint(hcd, xhci_cdc_in_dev, xhci_cdc_in_epid,
                                 xhci_cdc_in_ring);
    dma_unmap_sg(&xhci_cdc_in_map);
    xhci_cdc_in_pending = 0;
    xhci_cdc_in_done = 0;
    xhci_cdc_in_hcd = 0;
    xhci_cdc_in_ring = 0;
    xhci_cdc_in_cpu = 0;
    xhci_cdc_in_len = 0;
    xhci_cdc_in_slot = 0;
}

/* Forget a posted CDC IN without Stop-EP. Used for ELF stream loads —
   Stop wedged Intel xHCI so the following MSC read never completed. */
static void xhci_cdc_in_abandon(void)
{
    if (!xhci_cdc_in_pending)
        return;
    dma_unmap_sg(&xhci_cdc_in_map);
    xhci_cdc_in_pending = 0;
    xhci_cdc_in_done = 0;
    xhci_cdc_in_hcd = 0;
    xhci_cdc_in_ring = 0;
    xhci_cdc_in_cpu = 0;
    xhci_cdc_in_len = 0;
    xhci_cdc_in_slot = 0;
}

static int xhci_cdc_in_take(int *got, int len)
{
    DWORD cc;
    if (!xhci_cdc_in_pending || !xhci_cdc_in_done)
        return -1;
    cc = xhci_cdc_in_cc;
    xhci_last_remain = xhci_cdc_in_remain;
    dma_unmap_sg(&xhci_cdc_in_map);
    xhci_cdc_in_pending = 0;
    xhci_cdc_in_done = 0;
    xhci_cdc_in_hcd = 0;
    xhci_cdc_in_ring = 0;
    xhci_cdc_in_cpu = 0;
    xhci_cdc_in_len = 0;
    xhci_cdc_in_slot = 0;
    if (cc != XHCI_CC_SUCCESS && cc != XHCI_CC_SHORT_PACKET)
        return 0;
    if (got) {
        int n = len - (int)xhci_last_remain;
        if (n < 0)
            n = 0;
        if (n > len)
            n = len;
        *got = n;
    }
    return 1;
}

/* Before MSC/BOT: drain a completed CDC IN. Live TRBs stay posted;
   xhci_next_event stashes their Transfer Events. Stop-EP only if
   policy allows (currently never — wedges Intel CDC IN). */
static void xhci_cdc_quiesce_for_msc(xhci_hcd *hcd)
{
    if (!xhci_cdc_in_pending)
        return;
    if (xhci_cdc_in_done) {
        (void)xhci_cdc_in_take(0, 0);
        return;
    }
    if (xhci_cdc_in_cancel_for_msc())
        xhci_cdc_in_drop(hcd ? hcd : xhci_cdc_in_hcd);
}

/* Short-timeout bulk IN. 1 = completed (got may be 0 on ZLP), 0 = no data
   yet. Empty polls leave the TRB posted unless policy says to stop.
   max_spins == 0: take if done / post if idle, do not wait (MSC kick). */
static int xhci_bulk_in_try(xhci_hcd *hcd, DWORD dev, BYTE endpoint,
                            BYTE *data, int len, int *got, DWORD max_spins)
{
    DWORD endpoint_id = xhci_endpoint_id(endpoint, 1);
    xhci_ring *ring = xhci_ep_in_ring_for(hcd, dev);
    xhci_usbdev *ud = xhci_ud(hcd, dev);
    dma_segment segment;
    DWORD cc = 0;
    int result = 0;
    int saved;

    if (got)
        *got = 0;
    if (!ud || !ring || !data || len <= 0)
        return 0;
    if (ud->slot && !xhci_usbdev_connected(hcd, dev))
        return 0;

    /* Never Stop-EP a live CDC IN on buffer/len mismatch — that wedges
       Intel ADL-N. Soft-abandon and re-post with the new mapping. */
    if (xhci_cdc_in_pending &&
        (xhci_cdc_in_hcd != hcd || xhci_cdc_in_dev != dev ||
         xhci_cdc_in_cpu != data || xhci_cdc_in_len != len))
        xhci_cdc_in_abandon();


    {
        int taken = xhci_cdc_in_take(got, len);
        if (taken >= 0)
            return taken;
    }

    if (!xhci_cdc_in_pending) {
        segment.cpu_addr = data;
        segment.length = (DWORD)len;
        xhci_cdc_in_first = ring->enqueue;
        if (!dma_map_sg_device(&xhci_cdc_in_map, &segment, 1, DMA_FROM_DEVICE,
                               &hcd->stream_dma))
            return 0;
        if (!xhci_ring_buffer(ring, xhci_cdc_in_map.mappings[0].dma_addr,
                              xhci_cdc_in_map.mappings[0].length,
                              XHCI_TRB_NORMAL, xhci_cdc_in_trb_flags())) {
            dma_unmap_sg(&xhci_cdc_in_map);
            return 0;
        }
        xhci_mb();
        hcd->doorbell[ud->slot] = endpoint_id;
        xhci_mb();
        xhci_cdc_in_pending = 1;
        xhci_cdc_in_hcd = hcd;
        xhci_cdc_in_dev = dev;
        xhci_cdc_in_epid = endpoint_id;
        xhci_cdc_in_slot = ud->slot;
        xhci_cdc_in_ring = ring;
        xhci_cdc_in_len = len;
        xhci_cdc_in_cpu = data;
        xhci_cdc_in_done = 0;
    }

    if (max_spins == 0)
        return 0;

    saved = xhci_no_recover;
    xhci_no_recover = 1;
    if (!xhci_next_event(hcd, XHCI_TRB_TRANSFER_EV, 0, ring,
                         xhci_cdc_in_first, endpoint_id, &cc, 0,
                         max_spins)) {
        xhci_no_recover = saved;
        if (xhci_cdc_in_stop_on_timeout())
            xhci_cdc_in_drop(hcd);
        return 0;
    }
    xhci_no_recover = saved;
    dma_unmap_sg(&xhci_cdc_in_map);
    xhci_cdc_in_pending = 0;
    xhci_cdc_in_done = 0;
    xhci_cdc_in_hcd = 0;
    xhci_cdc_in_ring = 0;
    xhci_cdc_in_cpu = 0;
    xhci_cdc_in_len = 0;
    xhci_cdc_in_slot = 0;
    if (cc != XHCI_CC_SUCCESS && cc != XHCI_CC_SHORT_PACKET)
        return 0;
    result = 1;
    if (got) {
        int n = len - (int)xhci_last_remain;
        if (n < 0)
            n = 0;
        if (n > len)
            n = len;
        *got = n;
    }
    return result;
}

static int xhci_recover_endpoint(xhci_hcd *hcd, DWORD dev, DWORD endpoint,
                                 xhci_ring *ring)
{
    xhci_usbdev *ud = xhci_ud(hcd, dev);
    u64 dequeue;
    unsigned long long trb_dma;
    if (!ud || !ring)
        return 1;
    if (endpoint >= 32 || !(hcd->stalled_endpoints & (1u << endpoint)))
        return 1;
    if (!xhci_command(hcd, 0, 0, XHCI_TRB_RESET_EP |
                      (endpoint << 16) | (ud->slot << 24), 0))
        return 0;
    if (!dma_region_map(&ring->dma, &ring->trbs[ring->enqueue],
                        sizeof(xhci_trb), &trb_dma))
        return 0;
    dequeue = trb_dma | ring->cycle;
    if (!xhci_command(hcd, dequeue, 0, XHCI_TRB_SET_DEQUEUE |
                      (endpoint << 16) | (ud->slot << 24), 0))
        return 0;
    hcd->stalled_endpoints &= ~(1u << endpoint);
    return 1;
}

static int xhci_recover_bulk_endpoints(xhci_hcd *hcd, DWORD dev, BYTE ep_in,
                                       BYTE ep_out)
{
    DWORD in_id = xhci_endpoint_id(ep_in, 1);
    DWORD out_id = xhci_endpoint_id(ep_out, 0);
    if (!xhci_recover_endpoint(hcd, dev, in_id,
                               xhci_ep_in_ring_for(hcd, dev)) ||
        !xhci_recover_endpoint(hcd, dev, out_id,
                               xhci_ep_out_ring_for(hcd, dev)))
        return 0;
    return hcd->stalled_endpoints == 0;
}

static int xhci_configure_endpoints(xhci_hcd *hcd, DWORD dev, BYTE ep_in,
                                    WORD mps_in, BYTE burst_in, BYTE ep_out,
                                    WORD mps_out, BYTE burst_out)
{
    xhci_usbdev *ud = xhci_ud(hcd, dev);
    xhci_ring *in_ring = xhci_ep_in_ring_for(hcd, dev);
    xhci_ring *out_ring = xhci_ep_out_ring_for(hcd, dev);
    DWORD in_id, out_id, entries;
    DWORD *control;
    if (!ud || !out_ring)
        return 0;
    out_id = xhci_endpoint_id(ep_out, 0);
    in_id = in_ring ? xhci_endpoint_id(ep_in, 1) : 0;
    entries = in_id > out_id ? in_id : out_id;
    memset(xhci_input_ctx_base(hcd, dev), 0,
           dev ? hcd->cdc_input_ctx_dma.length : hcd->input_ctx_dma.length);
    if ((in_ring && !xhci_ring_init(in_ring, XHCI_TRB_CHAIN)) ||
        !xhci_ring_init(out_ring, XHCI_TRB_CHAIN)) {
        printf("xhci: bulk ring DMA mapping failed\n");
        return 0;
    }
    control = xhci_input_context_dev(hcd, dev, 0);
    control[1] = (1u << 0) | (in_id ? (1u << in_id) : 0) | (1u << out_id);
    xhci_fill_slot_context(hcd, dev, xhci_input_context_dev(hcd, dev, 1),
                           entries);
    if (in_ring)
        xhci_fill_ep_context(hcd, xhci_input_context_dev(hcd, dev, in_id + 1),
                             6, mps_in, burst_in, in_ring, USB_BULK_MAX);
    xhci_fill_ep_context(hcd, xhci_input_context_dev(hcd, dev, out_id + 1), 2,
                         mps_out, burst_out, out_ring, USB_BULK_MAX);
    if (!xhci_command(hcd,
                      dev ? hcd->cdc_input_ctx_dma.dma_addr
                          : hcd->input_ctx_dma.dma_addr, 0,
                      XHCI_TRB_CONFIG_EP | (ud->slot << 24), 0))
        return 0;
    printf("xhci: configured bulk endpoints in=%u out=%u (dev=%u)\n",
           in_id, out_id, dev);
    return 1;
}

static int xhci_legacy_handoff(xhci_hcd *hcd, DWORD hcc, u64 bar_size)
{
    DWORD off = ((hcc >> 16) & 0xFFFF) * 4;
    DWORD guard = 64;
    while (off && (u64)off + 8 <= bar_size && guard--) {
        DWORD cap = xhci_r32(hcd->mmio, off);
        DWORD id = cap & 0xFF;
        DWORD next = (cap >> 8) & 0xFF;
        if (id == 1) {
            DWORD value = cap | (1u << 24);
            xhci_w32(hcd->mmio, off, value);
            if (!xhci_wait32(hcd->mmio, off, 1u << 16, 0)) {
                printf("xhci: firmware ownership handoff timed out\n");
                return 0;
            }
            xhci_w32(hcd->mmio, off + 4, 0);
            return 1;
        }
        if (!next)
            break;
        off += next * 4;
    }
    return 1;
}

static int xhci_reset_port_dev(xhci_hcd *hcd, DWORD dev)
{
    xhci_usbdev *ud = xhci_ud(hcd, dev);
    DWORD p;
    DWORD off;
    if (!ud || !ud->port)
        return 0;
    off = 0x400 + (ud->port - 1) * 0x10;
    p = xhci_r32(hcd->op, off);
    if (!(p & XHCI_PORT_CCS)) {
        printf("xhci: port %u reset failed sc=0x%x (no ccs)\n",
               ud->port, p);
        return 0;
    }
    /* Same PORTSC write that enabled ports 4/5 on the N150. Clearing PRC
       or calling usb_wait_ms here dropped PED on that controller. */
    xhci_w32(hcd->op, off, (p & ~XHCI_PORT_CHANGE) | XHCI_PORT_RESET);
    if (!xhci_wait32(hcd->op, off, XHCI_PORT_RESET, 0)) {
        printf("xhci: port %u reset failed sc=0x%x (pr stuck)\n",
               ud->port, xhci_r32(hcd->op, off));
        return 0;
    }
    p = xhci_r32(hcd->op, off);
    ud->speed = XHCI_PORT_SPEED(p);
    if (ud->speed >= 4)
        ud->ep0_mps = 512;
    else if (ud->speed == 3)
        ud->ep0_mps = 64;
    else
        ud->ep0_mps = 8;
    if ((p & (XHCI_PORT_CCS | XHCI_PORT_PED)) !=
        (XHCI_PORT_CCS | XHCI_PORT_PED)) {
        printf("xhci: port %u reset failed sc=0x%x (no ped speed=%u)\n",
               ud->port, p, ud->speed);
        return 0;
    }
    usb_spin_ms(10);
    return 1;
}

static void xhci_recount_usbdevs(xhci_hcd *hcd)
{
    DWORD i, n = 0;
    for (i = 0; i < XHCI_MAX_USBDEVS; i++)
        if (hcd->usbdevs[i].port)
            n = i + 1;
    hcd->usbdev_count = n;
}

static int xhci_release_dev(xhci_hcd *hcd, DWORD dev)
{
    xhci_usbdev *ud = xhci_ud(hcd, dev);
    if (!hcd || !ud)
        return 1;
    if (ud->slot) {
        (void)xhci_command(hcd, 0, 0,
                           XHCI_TRB_DISABLE_SLOT | (ud->slot << 24), 0);
        if (ud->slot < 256)
            hcd->dcbaa[ud->slot] = 0;
    }
    ud->slot = 0;
    ud->port = 0;
    ud->speed = 0;
    xhci_recount_usbdevs(hcd);
    return 1;
}

static int xhci_claim_port_dev(xhci_hcd *hcd, DWORD port, DWORD dev)
{
    xhci_usbdev *ud;
    xhci_ring *ep0;
    if (!hcd || !port || port > hcd->max_ports || dev >= XHCI_MAX_USBDEVS)
        return 0;
    xhci_release_dev(hcd, dev);
    ep0 = xhci_ep0_ring_for(hcd, dev);
    if (!xhci_ring_init(ep0, XHCI_TRB_CHAIN))
        return 0;
    memset(&hcd->usbdevs[dev], 0, sizeof(hcd->usbdevs[dev]));
    hcd->usbdevs[dev].port = port;
    if (hcd->usbdev_count <= dev)
        hcd->usbdev_count = dev + 1;
    if (!xhci_reset_port_dev(hcd, dev)) {
        hcd->usbdevs[dev].port = 0;
        xhci_recount_usbdevs(hcd);
        return 0;
    }
    ud = &hcd->usbdevs[dev];
    ud->slot = 0;
    if (dev == 0) {
        hcd->connection_lost = 0;
        hcd->stalled_endpoints = 0;
    }
    if (!xhci_command(hcd, 0, 0, XHCI_TRB_ENABLE_SLOT, &ud->slot) ||
        !ud->slot) {
        printf("xhci: port %u enable slot failed\n", port);
        hcd->usbdevs[dev].port = 0;
        xhci_recount_usbdevs(hcd);
        return 0;
    }
    hcd->dcbaa[ud->slot] = dev ? hcd->cdc_device_ctx_dma.dma_addr
                               : hcd->device_ctx_dma.dma_addr;
    /* BSR=0: the xHC issues SET_ADDRESS. GET_DESCRIPTOR follows on EP0. */
    if (!xhci_address_device(hcd, dev, 0)) {
        printf("xhci: port %u address failed\n", port);
        xhci_release_dev(hcd, dev);
        return 0;
    }
    usb_spin_ms(2);
    printf("xhci: controller at PCI %u:%u.%u devs=%u port=%u speed=%u slot=%u ctx=%u\n",
           hcd->pci_bus, hcd->pci_slot, hcd->pci_func,
           hcd->usbdev_count, ud->port, ud->speed, ud->slot, hcd->context_size);
    return 1;
}

static int xhci_claim_port(xhci_hcd *hcd, DWORD port)
{
    return xhci_claim_port_dev(hcd, port, 0);
}

static int xhci_init_hcd(xhci_hcd *hcd)
{
    BYTE bus, slot, func;
    u64 bar = 0, bar_size = 0, required;
    DWORD caplen, hcs1, hcs2, hcc, dboff, rtsoff;
    volatile BYTE *intr;
    if (!hcd)
        return 0;
    if (!hcd->pci_discovered && !xhci_discover_hcds()) {
        printf("xhci: no controller found\n");
        return 0;
    }
    if (!hcd->pci_discovered) {
        printf("xhci: HCD has no discovered PCI function\n");
        return 0;
    }
    bus = hcd->pci_bus;
    slot = hcd->pci_slot;
    func = hcd->pci_func;
    if (hcd->fault_fail_init) {
        hcd->fault_fail_init = 0;
        printf("xhci: test forcing recovery initialization failure\n");
        return 0;
    }
    if (!xhci_pci_bar(bus, slot, func, &bar, &bar_size) ||
        bar + bar_size < bar) {
        printf("xhci: unsupported BAR 0x%llx\n", (unsigned long long)bar);
        return 0;
    }
    {
        DWORD id = pci_read32(bus, slot, func, 0);
        printf("xhci: PCI %u:%u.%u id=%04x:%04x BAR=0x%llx size=%llu\n",
               bus, slot, func, (unsigned)(id & 0xFFFFu),
               (unsigned)((id >> 16) & 0xFFFFu),
               (unsigned long long)bar, (unsigned long long)bar_size);
    }
    pci_write16(bus, slot, func, 0x04,
                pci_read16(bus, slot, func, 0x04) | 0x06);
    hcd->mmio = (volatile BYTE *)mmio_map(bar, bar_size);
    if (!hcd->mmio) {
        printf("xhci: cannot map BAR 0x%llx size=%llu\n",
               (unsigned long long)bar, (unsigned long long)bar_size);
        return 0;
    }
    caplen = *(volatile BYTE *)hcd->mmio;
    hcs1 = xhci_r32(hcd->mmio, 0x04);
    hcs2 = xhci_r32(hcd->mmio, 0x08);
    hcc = xhci_r32(hcd->mmio, 0x10);
    dboff = xhci_r32(hcd->mmio, 0x14) & ~3u;
    rtsoff = xhci_r32(hcd->mmio, 0x18) & ~0x1Fu;
    hcd->op = hcd->mmio + caplen;
    hcd->runtime = hcd->mmio + rtsoff;
    hcd->doorbell = (volatile DWORD *)(hcd->mmio + dboff);
    hcd->context_size = (hcc & (1u << 2)) ? 64 : 32;
    hcd->max_ports = (hcs1 >> 24) & 0xFF;
    hcd->stream_dma.dma_mask = 0xFFFFFFFFULL;
    hcd->stream_dma.bounce_alignment = 64;
    hcd->stream_dma.force_bounce =
        strstr(kernel_cmdline, "xhci-bounce-test") != 0;
    hcd->stream_dma.allocate = malloc;
    hcd->stream_dma.release = free;
    hcd->bounce_to_device_qualified = 0;
    hcd->bounce_from_device_qualified = 0;
    hcd->bounce_qualified_reported = 0;
    required = caplen + 0x400 + hcd->max_ports * 0x10;
    if ((u64)required > bar_size || (u64)dboff + 0x24 > bar_size ||
        (u64)rtsoff + 0x40 > bar_size) {
        printf("xhci: register offsets exceed BAR size=%llu\n",
               (unsigned long long)bar_size);
        return 0;
    }
    if (!xhci_legacy_handoff(hcd, hcc, bar_size))
        return 0;
    if (strstr(kernel_cmdline, "xhci-vector-reservation-test") &&
        !xhci_test_vector_reserved) {
        if (!irq_vector_reserve(IRQ_VECTOR_FIRST_DEVICE,
                                &xhci_test_reservation_owner)) {
            printf("xhci: test platform vector reservation failed\n");
            return 0;
        }
        xhci_test_vector_reserved = 1;
        printf("xhci: test reserved platform vector=%u\n",
               IRQ_VECTOR_FIRST_DEVICE);
    }
    if (strstr(kernel_cmdline, "xhci-poll-test") ||
        !xhci_allow_msix(serial_com1_present(), lapic_x2apic_enabled())) {
        hcd->has_msix = 0;
        if (strstr(kernel_cmdline, "xhci-poll-test"))
            printf("xhci: test forcing polling fallback\n");
        printf("xhci: polling (com1=%d x2apic=%d)\n",
               serial_com1_present() ? 1 : 0,
               lapic_x2apic_enabled() ? 1 : 0);
    } else if (!xhci_setup_msix(hcd, bus, slot, func)) {
        printf("xhci: MSI-X unavailable; polling\n");
    }

    xhci_w32(hcd->op, 0x00, xhci_r32(hcd->op, 0x00) & ~XHCI_CMD_RUN);
    if (!xhci_wait32(hcd->op, 0x04, XHCI_STS_HALTED, XHCI_STS_HALTED))
        return 0;
    xhci_w32(hcd->op, 0x00, XHCI_CMD_RESET);
    if (!xhci_wait32(hcd->op, 0x00, XHCI_CMD_RESET, 0) ||
        !xhci_wait32(hcd->op, 0x04, XHCI_STS_CNR, 0)) {
        printf("xhci: reset timed out\n");
        return 0;
    }
    if (!(xhci_r32(hcd->op, 0x08) & 1)) {
        printf("xhci: 4K pages unsupported\n");
        return 0;
    }

    if (!xhci_alloc_dma_storage(hcd)) {
        printf("xhci: controller DMA allocation failed\n");
        return 0;
    }
    memset(hcd->dcbaa, 0, hcd->dcbaa_dma.length);
    if (!xhci_setup_scratchpads(hcd, hcs2))
        return 0;
    memset(hcd->event_trbs, 0, hcd->event_dma.length);
    memset(hcd->device_ctx, 0, hcd->device_ctx_dma.length);
    if (!xhci_ring_init(&hcd->cmd_ring, 0) ||
        !xhci_ring_init(&hcd->ep0_ring, XHCI_TRB_CHAIN)) {
        printf("xhci: command ring DMA mapping failed\n");
        return 0;
    }
    hcd->event_dequeue = 0;
    hcd->event_cycle = 1;
    hcd->erst->address = hcd->event_dma.dma_addr;
    hcd->erst->size = XHCI_EVENT_TRBS;
    hcd->erst->reserved = 0;
    intr = hcd->runtime + 0x20;
    xhci_w32(intr, 0x08, 1);
    xhci_w64(intr, 0x10, hcd->erst_dma.dma_addr);
    xhci_w64(intr, 0x18, hcd->event_dma.dma_addr);
    xhci_w64(hcd->op, 0x30, hcd->dcbaa_dma.dma_addr);
    xhci_w64(hcd->op, 0x18, hcd->cmd_ring.dma.dma_addr | 1);
    xhci_w32(hcd->op, 0x38,
             ((hcs1 & 0xFF) < XHCI_MAX_SLOTS) ? (hcs1 & 0xFF) : XHCI_MAX_SLOTS);
    if (hcd->has_msix)
        xhci_w32(intr, 0x00, 2);
    xhci_w32(hcd->op, 0x00, XHCI_CMD_RUN |
             (hcd->has_msix ? XHCI_CMD_INTE : 0));
    if (!xhci_wait32(hcd->op, 0x04, XHCI_STS_HALTED, 0)) {
        printf("xhci: controller failed to run\n");
        return 0;
    }
    {
        DWORD ccs = xhci_wait_connected_ports(hcd);
        DWORD psc, p;
        printf("xhci: ports=%u ccs=0x%x ctx=%u irq=%s\n",
               hcd->max_ports, ccs, hcd->context_size,
               hcd->has_msix ? "msix" : "poll");
        for (p = 1; p <= hcd->max_ports && p <= 32; p++) {
            if (!(ccs & xhci_ccs_bit(p)))
                continue;
            psc = xhci_r32(hcd->op, 0x400 + (p - 1) * 0x10);
            printf("xhci: port %u sc=0x%x speed=%u\n",
                   p, psc, XHCI_PORT_SPEED(psc));
        }
        if (!ccs) {
            printf("xhci: no enabled device port\n");
            return 0;
        }
    }
    hcd->usbdev_count = 0;
    hcd->enumerating = 0;
    memset(hcd->usbdevs, 0, sizeof(hcd->usbdevs));
    return 1;
}

static void xhci_stop_hcd(xhci_hcd *hcd)
{
    WORD control;
    xhci_cdc_in_drop(hcd);
    if (hcd->runtime && hcd->has_msix)
        xhci_w32(hcd->runtime + 0x20, 0x00, 1);
    if (hcd->op)
        xhci_w32(hcd->op, 0x00,
                 xhci_r32(hcd->op, 0x00) &
                 ~(XHCI_CMD_RUN | XHCI_CMD_INTE));
    if (!hcd->has_msix)
        return;
    if (hcd->op)
        (void)xhci_wait32(hcd->op, 0x04, XHCI_STS_HALTED,
                          XHCI_STS_HALTED);
    control = pci_read16(hcd->pci_bus, hcd->pci_slot, hcd->pci_func,
                         hcd->msix_cap + 2);
    pci_write16(hcd->pci_bus, hcd->pci_slot, hcd->pci_func,
                hcd->msix_cap + 2, (WORD)(control & ~0x8000));
    if (hcd->msix_entry) {
        hcd->msix_entry[3] = 1;
        xhci_mb();
    }
    if (!irq_vector_release(hcd->irq_vector, &hcd->irq_owner))
        printf("xhci: MSI-X vector release timed out\n");
    else {
        xhci_irq_route_unbind(hcd);
        hcd->irq_vector = 0;
    }
    hcd->has_msix = 0;
    hcd->msix_cap = 0;
    hcd->msix_entry = 0;
}

void xhci_irq(DWORD route)
{
    xhci_hcd *hcd;
    if (route >= XHCI_MAX_HCDS || !(hcd = xhci_irq_routes[route])) {
        lapic_eoi();
        return;
    }
    int entered = irq_vector_enter(hcd->irq_vector, &hcd->irq_owner);
    if (entered && hcd->runtime && hcd->has_msix) {
        volatile BYTE *intr = hcd->runtime + 0x20;
        DWORD iman = xhci_r32(intr, 0x00);
        hcd->irq_count++;
        xhci_w32(intr, 0x00, (iman & 2) | 1);
    }
    if (entered)
        irq_vector_exit(hcd->irq_vector, &hcd->irq_owner);
    lapic_eoi();
}
