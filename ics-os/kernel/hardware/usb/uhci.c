/*
  Name: uhci.c
  Description: Minimal UHCI host controller + USB Mass Storage (BBB/BOT)
               driver. Enumerates a USB thumb drive, registers it as a
               block device (usb0) and exposes MBR partitions (usb0p0, ...).

               Designed for QEMU (-device piix3-usb-uhci -device usb-storage)
               and PCs that still present a UHCI companion controller.
               Transfers are polled so the driver does not depend on PCI IRQs.
*/

#include "usb.h"
#include "usb_identity.h"
#include "usb_cdc_acm.h"
#include "usb_cdc_ecm.h"
#include "usb_cdc_rndis.h"
#include "usb_asix.h"
#include "usb_debug.h"
#include "../dma.h"
#include "../keyboard/kbd_boot_leds.h"
#include "../../console/klog.h"
/* CDC-ECM NIC: expose the USB bulk endpoints as a polled netif. The net
   stack lives in separate net_*.o objects linked with kernel32.o. */
#include "../../net/netif.h"
#include "../../net/pbuf.h"
#include "../../net/ethernet.h"
#include "../../net/inet_config.h"
#include "../../net/dhcp.h"
#include "../../net/icmp.h"
#include "../../net/udp.h"
#include "../../net/tcp.h"
#include "../../net/dns.h"
#include "../../net/arp.h"
#include "../../net/net_sync.h"
#include "../../net/softnet.h"

extern void serial_puts(const char *s);
extern int console_execute(const char *str);
extern void tty_input_fg(int c);
extern void machine_reboot(void);
extern int kexec_load_mem(const void *img, unsigned int sz);
extern void kexec_reboot(void);
extern int fbconsole_geom(unsigned int *width, unsigned int *height,
                          unsigned int *bpp);
extern int fbconsole_rgb_at(unsigned int x, unsigned int y,
                            unsigned char *r, unsigned char *g,
                            unsigned char *b);
extern unsigned int klog_count(void);
extern void klog_dump(int level_filter);
extern int kernel_kexeced;
extern char kernel_cmdline[];
extern void *malloc(unsigned int n);
extern void free(void *p);
extern int sprintf(char *s, const char *fmt, ...);

#define USB_BULK_MAX        (32 * 1024)
#define USB_MAX_TD          (USB_BULK_MAX / 64)
/* Max SCSI data per BOT command. Sized to the bounce buffer below; 16 KiB
   (32 x 512B) keeps the UHCI TD budget comfortable while freeing 16 KiB of
   BSS so the kernel stays under the 4 MiB user-ELF limit. */
#define MSC_BULK_MAX        (16 * 1024)
#define USB_TIMEOUT         2000000

#define UHCI_USBCMD         0x00
#define UHCI_USBSTS         0x02
#define UHCI_USBINTR        0x04
#define UHCI_FRNUM          0x06
#define UHCI_FLBASEADD      0x08
#define UHCI_SOFMOD         0x0C
#define UHCI_PORTSC1        0x10

#define UHCI_CMD_RS         0x0001
#define UHCI_CMD_HCRESET    0x0002
#define UHCI_CMD_GRESET     0x0004
#define UHCI_CMD_EGSM       0x0008
#define UHCI_CMD_FGR        0x0010
#define UHCI_CMD_SWDBG      0x0020
#define UHCI_CMD_CF         0x0040
#define UHCI_CMD_MAXP       0x0080

#define UHCI_PORT_CCS       0x0001
#define UHCI_PORT_CSC       0x0002
#define UHCI_PORT_PE        0x0004
#define UHCI_PORT_PEC       0x0008
#define UHCI_PORT_LS        0x0100
#define UHCI_PORT_RD        0x0200
#define UHCI_PORT_RESET     0x0200

#define TD_LINK_TERM        0x1
#define TD_LINK_QH          0x2
#define TD_LINK_VF          0x4

#define TD_CS_ACTLEN_MASK   0x7FF
#define TD_CS_BITSTUFF      (1 << 17)
#define TD_CS_CRC           (1 << 18)
#define TD_CS_NAK           (1 << 19)
#define TD_CS_BABBLE        (1 << 20)
#define TD_CS_DATABUF       (1 << 21)
#define TD_CS_STALLED       (1 << 22)
#define TD_CS_ACTIVE        (1 << 23)
#define TD_CS_IOC           (1 << 24)
#define TD_CS_IOS           (1 << 25)
#define TD_CS_LS            (1 << 26)
#define TD_CS_ERRCNT        (3 << 27)
#define TD_CS_SPD           (1 << 29)

#define TD_PID_SETUP        0x2D
#define TD_PID_IN           0x69
#define TD_PID_OUT          0xE1

#define USB_REQ_GET_STATUS        0x00
#define USB_REQ_CLEAR_FEATURE     0x01
#define USB_REQ_SET_FEATURE       0x03
#define USB_REQ_SET_ADDRESS       0x05
#define USB_REQ_GET_DESCRIPTOR    0x06
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_MSC_RESET         0xFF

#define USB_DESC_DEVICE       1
#define USB_DESC_CONFIG       2
#define USB_DESC_STRING       3
#define USB_DESC_INTERFACE    4
#define USB_DESC_ENDPOINT     5
#define USB_DESC_SS_EP_COMPANION 48

#define USB_CLASS_MASS        8
#define USB_SUBCLASS_SCSI     6
#define USB_PROTO_BBB         0x50

#define CBW_SIG               0x43425355
#define CSW_SIG               0x53425355
#define SCSI_TEST_UNIT_READY  0x00
#define SCSI_REQUEST_SENSE    0x03
#define SCSI_INQUIRY          0x12
#define SCSI_READ_CAPACITY    0x25
#define SCSI_READ10           0x28
#define SCSI_WRITE10          0x2A
#define SCSI_SYNC_CACHE10     0x35

typedef struct __attribute__((packed, aligned(16))) {
    volatile DWORD link;
    volatile DWORD cs;
    volatile DWORD token;
    volatile DWORD buffer;
} uhci_td;

typedef struct __attribute__((packed, aligned(16))) {
    volatile DWORD head;
    volatile DWORD element;
    DWORD pad[2];
} uhci_qh;

typedef struct __attribute__((packed)) {
    BYTE  bmRequestType;
    BYTE  bRequest;
    WORD  wValue;
    WORD  wIndex;
    WORD  wLength;
} usb_setup;

typedef struct __attribute__((packed)) {
    DWORD sig;
    DWORD tag;
    DWORD data_len;
    BYTE  flags;
    BYTE  lun;
    BYTE  cb_len;
    BYTE  cb[16];
} usb_cbw;

typedef struct __attribute__((packed)) {
    DWORD sig;
    DWORD tag;
    DWORD residue;
    BYTE  status;
} usb_csw;

typedef struct {
    int present;
    int deviceid;
    u64 total_blocks;
    DWORD block_size;
} usb_drive_info;

/*
  USB device registry (Phase 2): supports an arbitrary number of devices of
  arbitrary classes on the multi-device xHCI controller. Each bound device
  (usb_device) owns its control/bulk state (the former single-device globals)
  plus a class-specific priv heap block. A usb_driver vtable is selected by
  USB class/interface and performs enumerate+bind (probe) and teardown
  (remove); the registry claims a controller slot per connected port and tries
  each driver until one binds. Bulk bounce buffers (usb_dma_buf/cbw/csw) stay
  shared statics: all USB I/O is serialized by usb_io_lock, one transfer at a
  time, so no per-device bounce duplication is needed.
*/
#define USB_MAX_DEVICES   8
/* usb_device.driver indexes into usb_drivers[]. */
#define USB_DRV_NONE      (-1)
#define USB_DRV_MSC       0
#define USB_DRV_CDC_ACM   1
#define USB_DRV_CDC_ECM   2
#define USB_DRV_RNDIS     3
#define USB_DRV_ASIX      4

struct usb_device;
typedef struct usb_driver {
    const char *name;
    int  (*probe)(struct usb_device *dev);
    void (*remove)(struct usb_device *dev);
} usb_driver;

/* Per-device state for a USB Mass Storage (BBB/BOT) device. Only the UHCI
   control address and low-speed flag stay shared globals (uhci_ctrl/uhci_bulk
   read them; the xHCI path keeps the address in its per-slot context and
   ignores the data toggle, so xHCI multi-device needs neither). Everything
   that differs per device — endpoints, sizes/burst, class interface, BOT
   toggle, CBW tag, drive geometry, media identity — lives here. */
typedef struct {
    usb_drive_info drive;
    int   dev;             /* xHCI usbdevs[] index (0 for UHCI) for bulk/ctrl */
    int   name_index;      /* "usb%d" block-name index */
    BYTE  ep_in, ep_out;
    WORD  ep_in_mps, ep_out_mps;
    BYTE  ep_in_burst, ep_out_burst;
    BYTE  msc_interface;
    BYTE  toggle_in, toggle_out;
    DWORD tag;
    usb_media_identity expected_identity;
    int   media_established;
} usb_msc;

typedef struct usb_device {
    int   present;
    int   slot;          /* xHCI usbdev slot index; -1 if unclaimed */
    DWORD port;
    int   driver;        /* index into usb_drivers[]; -1 if none */
    void *priv;          /* class-specific heap block (usb_msc / ...) */
    char  name[16];      /* "usb0", "usb1", ... */
} usb_device;

static volatile DWORD uhci_framelist[1024] __attribute__((aligned(4096)));
static uhci_qh uhci_qh_ctl  __attribute__((aligned(16)));
static uhci_td uhci_tds[USB_MAX_TD] __attribute__((aligned(16)));
static BYTE usb_dma_buf[MSC_BULK_MAX] __attribute__((aligned(16)));
static BYTE usb_setup_buf[8] __attribute__((aligned(16)));
static BYTE usb_cbw_buf[32] __attribute__((aligned(16)));
static BYTE usb_csw_buf[16] __attribute__((aligned(16)));

static WORD uhci_iobase = 0;
static int  usb_devaddr = 0;   /* UHCI only (uhci_ctrl/uhci_bulk) */
static int  usb_lowspeed = 0;  /* UHCI only (uhci td_cs) */
static int usb_host = 0;
/* Primary (boot) MSC drive state. Secondary drives get their own heap
   usb_msc; usb_msc0 stays static so the recovery/hotplug/init paths (which
   manage the boot drive) have a stable object to point at. */
static usb_msc usb_msc0;
static spinlock_t usb_io_lock;
static int usb_xhci_recovering;
static int usb_xhci_recovery_count;
static int usb_xhci_stall_recovery_count;
static int usb_disconnect_dirty_pages;
static int usb_hotplug_monitor_started;
static volatile int usb_hotplug_transition;
#ifdef KTEST
/* Deterministic fault-injection latches driven by the in-kernel self-tests.
   Absent from production kernels (KTEST=0). */
static int usb_fault_invalid_cbw;
static int usb_fault_drop_stall_retry;
static BYTE usb_recovery_before[512];
static BYTE usb_recovery_after[512];
#endif

/* Device registry (Phase 2). usb_devices[] holds every bound device; the
   xHCI controller owns one slot per entry. usb_cdc_dev tracks the slot the
   CDC-ACM console claimed (it was the fixed USB_CDC_DEV before multi-device). */
static usb_device usb_devices[USB_MAX_DEVICES];

/* Secondary (non-boot) MSC drives. The boot drive is the static usb_msc0 in
   usbdevs[0]; each additional drive gets a heap usb_msc in a later usbdevs
   index (2..7, since index 1 is reserved for the CDC-ACM console). */
#define USB_MAX_SECONDARY   6
static usb_msc *usb_secondary[USB_MAX_SECONDARY];
static int usb_secondary_count;
static int usb_secondary_dev_base;   /* usbdevs index of the next secondary */

static void usb_secondary_reset(void)
{
    int i;
    for (i = 0; i < usb_secondary_count; i++) {
        if (usb_secondary[i]) {
            free(usb_secondary[i]);
            usb_secondary[i] = 0;
        }
    }
    usb_secondary_count = 0;
    usb_secondary_dev_base = 2;
    memset(usb_devices, 0, sizeof(usb_devices));
}

static void usb_registry_add(int idx, DWORD port, int dev, int driver,
                             void *priv, const char *name)
{
    if (idx < 0 || idx >= USB_MAX_DEVICES)
        return;
    usb_devices[idx].present = 1;
    usb_devices[idx].slot = dev;
    usb_devices[idx].port = port;
    usb_devices[idx].driver = driver;
    usb_devices[idx].priv = priv;
    usb_devices[idx].name[0] = 0;
    if (name) {
        int n = 0;
        while (name[n] && n < 15)
            usb_devices[idx].name[n++] = name[n];
    }
}

static void usb_registry_clear(void)
{
    int i;
    for (i = 0; i < USB_MAX_DEVICES; i++)
        usb_devices[i].present = 0;
}

/* Same-CPU: a spinning spin_lock never schedules the hotplug thread that
   holds usb_io_lock during CDC OUT — openfilex then hangs forever on N150
   when ticks/preemption are weak. Yield while waiting. */
static void usb_io_lock_acquire(void)
{
    while (__sync_lock_test_and_set(&usb_io_lock.locked, 1)) {
        while (usb_io_lock.locked) {
            taskswitch();
            __asm__ __volatile__("pause");
        }
    }
}

static void usb_io_lock_release(void)
{
    spin_unlock(&usb_io_lock);
}
#define USB_CDC_TXQ 2048
#define USB_CDC_DEV 1
/* Slot the CDC-ACM console claimed (was the fixed USB_CDC_DEV). The I/O path
   below uses usb_cdc_dev, not the macro, so the console works on any slot. */
static DWORD usb_cdc_dev = USB_CDC_DEV;
static volatile DWORD usb_cdc_tx_head;
static volatile DWORD usb_cdc_tx_tail;
static unsigned char usb_cdc_txq[USB_CDC_TXQ];
static BYTE usb_cdc_dma_buf[512] __attribute__((aligned(16)));
static BYTE usb_cdc_rx_dma[512] __attribute__((aligned(16)));
static volatile int usb_cdc_ready;
static volatile int usb_cdc_pumping;
static volatile int usb_cdc_bulk_io_quiesced;
static BYTE usb_cdc_ep_out;
static BYTE usb_cdc_ep_in;
static WORD usb_cdc_mps_out;
static WORD usb_cdc_mps_in;
static BYTE usb_cdc_comm_if;
static int usb_cdc_in_skip;
static unsigned char usb_cdc_line[USB_DEBUG_LINE_MAX];
static unsigned int usb_cdc_linelen;
static unsigned int usb_cdc_kexec_left;
static unsigned int usb_cdc_kexec_got;
static unsigned int usb_cdc_kexec_size;
static unsigned char *usb_cdc_kexec_img;
static unsigned int usb_cdc_discard;
static unsigned int usb_cdc_kexec_seq;
static volatile int usb_cdc_kexec_finish_pending;
static unsigned int usb_cdc_kexec_stall;
static int usb_cdc_rx_seen;
static int usb_cdc_tx_fails;

static int usb_enumerate_msc(usb_msc *m, int dev);
static int usb_xhci_recover(void);
static int usb_publish_storage_devices(usb_msc *m, int index);
static void usb_xhci_hotplug_monitor(void);
static void usb_cdc_after_msc(void);

static int usb_for_each_part(int (*cb)(int deviceid))
{
    int i, sum = 0;
    for (i = 0; i < partdev_count(); i++) {
        const partdev_entry *e = partdev_get(i);
        if (e->parent_deviceid == usb_msc0.drive.deviceid && e->mydeviceid >= 0)
            sum += cb(e->mydeviceid);
    }
    return sum;
}

static int usb_invalidate_cb(int deviceid)
{
    return blkcache_invalidate_device(deviceid);
}

static int usb_quiesce_cb(int deviceid)
{
    devmgr_quiesce_device(deviceid);
    return 0;
}

static void usb_invalidate_storage_cache(void)
{
    int dirty = 0;
    if (usb_msc0.drive.deviceid >= 0) {
        dirty += blkcache_invalidate_device(usb_msc0.drive.deviceid);
        dirty += usb_for_each_part(usb_invalidate_cb);
    }
    usb_disconnect_dirty_pages = dirty;
    if (dirty)
        printf("usb: disconnect discarded %d dirty cache page(s)\n", dirty);
}

static void usb_quiesce_storage_devices(void)
{
    if (usb_msc0.drive.deviceid >= 0) {
        devmgr_quiesce_device(usb_msc0.drive.deviceid);
        usb_for_each_part(usb_quiesce_cb);
    }
}

static void usb_xhci_disconnect_offline(void)
{
    if (!usb_msc0.drive.present)
        return;
    usb_msc0.drive.present = 0;
    usb_invalidate_storage_cache();
    usb_quiesce_storage_devices();
    partdev_remove(usb_msc0.drive.deviceid);
    usb_msc0.drive.deviceid = -1;
    printf("xhci: device disconnected; storage offline\n");
}

static DWORD pci_cfg_addr(BYTE bus, BYTE slot, BYTE func, BYTE off)
{
    return 0x80000000u | ((DWORD)bus << 16) | ((DWORD)slot << 11) |
           ((DWORD)func << 8) | (off & 0xFC);
}

static DWORD pci_read32(BYTE bus, BYTE slot, BYTE func, BYTE off)
{
    outportl(0xCF8, pci_cfg_addr(bus, slot, func, off));
    return inportl(0xCFC);
}

static void pci_write32(BYTE bus, BYTE slot, BYTE func, BYTE off, DWORD val)
{
    outportl(0xCF8, pci_cfg_addr(bus, slot, func, off));
    outportl(0xCFC, val);
}

static WORD pci_read16(BYTE bus, BYTE slot, BYTE func, BYTE off)
{
    DWORD v = pci_read32(bus, slot, func, off & 0xFC);
    return (WORD)((v >> ((off & 2) * 8)) & 0xFFFF);
}

static void pci_write16(BYTE bus, BYTE slot, BYTE func, BYTE off, WORD val)
{
    DWORD v = pci_read32(bus, slot, func, off & 0xFC);
    DWORD shift = (off & 2) * 8;
    v &= ~(0xFFFFu << shift);
    v |= ((DWORD)val) << shift;
    pci_write32(bus, slot, func, off & 0xFC, v);
}

static void usb_io_delay(void)
{
    inportb(0x80);
}

static void usb_wait_ms(int ms)
{
    /* delay() is in milliseconds (timer-based). Fall back to a port delay
       if the scheduler/timer is not yet producing ticks. */
    DWORD start = ticks;
    if (start != 0 || ms > 0) {
        DWORD t1 = ticks + (DWORD)ms * 2 + 2;
        DWORD spins = 0;
        while (ticks < t1 && spins < 20000000u) {
            usb_io_delay();
            spins++;
        }
        if (ticks != start)
            return;
    }
    /* busy-wait fallback (~1ms * ms on typical QEMU) */
    while (ms-- > 0) {
        DWORD i;
        for (i = 0; i < 20000; i++)
            usb_io_delay();
    }
}

/* Bounded millisecond spin that never waits on `ticks` (USB probe can
   run before the scheduler). usb_wait_ms with ticks==0 spins 20e6. */
static void usb_spin_ms(int ms)
{
    while (ms-- > 0) {
        DWORD i;
        for (i = 0; i < 20000; i++)
            usb_io_delay();
    }
}

static DWORD usb_phys(void *p)
{
    unsigned long long dma_addr;
    if (!dma_identity_map(p, 1, 1, 0xFFFFFFFFULL, &dma_addr))
        return 0;
    return (DWORD)dma_addr;
}

#define USB_HOST_UHCI 1
#define USB_HOST_XHCI 2
#include "xhci.c"
static xhci_hcd *usb_xhci_hcd = &xhci_primary_hcd;

static void uhci_stop(void)
{
    outportw(uhci_iobase + UHCI_USBCMD, 0);
    usb_wait_ms(1);
}

static int uhci_reset_controller(void)
{
    int i;
    outportw(uhci_iobase + UHCI_USBCMD, UHCI_CMD_HCRESET);
    for (i = 0; i < 100; i++) {
        usb_wait_ms(1);
        if ((inportw(uhci_iobase + UHCI_USBCMD) & UHCI_CMD_HCRESET) == 0)
            return 1;
    }
    return 0;
}

static void uhci_build_schedule(void)
{
    int i;
    uhci_qh_ctl.head = TD_LINK_TERM;
    uhci_qh_ctl.element = TD_LINK_TERM;
    for (i = 0; i < 1024; i++)
        uhci_framelist[i] = usb_phys(&uhci_qh_ctl) | TD_LINK_QH;
}

static int uhci_run(void)
{
    outportw(uhci_iobase + UHCI_USBINTR, 0);
    outportw(uhci_iobase + UHCI_FRNUM, 0);
    outportl(uhci_iobase + UHCI_FLBASEADD, usb_phys(uhci_framelist));
    outportb(uhci_iobase + UHCI_SOFMOD, 0x40);
    outportw(uhci_iobase + UHCI_USBSTS, 0xFFFF);
    outportw(uhci_iobase + UHCI_USBCMD, UHCI_CMD_RS | UHCI_CMD_CF | UHCI_CMD_MAXP);
    usb_wait_ms(1);
    return (inportw(uhci_iobase + UHCI_USBCMD) & UHCI_CMD_RS) ? 1 : 0;
}

static DWORD td_token(BYTE pid, BYTE addr, BYTE endp, BYTE toggle, WORD len)
{
    DWORD maxlen = (len == 0) ? 0x7FF : ((DWORD)len - 1);
    return ((maxlen & 0x7FF) << 21) | ((DWORD)toggle << 19) |
           ((DWORD)endp << 15) | ((DWORD)addr << 8) | pid;
}

static DWORD td_cs(int ioc)
{
    DWORD cs = TD_CS_ACTIVE | TD_CS_ERRCNT;
    if (usb_lowspeed)
        cs |= TD_CS_LS;
    if (ioc)
        cs |= TD_CS_IOC;
    return cs;
}

static int uhci_wait_tds(uhci_td *tds, int count)
{
    DWORD spins;
    int i;
    for (spins = 0; spins < USB_TIMEOUT; spins++) {
        int active = 0;
        for (i = 0; i < count; i++) {
            if (tds[i].cs & TD_CS_ACTIVE)
                active = 1;
        }
        if (!active)
            break;
        usb_io_delay();
    }
    uhci_qh_ctl.element = TD_LINK_TERM;
    for (i = 0; i < count; i++) {
        DWORD cs = tds[i].cs;
        if (cs & (TD_CS_STALLED | TD_CS_DATABUF | TD_CS_BABBLE |
                  TD_CS_CRC | TD_CS_BITSTUFF | TD_CS_ACTIVE))
            return 0;
    }
    return 1;
}

static int uhci_ctrl(usb_setup *setup, void *data, int len)
{
    int ntd = 0;
    int dir_in = (setup->bmRequestType & 0x80) ? 1 : 0;
    BYTE *payload = (BYTE*)data;
    int remaining = len;
    BYTE toggle = 1;
    int i;

    memcpy(usb_setup_buf, setup, 8);

    uhci_tds[0].link = usb_phys(&uhci_tds[1]) | TD_LINK_VF;
    uhci_tds[0].cs = td_cs(0);
    uhci_tds[0].token = td_token(TD_PID_SETUP, (BYTE)usb_devaddr, 0, 0, 8);
    uhci_tds[0].buffer = usb_phys(usb_setup_buf);
    ntd = 1;

    while (remaining > 0 && ntd < USB_MAX_TD - 1) {
        int chunk = remaining > 8 ? 8 : remaining;
        uhci_td *td = &uhci_tds[ntd];
        td->link = usb_phys(&uhci_tds[ntd + 1]) | TD_LINK_VF;
        td->cs = td_cs(0);
        td->token = td_token(dir_in ? TD_PID_IN : TD_PID_OUT,
                             (BYTE)usb_devaddr, 0, toggle, (WORD)chunk);
        td->buffer = usb_phys(payload);
        toggle ^= 1;
        payload += chunk;
        remaining -= chunk;
        ntd++;
    }

    uhci_tds[ntd - 1].link = usb_phys(&uhci_tds[ntd]) | TD_LINK_VF;
    uhci_tds[ntd].link = TD_LINK_TERM;
    uhci_tds[ntd].cs = td_cs(1);
    uhci_tds[ntd].token = td_token(dir_in ? TD_PID_OUT : TD_PID_IN,
                                   (BYTE)usb_devaddr, 0, 1, 0);
    uhci_tds[ntd].buffer = 0;
    ntd++;

    asm volatile ("wbinvd");
    uhci_qh_ctl.element = usb_phys(&uhci_tds[0]);
    if (!uhci_wait_tds(uhci_tds, ntd))
        return 0;
    asm volatile ("wbinvd");
    (void)i;
    return 1;
}

static int uhci_bulk(BYTE endp, int in, BYTE *data, int len, BYTE *toggle)
{
    int ntd = 0;
    int remaining = len;
    BYTE *ptr = data;
    BYTE pid = in ? TD_PID_IN : TD_PID_OUT;

    if (len < 0 || len > USB_BULK_MAX)
        return 0;

    if (len == 0) {
        uhci_tds[0].link = TD_LINK_TERM;
        uhci_tds[0].cs = td_cs(1);
        uhci_tds[0].token = td_token(pid, (BYTE)usb_devaddr, endp, *toggle, 0);
        uhci_tds[0].buffer = 0;
        ntd = 1;
        *toggle ^= 1;
    } else {
        while (remaining > 0 && ntd < USB_MAX_TD) {
            int chunk = remaining > 64 ? 64 : remaining;
            uhci_td *td = &uhci_tds[ntd];
            td->link = (remaining - chunk > 0)
                       ? (usb_phys(&uhci_tds[ntd + 1]) | TD_LINK_VF)
                       : TD_LINK_TERM;
            td->cs = td_cs(remaining - chunk <= 0);
            td->token = td_token(pid, (BYTE)usb_devaddr, endp, *toggle, (WORD)chunk);
            td->buffer = usb_phys(ptr);
            *toggle ^= 1;
            ptr += chunk;
            remaining -= chunk;
            ntd++;
        }
    }

    asm volatile ("wbinvd");
    uhci_qh_ctl.element = usb_phys(&uhci_tds[0]);
    if (!uhci_wait_tds(uhci_tds, ntd))
        return 0;
    asm volatile ("wbinvd");
    return 1;
}

static int usb_ctrl_dev(DWORD dev, usb_setup *setup, void *data, int len)
{
    if (usb_host == USB_HOST_XHCI)
        return xhci_control(usb_xhci_hcd, dev, setup, data, len);
    if (dev != 0)
        return 0;
    return uhci_ctrl(setup, data, len);
}

static int usb_ctrl(usb_setup *setup, void *data, int len)
{
    return usb_ctrl_dev(0, setup, data, len);
}

static int usb_bulk(BYTE endp, int in, BYTE *data, int len, BYTE *toggle)
{
    if (usb_host == USB_HOST_XHCI)
        return xhci_bulk(usb_xhci_hcd, 0, endp, in, data, len);
    return uhci_bulk(endp, in, data, len, toggle);
}

/* Per-slot bulk: xHCI routes to the device's controller slot (multi-device);
   UHCI is single-device so the slot is ignored. */
static int usb_bulk_dev(DWORD dev, BYTE endp, int in, BYTE *data, int len,
                        BYTE *toggle)
{
    if (usb_host == USB_HOST_XHCI)
        return xhci_bulk(usb_xhci_hcd, dev, endp, in, data, len);
    if (dev != 0)
        return 0;
    return uhci_bulk(endp, in, data, len, toggle);
}

static int usb_get_desc_dev(DWORD dev, BYTE type, BYTE index, void *buf,
                            WORD len)
{
    usb_setup s;
    memset(&s, 0, sizeof(s));
    s.bmRequestType = 0x80;
    s.bRequest = USB_REQ_GET_DESCRIPTOR;
    s.wValue = ((WORD)type << 8) | index;
    s.wIndex = 0;
    s.wLength = len;
    memset(usb_dma_buf, 0, sizeof(usb_dma_buf));
    if (!usb_ctrl_dev(dev, &s, usb_dma_buf, len))
        return 0;
    memcpy(buf, usb_dma_buf, len);
    return 1;
}

static int usb_get_desc(BYTE type, BYTE index, void *buf, WORD len)
{
    return usb_get_desc_dev(0, type, index, buf, len);
}

static int usb_set_address(BYTE addr)
{
    usb_setup s;
    if (usb_host == USB_HOST_XHCI) {
        /* Address Device already ran with BSR=0 in xhci_claim_port(). */
        usb_devaddr = addr;
        return 1;
    }
    memset(&s, 0, sizeof(s));
    s.bmRequestType = 0x00;
    s.bRequest = USB_REQ_SET_ADDRESS;
    s.wValue = addr;
    if (!usb_ctrl(&s, 0, 0))
        return 0;
    usb_devaddr = addr;
    usb_wait_ms(2);
    return 1;
}

static int usb_set_config_dev(DWORD dev, BYTE cfg)
{
    usb_setup s;
    memset(&s, 0, sizeof(s));
    s.bmRequestType = 0x00;
    s.bRequest = USB_REQ_SET_CONFIGURATION;
    s.wValue = cfg;
    return usb_ctrl_dev(dev, &s, 0, 0);
}

static int usb_set_config(BYTE cfg)
{
    return usb_set_config_dev(0, cfg);
}

static int usb_msc_bot_once(usb_msc *m, BYTE *cdb, int cdb_len, int in,
                            BYTE *data, DWORD len)
{
    usb_cbw cbw;
    usb_csw csw;
    memset(&cbw, 0, sizeof(cbw));
    cbw.sig = CBW_SIG;
    cbw.tag = m->tag++;
    cbw.data_len = len;
    cbw.flags = in ? 0x80 : 0x00;
    cbw.lun = 0;
    cbw.cb_len = (BYTE)cdb_len;
    memcpy(cbw.cb, cdb, cdb_len);
    memcpy(usb_cbw_buf, &cbw, 31);
#ifdef KTEST
    if (usb_fault_invalid_cbw) {
        memset(usb_cbw_buf, 0, 4);
        usb_fault_invalid_cbw = 0;
        printf("xhci: test sending invalid BOT CBW\n");
    }
#endif

    if (!usb_bulk_dev(m->dev, m->ep_out, 0, usb_cbw_buf, 31, &m->toggle_out))
        return 0;

    if (len && data) {
        if (in) {
            memset(usb_dma_buf, 0, len > sizeof(usb_dma_buf) ? sizeof(usb_dma_buf) : len);
            if (!usb_bulk_dev(m->dev, m->ep_in, 1, usb_dma_buf, (int)len,
                              &m->toggle_in))
                return 0;
            memcpy(data, usb_dma_buf, len);
        } else {
            memcpy(usb_dma_buf, data, len);
            if (!usb_bulk_dev(m->dev, m->ep_out, 0, usb_dma_buf, (int)len,
                              &m->toggle_out))
                return 0;
        }
    }

    memset(usb_csw_buf, 0, sizeof(usb_csw_buf));
    if (!usb_bulk_dev(m->dev, m->ep_in, 1, usb_csw_buf, 13, &m->toggle_in))
        return 0;
    memcpy(&csw, usb_csw_buf, 13);
    if (csw.sig != CSW_SIG || csw.status != 0)
        return 0;
    return 1;
}

static int usb_clear_endpoint_halt(usb_msc *m, BYTE endpoint)
{
    usb_setup setup;
    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = 0x02;
    setup.bRequest = USB_REQ_CLEAR_FEATURE;
    setup.wIndex = endpoint;
    return usb_ctrl_dev(m->dev, &setup, 0, 0);
}

static int usb_xhci_recover_stall(usb_msc *m)
{
    usb_setup setup;
    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = 0x21;
    setup.bRequest = USB_REQ_MSC_RESET;
    setup.wIndex = m->msc_interface;
    if (!usb_ctrl_dev(m->dev, &setup, 0, 0) ||
        !usb_clear_endpoint_halt(m, (BYTE)(m->ep_in | 0x80)) ||
        !usb_clear_endpoint_halt(m, m->ep_out) ||
        !xhci_recover_bulk_endpoints(usb_xhci_hcd, m->dev, m->ep_in,
                     m->ep_out)) {
        usb_xhci_hcd->recovery_needed = 1;
        return 0;
    }
    m->toggle_in = 0;
    m->toggle_out = 0;
    usb_xhci_stall_recovery_count++;
    printf("xhci: BOT stall recovery complete count=%d\n",
           usb_xhci_stall_recovery_count);
    return 1;
}

static int usb_msc_bot(usb_msc *m, BYTE *cdb, int cdb_len, int in,
                       BYTE *data, DWORD len)
{
    if (usb_msc_bot_once(m, cdb, cdb_len, in, data, len))
        return 1;
    if (usb_host == USB_HOST_XHCI && usb_xhci_hcd->connection_lost) {
        usb_xhci_disconnect_offline();
        return 0;
    }
    if (usb_host == USB_HOST_XHCI && usb_xhci_hcd->stalled_endpoints &&
        !usb_xhci_recovering && usb_xhci_recover_stall(m)) {
#ifdef KTEST
        if (usb_fault_drop_stall_retry) {
            usb_fault_drop_stall_retry = 0;
            usb_xhci_hcd->fault_drop_next = 1;
        }
#endif
        if (usb_msc_bot_once(m, cdb, cdb_len, in, data, len))
            return 1;
        usb_xhci_hcd->recovery_needed = 1;
    }
    if (usb_host == USB_HOST_XHCI && usb_xhci_hcd->connection_lost) {
        usb_xhci_disconnect_offline();
        return 0;
    }
    if (usb_host != USB_HOST_XHCI || !usb_xhci_hcd->recovery_needed ||
        usb_xhci_recovering)
        return 0;
    if (!usb_xhci_recover())
        return 0;
    return usb_msc_bot_once(m, cdb, cdb_len, in, data, len);
}

static int usb_scsi_ready(usb_msc *m)
{
    BYTE cdb[16];
    int i;
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_TEST_UNIT_READY;
    for (i = 0; i < 8; i++) {
        if (usb_msc_bot(m, cdb, 12, 1, 0, 0))
            return 1;
        usb_wait_ms(20);
    }
    return 0;
}

static int usb_scsi_inquiry(usb_msc *m)
{
    BYTE cdb[16];
    BYTE inq[36];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_INQUIRY;
    cdb[4] = 36;
    return usb_msc_bot(m, cdb, 12, 1, inq, 36);
}

static int usb_scsi_capacity(usb_msc *m, u64 *blocks, DWORD *bsize)
{
    BYTE cdb[16];
    BYTE cap[8];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_READ_CAPACITY;
    if (!usb_msc_bot(m, cdb, 10, 1, cap, 8))
        return 0;
    /* SCSI READ CAPACITY(10) reports a 32-bit LBA, so the value always
       fits; the u64 field simply preserves it without truncation. */
    *blocks = ((u64)cap[0] << 24) | ((u64)cap[1] << 16) |
              ((u64)cap[2] << 8) | cap[3];
    *blocks += 1;
    *bsize = ((DWORD)cap[4] << 24) | ((DWORD)cap[5] << 16) |
             ((DWORD)cap[6] << 8) | cap[7];
    return 1;
}

static int usb_scsi_rw(usb_msc *m, int write, DWORD lba, DWORD nblocks,
                       char *buf)
{
    BYTE cdb[16];
    DWORD done = 0;
    DWORD bsize = m->drive.block_size ? m->drive.block_size : 512;
    /* BOT/UHCI TD budget: transfer up to MSC_BULK_MAX per SCSI command,
       computed from the drive's block size so the bounce buffer is never
       overflowed. 16 KiB (32 x 512B) is the classic USB MSC chunk. */
    DWORD max_per_cmd = MSC_BULK_MAX / bsize;
    if (max_per_cmd < 1)
        max_per_cmd = 1;

    while (done < nblocks) {
        DWORD n = nblocks - done;
        if (n > max_per_cmd) n = max_per_cmd;
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = write ? SCSI_WRITE10 : SCSI_READ10;
        cdb[2] = (BYTE)((lba + done) >> 24);
        cdb[3] = (BYTE)((lba + done) >> 16);
        cdb[4] = (BYTE)((lba + done) >> 8);
        cdb[5] = (BYTE)(lba + done);
        cdb[7] = (BYTE)(n >> 8);
        cdb[8] = (BYTE)(n & 0xFF);
        if (!usb_msc_bot(m, cdb, 10, write ? 0 : 1,
                         (BYTE*)(buf + done * bsize), n * bsize))
            return 0;
        done += n;
    }
    return 1;
}

/* Resolve the MSC drive that owns a VFS device context (the disk itself or a
   partition carved from it). Returns the drive's usb_msc or 0. */
static usb_msc *usb_msc_by_deviceid(int deviceid)
{
    int i;
    for (i = 0; i < USB_MAX_DEVICES; i++) {
        if (usb_devices[i].present && usb_devices[i].driver == USB_DRV_MSC &&
            ((usb_msc *)usb_devices[i].priv)->drive.deviceid == deviceid)
            return (usb_msc *)usb_devices[i].priv;
    }
    return 0;
}

static usb_msc *usb_msc_from_context(void)
{
    int ctx = devmgr_getcontext();
    int i;
    usb_msc *m;
    if (usb_hotplug_transition)
        return 0;
    m = usb_msc_by_deviceid(ctx);
    if (m)
        return m;
    for (i = 0; i < USB_MAX_DEVICES; i++) {
        if (usb_devices[i].present && usb_devices[i].driver == USB_DRV_MSC) {
            m = (usb_msc *)usb_devices[i].priv;
            if (m->drive.deviceid >= 0 &&
                partdev_is_child(ctx, m->drive.deviceid))
                return m;
        }
    }
    return 0;
}

static int usb_read_block_raw(usb_msc *m, u64 block, char *blockbuff,
                              DWORD numblocks)
{
    int result;
    usb_io_lock_acquire();
    result = m->drive.present
        ? usb_scsi_rw(m, 0, (DWORD)block, numblocks, blockbuff) : 0;
    usb_io_lock_release();
    /* Re-arm CDC IN after MSC (skipped while ELF stream quiesced). */
    usb_cdc_after_msc();
    return result;
}

static int usb_write_block_raw(usb_msc *m, u64 block, char *blockbuff,
                               DWORD numblocks)
{
    int result;
    usb_io_lock_acquire();
    result = m->drive.present
        ? usb_scsi_rw(m, 1, (DWORD)block, numblocks, blockbuff) : 0;
    usb_io_lock_release();
    usb_cdc_after_msc();
    return result;
}

static DWORD usb_read_le32(const BYTE *value)
{
    return (DWORD)value[0] | ((DWORD)value[1] << 8) |
           ((DWORD)value[2] << 16) | ((DWORD)value[3] << 24);
}

static int usb_read_identity_blocks(usb_msc *m, DWORD block, DWORD count,
                                    BYTE *buffer)
{
    return usb_scsi_rw(m, 0, block, count, (char *)buffer);
}

static int usb_capture_volume_identity(usb_msc *m, u64 startlba, u64 sectors,
                                       usb_volume_identity *identity)
{
    BYTE data[1024];
    /* SCSI READ(10) addressing is 32-bit; identity records keep their
       32-bit fields for the same reason. */
    usb_volume_identity_init(identity, (unsigned int)startlba,
                             (unsigned int)sectors);
    if (!usb_read_identity_blocks(m, (DWORD)startlba, 1, data))
        return 0;
    if (usb_volume_identity_from_boot(data, identity))
        return 1;
    if (sectors >= 4 && usb_read_identity_blocks(m, (DWORD)startlba + 2, 2, data) &&
        usb_volume_identity_from_ext4(data, identity))
        return 1;
    if (sectors > 64 && usb_read_identity_blocks(m, (DWORD)startlba + 64, 1, data) &&
        usb_volume_identity_from_iso9660(data, identity))
        return 1;
    return 0;
}

static int usb_capture_media_identity(usb_msc *m, usb_media_identity *identity)
{
    BYTE mbr[512];
    int i;
    int partitioned = 0;
    memset(identity, 0, sizeof(*identity));
    if (!usb_read_identity_blocks(m, 0, 1, mbr))
        return 0;
    if (mbr[510] == 0x55 && mbr[511] == 0xAA)
        for (i = 0; i < 4; i++)
            if (mbr[446 + i * 16 + 4] != 0)
                partitioned = 1;
    if (!partitioned) {
        if (!usb_capture_volume_identity(m, 0, m->drive.total_blocks,
                                         &identity->volumes[0]))
            return 0;
        identity->count = 1;
    } else {
        for (i = 0; i < 4; i++) {
            BYTE *entry = mbr + 446 + i * 16;
            DWORD startlba;
            DWORD sectors;
            if (entry[4] == 0)
                continue;
            startlba = usb_read_le32(entry + 8);
            sectors = usb_read_le32(entry + 12);
            if (!startlba || !sectors ||
                !usb_capture_volume_identity(m, startlba, sectors,
                    &identity->volumes[identity->count]))
                return 0;
            identity->count++;
        }
    }
    identity->valid = identity->count > 0;
    return identity->valid;
}

static int usb_read_block(u64 block, char *blockbuff, DWORD numblocks)
{
    usb_msc *m = usb_msc_from_context();
    if (!m)
        return 0;
    return usb_read_block_raw(m, block, blockbuff, numblocks);
}

static int usb_write_block(u64 block, char *blockbuff, DWORD numblocks)
{
    usb_msc *m = usb_msc_from_context();
    if (!m)
        return 0;
    return usb_write_block_raw(m, block, blockbuff, numblocks);
}

static u64 usb_total_blocks(void)
{
    usb_msc *m = usb_msc_from_context();
    if (!m)
        return 0;
    return m->drive.total_blocks;
}

static int usb_get_block_size(void)
{
    usb_msc *m = usb_msc_from_context();
    if (!m)
        return 0;
    return (int)(m->drive.block_size ? m->drive.block_size : 512);
}

static int usb_flush_device(void)
{
    usb_msc *m = usb_msc_from_context();
    BYTE cdb[16];
    int result;
    if (!m)
        return -1;
    usb_io_lock_acquire();
    if (!m->drive.present) {
        usb_io_lock_release();
        return -1;
    }
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_SYNC_CACHE10;
    result = usb_msc_bot(m, cdb, 10, 0, 0, 0);
    usb_io_lock_release();
    if (!result) {
        /* A failed flush can lose data: keep this on the live console. */
        klog(KLOG_ERR, "usb: SYNCHRONIZE CACHE failed\n");
        return -1;
    }
    /* Steady-state trace: every fsync/close emits one. dmesg-only by default
       (KLOG_DEBUG < console threshold) so it cannot corrupt a user TUI. */
    klog(KLOG_DEBUG, "usb: cache synchronized\n");
    return 0;
}

/* Raw accessors for the partdev layer. They take the USB I/O queue lock for
   the parent device (matching the pre-partdev partition callbacks) and issue
   the SCSI read/write at the disk LBA already translated by partdev. */
static int usb_part_raw_read(int parent_id, u64 lba, char *buf, DWORD nblocks)
{
    usb_msc *m = usb_msc_by_deviceid(parent_id);
    int result;
    if (!m)
        return 0;
    blk_mq_lock(parent_id);
    result = usb_read_block_raw(m, lba, buf, nblocks);
    blk_mq_unlock(parent_id);
    return result;
}

static int usb_part_raw_write(int parent_id, u64 lba, char *buf, DWORD nblocks)
{
    usb_msc *m = usb_msc_by_deviceid(parent_id);
    int result;
    if (!m)
        return 0;
    blk_mq_lock(parent_id);
    result = usb_write_block_raw(m, lba, buf, nblocks);
    blk_mq_unlock(parent_id);
    return result;
}

static int usb_gpt_read(unsigned long long lba, void *buf,
                        unsigned int sectors, void *arg)
{
    usb_msc *m = (usb_msc *)arg;
    if (!m)
        return 0;
    return usb_read_block_raw(m, lba, (char *)buf, (DWORD)sectors) ? 1 : 0;
}

static int usb_looks_like_fat(unsigned char *s)
{
    WORD bps = (WORD)(s[11] | (s[12] << 8));
    return ((s[0] == 0xEB || s[0] == 0xE9) &&
            (bps == 512 || bps == 1024 || bps == 2048 || bps == 4096));
}

static void usb_register_gpt(usb_msc *m, int deviceid, u64 total_blocks)
{
    gpt_disk gpt;
    char guidbuf[40];
    char diskname[16];
    int i;
    sprintf(diskname,"usb%d", m->name_index);
    /* gpt_parse returns 1 on success (same contract as ide_register_gpt).
       The inverted != 0 check rejected a valid GPT ESP and left usb0
       unpartitioned (test-usb-uefi-gpt / N150 Etcher image). */
    if (!gpt_parse(usb_gpt_read, m, total_blocks, &gpt)) {
        printf("GPT_WARN %s GPT detected but failed validation; no partitions registered\n",
               diskname);
        return;
    }
    if (gpt.used_backup)
        printf("GPT_WARN %s using backup GPT header\n", diskname);
    else {
        if (!gpt.primary_header_ok)
            printf("GPT_WARN %s primary GPT header failed validation\n", diskname);
        if (!gpt.primary_array_ok)
            printf("GPT_WARN %s primary GPT entry array failed validation\n", diskname);
    }
    partdev_set_disk(deviceid, diskname, PARTDEV_TABLE_GPT, gpt.disk_guid,
                     gpt.used_backup, gpt.entry_count);
    partdev_format_guid(gpt.disk_guid, guidbuf, sizeof(guidbuf));
    printf("GPT_DETECT %s entries=%d diskguid=%s%s\n",
           diskname, gpt.entry_count, guidbuf, gpt.used_backup ? " (backup)" : "");
    for (i = 0; i < gpt.entry_count; i++) {
        char name[20];
        const gpt_entry *e = &gpt.entries[i];
        sprintf(name, "%sp%d", diskname, e->index);
        if (partdev_register(deviceid, name, e->type_name,
                              (int)m->drive.block_size,
                              e->first_lba, e->last_lba + 1,
                              usb_part_raw_read, usb_part_raw_write, 0,
                              e->type_name, e->index,
                              (u64)e->attributes, e->name) < 0) {
            printf("GPT_WARN %s partition %d not registered (cap reached)\n",
                   diskname, e->index);
            continue;
        }
        printf("usb: registered %s (LBA %u)\n", name, (unsigned)e->first_lba);
    }
}

static void usb_register_mbr(usb_msc *m, int deviceid)
{
    unsigned char mbr[512];
    partition_mbr *pmbr;
    char diskname[16];
    int i;

    sprintf(diskname,"usb%d", m->name_index);
    memset(mbr, 0, 512);
    if (!usb_read_block_raw(m, 0, (char *)mbr, 1)) {
        printf("PART_SCAN %s MBR read failed\n", diskname);
        return;
    }
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        printf("PART_SCAN %s bad MBR signature; unpartitioned\n", diskname);
        return;
    }
    pmbr = (partition_mbr *)mbr;
    if (usb_looks_like_fat(mbr) && pmbr->tables[0].type == 0 &&
        pmbr->tables[1].type == 0 && pmbr->tables[2].type == 0 &&
        pmbr->tables[3].type == 0) {
        printf("usb: FAT volume on %s (no partition table)\n", diskname);
        return;
    }
    partdev_set_disk(deviceid, diskname, PARTDEV_TABLE_MBR,
                     (const unsigned char *)0, 0, 4);
    for (i = 0; i < 4; i++) {
        char name[20];
        char desc[64];
        BYTE ptype = pmbr->tables[i].type;
        if (ptype == 0)
            continue;
        sprintf(name, "%sp%d", diskname, i);
        sprintf(desc, "%s on USB partition %d",
                ide_identify_partition_type(ptype), i);
      if (partdev_register(deviceid,
                               name,
                               desc,
                               (int)m->drive.block_size,
                               pmbr->tables[i].startlba,
                               pmbr->tables[i].startlba +
                                   pmbr->tables[i].sector_size,
                               usb_part_raw_read,
                               usb_part_raw_write,
                               0,
                               ide_identify_partition_type(ptype),
                               i,
                               0,
                               (const char *)0) < 0) {
            printf("PART_WARN %s MBR entry %d not registered (cap reached)\n",
                   diskname, i);
            continue;
        }
        printf("usb: registered %s (LBA %u)\n", name,
               (unsigned)pmbr->tables[i].startlba);
    }
}

static int usb_register_partitions(usb_msc *m, int deviceid)
{
    u64 total_blocks;
    int kind;
    char diskname[16];

    if (!m->drive.present || m->drive.block_size != 512 || deviceid < 0)
        return 0;
    sprintf(diskname,"usb%d", m->name_index);
    total_blocks = m->drive.total_blocks;
    kind = gpt_detect(usb_gpt_read, m);
    if (kind < 0) {
        printf("PART_SCAN %s MBR read failed\n", diskname);
        return 0;
    }
    if (kind == 1)
        usb_register_gpt(m, deviceid, total_blocks);
    else if (kind == 0)
        usb_register_mbr(m, deviceid);
    else
        printf("PART_SCAN %s no partition table (unpartitioned)\n", diskname);
    return 1;
}

static int usb_publish_storage_devices(usb_msc *m, int index)
{
    devmgr_block_desc blk;
    char name[16];
    if (!m->expected_identity.valid &&
        !usb_capture_media_identity(m, &m->expected_identity)) {
        printf("usb: stable volume identity unavailable\n");
    }
    /* Drop any stale partition metadata keyed on the previous id before
       (re)publishing; the disconnect path normally already cleared it. */
    partdev_remove(m->drive.deviceid);
    memset(&blk, 0, sizeof(blk));
    sprintf(name,"usb%d", index);
    strcpy(blk.hdr.name, name);
    strcpy(blk.hdr.description, usb_host == USB_HOST_XHCI
        ? "USB Mass Storage (xHCI)" : "USB Mass Storage (UHCI)");
    blk.hdr.type = DEVMGR_BLOCK;
    blk.hdr.size = sizeof(blk);
    blk.read_block = usb_read_block;
    blk.write_block = usb_write_block;
    blk.total_blocks = usb_total_blocks;
    blk.flush_device = usb_flush_device;
    blk.get_block_size = usb_get_block_size;
    m->name_index = index;
    m->drive.deviceid = devmgr_register((devmgr_generic*)&blk);
    if (m->drive.deviceid < 0)
        return 0;
    usb_register_partitions(m, m->drive.deviceid);
    m->media_established = 1;
    printf("usb: registered block device usb%d\n", index);
    return 1;
}

static int usb_parse_config(usb_msc *m, BYTE *cfg, WORD total)
{
    int off = 0;
    int found = 0;
    BYTE last_ep = 0;
    m->ep_in = 0;
    m->ep_out = 0;
    m->msc_interface = 0;
    m->ep_in_mps = 64;
    m->ep_out_mps = 64;
    m->ep_in_burst = 0;
    m->ep_out_burst = 0;
    while (off + 2 <= total) {
        BYTE len = cfg[off];
        BYTE type = cfg[off + 1];
        if (len < 2 || off + len > total)
            return 0;
        if (type == USB_DESC_INTERFACE && len >= 9) {
            if (found && m->ep_in && m->ep_out)
                return 1;
            last_ep = 0;
            m->ep_in = 0;
            m->ep_out = 0;
            m->ep_in_mps = 64;
            m->ep_out_mps = 64;
            m->ep_in_burst = 0;
            m->ep_out_burst = 0;
            if (cfg[off + 5] == USB_CLASS_MASS &&
                cfg[off + 6] == USB_SUBCLASS_SCSI &&
                cfg[off + 7] == USB_PROTO_BBB && cfg[off + 3] == 0) {
                found = 1;
                m->msc_interface = cfg[off + 2];
            } else
                found = 0;
        } else if (found && type == USB_DESC_ENDPOINT && len >= 7) {
            BYTE addr = cfg[off + 2];
            BYTE attr = cfg[off + 3];
            if ((attr & 3) == 2) {
                if (addr & 0x80)
                    m->ep_in = addr & 0x0F;
                else
                    m->ep_out = addr & 0x0F;
                if (addr & 0x80)
                    m->ep_in_mps = (WORD)(cfg[off + 4] | (cfg[off + 5] << 8));
                else
                    m->ep_out_mps = (WORD)(cfg[off + 4] | (cfg[off + 5] << 8));
                last_ep = addr;
            }
        } else if (found && type == USB_DESC_SS_EP_COMPANION && len >= 6 &&
                   last_ep) {
            if (cfg[off + 2] > 15)
                return 0;
            if (last_ep & 0x80)
                m->ep_in_burst = cfg[off + 2];
            else
                m->ep_out_burst = cfg[off + 2];
            last_ep = 0;
        } else {
            last_ep = 0;
        }
        off += len;
    }
    return found && m->ep_in && m->ep_out;
}

static int uhci_reset_port(int port)
{
    WORD psc;
    WORD reg = (WORD)(UHCI_PORTSC1 + port * 2);
    int i;

    psc = inportw(uhci_iobase + reg);
    if (psc == 0xFFFF)
        return 0;
    if (!(psc & UHCI_PORT_CCS))
        return 0;

    outportw(uhci_iobase + reg, UHCI_PORT_RESET | UHCI_PORT_CSC);
    usb_wait_ms(50);
    psc = inportw(uhci_iobase + reg);
    outportw(uhci_iobase + reg, psc & (WORD)~UHCI_PORT_RESET);
    usb_wait_ms(10);

    for (i = 0; i < 10; i++) {
        psc = inportw(uhci_iobase + reg);
        if (psc & UHCI_PORT_PE)
            break;
        outportw(uhci_iobase + reg, (psc & 0x0F34) | UHCI_PORT_PE | UHCI_PORT_CSC | UHCI_PORT_PEC);
        usb_wait_ms(10);
    }
    psc = inportw(uhci_iobase + reg);
    if (!(psc & UHCI_PORT_PE) || !(psc & UHCI_PORT_CCS))
        return 0;
    usb_lowspeed = (psc & UHCI_PORT_LS) ? 1 : 0;
    return 1;
}

static int usb_enumerate_msc(usb_msc *m, int dev)
{
    BYTE devdesc[18];
    BYTE cfghdr[9];
    WORD total;
    BYTE cfg[256];
    BYTE cfgval;

    m->dev = dev;
    usb_devaddr = 0;
    m->toggle_in = 0;
    m->toggle_out = 0;
    if (usb_host == USB_HOST_XHCI)
        printf("usb: enumerate port=%u speed=%u slot=%u\n",
               usb_xhci_hcd->usbdevs[dev].port,
               usb_xhci_hcd->usbdevs[dev].speed,
               usb_xhci_hcd->usbdevs[dev].slot);

    if (!usb_get_desc_dev(dev, USB_DESC_DEVICE, 0, devdesc, 8))
        return 0;
    if (usb_host == USB_HOST_XHCI &&
        !xhci_set_ep0_packet_size(usb_xhci_hcd, dev, devdesc[7]))
        return 0;
    if (!usb_set_address(1))
        return 0;
    if (!usb_get_desc_dev(dev, USB_DESC_DEVICE, 0, devdesc, 18))
        return 0;
    printf("usb: vid=%04x pid=%04x class=%u subclass=%u proto=%u\n",
           (unsigned)(devdesc[8] | (devdesc[9] << 8)),
           (unsigned)(devdesc[10] | (devdesc[11] << 8)),
           (unsigned)devdesc[4], (unsigned)devdesc[5],
           (unsigned)devdesc[6]);
    if (devdesc[4] == 9)
        printf("usb: hub (no hub driver yet)\n");
    if (!usb_get_desc_dev(dev, USB_DESC_CONFIG, 0, cfghdr, 9))
        return 0;
    total = cfghdr[2] | (cfghdr[3] << 8);
    if (total < 9 || total > sizeof(cfg))
        total = 9;
    if (!usb_get_desc_dev(dev, USB_DESC_CONFIG, 0, cfg, total))
        return 0;
    if (!usb_parse_config(m, cfg, total)) {
        printf("usb: no BBB mass-storage interface\n");
        return 0;
    }
    cfgval = cfg[5] ? cfg[5] : 1;
    if (!usb_set_config_dev(dev, cfgval))
        return 0;
    if (usb_host == USB_HOST_XHCI &&
        !xhci_configure_endpoints(usb_xhci_hcd, dev,
                      m->ep_in, m->ep_in_mps,
                                  m->ep_in_burst, m->ep_out,
                                  m->ep_out_mps, m->ep_out_burst))
        return 0;
    printf("usb: MSC endpoints in=%d out=%d\n", m->ep_in, m->ep_out);
    usb_scsi_inquiry(m);
    if (!usb_scsi_ready(m))
        printf("usb: TEST UNIT READY failed (continuing)\n");
    if (!usb_scsi_capacity(m, &m->drive.total_blocks, &m->drive.block_size)) {
        printf("usb: READ CAPACITY failed\n");
        return 0;
    }
    if (m->drive.block_size != 512) {
        printf("usb: unsupported block size %u\n",
               (unsigned)m->drive.block_size);
        return 0;
    }
    printf("usb: %u blocks, %u bytes/block\n",
           (unsigned)m->drive.total_blocks,
           (unsigned)m->drive.block_size);
    return 1;
}

static int usb_xhci_bind_msc(void)
{
    DWORD port;
    DWORD ccs;

    ccs = xhci_port_ccs_mask(usb_xhci_hcd);
    if (!ccs)
        ccs = xhci_wait_connected_ports(usb_xhci_hcd);
    printf("xhci: bind ccs=0x%x\n", ccs);
    /* (Re)bind the boot drive only; secondary drives are bound by
       usb_bind_secondary_msc() after the primary is published. */
    usb_secondary_reset();
    usb_xhci_hcd->enumerating = 1;
    usb_xhci_hcd->recovery_needed = 0;
    for (port = 1; port <= usb_xhci_hcd->max_ports && port <= 32; port++) {
        if (!(xhci_port_ccs_mask(usb_xhci_hcd) & xhci_ccs_bit(port)))
            continue;
        printf("xhci: trying port %u\n", port);
        if (!xhci_claim_port_dev(usb_xhci_hcd, port, 0))
            continue;
        if (usb_enumerate_msc(&usb_msc0, 0)) {
            usb_registry_add(0, port, 0, USB_DRV_MSC, &usb_msc0, "usb0");
            usb_xhci_hcd->enumerating = 0;
            return 1;
        }
        printf("xhci: port %u not mass-storage\n", port);
        xhci_release_dev(usb_xhci_hcd, 0);
        usb_xhci_hcd->recovery_needed = 0;
    }
    usb_xhci_hcd->enumerating = 0;
    return 0;
}

/* Bind every additional connected MSC drive as a secondary (usb1, usb2, ...).
   The boot drive is usbdevs[0] and the CDC console is usb_cdc_dev; everything
   else is claimed into the next free secondary index (2..7). */
static void usb_bind_secondary_msc(void)
{
    DWORD port;
    DWORD bound_ports = 0;
    int name_seq = 1;
    if (usb_host != USB_HOST_XHCI || !usb_xhci_hcd)
        return;
    if (usb_xhci_hcd->usbdevs[0].port)
        bound_ports |= xhci_ccs_bit(usb_xhci_hcd->usbdevs[0].port);
    if (usb_xhci_hcd->usbdevs[usb_cdc_dev].port)
        bound_ports |= xhci_ccs_bit(usb_xhci_hcd->usbdevs[usb_cdc_dev].port);
    usb_xhci_hcd->enumerating = 1;
    usb_xhci_hcd->recovery_needed = 0;
    for (port = 1; port <= usb_xhci_hcd->max_ports && port <= 32; port++) {
        usb_msc *m;
        int dev_idx, sec;
        char name[16];
        if (!(xhci_port_ccs_mask(usb_xhci_hcd) & xhci_ccs_bit(port)))
            continue;
        if (bound_ports & xhci_ccs_bit(port))
            continue;
        if (usb_secondary_dev_base - 2 >= USB_MAX_SECONDARY)
            break;
        sec = usb_secondary_dev_base - 2;
        dev_idx = usb_secondary_dev_base++;
        m = (usb_msc *)malloc(sizeof(usb_msc));
        if (!m)
            break;
        memset(m, 0, sizeof(usb_msc));
        m->tag = 1;
        m->ep_in_mps = 64;
        m->ep_out_mps = 64;
        m->drive.deviceid = -1;
        if (!xhci_claim_port_dev(usb_xhci_hcd, port, dev_idx)) {
            free(m);
            usb_secondary_dev_base--;
            continue;
        }
        if (!usb_enumerate_msc(m, dev_idx)) {
            printf("xhci: secondary port %u not mass-storage\n", port);
            xhci_release_dev(usb_xhci_hcd, dev_idx);
            free(m);
            usb_secondary_dev_base--;
            continue;
        }
        usb_secondary[sec] = m;
        usb_secondary_count = sec + 1;
        m->drive.present = 1;
        sprintf(name,"usb%d", name_seq);
        /* Registry index == xHCI dev slot (2..7) so the CDC slot (1) is
           never clobbered by a secondary, regardless of bind order. */
        usb_registry_add(dev_idx, port, dev_idx, USB_DRV_MSC, m, name);
        usb_publish_storage_devices(m, name_seq);
        name_seq++;
        bound_ports |= xhci_ccs_bit(port);
    }
    usb_xhci_hcd->enumerating = 0;
}

static void usb_cdc_reset_state(void)
{
    if (usb_xhci_hcd)
        xhci_cdc_in_drop(usb_xhci_hcd);
    usb_cdc_ready = 0;
    usb_cdc_tx_head = 0;
    usb_cdc_tx_tail = 0;
    usb_cdc_ep_out = 0;
    usb_cdc_ep_in = 0;
    usb_cdc_mps_out = 64;
    usb_cdc_mps_in = 64;
    usb_cdc_comm_if = 0;
    usb_cdc_in_skip = 0;
    usb_cdc_linelen = 0;
    usb_cdc_kexec_left = 0;
    usb_cdc_kexec_got = 0;
    usb_cdc_kexec_size = 0;
    usb_cdc_discard = 0;
    usb_cdc_kexec_seq = 0;
    usb_cdc_kexec_finish_pending = 0;
    usb_cdc_kexec_stall = 0;
    usb_cdc_rx_seen = 0;
    usb_cdc_tx_fails = 0;
    if (usb_cdc_kexec_img) {
        free(usb_cdc_kexec_img);
        usb_cdc_kexec_img = 0;
    }
}

static DWORD usb_cdc_tx_space(void)
{
    DWORD head = usb_cdc_tx_head;
    DWORD tail = usb_cdc_tx_tail;
    DWORD next = (head + 1) % USB_CDC_TXQ;
    if (next == tail)
        return 0;
    if (head >= tail)
        return USB_CDC_TXQ - 1 - (head - tail);
    return tail - head - 1;
}

static int usb_cdc_pump_tx(void);

void usb_cdc_putc(int c)
{
    DWORD next;
    unsigned long flags;
    unsigned char byte;
    if (!usb_cdc_ready)
        return;
    byte = (unsigned char)c;
    __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    if (byte == '\n') {
        next = (usb_cdc_tx_head + 1) % USB_CDC_TXQ;
        if (next != usb_cdc_tx_tail) {
            usb_cdc_txq[usb_cdc_tx_head] = '\r';
            usb_cdc_tx_head = next;
        }
    }
    next = (usb_cdc_tx_head + 1) % USB_CDC_TXQ;
    if (next != usb_cdc_tx_tail) {
        usb_cdc_txq[usb_cdc_tx_head] = byte;
        usb_cdc_tx_head = next;
    }
    if (flags & (1ULL << 9))
        __asm__ __volatile__("sti" : : : "memory");
}

int usb_cdc_write_raw(const void *p, int n)
{
    const unsigned char *s = (const unsigned char *)p;
    int i;
    if (!usb_cdc_ready || !s || n <= 0)
        return 0;
    for (i = 0; i < n; i++) {
        DWORD next;
        unsigned long flags;
        unsigned spins = 0;
        while (usb_cdc_tx_space() == 0 && spins++ < 64)
            usb_cdc_pump_tx();
        __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
        next = (usb_cdc_tx_head + 1) % USB_CDC_TXQ;
        if (next != usb_cdc_tx_tail) {
            usb_cdc_txq[usb_cdc_tx_head] = s[i];
            usb_cdc_tx_head = next;
        }
        if (flags & (1ULL << 9))
            __asm__ __volatile__("sti" : : : "memory");
    }
    usb_cdc_pump_tx();
    return n;
}

int usb_cdc_present(void)
{
    return usb_cdc_ready;
}

static void usb_cdc_reply_ok(unsigned int seq, const void *payload, int n)
{
    char hdr[48];
    char end[32];
    int hn, en;
    if (n < 0)
        n = 0;
    hn = usb_debug_format_ok(hdr, (int)sizeof(hdr), seq, (unsigned int)n);
    if (hn)
        usb_cdc_write_raw(hdr, hn);
    if (n && payload)
        usb_cdc_write_raw(payload, n);
    en = usb_debug_format_end(end, (int)sizeof(end), seq);
    if (en)
        usb_cdc_write_raw(end, en);
}

static void usb_cdc_reply_err(unsigned int seq, int code, const char *msg)
{
    char line[80];
    int n = sprintf(line, "%cICS %u ERR %d %s\n",
                    (char)USB_DEBUG_RS, seq, code, msg ? msg : "err");
    if (n > 0)
        usb_cdc_write_raw(line, n);
}

static void usb_cdc_send_status(unsigned int seq)
{
    char body[512];
    char ver[192];
    unsigned int fw = 0, fh = 0, fb = 0;
    int vn, n;
    vn = usb_debug_format_status_ver(ver, (int)sizeof(ver),
                                     ICSOS_RELEASE, ICSOS_GIT_HASH,
                                     ICSOS_GIT_DIRTY[0] == '1',
                                     ICSOS_BUILD_TS,
                                     __DATE__ " " __TIME__);
    n = sprintf(body,
                "cdc=1\nusb_root=%d\nfb=%ux%u bpp=%u\nklog=%u\nkexeced=%d\n"
                "%scmdline=%s\n",
                usb_storage_available() ? 1 : 0,
                fbconsole_geom(&fw, &fh, &fb) ? fw : 0,
                fh, fb, klog_count(), kernel_kexeced ? 1 : 0,
                vn > 0 ? ver : "",
                kernel_cmdline[0] ? kernel_cmdline : "");
    if (n < 0)
        n = 0;
    usb_cdc_reply_ok(seq, body, n);
}

static void usb_cdc_send_screen(unsigned int seq)
{
    unsigned char cells[80 * 25 * 2];
    char text[80 * 25 + 25 + 1];
    int n;
    if (!ActiveDDL) {
        usb_cdc_reply_err(seq, USB_DEBUG_ERR_IO, "no-ddl");
        return;
    }
    Dex32GetText(ActiveDDL, 0, 0, 79, 24, (char *)cells);
    n = usb_debug_screen_text(cells, 80, 25, text, (int)sizeof(text));
    usb_cdc_reply_ok(seq, text, n);
}

static void usb_cdc_send_fb(unsigned int seq, unsigned int scale)
{
    unsigned int sw = 0, sh = 0, bpp = 0, dw, dh, x, y;
    char hdr[32];
    int hn;
    if (!fbconsole_geom(&sw, &sh, &bpp) || !sw || !sh) {
        usb_cdc_reply_err(seq, USB_DEBUG_ERR_IO, "no-fb");
        return;
    }
    usb_debug_fb_size(sw, sh, scale, &dw, &dh);
    hn = usb_debug_ppm_header(hdr, (int)sizeof(hdr), dw, dh);
    {
        char ok[48];
        int n = usb_debug_format_ok(ok, (int)sizeof(ok), seq,
                                    (unsigned int)hn + dw * dh * 3u);
        if (n)
            usb_cdc_write_raw(ok, n);
    }
    if (hn)
        usb_cdc_write_raw(hdr, hn);
    for (y = 0; y < dh; y++) {
        unsigned char row[16 * 3];
        unsigned int xi = 0;
        for (x = 0; x < dw; x++) {
            unsigned char r = 0, g = 0, b = 0;
            unsigned int sx = x * usb_debug_fb_scale(scale);
            unsigned int sy = y * usb_debug_fb_scale(scale);
            fbconsole_rgb_at(sx, sy, &r, &g, &b);
            row[xi++] = r;
            row[xi++] = g;
            row[xi++] = b;
            if (xi >= sizeof(row)) {
                usb_cdc_write_raw(row, (int)xi);
                xi = 0;
            }
        }
        if (xi)
            usb_cdc_write_raw(row, (int)xi);
    }
    {
        char end[32];
        int n = usb_debug_format_end(end, (int)sizeof(end), seq);
        if (n)
            usb_cdc_write_raw(end, n);
    }
}

static void usb_cdc_handle_req(const usb_debug_req *req)
{
    unsigned int i;
    if (!req)
        return;
    if (req->verb == USB_DEBUG_VERB_PING)
        return;
    if (req->verb == USB_DEBUG_VERB_KEYS) {
        for (i = 0; i < req->nkeys; i++)
            tty_input_fg(req->keys[i]);
        usb_cdc_reply_ok(req->seq, 0, 0);
        return;
    }
    if (req->verb == USB_DEBUG_VERB_CMD) {
        /* Do not console_execute on the hotplug/CDC pump thread: that
           nested VFS/MSC under pump and hung Intel after the first ls.
           Inject as keystrokes so the console thread runs the command. */
        if (req->cmd[0]) {
            const char *p = req->cmd;
            while (*p)
                tty_input_fg((unsigned char)*p++);
            tty_input_fg('\r');
        }
        usb_cdc_reply_ok(req->seq, 0, 0);
        return;
    }
    if (req->verb == USB_DEBUG_VERB_STATUS) {
        usb_cdc_send_status(req->seq);
        return;
    }
    if (req->verb == USB_DEBUG_VERB_SCREEN) {
        usb_cdc_send_screen(req->seq);
        return;
    }
    if (req->verb == USB_DEBUG_VERB_FB) {
        usb_cdc_send_fb(req->seq, req->fb_scale);
        return;
    }
    if (req->verb == USB_DEBUG_VERB_DMESG) {
        klog_dump(-1);
        usb_cdc_reply_ok(req->seq, 0, 0);
        return;
    }
    if (req->verb == USB_DEBUG_VERB_REBOOT) {
        usb_cdc_reply_ok(req->seq, 0, 0);
        machine_reboot();
        return;
    }
    if (req->verb == USB_DEBUG_VERB_KEXEC) {
        int i;
        if (usb_cdc_kexec_img) {
            free(usb_cdc_kexec_img);
            usb_cdc_kexec_img = 0;
        }
        usb_cdc_kexec_size = req->kexec_bytes;
        usb_cdc_kexec_got = 0;
        usb_cdc_kexec_seq = req->seq;
        usb_cdc_kexec_finish_pending = 0;
        usb_cdc_kexec_stall = 0;
        usb_cdc_kexec_left = 0;
        if (!usb_cdc_kexec_size || usb_cdc_kexec_size > 8u * 1024u * 1024u) {
            usb_cdc_reply_err(req->seq, USB_DEBUG_ERR_ARG, "size");
            return;
        }
        usb_cdc_kexec_img = malloc(usb_cdc_kexec_size);
        if (!usb_cdc_kexec_img) {
            usb_cdc_discard = usb_cdc_kexec_size;
            usb_cdc_reply_err(req->seq, USB_DEBUG_ERR_IO, "nomem");
            return;
        }
        /* ACK before body so the Pico does not flood CDC IN during malloc. */
        usb_cdc_reply_ok(req->seq, "ready", 5);
        for (i = 0; i < 32; i++)
            if (!usb_cdc_pump_tx())
                break;
        /* Drop any further console TX until the image is in — TX on the
           shared event ring wedged CDC IN around ~700 KiB on N150. */
        usb_cdc_tx_head = usb_cdc_tx_tail;
        usb_cdc_kexec_left = usb_cdc_kexec_size;
        serial_puts("usb-cdc: kexec ready\n");
        return;
    }
}

static void usb_cdc_kexec_abort(const char *why)
{
    if (usb_cdc_kexec_img) {
        free(usb_cdc_kexec_img);
        usb_cdc_kexec_img = 0;
    }
    usb_cdc_kexec_left = 0;
    usb_cdc_kexec_got = 0;
    usb_cdc_kexec_size = 0;
    usb_cdc_kexec_finish_pending = 0;
    usb_cdc_kexec_stall = 0;
    usb_cdc_reply_err(usb_cdc_kexec_seq, USB_DEBUG_ERR_IO, why ? why : "abort");
}

static void usb_cdc_kexec_finish(void)
{
    int r;
    if (!usb_cdc_kexec_img) {
        usb_cdc_kexec_left = 0;
        usb_cdc_kexec_finish_pending = 0;
        return;
    }
    printf("usb-cdc: kexec image %u bytes, loading\n", usb_cdc_kexec_got);
    r = kexec_load_mem(usb_cdc_kexec_img, usb_cdc_kexec_got);
    if (r != 0) {
        usb_cdc_kexec_abort("elf");
        return;
    }
    /* ACK already drains via usb_cdc_write_raw→pump_tx. Do not spin more
       USB OUT here — that hung N150 after a successful load (never reached
       kexec_reboot; keyboard/CDC dead). */
    usb_cdc_reply_ok(usb_cdc_kexec_seq, "done", 4);
    free(usb_cdc_kexec_img);
    usb_cdc_kexec_img = 0;
    usb_cdc_kexec_left = 0;
    usb_cdc_kexec_finish_pending = 0;
    usb_cdc_kexec_stall = 0;
    serial_puts("usb-cdc: kexec reboot\n");
    kexec_reboot();
}

static void usb_cdc_feed(const unsigned char *p, int n)
{
    int i;
    if (!p || n <= 0)
        return;
    if (!usb_cdc_rx_seen) {
        usb_cdc_rx_seen = 1;
        serial_puts("USB_CDC_RX\n");
    }
    for (i = 0; i < n; i++) {
        unsigned char c = p[i];
        if (usb_cdc_discard) {
            usb_cdc_discard--;
            continue;
        }
        if (usb_cdc_kexec_left && usb_cdc_kexec_img) {
            usb_cdc_kexec_img[usb_cdc_kexec_got++] = c;
            usb_cdc_kexec_left--;
            usb_cdc_kexec_stall = 0;
            /* No printf here — console TX during receive kills CDC IN. */
            if ((usb_cdc_kexec_got & 0x1ffffu) == 0) {
                char m[48];
                int n = sprintf(m, "usb-cdc: kexec %u/%u\n",
                                usb_cdc_kexec_got, usb_cdc_kexec_size);
                if (n > 0)
                    serial_puts(m);
            }
            if (!usb_cdc_kexec_left)
                usb_cdc_kexec_finish_pending = 1;
            continue;
        }
        if (c == '\n' || usb_cdc_linelen >= USB_DEBUG_LINE_MAX - 1) {
            usb_debug_req req;
            int parsed;
            unsigned int j;
            usb_cdc_line[usb_cdc_linelen] = 0;
            parsed = usb_cdc_linelen ?
                usb_debug_parse_line((char *)usb_cdc_line, &req) : USB_DEBUG_ERR_PARSE;
            if (parsed == USB_DEBUG_OK)
                usb_cdc_handle_req(&req);
            else if (usb_cdc_linelen && usb_cdc_line[0] != USB_DEBUG_RS) {
                for (j = 0; j < usb_cdc_linelen; j++)
                    tty_input_fg(usb_cdc_line[j]);
                if (c == '\n')
                    tty_input_fg('\n');
            }
            usb_cdc_linelen = 0;
            if (c != '\n' && c != '\r') {
                usb_cdc_line[usb_cdc_linelen++] = c;
            }
            continue;
        }
        if (c == '\r')
            continue;
        usb_cdc_line[usb_cdc_linelen++] = c;
    }
}

static int usb_cdc_pump_tx(void);
static void usb_cdc_feed_in(unsigned char *p, int n);

/* After MSC: take a stashed CDC IN if complete and re-post if idle.
   Arm-only (timeout 0) — never the full 40k-spin empty poll. Skipped while
   ELF stream quiesced so hello.exe loads stay fast. Called after each
   block unlock and from usb_cdc_bulk_io_end. */
static void usb_cdc_after_msc(void)
{
    int got = 0;
    unsigned int cap;
    unsigned char rx[512];

    if (!usb_cdc_ready || !usb_xhci_hcd || !usb_cdc_ep_in)
        return;
    /* Boot FAT mount hammers MSC before hotplug pumps CDC; arming there
       raced with BOT and left IN dead (STATUS 504 at idle prompt). */
    if (!usb_hotplug_monitor_started)
        return;
    if (usb_cdc_pumping || usb_io_lock.locked || usb_cdc_bulk_io_quiesced)
        return;
    usb_cdc_pumping = 1;
    usb_io_lock_acquire();
    /* Always post the full DMA buffer so len never mismatches a live TRB. */
    cap = sizeof(usb_cdc_rx_dma);
   if (xhci_bulk_in_try(usb_xhci_hcd, usb_cdc_dev, usb_cdc_ep_in,
                          usb_cdc_rx_dma, (int)cap, &got, 0) && got > 0) {
        if (got > (int)sizeof(rx))
            got = (int)sizeof(rx);
        memcpy(rx, usb_cdc_rx_dma, (unsigned)got);
    } else {
        got = 0;
    }
    usb_io_lock_release();
    usb_cdc_pumping = 0;
    if (got)
        usb_cdc_feed_in(rx, got);
}

void usb_cdc_bulk_io_begin(void)
{
    int waits;

    /* Signal first so in-flight CDC next_event aborts and releases the lock. */
    usb_cdc_bulk_io_quiesced = 1;
    usb_cdc_tx_head = usb_cdc_tx_tail;
    for (waits = 0; waits < 100000; waits++) {
        if (!usb_io_lock.locked && !usb_cdc_pumping)
            break;
        taskswitch();
    }
    if (usb_io_lock.locked || usb_cdc_pumping) {
        usb_cdc_pumping = 0;
        usb_io_lock.locked = 0;
    }
    if (!usb_cdc_ready || !usb_xhci_hcd)
        return;
    usb_io_lock_acquire();
    /* Take completed IN only. Do not abandon a live TRB (ring desync
       killed Pico RPC after MSC). Pump stays off via quiesced. */
    if (xhci_cdc_in_pending && xhci_cdc_in_done)
        (void)xhci_cdc_in_take(0, 0);
    usb_io_lock_release();
}

void usb_cdc_bulk_io_end(void)
{
    if (!usb_cdc_ready) {
        usb_cdc_bulk_io_quiesced = 0;
        return;
    }
    usb_cdc_bulk_io_quiesced = 0;
    usb_cdc_after_msc();
    (void)usb_cdc_pump_tx();
}

static int usb_cdc_pump_tx(void)
{
    DWORD head, tail, tail_start, n, i, cap;
    unsigned long flags;
    if (!usb_cdc_ready || usb_cdc_pumping || usb_host != USB_HOST_XHCI)
        return 0;
    if (usb_cdc_bulk_io_quiesced || usb_io_lock.locked)
        return 0;
    usb_cdc_pumping = 1;
    usb_io_lock_acquire();
    __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    tail = usb_cdc_tx_tail;
    tail_start = tail;
    head = usb_cdc_tx_head;
    cap = usb_cdc_mps_out;
    if (!cap || cap > sizeof(usb_cdc_dma_buf))
        cap = sizeof(usb_cdc_dma_buf);
    n = 0;
    while (tail != head && n < cap) {
        usb_cdc_dma_buf[n++] = usb_cdc_txq[tail];
        tail = (tail + 1) % USB_CDC_TXQ;
    }
    usb_cdc_tx_tail = tail;
    if (flags & (1ULL << 9))
        __asm__ __volatile__("sti" : : : "memory");
    i = 0;
    if (n) {
       if (!xhci_bulk(usb_xhci_hcd, usb_cdc_dev, usb_cdc_ep_out, 0,
                        usb_cdc_dma_buf, (int)n)) {
            /* Roll the tail back so the bytes stay in the TX ring and are
               retransmitted on the next pump. A short CDC-OUT timeout
               (Pico FIFO briefly full while its firmware is busy) must not
               drop console or RPC bytes; the old code advanced the tail
               before xhci_bulk and lost the chunk on every timeout. */
            usb_cdc_tx_tail = tail_start;
            serial_puts("USB_CDC_TX_FAIL\n");
            usb_cdc_tx_fails++;
            if (usb_cdc_tx_fails > 8)
                usb_cdc_ready = 0;
            n = 0;
        } else {
            usb_cdc_tx_fails = 0;
            i = n;
        }
    }
    usb_io_lock_release();
    usb_cdc_pumping = 0;
    return (int)i;
}

static void usb_cdc_feed_in(unsigned char *p, int n)
{
    if (usb_cdc_in_skip) {
        if (n <= usb_cdc_in_skip)
            return;
        p += usb_cdc_in_skip;
        n -= usb_cdc_in_skip;
    }
    usb_cdc_feed(p, n);
}

/* 1 while an ELF stream load / MSC bulk I/O is in flight (console active).
   The hotplug monitor uses this to defer controller rebinds that would
   otherwise reset the xHCI controller mid-load and wedge the console. */
int usb_cdc_bulk_io_active(void)
{
    return usb_cdc_bulk_io_quiesced != 0;
}

int usb_cdc_pump(void)
{
    int sent = 0;
    int got = 0;
    unsigned int cap;
    unsigned char rx[512];
    int packets;

    if (!usb_cdc_ready || usb_cdc_pumping || usb_host != USB_HOST_XHCI)
        return 0;
    /* ELF stream load: no CDC IN or OUT — OUT during MSC hung hello.exe. */
    if (usb_cdc_bulk_io_quiesced)
        return 0;
    if (usb_io_lock.locked || !usb_cdc_ep_in)
        return usb_cdc_pump_tx();

    /* Poll a posted IN first so a Pico write can complete before TX
       cancels that TRB for the shared event ring. */
    usb_cdc_pumping = 1;
    usb_io_lock_acquire();
    cap = sizeof(usb_cdc_rx_dma);
    if (xhci_bulk_in_try(usb_xhci_hcd, usb_cdc_dev, usb_cdc_ep_in,
                         usb_cdc_rx_dma, (int)cap, &got,
                         XHCI_CDC_IN_SPINS) && got > 0) {
        if (got > (int)sizeof(rx))
            got = (int)sizeof(rx);
        memcpy(rx, usb_cdc_rx_dma, (unsigned)got);
    } else {
        got = 0;
    }
    usb_io_lock_release();
    usb_cdc_pumping = 0;
    if (got) {
        /* Re-arm bulk IN before handling the RPC. STATUS/SCREEN replies
           TX for a long time; without a posted IN the Pico's next write
           is dropped and gadget RPC stays 504 after the first MSC+STATUS. */
        usb_cdc_pumping = 1;
        usb_io_lock_acquire();
        cap = sizeof(usb_cdc_rx_dma);
        (void)xhci_bulk_in_try(usb_xhci_hcd, usb_cdc_dev, usb_cdc_ep_in,
                               usb_cdc_rx_dma, (int)cap, 0, 0);
        usb_io_lock_release();
        usb_cdc_pumping = 0;
        usb_cdc_feed_in(rx, got);
    }

    /* While absorbing a kexec image, do not TX — shared event ring. */
    if (!usb_cdc_kexec_left && !usb_cdc_kexec_finish_pending) {
        for (packets = 0; packets < 32; packets++) {
            int n = usb_cdc_pump_tx();
            sent += n;
            if (!n)
                break;
        }
    }

    got = 0;
    if (usb_cdc_ready && !usb_cdc_pumping && !usb_io_lock.locked &&
        usb_cdc_ep_in) {
        usb_cdc_pumping = 1;
        usb_io_lock_acquire();
        cap = sizeof(usb_cdc_rx_dma);
        if (xhci_bulk_in_try(usb_xhci_hcd, usb_cdc_dev, usb_cdc_ep_in,
                             usb_cdc_rx_dma, (int)cap, &got,
                             XHCI_CDC_IN_SPINS) && got > 0) {
            if (got > (int)sizeof(rx))
                got = (int)sizeof(rx);
            memcpy(rx, usb_cdc_rx_dma, (unsigned)got);
        } else {
            got = 0;
        }
        usb_io_lock_release();
        usb_cdc_pumping = 0;
        if (got)
            usb_cdc_feed_in(rx, got);
    }
    /* Receive kexec with short bursts + yields. The old 400k×IN_SPINS
       drain froze the BSP (keyboard dead) when the Pico finished TX
       before every byte landed. */
    if (usb_cdc_kexec_left && usb_cdc_ready && !usb_io_lock.locked) {
        int bursts;
        for (bursts = 0; bursts < 8 && usb_cdc_kexec_left; bursts++) {
            got = 0;
            usb_cdc_pumping = 1;
            usb_io_lock_acquire();
            cap = sizeof(usb_cdc_rx_dma);
            if (xhci_bulk_in_try(usb_xhci_hcd, usb_cdc_dev, usb_cdc_ep_in,
                                 usb_cdc_rx_dma, (int)cap, &got,
                                 XHCI_CDC_IN_SPINS) && got > 0) {
                if (got > (int)sizeof(rx))
                    got = (int)sizeof(rx);
                memcpy(rx, usb_cdc_rx_dma, (unsigned)got);
            } else {
                got = 0;
            }
            usb_io_lock_release();
            usb_cdc_pumping = 0;
            if (got)
                usb_cdc_feed_in(rx, got);
            else
                break;
            taskswitch();
        }
        if (usb_cdc_kexec_left && ++usb_cdc_kexec_stall > 3000) {
            printf("usb-cdc: kexec stall got=%u left=%u\n",
                   usb_cdc_kexec_got, usb_cdc_kexec_left);
            usb_cdc_kexec_abort("stall");
        }
    }
    if (usb_cdc_kexec_finish_pending && !usb_cdc_kexec_left)
        usb_cdc_kexec_finish();
    return sent;
}

static void usb_cdc_pump_flush(void)
{
    int i;
    for (i = 0; i < 64; i++)
        if (!usb_cdc_pump_tx())
            break;
}

static int usb_cdc_set_line(void)
{
    usb_setup s;
    BYTE coding[7];
    DWORD baud = 115200;
    memset(&s, 0, sizeof(s));
    memset(coding, 0, sizeof(coding));
    coding[0] = (BYTE)baud;
    coding[1] = (BYTE)(baud >> 8);
    coding[2] = (BYTE)(baud >> 16);
    coding[3] = (BYTE)(baud >> 24);
    coding[4] = 0;
    coding[5] = 0;
    coding[6] = 8;
    memcpy(usb_cdc_dma_buf, coding, 7);
    s.bmRequestType = 0x21;
    s.bRequest = USB_CDC_REQ_SET_LINE_CODING;
    s.wIndex = usb_cdc_comm_if;
    s.wLength = 7;
    if (!usb_ctrl_dev(USB_CDC_DEV, &s, usb_cdc_dma_buf, 7))
        return 0;
    memset(&s, 0, sizeof(s));
    s.bmRequestType = 0x21;
    s.bRequest = USB_CDC_REQ_SET_CONTROL_LINE_STATE;
    s.wValue = USB_CDC_LINE_DTR | USB_CDC_LINE_RTS;
    s.wIndex = usb_cdc_comm_if;
    return usb_ctrl_dev(USB_CDC_DEV, &s, 0, 0);
}

static int usb_enumerate_cdc(void)
{
    BYTE devdesc[18];
    BYTE cfghdr[9];
    BYTE cfg[256];
    WORD total;
    usb_cdc_acm_info info;

    if (!usb_get_desc_dev(USB_CDC_DEV, USB_DESC_DEVICE, 0, devdesc, 8))
        return 0;
    if (!xhci_set_ep0_packet_size(usb_xhci_hcd, USB_CDC_DEV, devdesc[7]))
        return 0;
    if (!usb_get_desc_dev(USB_CDC_DEV, USB_DESC_DEVICE, 0, devdesc, 18))
        return 0;
    printf("usb: cdc vid=%04x pid=%04x class=%u\n",
           (unsigned)(devdesc[8] | (devdesc[9] << 8)),
           (unsigned)(devdesc[10] | (devdesc[11] << 8)),
           (unsigned)devdesc[4]);
    if (devdesc[4] == 9)
        return 0;
    if (!usb_get_desc_dev(USB_CDC_DEV, USB_DESC_CONFIG, 0, cfghdr, 9))
        return 0;
    total = cfghdr[2] | (cfghdr[3] << 8);
    if (total < 9 || total > sizeof(cfg))
        total = 9;
    if (!usb_get_desc_dev(USB_CDC_DEV, USB_DESC_CONFIG, 0, cfg, total))
        return 0;
    {
        int acm = usb_parse_cdc_acm(cfg, total, &info);
        if (!acm && !usb_parse_vendor_bulk_serial(cfg, total, &info)) {
            printf("usb: no CDC-ACM or vendor serial interface\n");
            return 0;
        }
        if (!acm)
            printf("usb: vendor bulk serial (FTDI-style)\n");
        if (!usb_set_config_dev(USB_CDC_DEV, info.cfgval))
            return 0;
        if (!xhci_configure_endpoints(usb_xhci_hcd, USB_CDC_DEV,
                                      info.ep_in, info.mps_in, info.burst_in,
                                      info.ep_out, info.mps_out,
                                      info.burst_out))
            return 0;
        usb_cdc_ep_in = info.ep_in;
        usb_cdc_ep_out = info.ep_out;
        usb_cdc_mps_out = info.mps_out;
        usb_cdc_mps_in = info.mps_in;
        usb_cdc_comm_if = info.comm_if;
        usb_cdc_in_skip = acm ? 0 : 2;
        if (acm)
            (void)usb_cdc_set_line();
        printf("usb: serial console endpoints in=%d out=%d acm=%d\n",
               usb_cdc_ep_in, usb_cdc_ep_out, acm);
    }
    return 1;
}

static int usb_xhci_bind_cdc(void)
{
    DWORD port;
    DWORD used_ports = 0;
    int i;

    usb_cdc_reset_state();
    if (!usb_xhci_hcd)
        return 0;
    /* Skip every port already claimed by another driver (MSC root, secondary
       MSC, CDC-ECM NIC). The console must only take a genuine serial gadget,
       never a network gadget that merely exposes an ACM/RNDIS config. */
    for (i = 0; i < XHCI_MAX_USBDEVS; i++)
        if (usb_xhci_hcd->usbdevs[i].port)
            used_ports |= xhci_ccs_bit(usb_xhci_hcd->usbdevs[i].port);
    usb_xhci_hcd->enumerating = 1;
    usb_xhci_hcd->recovery_needed = 0;
    for (port = 1; port <= usb_xhci_hcd->max_ports && port <= 32; port++) {
        if (used_ports & xhci_ccs_bit(port))
            continue;
        if (!(xhci_port_ccs_mask(usb_xhci_hcd) & xhci_ccs_bit(port)))
            continue;
        printf("xhci: trying CDC port %u\n", port);
        if (!xhci_claim_port_dev(usb_xhci_hcd, port, USB_CDC_DEV))
            continue;
        /* Tag this slot as the polled console so the xHCI transfer path
           applies the CDC-ACM spin policy (short IN spin, posted bulk-IN,
           stashed completions) to all of its endpoints. */
        xhci_set_console(usb_xhci_hcd, USB_CDC_DEV, 1);
        if (usb_enumerate_cdc()) {
            char ver[192];
            int vn;
            usb_xhci_hcd->enumerating = 0;
            usb_cdc_ready = 1;
            usb_cdc_dev = USB_CDC_DEV;
            usb_registry_add(1, port, USB_CDC_DEV, USB_DRV_CDC_ACM, 0,
                             "usb-cdc");
            serial_puts("USB_CDC_CONSOLE_OK\n");
            vn = usb_debug_format_icsos_ver(ver, (int)sizeof(ver),
                                            ICSOS_RELEASE, ICSOS_GIT_HASH,
                                            ICSOS_GIT_DIRTY[0] == '1',
                                            ICSOS_BUILD_TS,
                                            __DATE__ " " __TIME__);
            if (vn > 0)
                serial_puts(ver);
            printf("usb: CDC-ACM console ready\n");
            usb_cdc_pump_flush();
            return 1;
        }
       printf("xhci: port %u not CDC-ACM\n", port);
        xhci_release_dev(usb_xhci_hcd, USB_CDC_DEV);
        usb_xhci_hcd->recovery_needed = 0;
    }
    usb_xhci_hcd->enumerating = 0;
    return 0;
}

/* =========================================================================
   CDC-ECM (Ethernet Control Model) NIC

   Presents the bulk IN/OUT endpoints of a CDC-ECM gadget as a polled netif.
   There is no controller interrupt: RX is a posted bulk-IN (xhci_bulk_in_try)
   harvested from netif_poll -- driven by the protocol wait loops and softnet
   -- and TX is a synchronous bulk-OUT. The posted-IN completion stash is the
   single xhci_cdc_in_* instance shared with the CDC-ACM console, so a console
   and an ECM NIC must not be active at once; the ECM acceptance boot attaches
   no CDC console. All USB I/O is serialized by usb_io_lock and the posted-IN
   state machine, so a user protocol client and softnet may poll concurrently
   (the first xhci_cdc_in_take() wins the frame; the stash is the mailbox).
   ========================================================================= */

#define USB_ECM_BUF_SZ   ETH_MAX_FRAME

static struct {
    int ready;
    DWORD dev;
    DWORD port;
    BYTE  ep_in;
    BYTE  ep_out;
    BYTE  mac[ETH_ADDR_LEN];
    struct netif nif;
    struct netdev ndev;
    /* RX DMA buffer: heap-allocated at bind.  The kernel image sits at the
       4MiB user-ELF cap with no BSS slack, so a static 1514-byte buffer here
       would push the boot page tables past 0x400000.  The DMA layer maps
       single buffers with alignment 1, so a plain malloc is DMA-safe. */
    BYTE  *buf;
} usb_ecm;

static void usb_ecm_get_mac(void *drv, unsigned char mac[ETH_ADDR_LEN])
{
    int i;
    (void)drv;
    for (i = 0; i < ETH_ADDR_LEN; i++)
        mac[i] = usb_ecm.mac[i];
}

static int usb_ecm_link_up(void *drv)
{
    (void)drv;
    return usb_ecm.ready ? 1 : 0;
}

static int usb_ecm_transmit(void *drv, struct pbuf *p)
{
    (void)drv;
    if (!usb_ecm.ready || !p || p->len < 1 || p->len > USB_ECM_BUF_SZ)
        return -1;
    usb_io_lock_acquire();
    if (!xhci_bulk(usb_xhci_hcd, usb_ecm.dev, usb_ecm.ep_out, 0,
                   p->data, (int)p->len)) {
        usb_io_lock_release();
        return -1;
    }
    usb_io_lock_release();
    return 0;
}

static void usb_ecm_rx_one(void *drv)
{
    int got = 0;
    struct pbuf *p;
    (void)drv;
    if (!usb_ecm.ready)
        return;
    usb_io_lock_acquire();
    if (!xhci_bulk_in_try(usb_xhci_hcd, usb_ecm.dev, usb_ecm.ep_in,
                          usb_ecm.buf, USB_ECM_BUF_SZ, &got,
                          XHCI_ECM_IN_SPINS) ||
        got < ETH_HDR_LEN || got > USB_ECM_BUF_SZ) {
        usb_io_lock_release();
        return;
    }
    usb_io_lock_release();
    p = pbuf_alloc((u16)got);
    if (!p)
        return;
    memcpy(p->data, usb_ecm.buf, (unsigned int)got);
    net_lock();
    netif_input(&usb_ecm.nif, p);
    net_unlock();
}

static const struct netdev_ops usb_ecm_ops = {
    .transmit = usb_ecm_transmit,
    .link_up = usb_ecm_link_up,
    .poll = usb_ecm_rx_one,
    .get_mac = usb_ecm_get_mac,
};

void usb_ecm_poll(void)
{
    if (usb_ecm.ready)
        usb_ecm_rx_one(&usb_ecm);
}

/* =========================================================================
   RNDIS USB NIC
   ---------------------------------------------------------------------------
   RNDIS (Remote NDIS) is the Microsoft CDC-ACM-shaped USB Ethernet protocol.
   It is a composite gadget identical in descriptor shape to CDC-ACM serial:
   a Communications interface (class 2, subclass ACM) plus a CDC Data
   interface (class 10) with bulk IN/OUT. The difference is the control
   protocol: RNDIS messages (INIT/QUERY/SET/RESET/KEEPALIVE) travel over the
   CDC SEND/GET_ENCAPSULATED_COMMAND/RESPONSE class requests on endpoint 0,
   while Ethernet frames travel over the bulk endpoints wrapped in
   RNDIS_PACKET_MSG envelopes. There is no Ethernet functional descriptor, so
   the MAC and MTU come from RNDIS queries, not the descriptor.

   A plain CDC-ACM serial gadget presents the same descriptors, so the bind
   path commits to RNDIS only after a successful INITIALIZE probe (a serial
   device STALLs SET_ENCAPSULATED_COMMAND). Ground truth: QEMU usb-net
   (vendor 0x0525, product 0xa4a2) exposes RNDIS as config value 2 and
   CDC-ECM as config value 1; the ECM binder runs first and normally claims
   the gadget, so RNDIS binds only when the port is free or a RNDIS-only
   gadget is attached.
   ========================================================================= */

#define USB_RNDIS_HDR_SZ      44    /* sizeof(rndis_packet_msg) */
#define USB_RNDIS_BUF_SZ      (USB_RNDIS_HDR_SZ + ETH_MAX_FRAME)
#define USB_RNDIS_CBUF_SZ     128   /* largest RNDIS control message */

#define RNDIS_MSG_PACKET          1
#define RNDIS_MSG_INITIALIZE      2
#define RNDIS_MSG_QUERY           4
#define RNDIS_MSG_SET             5
#define RNDIS_MSG_RESET           6
#define RNDIS_MSG_KEEPALIVE       8
#define RNDIS_MSG_INIT_CMPLT      0x80000002u
#define RNDIS_MSG_QUERY_CMPLT     0x80000004u
#define RNDIS_MSG_SET_CMPLT       0x80000005u
#define RNDIS_MSG_RESET_CMPLT     0x80000006u
#define RNDIS_MSG_KEEPALIVE_CMPLT 0x80000008u
#define RNDIS_MSG_INDICATE_STATUS 7
#define RNDIS_STATUS_SUCCESS      0x00000000u
#define RNDIS_OID_PERM_MAC        0x01010101u
#define RNDIS_OID_MAX_FRAME_SIZE  0x00010106u
#define RNDIS_OID_PACKET_FILTER   0x0001010eu
#define RNDIS_FILTER_BROADCAST    0x0002u
#define RNDIS_FILTER_MULTICAST    0x0004u
#define RNDIS_FILTER_ALLMULTI     0x8000u

/* CDC class-specific interface requests carrying the RNDIS message set. */
#define USB_CDC_SEND_ENCAP_CMD    0x00
#define USB_CDC_GET_ENCAP_RESP    0x01
#define USB_RT_CLASS_IF_H2D       0x21
#define USB_RT_CLASS_IF_D2H       0xa1

typedef struct __attribute__((packed)) {
    DWORD MessageType;
    DWORD MessageLength;
    DWORD RequestID;
    DWORD MajorVersion;
    DWORD MinorVersion;
    DWORD MaxTransferSize;
} rndis_init_msg;

typedef struct __attribute__((packed)) {
    DWORD MessageType;
    DWORD MessageLength;
    DWORD RequestID;
    DWORD Status;
    DWORD InformationBufferLength;
    DWORD InformationBufferOffset;
} rndis_query_cmplt;

typedef struct __attribute__((packed)) {
    DWORD MessageType;
    DWORD MessageLength;
    DWORD RequestID;
    DWORD Status;
} rndis_set_cmplt;

typedef struct __attribute__((packed)) {
    DWORD MessageType;
    DWORD MessageLength;
    DWORD DataOffset;
    DWORD DataLength;
    DWORD OOBDataOffset;
    DWORD OOBDataLength;
    DWORD NumOOBDataElements;
    DWORD PerPacketInfoOffset;
    DWORD PerPacketInfoLength;
    DWORD VcHandle;
    DWORD Reserved;
} rndis_packet_msg;

static struct {
    int ready;
    DWORD dev;
    DWORD port;
    BYTE  ep_in;
    BYTE  ep_out;
    BYTE  comm_if;
    BYTE  mac[ETH_ADDR_LEN];
    WORD  mtu;
    DWORD reqid;
    DWORD last_keepalive_secs;
    struct netif nif;
    struct netdev ndev;
    /* TX/RX envelope buffer (heap at bind) and a small control buffer for
       the encapsulated RNDIS messages. */
    BYTE  *buf;
    BYTE   cbuf[USB_RNDIS_CBUF_SZ];
} usb_rndis;

static void usb_rndis_get_mac(void *drv, unsigned char mac[ETH_ADDR_LEN])
{
    int i;
    (void)drv;
    for (i = 0; i < ETH_ADDR_LEN; i++)
        mac[i] = usb_rndis.mac[i];
}

static int usb_rndis_link_up(void *drv)
{
    (void)drv;
    return usb_rndis.ready ? 1 : 0;
}

/* Wrap the Ethernet frame in a RNDIS_PACKET_MSG envelope and push it on the
   bulk OUT endpoint. The device reads the frame at 8 + DataOffset. */
static int usb_rndis_transmit(void *drv, struct pbuf *p)
{
    rndis_packet_msg *h;
    unsigned int flen;
    (void)drv;
    if (!usb_rndis.ready || !p || p->len < 1 ||
        p->len > ETH_MAX_FRAME || !usb_rndis.buf)
        return -1;
    flen = (unsigned int)p->len;
    h = (rndis_packet_msg *)usb_rndis.buf;
    memset(h, 0, sizeof(*h));
    h->MessageType = RNDIS_MSG_PACKET;
    h->MessageLength = USB_RNDIS_HDR_SZ + flen;
    h->DataOffset = USB_RNDIS_HDR_SZ - 8;
    h->DataLength = flen;
    memcpy(usb_rndis.buf + USB_RNDIS_HDR_SZ, p->data, flen);
    usb_io_lock_acquire();
    if (!xhci_bulk(usb_xhci_hcd, usb_rndis.dev, usb_rndis.ep_out, 0,
                   usb_rndis.buf, (int)(USB_RNDIS_HDR_SZ + flen))) {
        usb_io_lock_release();
        return -1;
    }
    usb_io_lock_release();
    return 0;
}

/* Harvest one posted bulk-IN. The device NAKs when no frame is pending, so a
   0-byte result simply means idle. A RNDIS_PACKET_MSG carries the frame at
   8 + DataOffset; an INDICATE_STATUS message carries link state. */
static void usb_rndis_rx_one(void *drv)
{
    int got = 0;
    struct pbuf *p;
    rndis_packet_msg *h;
    DWORD type, mlen, doff, dlen;
    (void)drv;
    if (!usb_rndis.ready || !usb_rndis.buf)
        return;
    usb_io_lock_acquire();
    if (!xhci_bulk_in_try(usb_xhci_hcd, usb_rndis.dev, usb_rndis.ep_in,
                          usb_rndis.buf, USB_RNDIS_BUF_SZ, &got,
                          XHCI_ECM_IN_SPINS) ||
        got < 8) {
        usb_io_lock_release();
        return;
    }
    h = (rndis_packet_msg *)usb_rndis.buf;
    type = h->MessageType;
    mlen = h->MessageLength;
    doff = h->DataOffset;
    dlen = h->DataLength;
    usb_io_lock_release();
    if (type == RNDIS_MSG_PACKET) {
        unsigned int off = 8 + (unsigned int)doff;
        if (dlen < ETH_HDR_LEN || dlen > ETH_MAX_FRAME ||
            off + (unsigned int)dlen > (unsigned int)got ||
            off + (unsigned int)dlen > USB_RNDIS_BUF_SZ)
            return;
        p = pbuf_alloc((u16)dlen);
        if (!p)
            return;
        memcpy(p->data, usb_rndis.buf + off, (unsigned int)dlen);
        net_lock();
        netif_input(&usb_rndis.nif, p);
        net_unlock();
    } else if (type == RNDIS_MSG_INDICATE_STATUS) {
        (void)mlen;
    }
}

static const struct netdev_ops usb_rndis_ops = {
    .transmit = usb_rndis_transmit,
    .link_up = usb_rndis_link_up,
    .poll = usb_rndis_rx_one,
    .get_mac = usb_rndis_get_mac,
};

/* Issue a RNDIS message on the CDC SEND_ENCAPSULATED_COMMAND request (data
   out) and read the matching completion off GET_ENCAPSULATED_RESPONSE (data
   in). The CDC encapsulation requests carry wIndex = 0 (QEMU rejects any
   non-zero index). When no completion is queued the device returns a single
   zero byte, so we clear the buffer first and treat a zero MessageType as
   "no response". *resp_len is set to the RNDIS MessageLength when a valid
   completion arrives, else 0. */
static int usb_rndis_cmd(const void *req, int reqlen, int *resp_len)
{
    usb_setup s;
    DWORD mlen;
    int n;

    if (!usb_rndis.ready && !usb_rndis.dev)
        return 0;
    memset(&s, 0, sizeof(s));
    s.bmRequestType = USB_RT_CLASS_IF_H2D;
    s.bRequest = USB_CDC_SEND_ENCAP_CMD;
    s.wValue = 0;
    s.wIndex = 0;
    s.wLength = (WORD)reqlen;
    if (!usb_ctrl_dev(usb_rndis.dev, &s, (void *)req, reqlen))
        return 0;
    memset(usb_rndis.cbuf, 0, sizeof(usb_rndis.cbuf));
    memset(&s, 0, sizeof(s));
    s.bmRequestType = USB_RT_CLASS_IF_D2H;
    s.bRequest = USB_CDC_GET_ENCAP_RESP;
    s.wValue = 0;
    s.wIndex = 0;
    s.wLength = USB_RNDIS_CBUF_SZ;
    if (!usb_ctrl_dev(usb_rndis.dev, &s, usb_rndis.cbuf, USB_RNDIS_CBUF_SZ))
        return 0;
    /* cbuf[0] is the low byte of MessageType; 0 means the device returned
       the no-response marker (a lone 0 byte) rather than a completion. */
    if (usb_rndis.cbuf[0] == 0) {
        *resp_len = 0;
        return 1;
    }
    mlen = (DWORD)usb_rndis.cbuf[4] | ((DWORD)usb_rndis.cbuf[5] << 8) |
           ((DWORD)usb_rndis.cbuf[6] << 16) | ((DWORD)usb_rndis.cbuf[7] << 24);
    n = (int)mlen;
    if (n < 4 || n > USB_RNDIS_CBUF_SZ)
        n = 0;
    *resp_len = n;
    return 1;
}

static int usb_rndis_init(void)
{
    rndis_init_msg m;
    rndis_query_cmplt *c;
    int n;

    m.MessageType = RNDIS_MSG_INITIALIZE;
    m.MessageLength = sizeof(m);
    m.RequestID = ++usb_rndis.reqid;
    m.MajorVersion = 1;
    m.MinorVersion = 0;
    m.MaxTransferSize = USB_RNDIS_HDR_SZ + ETH_MAX_FRAME;
    if (!usb_rndis_cmd(&m, (int)sizeof(m), &n) || n < 4)
        return 0;
    c = (rndis_query_cmplt *)usb_rndis.cbuf;
    if (c->MessageType != RNDIS_MSG_INIT_CMPLT ||
        c->Status != RNDIS_STATUS_SUCCESS)
        return 0;
    return 1;
}

/* Query an OID. The information buffer (MAC bytes / MTU) follows the
   24-byte completion at 8 + InformationBufferOffset. */
static int usb_rndis_query(DWORD oid, void *out, int outlen)
{
    BYTE req[28];
    DWORD *w;
    rndis_query_cmplt *c;
    int n;
    int i;

    memset(req, 0, sizeof(req));
    w = (DWORD *)req;
    w[0] = RNDIS_MSG_QUERY;
    w[1] = 28;
    w[2] = ++usb_rndis.reqid;
    w[3] = oid;
    w[4] = 0;
    w[5] = 0;
    w[6] = 0;
    if (!usb_rndis_cmd(req, 28, &n) || n < 24)
        return 0;
    c = (rndis_query_cmplt *)usb_rndis.cbuf;
    if (c->MessageType != RNDIS_MSG_QUERY_CMPLT ||
        c->Status != RNDIS_STATUS_SUCCESS)
        return 0;
    /* RNDIS offsets are measured from the byte after the 8-byte
       MessageType/MessageLength header, so the buffer sits at
       8 + InformationBufferOffset (QEMU: offset 16 -> absolute 24). */
    if ((int)c->InformationBufferLength < outlen)
        return 0;
    if (8 + (unsigned int)c->InformationBufferOffset + (unsigned int)outlen
        > USB_RNDIS_CBUF_SZ)
        return 0;
    for (i = 0; i < outlen; i++)
        ((BYTE *)out)[i] =
            usb_rndis.cbuf[8 + c->InformationBufferOffset + i];
    return 1;
}

static int usb_rndis_set(DWORD oid, const void *val, int vallen)
{
    BYTE req[32];
    DWORD *w;
    rndis_set_cmplt *c;
    int n;

    if (vallen < 0 || 28 + (unsigned int)vallen > sizeof(req))
        return 0;
    memset(req, 0, sizeof(req));
    w = (DWORD *)req;
    w[0] = RNDIS_MSG_SET;
    w[1] = 28 + (DWORD)vallen;
    w[2] = ++usb_rndis.reqid;
    w[3] = oid;
    w[4] = (DWORD)vallen;
    w[5] = 20;
    w[6] = 0;
    memcpy(req + 28, val, (unsigned int)vallen);
    if (!usb_rndis_cmd(req, 28 + (int)vallen, &n) || n < 16)
        return 0;
    c = (rndis_set_cmplt *)usb_rndis.cbuf;
    if (c->MessageType != RNDIS_MSG_SET_CMPLT ||
        c->Status != RNDIS_STATUS_SUCCESS)
        return 0;
    return 1;
}

static int usb_rndis_keepalive(void)
{
    BYTE req[12];
    DWORD *w;
    rndis_set_cmplt *c;
    int n;

    w = (DWORD *)req;
    w[0] = RNDIS_MSG_KEEPALIVE;
    w[1] = 12;
    w[2] = ++usb_rndis.reqid;
    if (!usb_rndis_cmd(req, 12, &n) || n < 12)
        return 0;
    c = (rndis_set_cmplt *)usb_rndis.cbuf;
    if (c->MessageType != RNDIS_MSG_KEEPALIVE_CMPLT ||
        c->Status != RNDIS_STATUS_SUCCESS)
        return 0;
    return 1;
}

/* Enumerate a RNDIS config on an already-claimed device: activate it by
   value, configure the bulk endpoints, then run the control handshake
   (INIT -> QUERY MAC -> QUERY MTU -> SET packet filter). The filter SET is
   what moves the device into DATA_INITIALIZED and starts delivering frames
   on the bulk IN endpoint. */
static int usb_enumerate_rndis(DWORD dev)
{
    BYTE devdesc[18];
    BYTE cfghdr[9];
    BYTE cfg[256];
    WORD total;
    usb_cdc_rndis_info info;
    DWORD filter;
    int nconf, ci, found = 0;
    int i;

    if (!usb_get_desc_dev(dev, USB_DESC_DEVICE, 0, devdesc, 8))
        return 0;
    if (!xhci_set_ep0_packet_size(usb_xhci_hcd, dev, devdesc[7]))
        return 0;
    if (!usb_get_desc_dev(dev, USB_DESC_DEVICE, 0, devdesc, 18))
        return 0;
    printf("usb: rndis vid=%04x pid=%04x class=%u\n",
           (unsigned)(devdesc[8] | (devdesc[9] << 8)),
           (unsigned)(devdesc[10] | (devdesc[11] << 8)),
           (unsigned)devdesc[4]);
    if (devdesc[4] == 9)
        return 0;
    nconf = devdesc[17];
    if (nconf < 1)
        nconf = 1;
    if (nconf > 8)
        nconf = 8;
    for (ci = 0; ci < nconf && !found; ci++) {
        if (!usb_get_desc_dev(dev, USB_DESC_CONFIG, (BYTE)ci, cfghdr, 9))
            break;
        total = (WORD)(cfghdr[2] | (cfghdr[3] << 8));
        if (total < 9 || total > (WORD)sizeof(cfg))
            total = 9;
        if (!usb_get_desc_dev(dev, USB_DESC_CONFIG, (BYTE)ci, cfg, total))
            break;
        if (usb_parse_cdc_rndis(cfg, (unsigned int)total, &info))
            found = 1;
    }
    if (!found) {
        printf("usb: no RNDIS data interface (tried %d config%s)\n",
               nconf, nconf == 1 ? "" : "s");
        return 0;
    }
    if (!usb_set_config_dev(dev, info.cfgval))
        return 0;
    if (!xhci_configure_endpoints(usb_xhci_hcd, dev,
                                  info.ep_in, info.mps_in, info.burst_in,
                                  info.ep_out, info.mps_out, info.burst_out))
        return 0;
    usb_rndis.dev = dev;
    usb_rndis.ep_in = info.ep_in;
    usb_rndis.ep_out = info.ep_out;
    usb_rndis.comm_if = info.comm_if;
    /* Probe: a real RNDIS device answers INITIALIZE_CMPLT; a CDC-ACM serial
       gadget STALLs the encapsulation request and this bails. */
    if (!usb_rndis_init()) {
        printf("usb: rndis INIT probe failed (not RNDIS)\n");
        return 0;
    }
    if (!usb_rndis_query(RNDIS_OID_PERM_MAC, usb_rndis.mac, ETH_ADDR_LEN)) {
        usb_rndis.mac[0] = 0x02;
        usb_rndis.mac[1] = 0x00;
        usb_rndis.mac[2] = (BYTE)(dev + 1);
        usb_rndis.mac[3] = (BYTE)(info.data_if + 1);
        usb_rndis.mac[4] = 0x52;
        usb_rndis.mac[5] = (BYTE)(dev + 0x41);
        printf("usb: rndis MAC query failed; using fallback MAC\n");
    }
    {
        DWORD mtu = 0;
        if (usb_rndis_query(RNDIS_OID_MAX_FRAME_SIZE, &mtu, (int)sizeof(mtu)))
            usb_rndis.mtu =
                (mtu >= ETH_HDR_LEN && mtu <= ETH_MAX_FRAME) ? (WORD)mtu
                                                            : ETH_MTU;
        else
            usb_rndis.mtu = ETH_MTU;
    }
    filter = RNDIS_FILTER_BROADCAST | RNDIS_FILTER_MULTICAST |
             RNDIS_FILTER_ALLMULTI;
    if (!usb_rndis_set(RNDIS_OID_PACKET_FILTER, &filter, 4)) {
        printf("usb: rndis packet-filter SET failed\n");
        return 0;
    }
    printf("usb: rndis in=%d out=%d mtu=%u mac=",
           (int)usb_rndis.ep_in, (int)usb_rndis.ep_out,
           (unsigned)usb_rndis.mtu);
    for (i = 0; i < ETH_ADDR_LEN; i++)
        printf("%02x%s", usb_rndis.mac[i], i == ETH_ADDR_LEN - 1 ? "\n" : ":");
    return 1;
}

/*
  Drop the RNDIS NIC's driver-visible state ahead of a full controller reset
  (mirrors usb_ecm_tear_down). transmit()/rx_one() are gated on
  usb_rndis.ready, so clearing it stops any in-flight polling; the netif is
  re-initialized in place by usb_xhci_bind_rndis().
 */
static void usb_rndis_tear_down(void)
{
    if (!usb_rndis.ready)
        return;
    usb_rndis.ready = 0;
    usb_rndis.nif.link_up = 0;
}

static int usb_xhci_bind_rndis(void)
{
    DWORD used_ports = 0;
    DWORD dev_slot = 0;
    DWORD port;
    int i;

    if (!usb_xhci_hcd || usb_rndis.ready)
        return 0;
    for (i = 0; i < XHCI_MAX_USBDEVS; i++)
        if (usb_xhci_hcd->usbdevs[i].port)
            used_ports |= xhci_ccs_bit(usb_xhci_hcd->usbdevs[i].port);
    for (dev_slot = 2; dev_slot < XHCI_MAX_USBDEVS; dev_slot++) {
        if (!usb_xhci_hcd->usbdevs[dev_slot].port)
            break;
    }
    if (dev_slot >= XHCI_MAX_USBDEVS)
        dev_slot = 0;
    if (!dev_slot) {
        printf("usb: no free xHCI slot for RNDIS NIC\n");
        return 0;
    }
    usb_xhci_hcd->enumerating = 1;
    usb_xhci_hcd->recovery_needed = 0;
    for (port = 1; port <= usb_xhci_hcd->max_ports && port <= 32; port++) {
        if (!(xhci_port_ccs_mask(usb_xhci_hcd) & xhci_ccs_bit(port)))
            continue;
        if (used_ports & xhci_ccs_bit(port))
            continue;
        printf("xhci: trying RNDIS port %u (slot %u)\n", port, dev_slot);
        if (!xhci_claim_port_dev(usb_xhci_hcd, port, dev_slot))
            continue;
        if (usb_enumerate_rndis(dev_slot)) {
            if (!usb_rndis.buf)
                usb_rndis.buf = (BYTE *)malloc(USB_RNDIS_BUF_SZ);
            if (!usb_rndis.buf) {
                printf("xhci: no DMA buffer for RNDIS NIC\n");
                xhci_release_dev(usb_xhci_hcd, dev_slot);
                usb_xhci_hcd->recovery_needed = 0;
                continue;
            }
            usb_rndis.port = port;
            usb_rndis.ndev.drv = &usb_rndis;
            usb_rndis.ndev.ops = &usb_rndis_ops;
            usb_rndis.ndev.name[0] = 'r';
            usb_rndis.ndev.name[1] = 'n';
            usb_rndis.ndev.name[2] = 'd';
            usb_rndis.ndev.name[3] = 'i';
            usb_rndis.ndev.name[4] = 's';
            usb_rndis.ndev.name[5] = '0';
            usb_rndis.ndev.name[6] = 0;
            netif_init(&usb_rndis.nif, &usb_rndis.ndev);
            if (usb_rndis.mtu)
                usb_rndis.nif.mtu = usb_rndis.mtu;
            if (!netif_default())
                netif_set_default(&usb_rndis.nif);
            usb_rndis.ready = 1;
            usb_registry_add((int)dev_slot, port, (int)dev_slot,
                             USB_DRV_RNDIS, &usb_rndis, "usb-rndis");
            usb_xhci_hcd->enumerating = 0;
            serial_puts("USB_RNDIS_NIC_OK\n");
            return 1;
        }
        printf("xhci: port %u not RNDIS\n", port);
        xhci_release_dev(usb_xhci_hcd, dev_slot);
        usb_xhci_hcd->recovery_needed = 0;
    }
    usb_xhci_hcd->enumerating = 0;
    return 0;
}

void usb_rndis_poll(void)
{
    extern unsigned int time_count;
    if (usb_rndis.ready)
        usb_rndis_rx_one(&usb_rndis);
    /* RNDIS spec: host must keep the session alive with a KEEPALIVE roughly
       every 5 seconds or the device drops the link. QEMU tolerates silence,
       but real RNDIS gadgets do not, so send one on a 4-second cadence. */
    if (usb_rndis.ready && time_count != usb_rndis.last_keepalive_secs) {
        if (usb_rndis.last_keepalive_secs &&
            time_count - usb_rndis.last_keepalive_secs >= 4) {
            if (usb_rndis_keepalive())
                usb_rndis.last_keepalive_secs = time_count;
        } else {
            usb_rndis.last_keepalive_secs = time_count;
        }
    }
}

#ifdef KTEST
/* In-kernel RNDIS acceptance self-test (KTEST builds only). Boots with a
   QEMU usb-net gadget bound as a RNDIS NIC (the ECM binder is held back on
   the usb-rndis-test cmdline) and runs the same protocol suite as the CDC-ECM
   self-test -- DHCP DORA + renew/rebind, ARP/ICMP ping, UDP echo, TCP echo +
   RTO retransmit, DNS A, softnet -- over the RNDIS bulk endpoints, proving
   the RNDIS_PACKET_MSG framing and the encapsulated-control path both work
   end to end. Prints NET_RNDIS_PASS/FAIL plus the shared NET_* markers. */
static void usb_rndis_selftest(void)
{
    struct inet_config cfg;
    unsigned int dip;
    int dhcp_ok = 0;
    int pass = 1;

    if (!usb_rndis.ready) {
        printf("NET_RNDIS_FAIL not-ready\n");
        return;
    }
    printf("NET_RNDIS_START\n");
    inet_config_from_cmdline(&cfg, kernel_cmdline);
    arp_init(&usb_rndis.nif);
    tcp_init();
    usb_rndis.nif.link_up = 1;
    if (dhcp_client(&usb_rndis.nif, &cfg, 4000000) == 0) {
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
    netif_set_addr(&usb_rndis.nif, cfg.ip, cfg.netmask, cfg.gateway);
    netif_set_up(&usb_rndis.nif);
    printf("NETIF_UP ip=%u.%u.%u.%u gw=%u.%u.%u.%u dhcp=%d\n",
           (cfg.ip >> 24) & 0xFF, (cfg.ip >> 16) & 0xFF,
           (cfg.ip >> 8) & 0xFF, cfg.ip & 0xFF,
           (cfg.gateway >> 24) & 0xFF, (cfg.gateway >> 16) & 0xFF,
           (cfg.gateway >> 8) & 0xFF, cfg.gateway & 0xFF, dhcp_ok);
    if (dhcp_ok) {
        dhcp_force_timer_due(1, 0);
        if (dhcp_service(&usb_rndis.nif, &cfg, 1, 4000000) == 0)
            printf("NET_DHCP_RENEW_OK\n");
        else {
            printf("NET_DHCP_RENEW_FAIL\n");
            pass = 0;
        }
        dhcp_force_timer_due(1, 1);
        if (dhcp_service(&usb_rndis.nif, &cfg, 1, 4000000) == 0)
            printf("NET_DHCP_REBIND_OK\n");
        else {
            printf("NET_DHCP_REBIND_FAIL\n");
            pass = 0;
        }
    }
    tcp_listen_echo(TCP_ECHO_PORT);
    if (icmp_ping(&usb_rndis.nif, cfg.gateway, 300) == 0)
        printf("NET_PING_OK\n");
    else {
        printf("NET_PING_FAIL\n");
        pass = 0;
    }
    if (udp_echo_client(&usb_rndis.nif, cfg.gateway, UDP_TEST_PORT,
                        2000000) == 0)
        printf("NET_UDP_OK\n");
    else {
        printf("NET_UDP_FAIL\n");
        pass = 0;
    }
    if (tcp_echo_client(&usb_rndis.nif, cfg.gateway, TCP_TEST_PORT,
                        4000000) == 0)
        printf("NET_TCP_OK\n");
    else {
        printf("NET_TCP_FAIL\n");
        pass = 0;
    }
    if (tcp_echo_rexmit_selftest(&usb_rndis.nif, cfg.gateway, TCP_TEST_PORT,
                                 8000000) == 0)
        printf("NET_TCP_REXMIT_OK\n");
    else {
        printf("NET_TCP_REXMIT_FAIL\n");
        pass = 0;
    }
    dip = 0;
    if (dns_query_a(&usb_rndis.nif, cfg.gateway, DNS_TEST_PORT, "icsos.test",
                    &dip, 4000000) == 0 && dip == 0x0A000202u)
        printf("NET_DNS_OK ip=%u.%u.%u.%u\n",
               (dip >> 24) & 0xFF, (dip >> 16) & 0xFF,
               (dip >> 8) & 0xFF, dip & 0xFF);
    else {
        printf("NET_DNS_FAIL\n");
        pass = 0;
    }
    softnet_init();
    printf("NET_SOFTNET_OK\n");
    if (pass)
        printf("NET_RNDIS_PASS\n");
    else
        printf("NET_RNDIS_FAIL\n");
}
#endif /* KTEST: usb_rndis_selftest */

static int usb_ecm_read_mac(DWORD dev, BYTE imac)
{
    BYTE sbuf[34];
    int digits[12];
    int i;

    if (!imac)
        return 0;
    memset(sbuf, 0, sizeof(sbuf));
    if (!usb_get_desc_dev(dev, USB_DESC_STRING, imac, sbuf,
                          (WORD)sizeof(sbuf)))
        return 0;
    if (sbuf[1] != USB_DESC_STRING || sbuf[0] < 26)
        return 0;
    /* iMACAddress is a 12-char ASCII hex string in UTF-16LE; each 16-bit
       unit holds one hex digit (high byte 0). */
    for (i = 0; i < 12; i++) {
        BYTE c = sbuf[2 + (unsigned int)i * 2];
        if (c >= '0' && c <= '9')
            digits[i] = c - '0';
        else if (c >= 'a' && c <= 'f')
            digits[i] = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            digits[i] = c - 'A' + 10;
        else
            return 0;
    }
    for (i = 0; i < ETH_ADDR_LEN; i++)
        usb_ecm.mac[i] = (BYTE)((digits[i * 2] << 4) | digits[i * 2 + 1]);
    return 1;
}

static int usb_enumerate_ecm(DWORD dev)
{
    BYTE devdesc[18];
    BYTE cfghdr[9];
    BYTE cfg[256];
    WORD total;
    usb_cdc_ecm_info info;
    int i;

    if (!usb_get_desc_dev(dev, USB_DESC_DEVICE, 0, devdesc, 8))
        return 0;
    if (!xhci_set_ep0_packet_size(usb_xhci_hcd, dev, devdesc[7]))
        return 0;
    if (!usb_get_desc_dev(dev, USB_DESC_DEVICE, 0, devdesc, 18))
        return 0;
    printf("usb: ecm vid=%04x pid=%04x class=%u\n",
           (unsigned)(devdesc[8] | (devdesc[9] << 8)),
           (unsigned)(devdesc[10] | (devdesc[11] << 8)),
           (unsigned)devdesc[4]);
    if (devdesc[4] == 9)
        return 0;
    /*
     * A composite gadget can expose several configurations. QEMU usb-net is
     * the canonical case: config[0] is RNDIS (comm subclass 2) while
     * config[1] is the real CDC-ECM (Ethernet Networking, comm subclass 6).
     * Enumerate every configuration by index and bind the one that presents a
     * CDC-ECM data interface; SET_CONFIGURATION then activates it by value.
     */
    {
        int nconf = devdesc[17];
        int found = 0;
        int ci;
        if (nconf < 1)
            nconf = 1;
        if (nconf > 8)
            nconf = 8;
        for (ci = 0; ci < nconf && !found; ci++) {
            if (!usb_get_desc_dev(dev, USB_DESC_CONFIG, (BYTE)ci, cfghdr, 9))
                break;
            total = (WORD)(cfghdr[2] | (cfghdr[3] << 8));
            if (total < 9 || total > (WORD)sizeof(cfg))
                total = 9;
            if (!usb_get_desc_dev(dev, USB_DESC_CONFIG, (BYTE)ci, cfg, total))
                break;
            if (usb_parse_cdc_ecm(cfg, (unsigned int)total, &info))
                found = 1;
        }
        if (!found) {
            printf("usb: no CDC-ECM data interface (tried %d config%s)\n",
                   nconf, nconf == 1 ? "" : "s");
            return 0;
        }
        printf("usb: ecm config value=%u mtu=%u\n",
               (unsigned)info.cfgval, (unsigned)info.mtu);
    }
    if (!usb_set_config_dev(dev, info.cfgval))
        return 0;
    if (!xhci_configure_endpoints(usb_xhci_hcd, dev,
                                  info.ep_in, info.mps_in, info.burst_in,
                                  info.ep_out, info.mps_out, info.burst_out))
        return 0;
    usb_ecm.dev = dev;
    usb_ecm.ep_in = info.ep_in;
    usb_ecm.ep_out = info.ep_out;
    if (!usb_ecm_read_mac(dev, info.imac)) {
        usb_ecm.mac[0] = 0x02;
        usb_ecm.mac[1] = 0x00;
        usb_ecm.mac[2] = (BYTE)(dev + 1);
        usb_ecm.mac[3] = (BYTE)(info.data_if + 1);
        usb_ecm.mac[4] = 0x53;
        usb_ecm.mac[5] = (BYTE)(dev + 0xA1);
        printf("usb: ecm iMACAddress string unreadable; using fallback MAC\n");
    }
    printf("usb: ecm in=%d out=%d mtu=%u mac=",
           (int)usb_ecm.ep_in, (int)usb_ecm.ep_out, (unsigned)info.mtu);
    for (i = 0; i < ETH_ADDR_LEN; i++)
        printf("%02x%s", usb_ecm.mac[i], i == ETH_ADDR_LEN - 1 ? "\n" : ":");
    return 1;
}

/*
  Drop the ECM NIC's driver-visible state ahead of a full controller reset
  (xhci_stop_hcd + xhci_init_hcd). The xHCI slot/endpoint context is cleared
  by xhci_init_hcd() and the heap RX buffer is reused by the re-bind, so only
  readiness and link state are cleared here. transmit()/rx_one() are already
  gated on usb_ecm.ready, so clearing it stops any in-flight polling. The
  netif object is re-initialized in place by usb_xhci_bind_ecm() (it is a
  single global default pointer, not a list, so in-place re-init is safe).
 */
static void usb_ecm_tear_down(void)
{
    if (!usb_ecm.ready)
        return;
    usb_ecm.ready = 0;
    usb_ecm.nif.link_up = 0;
}

static int usb_xhci_bind_ecm(void)
{
    DWORD used_ports = 0;
    DWORD dev_slot = 0;
    DWORD port;
    int i;

    if (!usb_xhci_hcd || usb_ecm.ready)
        return 0;
    for (i = 0; i < XHCI_MAX_USBDEVS; i++)
        if (usb_xhci_hcd->usbdevs[i].port)
            used_ports |= xhci_ccs_bit(usb_xhci_hcd->usbdevs[i].port);
    for (dev_slot = 2; dev_slot < XHCI_MAX_USBDEVS; dev_slot++) {
        if (!usb_xhci_hcd->usbdevs[dev_slot].port)
            break;
    }
    if (dev_slot >= XHCI_MAX_USBDEVS)
        dev_slot = 0;
    if (!dev_slot) {
        printf("usb: no free xHCI slot for ECM NIC\n");
        return 0;
    }
    usb_xhci_hcd->enumerating = 1;
    usb_xhci_hcd->recovery_needed = 0;
    for (port = 1; port <= usb_xhci_hcd->max_ports && port <= 32; port++) {
        if (!(xhci_port_ccs_mask(usb_xhci_hcd) & xhci_ccs_bit(port)))
            continue;
        if (used_ports & xhci_ccs_bit(port))
            continue;
        printf("xhci: trying CDC-ECM port %u (slot %u)\n", port, dev_slot);
        if (!xhci_claim_port_dev(usb_xhci_hcd, port, dev_slot))
            continue;
        if (usb_enumerate_ecm(dev_slot)) {
            /* Reused across re-binds (controller recovery); allocated once. */
            if (!usb_ecm.buf)
                usb_ecm.buf = (BYTE *)malloc(USB_ECM_BUF_SZ);
            if (!usb_ecm.buf) {
                printf("xhci: no DMA buffer for ECM NIC\n");
                xhci_release_dev(usb_xhci_hcd, dev_slot);
                usb_xhci_hcd->recovery_needed = 0;
                continue;
            }
            usb_ecm.port = port;
            usb_ecm.ndev.drv = &usb_ecm;
            usb_ecm.ndev.ops = &usb_ecm_ops;
            usb_ecm.ndev.name[0] = 'e';
            usb_ecm.ndev.name[1] = 'c';
            usb_ecm.ndev.name[2] = 'm';
            usb_ecm.ndev.name[3] = '0';
            usb_ecm.ndev.name[4] = 0;
            netif_init(&usb_ecm.nif, &usb_ecm.ndev);
            if (!netif_default())
                netif_set_default(&usb_ecm.nif);
            usb_ecm.ready = 1;
            usb_registry_add((int)dev_slot, port, (int)dev_slot,
                             USB_DRV_CDC_ECM, &usb_ecm, "usb-ecm");
            usb_xhci_hcd->enumerating = 0;
            serial_puts("USB_ECM_NIC_OK\n");
            return 1;
        }
        printf("xhci: port %u not CDC-ECM\n", port);
        xhci_release_dev(usb_xhci_hcd, dev_slot);
        usb_xhci_hcd->recovery_needed = 0;
    }
    usb_xhci_hcd->enumerating = 0;
    return 0;
}

#ifdef KTEST
/* In-kernel acceptance self-tests (KTEST builds only). A production kernel
   (KTEST=0) omits these plus the fault-injection latches they drive. */
static void usb_ecm_selftest(void)
{
    struct inet_config cfg;
    unsigned int dip;
    int dhcp_ok = 0;
    int pass = 1;

    if (!usb_ecm.ready)
        return;
    inet_config_from_cmdline(&cfg, kernel_cmdline);
    arp_init(&usb_ecm.nif);
    tcp_init();
    usb_ecm.nif.link_up = 1;
    if (dhcp_client(&usb_ecm.nif, &cfg, 4000000) == 0) {
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
    netif_set_addr(&usb_ecm.nif, cfg.ip, cfg.netmask, cfg.gateway);
    netif_set_up(&usb_ecm.nif);
    printf("NETIF_UP ip=%u.%u.%u.%u gw=%u.%u.%u.%u dhcp=%d\n",
           (cfg.ip >> 24) & 0xFF, (cfg.ip >> 16) & 0xFF,
           (cfg.ip >> 8) & 0xFF, cfg.ip & 0xFF,
           (cfg.gateway >> 24) & 0xFF, (cfg.gateway >> 16) & 0xFF,
           (cfg.gateway >> 8) & 0xFF, cfg.gateway & 0xFF, dhcp_ok);
    if (dhcp_ok) {
        dhcp_force_timer_due(1, 0);
        if (dhcp_service(&usb_ecm.nif, &cfg, 1, 4000000) == 0)
            printf("NET_DHCP_RENEW_OK\n");
        else {
            printf("NET_DHCP_RENEW_FAIL\n");
            pass = 0;
        }
        dhcp_force_timer_due(1, 1);
        if (dhcp_service(&usb_ecm.nif, &cfg, 1, 4000000) == 0)
            printf("NET_DHCP_REBIND_OK\n");
        else {
            printf("NET_DHCP_REBIND_FAIL\n");
            pass = 0;
        }
    }
    tcp_listen_echo(TCP_ECHO_PORT);
    if (icmp_ping(&usb_ecm.nif, cfg.gateway, 300) == 0)
        printf("NET_PING_OK\n");
    else {
        printf("NET_PING_FAIL\n");
        pass = 0;
    }
    if (udp_echo_client(&usb_ecm.nif, cfg.gateway, UDP_TEST_PORT,
                        2000000) == 0)
        printf("NET_UDP_OK\n");
    else {
        printf("NET_UDP_FAIL\n");
        pass = 0;
    }
    if (tcp_echo_client(&usb_ecm.nif, cfg.gateway, TCP_TEST_PORT,
                        4000000) == 0)
        printf("NET_TCP_OK\n");
    else {
        printf("NET_TCP_FAIL\n");
        pass = 0;
    }
    if (tcp_echo_rexmit_selftest(&usb_ecm.nif, cfg.gateway, TCP_TEST_PORT,
                                 8000000) == 0)
        printf("NET_TCP_REXMIT_OK\n");
    else {
        printf("NET_TCP_REXMIT_FAIL\n");
        pass = 0;
    }
    dip = 0;
    if (dns_query_a(&usb_ecm.nif, cfg.gateway, DNS_TEST_PORT, "icsos.test",
                    &dip, 4000000) == 0 && dip == 0x0A000202u)
        printf("NET_DNS_OK ip=%u.%u.%u.%u\n",
               (dip >> 24) & 0xFF, (dip >> 16) & 0xFF,
               (dip >> 8) & 0xFF, dip & 0xFF);
    else {
        printf("NET_DNS_FAIL\n");
        pass = 0;
    }
    softnet_init();
    printf("NET_SOFTNET_OK\n");
    if (pass)
        printf("NET_ECM_PASS\n");
    else
        printf("NET_ECM_FAIL\n");
}

/*
  Regression for the xHCI recovery path re-binding the CDC-ECM NIC.

  Boots with a live usb-net (ECM NIC bound alongside the MSC root), proves the
  NIC is functional (DHCP + ping to the gateway), then forces a full controller
  reset via usb_xhci_recover(). After the reset it asserts BOTH the MSC root and
  the ECM NIC come back and the NIC is functional again. Without the
  usb_xhci_bind_ecm() call in the recovery/reconnect paths, the NIC stays down
  after the reset (usb_ecm.ready == 0) and this fails with nic-not-rebound.
 */
static int usb_xhci_ecm_recover_selftest(void)
{
    int before = usb_xhci_recovery_count;
    struct inet_config cfg;

    if (usb_host != USB_HOST_XHCI || !usb_xhci_hcd || !usb_ecm.ready) {
        serial_puts("ECM_RECOVER_FAIL no-nic\n");
        return 0;
    }
    inet_config_from_cmdline(&cfg, kernel_cmdline);
    usb_ecm.nif.link_up = 1;
    if (dhcp_client(&usb_ecm.nif, &cfg, 4000000) != 0) {
        serial_puts("ECM_RECOVER_FAIL pre-dhcp\n");
        return 0;
    }
    netif_set_addr(&usb_ecm.nif, cfg.ip, cfg.netmask, cfg.gateway);
    netif_set_up(&usb_ecm.nif);
    if (icmp_ping(&usb_ecm.nif, cfg.gateway, 300) != 0) {
        serial_puts("ECM_RECOVER_FAIL pre-ping\n");
        return 0;
    }
    serial_puts("ECM_RECOVER_PRE_OK\n");

    if (!usb_xhci_recover()) {
        serial_puts("ECM_RECOVER_FAIL reset\n");
        return 0;
    }
    if (usb_xhci_recovery_count != before + 1) {
        serial_puts("ECM_RECOVER_FAIL count\n");
        return 0;
    }
    if (!usb_msc0.drive.present) {
        serial_puts("ECM_RECOVER_FAIL msc-gone\n");
        return 0;
    }
    if (!usb_ecm.ready) {
        serial_puts("ECM_RECOVER_FAIL nic-not-rebound\n");
        return 0;
    }
    serial_puts("ECM_RECOVER_REBIND_OK\n");

    /* The re-bound NIC must be usable again, not merely marked ready. */
    inet_config_from_cmdline(&cfg, kernel_cmdline);
    usb_ecm.nif.link_up = 1;
    if (dhcp_client(&usb_ecm.nif, &cfg, 4000000) != 0) {
        serial_puts("ECM_RECOVER_FAIL post-dhcp\n");
        return 0;
    }
    netif_set_addr(&usb_ecm.nif, cfg.ip, cfg.netmask, cfg.gateway);
    netif_set_up(&usb_ecm.nif);
    if (icmp_ping(&usb_ecm.nif, cfg.gateway, 300) != 0) {
        serial_puts("ECM_RECOVER_FAIL post-ping\n");
        return 0;
    }
    serial_puts("ECM_RECOVER_OK\n");
    return 1;
}
   #endif /* KTEST: usb_ecm_selftest / usb_xhci_ecm_recover_selftest */

/* =========================================================================
    ASIX AX88179 / AX88178A USB Gigabit Ethernet NIC
   ---------------------------------------------------------------------------
   Vendor-specific control protocol over endpoint 0:
     READ  bmRequestType=0xA0 (IN|VENDOR|DEVICE), bRequest=cmd, wValue, wIndex,
           wLength=size
     WRITE bmRequestType=0x20 (OUT|VENDOR|DEVICE), same fields
   MAC register access: cmd=AX_ACCESS_MAC, wValue=register, wIndex=size.
   PHY (MDIO) access:   cmd=AX_ACCESS_PHY,   wValue=phy_id(0x03), wIndex=reg.
   2- and 4-byte register values are little-endian on the wire.

   The NIC is a CDC-Ethernet (class 2) device: one Communications interface
   (interrupt IN 0x81) plus one Data interface (bulk IN 0x82, bulk OUT 0x03).
   The status interrupt endpoint is not used; link state is polled from the
   PHY PHYSR register.  TX prepends an 8-byte header (le32 len, le32 flags);
   RX returns a bundled multi-packet buffer whose last 4 bytes are
   {pkt_cnt, hdr_off} and whose per-packet metadata gives each frame length.

   Polling driver: softnet calls usb_asix_poll(), which posts a bulk IN and,
   on data, parses the bundle and feeds netif_input().  This mirrors the
   CDC-ECM and RNDIS NICs in this file.
   ========================================================================= */

#define USB_ASIX_VID          0x0b95
#define USB_ASIX_PID          0x1790
#define USB_ASIX_TXHDR_SZ     8
#define USB_ASIX_TXBUF_SZ     (USB_ASIX_TXHDR_SZ + ETH_MAX_FRAME)
#define USB_ASIX_RXBUF_SZ     20480   /* AX88179_BULKIN_SIZE[0] -> 1024*20 */

#define AX_ACCESS_MAC         0x01
#define AX_ACCESS_PHY         0x02
#define AX_RT_IN_VENDOR       0xa0
#define AX_RT_OUT_VENDOR      0x20
#define AX_PHY_ID             0x03

#define AX_NODE_ID            0x10
#define AX_GENERAL_STATUS     0x03
#define AX_PHYSICAL_LINK_STAT 0x02
#define AX_RX_CTL             0x0b
#define AX_MONITOR_MOD        0x24
#define AX_PHYPWR_RSTCTL      0x26
#define AX_RX_BULKIN_QCTRL    0x2e
#define AX_CLK_SELECT         0x33
#define AX_RXCOE_CTL          0x34
#define AX_TXCOE_CTL          0x35
#define AX_MEDIUM_STATUS_MODE 0x22
#define AX_PAUSE_WATERLVL_LOW  0x55
#define AX_PAUSE_WATERLVL_HIGH 0x54

#define AX_RX_CTL_DROPCRCERR  0x0100
#define AX_RX_CTL_IPE         0x0200
#define AX_RX_CTL_START       0x0080
#define AX_RX_CTL_AP          0x0020
#define AX_RX_CTL_AB          0x0008
#define AX_RX_CTL_AMALL       0x0002

#define AX_CLK_SELECT_BCS     0x01
#define AX_CLK_SELECT_ACS     0x02
#define AX_PHYPWR_RSTCTL_IPRL 0x0020

#define AX_RXCOE_IP           0x01
#define AX_RXCOE_TCP          0x02
#define AX_RXCOE_UDP          0x04
#define AX_RXCOE_TCPV6        0x20
#define AX_RXCOE_UDPV6        0x40

#define AX_MONITOR_MODE_RWMP    0x04
#define AX_MONITOR_MODE_PMEPOL  0x20
#define AX_MONITOR_MODE_PMETYPE 0x40

#define AX_MEDIUM_GIGAMODE       0x01
#define AX_MEDIUM_FULL_DUPLEX    0x02
#define AX_MEDIUM_EN_125MHZ      0x08
#define AX_MEDIUM_RXFLOW_CTRLEN  0x10
#define AX_MEDIUM_TXFLOW_CTRLEN  0x20
#define AX_MEDIUM_RECEIVE_EN     0x100
#define AX_MEDIUM_PS             0x200

#define AX_USB_SS  0x04
#define AX_USB_HS  0x02

#define GMII_PHY_PHYSR        0x11
#define GMII_PHY_PHYSR_SMASK  0xc000
#define GMII_PHY_PHYSR_GIGA   0x8000
#define GMII_PHY_PHYSR_100    0x4000
#define GMII_PHY_PHYSR_FULL   0x2000
#define GMII_PHY_PHYSR_LINK   0x400

static const BYTE ax_bulk_in_size[4][5] = {
    { 7, 0x4f, 0x00, 0x12, 0xff },
    { 7, 0x20, 0x03, 0x16, 0xff },
    { 7, 0xae, 0x07, 0x18, 0xff },
    { 7, 0xcc, 0x4c, 0x18, 0x08 }
};

static struct {
    int   ready;
    DWORD dev;
    DWORD port;
    BYTE  ep_in;
    BYTE  ep_out;
    WORD  mps_in;
    WORD  mps_out;
    BYTE  burst_in;
    BYTE  burst_out;
    BYTE  cfgval;
    BYTE  mac[ETH_ADDR_LEN];
    DWORD last_link_secs;
    int   link;
    BYTE  *txbuf;
    BYTE  *rxbuf;
    struct netif  nif;
    struct netdev ndev;
} usb_asix;

static int usb_asix_read_reg(int cmd, WORD value, WORD index, void *data,
                              int size)
{
    usb_setup s;
    int i;
    /* The ASIX USB-to-MAC bridge firmware can STALL the first vendor
       control right after SET_CONFIGURATION while it finishes coming up.
       Retry a bounded number of times with a short settle between. */
    for (i = 0; i < 4; i++) {
        memset(&s, 0, sizeof(s));
        s.bmRequestType = (BYTE)AX_RT_IN_VENDOR;
        s.bRequest = (BYTE)cmd;
        s.wValue = value;
        s.wIndex = index;
        s.wLength = (WORD)size;
        if (usb_ctrl_dev(usb_asix.dev, &s, data, size))
            return 1;
        if (i < 3)
            usb_wait_ms(25);
    }
    return 0;
}

static int usb_asix_write_reg(int cmd, WORD value, WORD index, const void *data,
                              int size)
{
    usb_setup s;
    int i;
    for (i = 0; i < 4; i++) {
        memset(&s, 0, sizeof(s));
        s.bmRequestType = (BYTE)AX_RT_OUT_VENDOR;
        s.bRequest = (BYTE)cmd;
        s.wValue = value;
        s.wIndex = index;
        s.wLength = (WORD)size;
        if (usb_ctrl_dev(usb_asix.dev, &s, (void *)data, size))
            return 1;
        if (i < 3)
            usb_wait_ms(25);
    }
    return 0;
}

static int usb_asix_mac_read(WORD reg, void *data, int size)
{
    return usb_asix_read_reg(AX_ACCESS_MAC, reg, (WORD)size, data, size);
}

static int usb_asix_mac_write(WORD reg, const void *data, int size)
{
    return usb_asix_write_reg(AX_ACCESS_MAC, reg, (WORD)size, data, size);
}

static int usb_asix_mdio_read(int loc, WORD *res)
{
    return usb_asix_read_reg(AX_ACCESS_PHY, AX_PHY_ID, (WORD)loc, res, 2);
}

static int usb_asix_mdio_write(int loc, WORD val)
{
    return usb_asix_write_reg(AX_ACCESS_PHY, AX_PHY_ID, (WORD)loc, &val, 2);
}

static int usb_asix_parse_config(BYTE *cfg, unsigned int total)
{
    unsigned int off = 0;
    int have_bulk = 0;
    BYTE last_ep = 0;

    if (!cfg || total < 9)
        return 0;
    usb_asix.cfgval = 1;
    usb_asix.ep_in = 0;
    usb_asix.ep_out = 0;
    usb_asix.mps_in = 64;
    usb_asix.mps_out = 64;
    usb_asix.burst_in = 0;
    usb_asix.burst_out = 0;
    while (off + 2 <= total) {
        int len = cfg[off];
        int type = cfg[off + 1];
        if (len < 2 || off + (unsigned int)len > total)
            break;
        if (type == 2 && len >= 9 && off == 0) {
            usb_asix.cfgval = cfg[5] ? cfg[5] : 1;
        } else if (type == 5 && len >= 7) {
            int addr = cfg[off + 2];
            int attr = cfg[off + 3];
            int mps = cfg[off + 4] | (cfg[off + 5] << 8);
            if ((attr & 3) == 2) {
                if (addr & 0x80) {
                    usb_asix.ep_in = (BYTE)(addr & 0x0F);
                    usb_asix.mps_in = (WORD)(mps ? mps : 64);
                } else {
                    usb_asix.ep_out = (BYTE)(addr & 0x0F);
                    usb_asix.mps_out = (WORD)(mps ? mps : 64);
                }
                have_bulk = 1;
                last_ep = (BYTE)addr;
            } else {
                last_ep = 0;
            }
        } else if (type == 48 && len >= 6 && last_ep) {
            if (cfg[off + 2] > 15)
                last_ep = 0;
            else if (last_ep & 0x80)
                usb_asix.burst_in = cfg[off + 2];
            else
                usb_asix.burst_out = cfg[off + 2];
            last_ep = 0;
        } else {
            last_ep = 0;
        }
        off += (unsigned int)len;
    }
    return have_bulk && usb_asix.ep_in && usb_asix.ep_out;
}

static int usb_asix_reset(void)
{
    BYTE buf[8];
    BYTE one;
    WORD two;

    /* Power up + release PHY reset. */
    two = 0;
    if (!usb_asix_mac_write(AX_PHYPWR_RSTCTL, &two, 2))
        return 0;
    two = AX_PHYPWR_RSTCTL_IPRL;
    if (!usb_asix_mac_write(AX_PHYPWR_RSTCTL, &two, 2))
        return 0;
    usb_wait_ms(200);

    /* 25 MHz crystal + 125 MHz PHY clock. */
    one = (BYTE)(AX_CLK_SELECT_ACS | AX_CLK_SELECT_BCS);
    if (!usb_asix_mac_write(AX_CLK_SELECT, &one, 1))
        return 0;
    usb_wait_ms(100);

    /* Read the MAC burned in the ASIX EEPROM; fall back to a local one. */
    if (!usb_asix_mac_read(AX_NODE_ID, usb_asix.mac, ETH_ADDR_LEN) ||
        (usb_asix.mac[0] == 0 && usb_asix.mac[1] == 0 &&
         usb_asix.mac[2] == 0 && usb_asix.mac[3] == 0 &&
         usb_asix.mac[4] == 0 && usb_asix.mac[5] == 0)) {
        usb_asix.mac[0] = 0x02;
        usb_asix.mac[1] = 0x00;
        usb_asix.mac[2] = 0x0A;
        usb_asix.mac[3] = (BYTE)(usb_asix.dev + 1);
        usb_asix.mac[4] = 0x51;
        usb_asix.mac[5] = (BYTE)(usb_asix.dev + 0x79);
        printf("usb: asix MAC read failed; using fallback MAC\n");
    }
    /* Program the resolved MAC into the NIC so its unicast filter and TX
       source address match what the driver reports. */
    if (!usb_asix_mac_write(AX_NODE_ID, usb_asix.mac, ETH_ADDR_LEN))
        return 0;

    /* RX bulk queue configuration (gigabit / SuperSpeed entry). */
    if (!usb_asix_mac_write(AX_RX_BULKIN_QCTRL, ax_bulk_in_size[0], 5))
        return 0;

    one = 0x34;
    usb_asix_mac_write(AX_PAUSE_WATERLVL_LOW, &one, 1);
    one = 0x52;
    usb_asix_mac_write(AX_PAUSE_WATERLVL_HIGH, &one, 1);

    /* Checksum offload. */
    one = (BYTE)(AX_RXCOE_IP | AX_RXCOE_TCP | AX_RXCOE_UDP | AX_RXCOE_TCPV6 |
                 AX_RXCOE_UDPV6);
    usb_asix_mac_write(AX_RXCOE_CTL, &one, 1);
    usb_asix_mac_write(AX_TXCOE_CTL, &one, 1);

    /* Start RX: drop CRC errors, accept unicast+multicast+broadcast. */
    two = (WORD)(AX_RX_CTL_DROPCRCERR | AX_RX_CTL_IPE | AX_RX_CTL_START |
                 AX_RX_CTL_AP | AX_RX_CTL_AMALL | AX_RX_CTL_AB);
    if (!usb_asix_mac_write(AX_RX_CTL, &two, 2))
        return 0;

    one = (BYTE)(AX_MONITOR_MODE_PMEPOL | AX_MONITOR_MODE_PMETYPE |
                 AX_MONITOR_MODE_RWMP);
    usb_asix_mac_write(AX_MONITOR_MOD, &one, 1);

    /* Default medium: gigabit full-duplex, flow control, RX enabled. */
    two = (WORD)(AX_MEDIUM_RECEIVE_EN | AX_MEDIUM_TXFLOW_CTRLEN |
                 AX_MEDIUM_RXFLOW_CTRLEN | AX_MEDIUM_FULL_DUPLEX |
                 AX_MEDIUM_GIGAMODE);
    usb_asix_mac_write(AX_MEDIUM_STATUS_MODE, &two, 2);
    (void)buf;
    return 1;
}

static int usb_asix_check_link(void)
{
    WORD physr = 0;
    BYTE linksts = 0;
    WORD mode;
    BYTE tmp[5];

    if (!usb_asix.ready)
        return usb_asix.link;
    if (usb_asix_mdio_read(GMII_PHY_PHYSR, &physr) < 0)
        return usb_asix.link;
    if (!(physr & GMII_PHY_PHYSR_LINK)) {
        usb_asix.link = 0;
        return 0;
    }
    usb_asix.link = 1;
    (void)usb_asix_mac_read(AX_PHYSICAL_LINK_STAT, &linksts, 1);
    mode = (WORD)(AX_MEDIUM_RECEIVE_EN | AX_MEDIUM_TXFLOW_CTRLEN |
                  AX_MEDIUM_RXFLOW_CTRLEN);
    if (GMII_PHY_PHYSR_GIGA == (physr & GMII_PHY_PHYSR_SMASK)) {
        mode |= (WORD)(AX_MEDIUM_GIGAMODE | AX_MEDIUM_EN_125MHZ);
        if (linksts & AX_USB_SS)
            memcpy(tmp, ax_bulk_in_size[0], 5);
        else if (linksts & AX_USB_HS)
            memcpy(tmp, ax_bulk_in_size[1], 5);
        else
            memcpy(tmp, ax_bulk_in_size[3], 5);
    } else if (GMII_PHY_PHYSR_100 == (physr & GMII_PHY_PHYSR_SMASK)) {
        mode |= AX_MEDIUM_PS;
        if (linksts & (AX_USB_SS | AX_USB_HS))
            memcpy(tmp, ax_bulk_in_size[2], 5);
        else
            memcpy(tmp, ax_bulk_in_size[3], 5);
    } else {
        memcpy(tmp, ax_bulk_in_size[3], 5);
    }
    usb_asix_mac_write(AX_RX_BULKIN_QCTRL, tmp, 5);
    if (physr & GMII_PHY_PHYSR_FULL)
        mode |= AX_MEDIUM_FULL_DUPLEX;
    usb_asix_mac_write(AX_MEDIUM_STATUS_MODE, &mode, 2);
    return 1;
}

static void usb_asix_get_mac(void *drv, unsigned char mac[ETH_ADDR_LEN])
{
    int i;
    (void)drv;
    for (i = 0; i < ETH_ADDR_LEN; i++)
        mac[i] = usb_asix.mac[i];
}

static int usb_asix_link_up(void *drv)
{
    (void)drv;
    return usb_asix.ready && usb_asix.link ? 1 : 0;
}

static int usb_asix_transmit(void *drv, struct pbuf *p)
{
    DWORD len;
    (void)drv;
    if (!usb_asix.ready || !p || !usb_asix.txbuf || p->len < 1 ||
        p->len > ETH_MAX_FRAME)
        return -1;
    len = (DWORD)p->len;
    asix_tx_header(len, usb_asix.mps_out, usb_asix.txbuf);
    memcpy(usb_asix.txbuf + USB_ASIX_TXHDR_SZ, p->data, len);
    usb_io_lock_acquire();
    if (!xhci_bulk(usb_xhci_hcd, usb_asix.dev, usb_asix.ep_out, 0,
                   usb_asix.txbuf, (int)(USB_ASIX_TXHDR_SZ + len))) {
        usb_io_lock_release();
        return -1;
    }
    usb_io_lock_release();
    return 0;
}

static void usb_asix_rx_one(void *drv)
{
    int got = 0;
    asix_rx_iter it;
    asix_rx_frame f;
    (void)drv;
    if (!usb_asix.ready || !usb_asix.rxbuf)
        return;
    usb_io_lock_acquire();
    if (!xhci_bulk_in_try(usb_xhci_hcd, usb_asix.dev, usb_asix.ep_in,
                          usb_asix.rxbuf, USB_ASIX_RXBUF_SZ, &got,
                          XHCI_ECM_IN_SPINS) ||
        got < 4 || got > USB_ASIX_RXBUF_SZ) {
        usb_io_lock_release();
        return;
    }
    usb_io_lock_release();
    asix_rx_iter_init(&it, usb_asix.rxbuf, (unsigned int)got);
    while (asix_rx_iter_next(&it, &f)) {
        struct pbuf *p = pbuf_alloc((u16)f.len);
        if (!p)
            continue;
        /* Each bundled frame is 2 IP-align bytes + the Ethernet frame. */
        memcpy(p->data, usb_asix.rxbuf + f.off, f.len);
        net_lock();
        netif_input(&usb_asix.nif, p);
        net_unlock();
    }
}

static const struct netdev_ops usb_asix_ops = {
    .transmit = usb_asix_transmit,
    .link_up = usb_asix_link_up,
    .poll = usb_asix_rx_one,
    .get_mac = usb_asix_get_mac,
};

void usb_asix_poll(void)
{
    extern DWORD time_count;
    DWORD now = time_count;
    if (!usb_asix.ready)
        return;
    if (now - usb_asix.last_link_secs >= 2) {
        usb_asix.last_link_secs = now;
        usb_asix.link = usb_asix_check_link();
        usb_asix.nif.link_up = usb_asix.link ? 1 : 0;
    }
    usb_asix_rx_one(&usb_asix);
}

static int usb_enumerate_asix(DWORD dev)
{
    BYTE devdesc[18];
    BYTE cfghdr[9];
    BYTE cfg[256];
    WORD total;
    DWORD vid, pid;

    if (!usb_get_desc_dev(dev, USB_DESC_DEVICE, 0, devdesc, 8))
        return 0;
    if (!xhci_set_ep0_packet_size(usb_xhci_hcd, dev, devdesc[7]))
        return 0;
    if (!usb_get_desc_dev(dev, USB_DESC_DEVICE, 0, devdesc, 18))
        return 0;
    vid = (DWORD)devdesc[8] | ((DWORD)devdesc[9] << 8);
    pid = (DWORD)devdesc[10] | ((DWORD)devdesc[11] << 8);
    if (vid != USB_ASIX_VID || pid != USB_ASIX_PID) {
        printf("usb: port is not ASIX AX88179 (vid=%04x pid=%04x)\n",
               (unsigned)vid, (unsigned)pid);
        return 0;
    }
    printf("usb: asix vid=%04x pid=%04x\n", (unsigned)vid, (unsigned)pid);
    if (!usb_get_desc_dev(dev, USB_DESC_CONFIG, 0, cfghdr, 9))
        return 0;
    total = (WORD)(cfghdr[2] | (cfghdr[3] << 8));
    if (total < 9 || total > (WORD)sizeof(cfg))
        total = 9;
    if (!usb_get_desc_dev(dev, USB_DESC_CONFIG, 0, cfg, total))
        return 0;
    if (!usb_asix_parse_config(cfg, total)) {
        printf("usb: no ASIX bulk endpoints in config\n");
        return 0;
    }
    if (!usb_set_config_dev(dev, usb_asix.cfgval))
        return 0;
    if (!xhci_configure_endpoints(usb_xhci_hcd, dev, usb_asix.ep_in,
                                  usb_asix.mps_in, usb_asix.burst_in,
                                  usb_asix.ep_out, usb_asix.mps_out,
                                  usb_asix.burst_out))
        return 0;
    usb_asix.dev = dev;
    /* Let the bridge firmware settle after SET_CONFIGURATION + EP config
       before the first vendor register access. */
    usb_wait_ms(100);
    if (!usb_asix_reset()) {
        printf("usb: asix reset/init failed\n");
        return 0;
    }
    usb_asix.link = usb_asix_check_link();
    printf("usb: asix ready ep_in=%u ep_out=%u mps=%u/%u link=%d\n",
           (unsigned)usb_asix.ep_in, (unsigned)usb_asix.ep_out,
           (unsigned)usb_asix.mps_in, (unsigned)usb_asix.mps_out,
           usb_asix.link);
    return 1;
}

static int usb_xhci_bind_asix(void)
{
    extern DWORD time_count;
    DWORD used_ports = 0;
    DWORD dev_slot = 0;
    DWORD port;
    int i;

    if (!usb_xhci_hcd || usb_asix.ready)
        return 0;
    for (i = 0; i < XHCI_MAX_USBDEVS; i++)
        if (usb_xhci_hcd->usbdevs[i].port)
            used_ports |= xhci_ccs_bit(usb_xhci_hcd->usbdevs[i].port);
    for (dev_slot = 2; dev_slot < XHCI_MAX_USBDEVS; dev_slot++) {
        if (!usb_xhci_hcd->usbdevs[dev_slot].port)
            break;
    }
    if (dev_slot >= XHCI_MAX_USBDEVS)
        dev_slot = 0;
    if (!dev_slot) {
        printf("usb: no free xHCI slot for ASIX NIC\n");
        return 0;
    }
    usb_xhci_hcd->enumerating = 1;
    usb_xhci_hcd->recovery_needed = 0;
    for (port = 1; port <= usb_xhci_hcd->max_ports && port <= 32; port++) {
        if (!(xhci_port_ccs_mask(usb_xhci_hcd) & xhci_ccs_bit(port)))
            continue;
        if (used_ports & xhci_ccs_bit(port))
            continue;
        printf("xhci: trying ASIX port %u (slot %u)\n", port, dev_slot);
        if (!xhci_claim_port_dev(usb_xhci_hcd, port, dev_slot))
            continue;
        if (usb_enumerate_asix(dev_slot)) {
            if (!usb_asix.txbuf)
                usb_asix.txbuf = (BYTE *)malloc(USB_ASIX_TXBUF_SZ);
            if (!usb_asix.rxbuf)
                usb_asix.rxbuf = (BYTE *)malloc(USB_ASIX_RXBUF_SZ);
            if (!usb_asix.txbuf || !usb_asix.rxbuf) {
                printf("xhci: no DMA buffer for ASIX NIC\n");
                xhci_release_dev(usb_xhci_hcd, dev_slot);
                usb_xhci_hcd->recovery_needed = 0;
                continue;
            }
            usb_asix.port = port;
            usb_asix.last_link_secs = time_count;
            usb_asix.ndev.drv = &usb_asix;
            usb_asix.ndev.ops = &usb_asix_ops;
            usb_asix.ndev.name[0] = 'a';
            usb_asix.ndev.name[1] = 'x';
            usb_asix.ndev.name[2] = '0';
            usb_asix.ndev.name[3] = 0;
            netif_init(&usb_asix.nif, &usb_asix.ndev);
            if (!netif_default())
                netif_set_default(&usb_asix.nif);
            usb_asix.ready = 1;
            usb_registry_add((int)dev_slot, port, (int)dev_slot,
                             USB_DRV_ASIX, &usb_asix, "usb-asix");
            usb_xhci_hcd->enumerating = 0;
            serial_puts("ASIX_NIC_OK\n");
            return 1;
        }
        xhci_release_dev(usb_xhci_hcd, dev_slot);
        usb_xhci_hcd->recovery_needed = 0;
    }
    usb_xhci_hcd->enumerating = 0;
    return 0;
}

static void usb_asix_tear_down(void)
{
    WORD two = 0;
    if (!usb_asix.ready)
        return;
    (void)usb_asix_mac_write(AX_RX_CTL, &two, 2);
    usb_asix.ready = 0;
    usb_asix.nif.link_up = 0;
}

#ifdef KTEST
/* In-kernel real-LAN acceptance self-test (KTEST builds only).
   Boot cmdline keys:
     asix-peer=A.B.C.D  host/peer to ping + run the echo servers on
     asix-gw=A.B.C.D    optional gateway override (else DHCP gateway)
     asix-udp=PORT      UDP echo port (default 20001)
     asix-tcp=PORT      TCP echo port (default 20002) */
static int asix_cmdline_u32(const char *key, unsigned int def)
{
    const char *p;
    unsigned int v = 0;
    int digits = 0;

    p = strstr(kernel_cmdline, key);
    if (!p)
        return (int)def;
    p += (unsigned int)strlen(key);
    while (*p >= '0' && *p <= '9') {
        v = v * 10u + (unsigned int)(*p - '0');
        p++;
        digits = 1;
    }
    return digits ? (int)v : (int)def;
}

static int asix_cmdline_ip(const char *key, unsigned int *out)
{
    const char *p;
    unsigned int a = 0, b = 0, c = 0, d = 0;
    int da = 0, db = 0, dc = 0, dd = 0;

    p = strstr(kernel_cmdline, key);
    if (!p)
        return 0;
    p += (unsigned int)strlen(key);
    while (*p >= '0' && *p <= '9') { a = a * 10u + (*p - '0'); p++; da = 1; }
    if (*p != '.')
        return 0;
    p++;
    while (*p >= '0' && *p <= '9') { b = b * 10u + (*p - '0'); p++; db = 1; }
    if (*p != '.')
        return 0;
    p++;
    while (*p >= '0' && *p <= '9') { c = c * 10u + (*p - '0'); p++; dc = 1; }
    if (*p != '.')
        return 0;
    p++;
    while (*p >= '0' && *p <= '9') { d = d * 10u + (*p - '0'); p++; dd = 1; }
    if (!da || !db || !dc || !dd)
        return 0;
    *out = (a << 24) | (b << 16) | (c << 8) | d;
    return 1;
}

static void usb_asix_selftest(void)
{
    struct inet_config cfg;
    unsigned int peer = 0, gw = 0;
    unsigned int udp_port = 20001, tcp_port = 20002;
    int have_peer = 0, have_gw = 0, dhcp_ok = 0;
    int pass = 1;

    if (!usb_asix.ready) {
        printf("ASIX_NET_FAIL not-ready\n");
        return;
    }
    printf("ASIX_START\n");
    inet_config_from_cmdline(&cfg, kernel_cmdline);
    if (asix_cmdline_ip("asix-peer=", &peer))
        have_peer = 1;
    if (asix_cmdline_ip("asix-gw=", &gw))
        have_gw = 1;
    udp_port = (unsigned int)asix_cmdline_u32("asix-udp=", 20001);
    tcp_port = (unsigned int)asix_cmdline_u32("asix-tcp=", 20002);

    arp_init(&usb_asix.nif);
    tcp_init();
    usb_asix.nif.link_up = 1;
    if (dhcp_client(&usb_asix.nif, &cfg, 8000000) == 0) {
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
    netif_set_addr(&usb_asix.nif, cfg.ip, cfg.netmask, cfg.gateway);
    netif_set_up(&usb_asix.nif);
    if (have_gw)
        usb_asix.nif.gateway = gw;
    printf("NETIF_UP ip=%u.%u.%u.%u gw=%u.%u.%u.%u dhcp=%d peer=%d\n",
           (cfg.ip >> 24) & 0xFF, (cfg.ip >> 16) & 0xFF,
           (cfg.ip >> 8) & 0xFF, cfg.ip & 0xFF,
           (usb_asix.nif.gateway >> 24) & 0xFF,
           (usb_asix.nif.gateway >> 16) & 0xFF,
           (usb_asix.nif.gateway >> 8) & 0xFF, usb_asix.nif.gateway & 0xFF,
           dhcp_ok, have_peer);

    /* Primary ping target is the host peer (always answers ICMP); the DHCP
       gateway is best-effort (routers often block ICMP). */
    if (have_peer) {
        if (icmp_ping(&usb_asix.nif, peer, 300) == 0)
            printf("NET_PING_OK peer\n");
        else {
            printf("NET_PING_FAIL peer\n");
            pass = 0;
        }
    } else if (icmp_ping(&usb_asix.nif, cfg.gateway, 300) == 0) {
        printf("NET_PING_OK gw\n");
    } else {
        printf("NET_PING_FAIL gw\n");
        pass = 0;
    }

    if (have_peer) {
        if (udp_echo_client(&usb_asix.nif, peer, (unsigned short)udp_port,
                            4000000) == 0)
            printf("NET_UDP_OK\n");
        else {
            printf("NET_UDP_FAIL\n");
            pass = 0;
        }
        if (tcp_echo_client(&usb_asix.nif, peer, (unsigned short)tcp_port,
                            8000000) == 0)
            printf("NET_TCP_OK\n");
        else {
            printf("NET_TCP_FAIL\n");
            pass = 0;
        }
    } else {
        printf("ASIX_NET_FAIL no-peer\n");
        pass = 0;
    }

    softnet_init();
    printf("NET_SOFTNET_OK\n");
    if (pass && dhcp_ok && have_peer)
        printf("ASIX_NET_PASS\n");
    else
        printf("ASIX_NET_FAIL\n");
}
#endif /* KTEST: usb_asix_selftest */

 static int usb_xhci_recover(void)
{
    usb_xhci_recovering = 1;
    usb_cdc_reset_state();
    usb_ecm_tear_down();
    usb_rndis_tear_down();
    usb_asix_tear_down();
    xhci_stop_hcd(usb_xhci_hcd);
    usb_xhci_hcd->recovery_needed = 0;
    if (!xhci_init_hcd(usb_xhci_hcd) || !usb_xhci_bind_msc()) {
        printf("xhci: controller recovery failed\n");
        xhci_stop_hcd(usb_xhci_hcd);
        usb_xhci_hcd->recovery_needed = 1;
        usb_msc0.drive.present = 0;
        usb_invalidate_storage_cache();
        usb_xhci_recovering = 0;
        usb_cdc_reset_state();
        usb_ecm_tear_down();
        usb_rndis_tear_down();
        usb_asix_tear_down();
        return 0;
    }
    usb_xhci_bind_ecm();
    usb_xhci_bind_rndis();
    usb_xhci_bind_asix();
    usb_xhci_bind_cdc();
    usb_xhci_recovery_count++;
    usb_xhci_hcd->recovery_needed = 0;
    usb_msc0.drive.present = 1;
    usb_xhci_recovering = 0;
    printf("xhci: controller recovery complete count=%d\n",
           usb_xhci_recovery_count);
    return 1;
}

static int usb_xhci_reconnect(void)
{
    usb_media_identity replacement_identity;
    u64 old_blocks = usb_msc0.drive.total_blocks;
    DWORD old_block_size = usb_msc0.drive.block_size;
    /* "First attach ever" is signalled by no previously-enumerated media,
       not by publish state: the reconnect self-tests enumerate the device
       without publishing it, yet must still verify the replacement. */
    int initial_attach = (old_blocks == 0);
    usb_xhci_recovering = 1;
    usb_cdc_reset_state();
    usb_ecm_tear_down();
    usb_rndis_tear_down();
    usb_asix_tear_down();
    xhci_stop_hcd(usb_xhci_hcd);
    usb_xhci_hcd->recovery_needed = 0;
    if (!xhci_init_hcd(usb_xhci_hcd) || !usb_xhci_bind_msc())
        goto fail;
    usb_xhci_bind_ecm();
    usb_xhci_bind_rndis();
    usb_xhci_bind_asix();
    usb_xhci_bind_cdc();
    if (!initial_attach) {
        if (usb_msc0.drive.total_blocks != old_blocks ||
            usb_msc0.drive.block_size != old_block_size)
            goto fail;
        if (!usb_msc0.expected_identity.valid ||
            !usb_capture_media_identity(&usb_msc0, &replacement_identity) ||
            !usb_media_identity_equal(&usb_msc0.expected_identity,
                                      &replacement_identity)) {
            printf("usb: replacement volume identity mismatch\n");
            goto fail;
        }
    }
    usb_msc0.drive.present = 1;
    usb_xhci_recovering = 0;
    printf("xhci: device re-enumerated\n");
    return 1;
fail:
    printf("xhci: device reconnect failed\n");
    xhci_stop_hcd(usb_xhci_hcd);
    usb_msc0.drive.total_blocks = old_blocks;
    usb_msc0.drive.block_size = old_block_size;
    usb_msc0.drive.present = 0;
    usb_xhci_recovering = 0;
    usb_ecm_tear_down();
    usb_rndis_tear_down();
    usb_asix_tear_down();
    return 0;
}

static void usb_xhci_hotplug_monitor(void)
{
    int attach_samples = 0;
    int reconnect_blocked = 0;
    int offline_reported = !usb_msc0.media_established;
    for (;;) {
        usb_cdc_pump();
        delay(xhci_cdc_hotplug_delay(usb_cdc_ready));
        if (usb_host != USB_HOST_XHCI)
            continue;
        if (usb_msc0.drive.present) {
            attach_samples = 0;
            reconnect_blocked = 0;
            offline_reported = 0;
            if (!xhci_usbdev_connected(usb_xhci_hcd, 0) &&
                !usb_io_lock.locked &&
                __sync_bool_compare_and_swap(&usb_hotplug_transition,0,1)) {
                if (!usb_io_lock.locked && usb_msc0.drive.present &&
                    !xhci_usbdev_connected(usb_xhci_hcd, 0)) {
                    usb_xhci_disconnect_offline();
                }
                __sync_lock_release(&usb_hotplug_transition);
            }
            continue;
        }
        if (!offline_reported) {
            serial_puts("XHCI_HOTPLUG_DISCONNECT_OK\n");
            offline_reported = 1;
        }
        if (!xhci_device_attached(usb_xhci_hcd)) {
            attach_samples = 0;
            reconnect_blocked = 0;
            continue;
        }
        /* Defer a full MSC reconnect (xhci_stop_hcd + xhci_init_hcd +
           re-enumeration) while the console is mid-ELF-load / MSC bulk I/O:
           the rebind resets the controller the load is reading through and
           wedges the console thread (xhci command-ring/MSC waits). Re-tried
           once the load finishes and quiesce clears. */
        if (usb_cdc_bulk_io_active())
            continue;
        if (reconnect_blocked || ++attach_samples < 3)
            continue;
        attach_samples = 0;
        if (!__sync_bool_compare_and_swap(&usb_hotplug_transition,0,1))
            continue;
        if (usb_xhci_reconnect() && usb_publish_storage_devices(&usb_msc0, 0)) {
            serial_puts("XHCI_HOTPLUG_RECONNECT_OK\n");
        } else {
            if (usb_msc0.drive.present)
                usb_xhci_disconnect_offline();
            reconnect_blocked = 1;
            serial_puts("XHCI_HOTPLUG_RECONNECT_REJECTED\n");
        }
        __sync_lock_release(&usb_hotplug_transition);
    }
}

#ifdef KTEST
/* xHCI fault-injection / recovery / disconnect / multi-MSC self-tests.
   Compiled only into KTEST (dev/test) kernels; stripped from production. */
static int usb_xhci_recovery_selftest(void)
{
    int before = usb_xhci_recovery_count;
    int pass;
    if (!usb_scsi_rw(&usb_msc0, 0, 0, 1, (char *)usb_recovery_before))
        goto fail;
    usb_xhci_hcd->fault_drop_next = 1;
    if (!usb_scsi_rw(&usb_msc0, 0, 0, 1, (char *)usb_recovery_after))
        goto fail;
    if (usb_xhci_recovery_count != before + 1 ||
        memcmp(usb_recovery_before, usb_recovery_after, 512) != 0)
        goto fail;
    usb_xhci_hcd->fault_drop_next = 1;
    if (!usb_scsi_rw(&usb_msc0, 0, 0, 1, (char *)usb_recovery_after) ||
        usb_xhci_recovery_count != before + 2 ||
        memcmp(usb_recovery_before, usb_recovery_after, 512) != 0)
        goto fail;
    usb_msc0.drive.present = 1;
    usb_xhci_hcd->fault_drop_next = 1;
    usb_xhci_hcd->fault_fail_init = 1;
    pass = usb_scsi_rw(&usb_msc0, 0, 0, 1, (char *)usb_recovery_after);
    if (pass || usb_msc0.drive.present ||
        usb_xhci_recovery_count != before + 2)
        goto fail;
    serial_puts("XHCI_RECOVERY_INIT_FAILURE_OK\n");
    if (!usb_xhci_recover() || !usb_msc0.drive.present ||
        usb_xhci_recovery_count != before + 3 ||
        !usb_scsi_rw(&usb_msc0, 0, 0, 1, (char *)usb_recovery_after) ||
        memcmp(usb_recovery_before, usb_recovery_after, 512) != 0)
        goto fail;
    serial_puts("XHCI_RESET_RECOVERY_OK\n");
    return 1;
fail:
    serial_puts("XHCI_RESET_RECOVERY_FAIL\n");
    return 0;
}

static int usb_xhci_stall_recovery_selftest(void)
{
    int controller_before = usb_xhci_recovery_count;
    int stall_before = usb_xhci_stall_recovery_count;
    if (!usb_scsi_rw(&usb_msc0, 0, 0, 1, (char *)usb_recovery_before))
        goto fail;
    usb_fault_invalid_cbw = 1;
    if (!usb_scsi_rw(&usb_msc0, 0, 0, 1, (char *)usb_recovery_after))
        goto fail;
    if (usb_xhci_recovery_count != controller_before ||
        usb_xhci_stall_recovery_count != stall_before + 1 ||
        memcmp(usb_recovery_before, usb_recovery_after, 512) != 0)
        goto fail;
    serial_puts("XHCI_BOT_SELECTIVE_RECOVERY_OK\n");
    usb_fault_invalid_cbw = 1;
    usb_fault_drop_stall_retry = 1;
    if (!usb_scsi_rw(&usb_msc0, 0, 0, 1, (char *)usb_recovery_after) ||
        usb_xhci_recovery_count != controller_before + 1 ||
        usb_xhci_stall_recovery_count != stall_before + 2 ||
        memcmp(usb_recovery_before, usb_recovery_after, 512) != 0)
        goto fail;
    serial_puts("XHCI_BOT_STALL_FALLBACK_OK\n");
    serial_puts("XHCI_BOT_STALL_RECOVERY_OK\n");
    return 1;
fail:
    serial_puts("XHCI_BOT_STALL_RECOVERY_FAIL\n");
    return 0;
}

static int usb_xhci_disconnect_selftest(int reconnect, int mismatch)
{
    int identity_mismatch =
        strstr(kernel_cmdline, "xhci-reconnect-identity-mismatch-test") != 0;
    int recovery_before = usb_xhci_recovery_count;
    int waits;
    u64 blocks_before = usb_msc0.drive.total_blocks;
    DWORD block_size_before = usb_msc0.drive.block_size;
    if (reconnect && !usb_msc0.expected_identity.valid &&
        !usb_capture_media_identity(&usb_msc0, &usb_msc0.expected_identity))
        goto fail;
    if (reconnect &&
        !usb_scsi_rw(&usb_msc0, 0, 0, 1, (char *)usb_recovery_before))
        goto fail;
    usb_msc0.drive.present = 1;
    usb_xhci_hcd->fault_disconnect_inflight = 1;
    if (usb_scsi_rw(&usb_msc0, 0, 0, 1, (char *)usb_recovery_after) ||
        usb_msc0.drive.present || !usb_xhci_hcd->connection_lost ||
        usb_xhci_recovery_count != recovery_before)
        goto fail;
    if (usb_read_block_raw(&usb_msc0, 0, (char *)usb_recovery_after, 1) ||
        usb_xhci_recovery_count != recovery_before)
        goto fail;
    if (reconnect) {
        serial_puts("XHCI_RECONNECT_READY\n");
           for (waits = 0; waits < 1000 &&
             !xhci_device_attached(usb_xhci_hcd); waits++)
            usb_wait_ms(10);
        if (!xhci_device_attached(usb_xhci_hcd))
            goto fail;
        if (mismatch || identity_mismatch) {
            if (usb_xhci_reconnect() || usb_msc0.drive.present ||
                usb_msc0.drive.total_blocks != blocks_before ||
                usb_msc0.drive.block_size != block_size_before)
                goto fail;
            if (identity_mismatch)
                serial_puts("XHCI_RECONNECT_IDENTITY_MISMATCH_OK\n");
            else
                serial_puts("XHCI_RECONNECT_MISMATCH_OK\n");
            return 1;
        }
        if (!usb_xhci_reconnect() ||
            !usb_read_block_raw(&usb_msc0, 0, (char *)usb_recovery_after, 1) ||
            memcmp(usb_recovery_before, usb_recovery_after, 512) != 0 ||
            usb_xhci_recovery_count != recovery_before)
            goto fail;
        serial_puts("XHCI_RECONNECT_OK\n");
        return 1;
    }
    serial_puts("XHCI_DISCONNECT_OK\n");
    return 1;
fail:
    serial_puts("XHCI_DISCONNECT_FAIL\n");
    return 0;
}

int usb_xhci_mounted_disconnect_selftest(void)
{
    BYTE cached[512];
    BYTE transfer[512];
    devmgr_block_desc *replacement;
    int last_context;
    int reconnect = strstr(kernel_cmdline, "xhci-mounted-reconnect-test") != 0 ||
                    strstr(kernel_cmdline, "xhci-mounted-remount-test") != 0;
    int waits;
    int partition;
    if (usb_host != USB_HOST_XHCI ||
        partdev_first_child(usb_msc0.drive.deviceid) < 0)
        goto fail;
    partition = partdev_first_child(usb_msc0.drive.deviceid);
    if (!blkcache_read(partition, 0, 1, cached))
        goto fail;
    if (!blkcache_write(partition, 0, 1, cached))
        goto fail;
    usb_disconnect_dirty_pages = 0;
    usb_xhci_hcd->fault_disconnect_inflight = 1;
    if (usb_read_block_raw(&usb_msc0, 0, (char *)transfer, 1) || usb_msc0.drive.present ||
        usb_disconnect_dirty_pages == 0 ||
        blkcache_read(partition, 0, 1, cached) ||
        devmgr_finddevice("usb0") != -1 ||
        devmgr_finddevice("usb0p0") != -1)
        goto fail;
    if (reconnect) {
        serial_puts("XHCI_RECONNECT_READY\n");
           for (waits = 0; waits < 1000 &&
             !xhci_device_attached(usb_xhci_hcd); waits++)
            usb_wait_ms(10);
        if (!xhci_device_attached(usb_xhci_hcd) ||
            !usb_xhci_reconnect() ||
            !usb_publish_storage_devices(&usb_msc0, 0))
            goto fail;
        last_context = devmgr_getcontext();
        devmgr_setcontext(partition);
        waits = partdev_partition_read(partition, 0, (char *)transfer, 1);
        devmgr_setcontext(last_context);
        if (waits || devmgr_finddevice("usb0") < 0 ||
            devmgr_finddevice("usb0p0") < 0 ||
            devmgr_finddevice("usb0p0") == partition)
            goto fail;
        replacement = (devmgr_block_desc*)devmgr_getdevice_ref(
            devmgr_finddevice("usb0p0"));
        if (replacement == (devmgr_block_desc*)-1)
            goto fail;
        waits = bridges_call((devmgr_generic*)replacement,
                             &replacement->read_block, 0, transfer, 1);
        devmgr_putdevice((devmgr_generic*)replacement);
        if (!waits)
            goto fail;
        serial_puts("XHCI_MOUNTED_RECONNECT_GENERATION_OK\n");
        return 1;
    }
    serial_puts("XHCI_MOUNTED_DISCONNECT_CACHE_OK\n");
    return 1;
fail:
    serial_puts("XCHI_MOUNTED_DISCONNECT_CACHE_FAIL\n");
    return 0;
}
#endif /* KTEST: end xHCI recovery/stall/disconnect/mounted self-tests */

/* Production: locate the UHCI host controller (class 0C/03, prog-if 00). */
static int uhci_find_controller(BYTE *bus, BYTE *slot, BYTE *func)
{
    BYTE b, s, f;
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
                BYTE baseclass = (BYTE)(classreg >> 24);
                BYTE subclass = (BYTE)(classreg >> 16);
                BYTE progif = (BYTE)(classreg >> 8);
                if (baseclass == 0x0C && subclass == 0x03 && progif == 0x00) {
                    *bus = b;
                    *slot = s;
                    *func = f;
                    return 1;
                }
            }
        }
    }
    return 0;
}

static int usb_xhci_probe_msc(void)
{
    DWORD index;
    DWORD count = xhci_discover_hcds();
    for (index = 0; index < count; index++) {
        usb_xhci_hcd = xhci_hcds[index];
        printf("usb: probing xHCI hcd=%u\n", index);
        if (xhci_init_hcd(usb_xhci_hcd) && usb_xhci_bind_msc()) {
            printf("usb: selected xHCI hcd=%u\n", index);
            return 1;
        }
        xhci_stop_hcd(usb_xhci_hcd);
    }
    usb_xhci_hcd = &xhci_primary_hcd;
    return 0;
}

#ifdef KTEST
/* Phase 2 two-disk gate (cmdline: xhci-multi-msc-test). Confirms the primary
   (usb0) and a secondary (usb1) are both bound as independent MSC drives:
   each is readable at block 0, their media identities differ (two real disks,
   not one bound twice), and a raw scratch write to the secondary survives a
   read-back (the host reads the same LBA from the image file to prove the
   write went through the USB MSC, not an in-memory bounce). Reports via
   serial markers without aborting the boot (the primary is the live root). */
#define USB_MULTI_MSC_TEST_BLOCK 512

static void usb_xhci_multi_msc_selftest(void)
{
    usb_msc *sec = (usb_secondary_count > 0) ? usb_secondary[0] : 0;
    usb_media_identity id0, id1;
    BYTE wr[512], rb[512];
    int i;

    if (usb_host != USB_HOST_XHCI) {
        serial_puts("XHCI_MULTI_MSC_FAIL not-xhci\n");
        return;
    }
    if (!usb_msc0.drive.present || usb_msc0.drive.deviceid < 0) {
        serial_puts("XHCI_MULTI_MSC_FAIL no-primary\n");
        return;
    }
    if (!sec || !sec->drive.present || sec->drive.deviceid < 0) {
        serial_puts("XHCI_MULTI_MSC_FAIL no-secondary\n");
        return;
    }
    if (!usb_read_block_raw(&usb_msc0, 0, (char *)wr, 1)) {
        serial_puts("XHCI_MULTI_MSC_FAIL primary-read0\n");
        return;
    }
    if (!usb_read_block_raw(sec, 0, (char *)rb, 1)) {
        serial_puts("XHCI_MULTI_MSC_FAIL secondary-read0\n");
        return;
    }
    if (!usb_capture_media_identity(&usb_msc0, &id0) ||
        !usb_capture_media_identity(sec, &id1) ||
        usb_media_identity_equal(&id0, &id1)) {
        serial_puts("XHCI_MULTI_MSC_FAIL same-identity\n");
        return;
    }
    memset(wr, 0xA5, sizeof(wr));
    wr[0] = 'I';
    wr[1] = 'C';
    wr[2] = 'S';
    wr[3] = 'M';
    if (!usb_write_block_raw(sec, USB_MULTI_MSC_TEST_BLOCK, (char *)wr, 1)) {
        serial_puts("XHCI_MULTI_MSC_FAIL secondary-write\n");
        return;
    }
    memset(rb, 0, sizeof(rb));
    if (!usb_read_block_raw(sec, USB_MULTI_MSC_TEST_BLOCK, (char *)rb, 1)) {
        serial_puts("XHCI_MULTI_MSC_FAIL secondary-reread\n");
        return;
    }
    for (i = 0; i < 512; i++) {
        if (rb[i] != wr[i]) {
            serial_puts("XHCI_MULTI_MSC_FAIL secondary-pattern\n");
            return;
        }
    }
    serial_puts("XHCI_MULTI_MSC_OK primary=usb0 secondary=usb1 block=512\n");
}
#endif /* KTEST: xHCI recovery/disconnect/multi-MSC self-tests */

int usb_init(void)
{
    BYTE bus, slot, func;
    DWORD bar;
    int port;
    int found_dev = 0;

    kbd_boot_leds_raw(KBD_LED_NUM);
    memset(&usb_msc0, 0, sizeof(usb_msc0));
    usb_msc0.tag = 1;
    usb_msc0.ep_in_mps = 64;
    usb_msc0.ep_out_mps = 64;
    usb_msc0.drive.deviceid = -1;
    memset(usb_devices, 0, sizeof(usb_devices));
    usb_cdc_dev = USB_CDC_DEV;

    if (!uhci_find_controller(&bus, &slot, &func)) {
        printf("usb: no UHCI controller found; probing xHCI\n");
        usb_host = USB_HOST_XHCI;
        if (!usb_xhci_probe_msc()) {
            printf("usb: no xHCI mass-storage device\n");
            return -1;
        }
#ifdef KTEST
        if (strstr(kernel_cmdline, "xhci-msix-test") &&
            !usb_xhci_hcd->has_msix) {
            serial_puts("XHCI_MSIX_FAIL\n");
            xhci_stop_hcd(usb_xhci_hcd);
            return -1;
        }
        if (strstr(kernel_cmdline, "xhci-poll-test"))
            serial_puts(usb_xhci_hcd->has_msix ? "XHCI_POLL_FAIL\n" :
                                                "XHCI_POLL_OK\n");
        if (strstr(kernel_cmdline, "xhci-recovery-test") &&
            !usb_xhci_recovery_selftest()) {
            xhci_stop_hcd(usb_xhci_hcd);
            return -1;
        }
        if (strstr(kernel_cmdline, "xhci-stall-recovery-test") &&
            !usb_xhci_stall_recovery_selftest()) {
            xhci_stop_hcd(usb_xhci_hcd);
            return -1;
        }
        if (strstr(kernel_cmdline, "xhci-disconnect-test") ||
            strstr(kernel_cmdline, "xhci-reconnect")) {
            int reconnect = strstr(kernel_cmdline, "xhci-reconnect") != 0;
            int mismatch = strstr(kernel_cmdline, "xhci-reconnect-mismatch-test") != 0;
            int pass = usb_xhci_disconnect_selftest(reconnect, mismatch);
            xhci_stop_hcd(usb_xhci_hcd);
            return pass ? 0 : -1;
        }
#endif
        found_dev = 1;
        goto register_device;
    }

    usb_host = USB_HOST_UHCI;

    bar = pci_read32(bus, slot, func, 0x20);
    uhci_iobase = (WORD)(bar & 0xFFE0);
    if (!uhci_iobase) {
        printf("usb: UHCI BAR4 is empty\n");
        return -1;
    }

    pci_write16(bus, slot, func, 0x04,
                pci_read16(bus, slot, func, 0x04) | 0x05);
    /* Release USB legacy keyboard/mouse capture if present */
    pci_write16(bus, slot, func, 0xC0, 0x8F00);

    printf("usb: UHCI at PCI %d:%d.%d io=0x%x\n",
           bus, slot, func, uhci_iobase);

    if (!uhci_reset_controller()) {
        printf("usb: controller reset timed out\n");
        return -1;
    }
    uhci_build_schedule();
    if (!uhci_run()) {
        printf("usb: failed to start controller\n");
        return -1;
    }

    for (port = 0; port < 2; port++) {
        if (!uhci_reset_port(port))
            continue;
        printf("usb: device on port %d (%s speed)\n",
               port, usb_lowspeed ? "low" : "full");
        if (usb_lowspeed)
            continue; /* mass storage is full-speed or faster */
        if (usb_enumerate_msc(&usb_msc0, 0)) {
            usb_registry_add(0, 0, 0, USB_DRV_MSC, &usb_msc0, "usb0");
            found_dev = 1;
            break;
        }
    }

    if (!found_dev) {
        printf("usb: no UHCI mass-storage device; probing xHCI\n");
        uhci_stop();
        usb_host = USB_HOST_XHCI;
        if (!usb_xhci_probe_msc()) {
            printf("usb: no xHCI mass-storage device\n");
            return -1;
        }
#ifdef KTEST
        if (strstr(kernel_cmdline, "xhci-msix-test") &&
            !usb_xhci_hcd->has_msix) {
            serial_puts("XHCI_MSIX_FAIL\n");
            xhci_stop_hcd(usb_xhci_hcd);
            return -1;
        }
        if (strstr(kernel_cmdline, "xhci-poll-test"))
            serial_puts(usb_xhci_hcd->has_msix ? "XHCI_POLL_FAIL\n" :
                                                "XHCI_POLL_OK\n");
        if (strstr(kernel_cmdline, "xhci-recovery-test") &&
            !usb_xhci_recovery_selftest()) {
            xhci_stop_hcd(usb_xhci_hcd);
            return -1;
        }
        if (strstr(kernel_cmdline, "xhci-stall-recovery-test") &&
            !usb_xhci_stall_recovery_selftest()) {
            xhci_stop_hcd(usb_xhci_hcd);
            return -1;
        }
        if (strstr(kernel_cmdline, "xhci-disconnect-test") ||
            strstr(kernel_cmdline, "xhci-reconnect")) {
            int reconnect = strstr(kernel_cmdline, "xhci-reconnect") != 0;
            int mismatch = strstr(kernel_cmdline, "xhci-reconnect-mismatch-test") != 0;
            int pass = usb_xhci_disconnect_selftest(reconnect, mismatch);
            xhci_stop_hcd(usb_xhci_hcd);
            return pass ? 0 : -1;
        }
#endif
        found_dev = 1;
    }

register_device:
    usb_msc0.drive.present = 1;
    if (!usb_publish_storage_devices(&usb_msc0, 0))
        return -1;
    /* Multi-MSC: bind every other connected drive as usb1, usb2, ... */
    if (usb_host == USB_HOST_XHCI)
        usb_bind_secondary_msc();
   /* CDC-ECM NIC: claim a free xHCI slot and expose it as a polled netif.
        Bind before the CDC-ACM console so a composite network gadget (QEMU
        usb-net advertises RNDIS as config[0] and CDC-ECM as config[1]) is
        claimed by the NIC, not mis-bound as a serial console. */
    if (usb_host == USB_HOST_XHCI) {
        /* The RNDIS self-test wants the same QEMU usb-net gadget as a RNDIS
           NIC (config value 2), so hold the ECM binder back on that path;
           the RNDIS binder below then claims the free port. */
        if (!strstr(kernel_cmdline, "usb-rndis-test"))
             usb_xhci_bind_ecm();
         usb_xhci_bind_rndis();
         /* ASIX AX88179 (0b95:1790) is a real usb-host passthrough NIC; the
            binder is a no-op unless that exact VID/PID is present, so it never
          steals the QEMU usb-net (RNDIS/ECM) gadget. */
    usb_xhci_bind_asix();
    }
    if (usb_host == USB_HOST_XHCI)
        usb_xhci_bind_cdc();
 #ifdef KTEST
    if (usb_host == USB_HOST_XHCI &&
        strstr(kernel_cmdline, "xhci-multi-msc-test"))
        usb_xhci_multi_msc_selftest();
    if (usb_host == USB_HOST_XHCI &&
        strstr(kernel_cmdline, "usb-ecm-test"))
        usb_ecm_selftest();
  if (usb_host == USB_HOST_XHCI &&
        strstr(kernel_cmdline, "usb-ecm-recover-test"))
        usb_xhci_ecm_recover_selftest();
 if (usb_host == USB_HOST_XHCI &&
        strstr(kernel_cmdline, "usb-rndis-test"))
        usb_rndis_selftest();
    if (usb_host == USB_HOST_XHCI &&
        strstr(kernel_cmdline, "asix-test"))
        usb_asix_selftest();
  #endif
    return 0;
}

int usb_storage_available(void)
{
    return usb_msc0.drive.present;
}

int usb_start_hotplug_monitor(void)
{
    if (usb_host != USB_HOST_XHCI || usb_hotplug_monitor_started)
        return 0;
    if (!createkthread_on_cpu((void*)usb_xhci_hotplug_monitor,
                              "xhci_hotplug",16384,0))
        return -1;
    usb_hotplug_monitor_started = 1;
    serial_puts("XHCI_HOTPLUG_MONITOR_READY\n");
    return 0;
}
