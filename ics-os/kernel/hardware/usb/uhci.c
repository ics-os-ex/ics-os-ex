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
#include "usb_debug.h"
#include "../dma.h"
#include "../keyboard/kbd_boot_leds.h"

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

static volatile DWORD uhci_framelist[1024] __attribute__((aligned(4096)));
static uhci_qh uhci_qh_ctl  __attribute__((aligned(16)));
static uhci_td uhci_tds[USB_MAX_TD] __attribute__((aligned(16)));
static BYTE usb_dma_buf[USB_BULK_MAX] __attribute__((aligned(16)));
static BYTE usb_setup_buf[8] __attribute__((aligned(16)));
static BYTE usb_cbw_buf[32] __attribute__((aligned(16)));
static BYTE usb_csw_buf[16] __attribute__((aligned(16)));

static WORD uhci_iobase = 0;
static int  usb_devaddr = 0;
static int  usb_lowspeed = 0;
static BYTE usb_ep_in = 0;
static BYTE usb_ep_out = 0;
static BYTE usb_msc_interface = 0;
static BYTE usb_toggle_in = 0;
static BYTE usb_toggle_out = 0;
static DWORD usb_tag = 1;
static int usb_host = 0;
static WORD usb_ep_in_mps = 64;
static WORD usb_ep_out_mps = 64;
static BYTE usb_ep_in_burst = 0;
static BYTE usb_ep_out_burst = 0;
static usb_drive_info usb_drive;
static usb_media_identity usb_expected_identity;
static int usb_media_established;
static spinlock_t usb_io_lock;
static int usb_xhci_recovering;
static int usb_xhci_recovery_count;
static int usb_xhci_stall_recovery_count;
static int usb_disconnect_dirty_pages;
static int usb_hotplug_monitor_started;
static volatile int usb_hotplug_transition;
static int usb_fault_invalid_cbw;
static int usb_fault_drop_stall_retry;
static BYTE usb_recovery_before[512];
static BYTE usb_recovery_after[512];

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

static int usb_enumerate_msc(void);
static int usb_xhci_recover(void);
static int usb_publish_storage_devices(void);
static void usb_xhci_hotplug_monitor(void);
static void usb_cdc_after_msc(void);

static int usb_for_each_part(int (*cb)(int deviceid))
{
    int i, sum = 0;
    for (i = 0; i < partdev_count(); i++) {
        const partdev_entry *e = partdev_get(i);
        if (e->parent_deviceid == usb_drive.deviceid && e->mydeviceid >= 0)
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
    if (usb_drive.deviceid >= 0) {
        dirty += blkcache_invalidate_device(usb_drive.deviceid);
        dirty += usb_for_each_part(usb_invalidate_cb);
    }
    usb_disconnect_dirty_pages = dirty;
    if (dirty)
        printf("usb: disconnect discarded %d dirty cache page(s)\n", dirty);
}

static void usb_quiesce_storage_devices(void)
{
    if (usb_drive.deviceid >= 0) {
        devmgr_quiesce_device(usb_drive.deviceid);
        usb_for_each_part(usb_quiesce_cb);
    }
}

static void usb_xhci_disconnect_offline(void)
{
    if (!usb_drive.present)
        return;
    usb_drive.present = 0;
    usb_invalidate_storage_cache();
    usb_quiesce_storage_devices();
    partdev_remove(usb_drive.deviceid);
    usb_drive.deviceid = -1;
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

static int usb_msc_bot_once(BYTE *cdb, int cdb_len, int in, BYTE *data,
                            DWORD len)
{
    usb_cbw cbw;
    usb_csw csw;
    memset(&cbw, 0, sizeof(cbw));
    cbw.sig = CBW_SIG;
    cbw.tag = usb_tag++;
    cbw.data_len = len;
    cbw.flags = in ? 0x80 : 0x00;
    cbw.lun = 0;
    cbw.cb_len = (BYTE)cdb_len;
    memcpy(cbw.cb, cdb, cdb_len);
    memcpy(usb_cbw_buf, &cbw, 31);
    if (usb_fault_invalid_cbw) {
        memset(usb_cbw_buf, 0, 4);
        usb_fault_invalid_cbw = 0;
        printf("xhci: test sending invalid BOT CBW\n");
    }

    if (!usb_bulk(usb_ep_out, 0, usb_cbw_buf, 31, &usb_toggle_out))
        return 0;

    if (len && data) {
        if (in) {
            memset(usb_dma_buf, 0, len > sizeof(usb_dma_buf) ? sizeof(usb_dma_buf) : len);
            if (!usb_bulk(usb_ep_in, 1, usb_dma_buf, (int)len, &usb_toggle_in))
                return 0;
            memcpy(data, usb_dma_buf, len);
        } else {
            memcpy(usb_dma_buf, data, len);
            if (!usb_bulk(usb_ep_out, 0, usb_dma_buf, (int)len, &usb_toggle_out))
                return 0;
        }
    }

    memset(usb_csw_buf, 0, sizeof(usb_csw_buf));
    if (!usb_bulk(usb_ep_in, 1, usb_csw_buf, 13, &usb_toggle_in))
        return 0;
    memcpy(&csw, usb_csw_buf, 13);
    if (csw.sig != CSW_SIG || csw.status != 0)
        return 0;
    return 1;
}

static int usb_clear_endpoint_halt(BYTE endpoint)
{
    usb_setup setup;
    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = 0x02;
    setup.bRequest = USB_REQ_CLEAR_FEATURE;
    setup.wIndex = endpoint;
    return usb_ctrl(&setup, 0, 0);
}

static int usb_xhci_recover_stall(void)
{
    usb_setup setup;
    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = 0x21;
    setup.bRequest = USB_REQ_MSC_RESET;
    setup.wIndex = usb_msc_interface;
    if (!usb_ctrl(&setup, 0, 0) ||
        !usb_clear_endpoint_halt((BYTE)(usb_ep_in | 0x80)) ||
        !usb_clear_endpoint_halt(usb_ep_out) ||
        !xhci_recover_bulk_endpoints(usb_xhci_hcd, 0, usb_ep_in,
                         usb_ep_out)) {
        usb_xhci_hcd->recovery_needed = 1;
        return 0;
    }
    usb_toggle_in = 0;
    usb_toggle_out = 0;
    usb_xhci_stall_recovery_count++;
    printf("xhci: BOT stall recovery complete count=%d\n",
           usb_xhci_stall_recovery_count);
    return 1;
}

static int usb_msc_bot(BYTE *cdb, int cdb_len, int in, BYTE *data, DWORD len)
{
    if (usb_msc_bot_once(cdb, cdb_len, in, data, len))
        return 1;
    if (usb_host == USB_HOST_XHCI && usb_xhci_hcd->connection_lost) {
        usb_xhci_disconnect_offline();
        return 0;
    }
    if (usb_host == USB_HOST_XHCI && usb_xhci_hcd->stalled_endpoints &&
        !usb_xhci_recovering && usb_xhci_recover_stall()) {
        if (usb_fault_drop_stall_retry) {
            usb_fault_drop_stall_retry = 0;
            usb_xhci_hcd->fault_drop_next = 1;
        }
        if (usb_msc_bot_once(cdb, cdb_len, in, data, len))
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
    return usb_msc_bot_once(cdb, cdb_len, in, data, len);
}

static int usb_scsi_ready(void)
{
    BYTE cdb[16];
    int i;
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_TEST_UNIT_READY;
    for (i = 0; i < 8; i++) {
        if (usb_msc_bot(cdb, 12, 1, 0, 0))
            return 1;
        usb_wait_ms(20);
    }
    return 0;
}

static int usb_scsi_inquiry(void)
{
    BYTE cdb[16];
    BYTE inq[36];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_INQUIRY;
    cdb[4] = 36;
    return usb_msc_bot(cdb, 12, 1, inq, 36);
}

static int usb_scsi_capacity(u64 *blocks, DWORD *bsize)
{
    BYTE cdb[16];
    BYTE cap[8];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_READ_CAPACITY;
    if (!usb_msc_bot(cdb, 10, 1, cap, 8))
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

static int usb_scsi_rw(int write, DWORD lba, DWORD nblocks, char *buf)
{
    BYTE cdb[16];
    DWORD done = 0;
    DWORD bsize = usb_drive.block_size ? usb_drive.block_size : 512;
    /* BOT/UHCI TD budget: transfer up to 32KB per SCSI command
       (classic USB MSC optimal chunk). */
    DWORD max_per_cmd = 64;

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
        if (!usb_msc_bot(cdb, 10, write ? 0 : 1,
                         (BYTE*)(buf + done * bsize), n * bsize))
            return 0;
        done += n;
    }
    return 1;
}

static int usb_read_block_raw(u64 block, char *blockbuff, DWORD numblocks)
{
    int result;
    usb_io_lock_acquire();
    result = usb_drive.present
        ? usb_scsi_rw(0, (DWORD)block, numblocks, blockbuff) : 0;
    usb_io_lock_release();
    /* Re-arm CDC IN after MSC (skipped while ELF stream quiesced). */
    usb_cdc_after_msc();
    return result;
}

static int usb_write_block_raw(u64 block, char *blockbuff, DWORD numblocks)
{
    int result;
    usb_io_lock_acquire();
    result = usb_drive.present
        ? usb_scsi_rw(1, (DWORD)block, numblocks, blockbuff) : 0;
    usb_io_lock_release();
    usb_cdc_after_msc();
    return result;
}

static DWORD usb_read_le32(const BYTE *value)
{
    return (DWORD)value[0] | ((DWORD)value[1] << 8) |
           ((DWORD)value[2] << 16) | ((DWORD)value[3] << 24);
}

static int usb_read_identity_blocks(DWORD block, DWORD count, BYTE *buffer)
{
    return usb_scsi_rw(0, block, count, (char *)buffer);
}

static int usb_capture_volume_identity(u64 startlba, u64 sectors,
                                       usb_volume_identity *identity)
{
    BYTE data[1024];
    /* SCSI READ(10) addressing is 32-bit; identity records keep their
       32-bit fields for the same reason. */
    usb_volume_identity_init(identity, (unsigned int)startlba,
                             (unsigned int)sectors);
    if (!usb_read_identity_blocks((DWORD)startlba, 1, data))
        return 0;
    if (usb_volume_identity_from_boot(data, identity))
        return 1;
    if (sectors >= 4 && usb_read_identity_blocks((DWORD)startlba + 2, 2, data) &&
        usb_volume_identity_from_ext4(data, identity))
        return 1;
    if (sectors > 64 && usb_read_identity_blocks((DWORD)startlba + 64, 1, data) &&
        usb_volume_identity_from_iso9660(data, identity))
        return 1;
    return 0;
}

static int usb_capture_media_identity(usb_media_identity *identity)
{
    BYTE mbr[512];
    int i;
    int partitioned = 0;
    memset(identity, 0, sizeof(*identity));
    if (!usb_read_identity_blocks(0, 1, mbr))
        return 0;
    if (mbr[510] == 0x55 && mbr[511] == 0xAA)
        for (i = 0; i < 4; i++)
            if (mbr[446 + i * 16 + 4] != 0)
                partitioned = 1;
    if (!partitioned) {
        if (!usb_capture_volume_identity(0, usb_drive.total_blocks,
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
                !usb_capture_volume_identity(startlba, sectors,
                    &identity->volumes[identity->count]))
                return 0;
            identity->count++;
        }
    }
    identity->valid = identity->count > 0;
    return identity->valid;
}

static int usb_context_is_current(void)
{
    int device_context = devmgr_getcontext();
    if (usb_hotplug_transition)
        return 0;
    if (device_context == usb_drive.deviceid)
        return 1;
    if (usb_drive.deviceid >= 0 &&
        partdev_is_child(device_context, usb_drive.deviceid))
        return 1;
    return 0;
}

static int usb_read_block(u64 block, char *blockbuff, DWORD numblocks)
{
    if (!usb_context_is_current())
        return 0;
    return usb_read_block_raw(block, blockbuff, numblocks);
}

static int usb_write_block(u64 block, char *blockbuff, DWORD numblocks)
{
    if (!usb_context_is_current())
        return 0;
    return usb_write_block_raw(block, blockbuff, numblocks);
}

static u64 usb_total_blocks(void)
{
    if (!usb_context_is_current())
        return 0;
    return usb_drive.total_blocks;
}

static int usb_get_block_size(void)
{
    if (!usb_context_is_current())
        return 0;
    return (int)(usb_drive.block_size ? usb_drive.block_size : 512);
}

static int usb_flush_device(void)
{
    BYTE cdb[16];
    int result;
    if (!usb_context_is_current())
        return -1;
    usb_io_lock_acquire();
    if (!usb_drive.present) {
        usb_io_lock_release();
        return -1;
    }
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_SYNC_CACHE10;
    result = usb_msc_bot(cdb, 10, 0, 0, 0);
    usb_io_lock_release();
    if (!result) {
        printf("usb: SYNCHRONIZE CACHE failed\n");
        return -1;
    }
    printf("usb: cache synchronized\n");
    return 0;
}

/* Raw accessors for the partdev layer. They take the USB I/O queue lock for
   the parent device (matching the pre-partdev partition callbacks) and issue
   the SCSI read/write at the disk LBA already translated by partdev. */
static int usb_part_raw_read(int parent_id, u64 lba, char *buf, DWORD nblocks)
{
    int result;
    blk_mq_lock(parent_id);
    result = usb_read_block_raw(lba, buf, nblocks);
    blk_mq_unlock(parent_id);
    return result;
}

static int usb_part_raw_write(int parent_id, u64 lba, char *buf, DWORD nblocks)
{
    int result;
    blk_mq_lock(parent_id);
    result = usb_write_block_raw(lba, buf, nblocks);
    blk_mq_unlock(parent_id);
    return result;
}

static int usb_gpt_read(unsigned long long lba, void *buf,
                        unsigned int sectors, void *arg)
{
    return usb_read_block_raw(lba, (char *)buf, (DWORD)sectors) ? 1 : 0;
}

static int usb_looks_like_fat(unsigned char *s)
{
    WORD bps = (WORD)(s[11] | (s[12] << 8));
    return ((s[0] == 0xEB || s[0] == 0xE9) &&
            (bps == 512 || bps == 1024 || bps == 2048 || bps == 4096));
}

static void usb_register_gpt(int deviceid, u64 total_blocks)
{
    gpt_disk gpt;
    char guidbuf[40];
    int i;
    /* gpt_parse returns 1 on success (same contract as ide_register_gpt).
       The inverted != 0 check rejected a valid GPT ESP and left usb0
       unpartitioned (test-usb-uefi-gpt / N150 Etcher image). */
    if (!gpt_parse(usb_gpt_read, NULL, total_blocks, &gpt)) {
        printf("GPT_WARN usb0 GPT detected but failed validation; no partitions registered\n");
        return;
    }
    if (gpt.used_backup)
        printf("GPT_WARN usb0 using backup GPT header\n");
    else {
        if (!gpt.primary_header_ok)
            printf("GPT_WARN usb0 primary GPT header failed validation\n");
        if (!gpt.primary_array_ok)
            printf("GPT_WARN usb0 primary GPT entry array failed validation\n");
    }
    partdev_set_disk(deviceid, "usb0", PARTDEV_TABLE_GPT, gpt.disk_guid,
                     gpt.used_backup, gpt.entry_count);
    partdev_format_guid(gpt.disk_guid, guidbuf, sizeof(guidbuf));
    printf("GPT_DETECT usb0 entries=%d diskguid=%s%s\n",
           gpt.entry_count, guidbuf, gpt.used_backup ? " (backup)" : "");
   for (i = 0; i < gpt.entry_count; i++) {
        char name[20];
        const gpt_entry *e = &gpt.entries[i];
        sprintf(name, "usb0p%d", e->index);
        if (partdev_register(deviceid, name, e->type_name,
                             (int)usb_drive.block_size,
                             e->first_lba, e->last_lba + 1,
                             usb_part_raw_read, usb_part_raw_write, 0,
                             e->type_name, e->index,
                             (u64)e->attributes, e->name) < 0) {
            printf("GPT_WARN usb0 partition %d not registered (cap reached)\n",
                   e->index);
            continue;
        }
        printf("usb: registered %s (LBA %u)\n", name, (unsigned)e->first_lba);
    }
}

static void usb_register_mbr(int deviceid)
{
    unsigned char mbr[512];
    partition_mbr *pmbr;
    int i;

    memset(mbr, 0, 512);
    if (!usb_read_block_raw(0, (char *)mbr, 1)) {
        printf("PART_SCAN usb0 MBR read failed\n");
        return;
    }
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        printf("PART_SCAN usb0 bad MBR signature; unpartitioned\n");
        return;
    }
    pmbr = (partition_mbr *)mbr;
    if (usb_looks_like_fat(mbr) && pmbr->tables[0].type == 0 &&
        pmbr->tables[1].type == 0 && pmbr->tables[2].type == 0 &&
        pmbr->tables[3].type == 0) {
        printf("usb: FAT volume on usb0 (no partition table)\n");
        return;
    }
    partdev_set_disk(deviceid, "usb0", PARTDEV_TABLE_MBR,
                     (const unsigned char *)0, 0, 4);
    for (i = 0; i < 4; i++) {
        char name[20];
        char desc[64];
        BYTE ptype = pmbr->tables[i].type;
        if (ptype == 0)
            continue;
        sprintf(name, "usb0p%d", i);
        sprintf(desc, "%s on USB partition %d",
                ide_identify_partition_type(ptype), i);
      if (partdev_register(deviceid,
                              name,
                              desc,
                              (int)usb_drive.block_size,
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
            printf("PART_WARN usb0 MBR entry %d not registered (cap reached)\n",
                   i);
            continue;
        }
        printf("usb: registered %s (LBA %u)\n", name,
               (unsigned)pmbr->tables[i].startlba);
    }
}

static int usb_register_partitions(int deviceid)
{
    u64 total_blocks;
    int kind;

    if (!usb_drive.present || usb_drive.block_size != 512 || deviceid < 0)
        return 0;
    total_blocks = usb_drive.total_blocks;
    kind = gpt_detect(usb_gpt_read, NULL);
    if (kind < 0) {
        printf("PART_SCAN usb0 MBR read failed\n");
        return 0;
    }
    if (kind == 1)
        usb_register_gpt(deviceid, total_blocks);
    else if (kind == 0)
        usb_register_mbr(deviceid);
    else
        printf("PART_SCAN usb0 no partition table (unpartitioned)\n");
    return 1;
}

static int usb_publish_storage_devices(void)
{
    devmgr_block_desc blk;
    if (!usb_expected_identity.valid &&
        !usb_capture_media_identity(&usb_expected_identity)) {
        printf("usb: stable volume identity unavailable\n");
    }
    /* Drop any stale partition metadata keyed on the previous usb0 id before
       (re)publishing; the disconnect path normally already cleared it. */
    partdev_remove(usb_drive.deviceid);
    memset(&blk, 0, sizeof(blk));
    strcpy(blk.hdr.name, "usb0");
    strcpy(blk.hdr.description, usb_host == USB_HOST_XHCI
        ? "USB Mass Storage (xHCI)" : "USB Mass Storage (UHCI)");
    blk.hdr.type = DEVMGR_BLOCK;
    blk.hdr.size = sizeof(blk);
    blk.read_block = usb_read_block;
    blk.write_block = usb_write_block;
    blk.total_blocks = usb_total_blocks;
    blk.flush_device = usb_flush_device;
    blk.get_block_size = usb_get_block_size;
    usb_drive.deviceid = devmgr_register((devmgr_generic*)&blk);
    if (usb_drive.deviceid < 0)
        return 0;
    usb_register_partitions(usb_drive.deviceid);
    usb_media_established = 1;
    printf("usb: registered block device usb0\n");
    return 1;
}

static int usb_parse_config(BYTE *cfg, WORD total)
{
    int off = 0;
    int found = 0;
    BYTE last_ep = 0;
    usb_ep_in = 0;
    usb_ep_out = 0;
    usb_msc_interface = 0;
    usb_ep_in_mps = 64;
    usb_ep_out_mps = 64;
    usb_ep_in_burst = 0;
    usb_ep_out_burst = 0;
    while (off + 2 <= total) {
        BYTE len = cfg[off];
        BYTE type = cfg[off + 1];
        if (len < 2 || off + len > total)
            return 0;
        if (type == USB_DESC_INTERFACE && len >= 9) {
            if (found && usb_ep_in && usb_ep_out)
                return 1;
            last_ep = 0;
            usb_ep_in = 0;
            usb_ep_out = 0;
            usb_ep_in_mps = 64;
            usb_ep_out_mps = 64;
            usb_ep_in_burst = 0;
            usb_ep_out_burst = 0;
            if (cfg[off + 5] == USB_CLASS_MASS &&
                cfg[off + 6] == USB_SUBCLASS_SCSI &&
                cfg[off + 7] == USB_PROTO_BBB && cfg[off + 3] == 0) {
                found = 1;
                usb_msc_interface = cfg[off + 2];
            } else
                found = 0;
        } else if (found && type == USB_DESC_ENDPOINT && len >= 7) {
            BYTE addr = cfg[off + 2];
            BYTE attr = cfg[off + 3];
            if ((attr & 3) == 2) {
                if (addr & 0x80)
                    usb_ep_in = addr & 0x0F;
                else
                    usb_ep_out = addr & 0x0F;
                if (addr & 0x80)
                    usb_ep_in_mps = (WORD)(cfg[off + 4] | (cfg[off + 5] << 8));
                else
                    usb_ep_out_mps = (WORD)(cfg[off + 4] | (cfg[off + 5] << 8));
                last_ep = addr;
            }
        } else if (found && type == USB_DESC_SS_EP_COMPANION && len >= 6 &&
                   last_ep) {
            if (cfg[off + 2] > 15)
                return 0;
            if (last_ep & 0x80)
                usb_ep_in_burst = cfg[off + 2];
            else
                usb_ep_out_burst = cfg[off + 2];
            last_ep = 0;
        } else {
            last_ep = 0;
        }
        off += len;
    }
    return found && usb_ep_in && usb_ep_out;
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

static int usb_enumerate_msc(void)
{
    BYTE devdesc[18];
    BYTE cfghdr[9];
    WORD total;
    BYTE cfg[256];
    BYTE cfgval;

    usb_devaddr = 0;
    usb_toggle_in = 0;
    usb_toggle_out = 0;
    if (usb_host == USB_HOST_XHCI)
        printf("usb: enumerate port=%u speed=%u slot=%u\n",
               usb_xhci_hcd->usbdevs[0].port,
               usb_xhci_hcd->usbdevs[0].speed,
               usb_xhci_hcd->usbdevs[0].slot);

    if (!usb_get_desc(USB_DESC_DEVICE, 0, devdesc, 8))
        return 0;
    if (usb_host == USB_HOST_XHCI &&
        !xhci_set_ep0_packet_size(usb_xhci_hcd, 0, devdesc[7]))
        return 0;
    if (!usb_set_address(1))
        return 0;
    if (!usb_get_desc(USB_DESC_DEVICE, 0, devdesc, 18))
        return 0;
    printf("usb: vid=%04x pid=%04x class=%u subclass=%u proto=%u\n",
           (unsigned)(devdesc[8] | (devdesc[9] << 8)),
           (unsigned)(devdesc[10] | (devdesc[11] << 8)),
           (unsigned)devdesc[4], (unsigned)devdesc[5],
           (unsigned)devdesc[6]);
    if (devdesc[4] == 9)
        printf("usb: hub (no hub driver yet)\n");
    if (!usb_get_desc(USB_DESC_CONFIG, 0, cfghdr, 9))
        return 0;
    total = cfghdr[2] | (cfghdr[3] << 8);
    if (total < 9 || total > sizeof(cfg))
        total = 9;
    if (!usb_get_desc(USB_DESC_CONFIG, 0, cfg, total))
        return 0;
    if (!usb_parse_config(cfg, total)) {
        printf("usb: no BBB mass-storage interface\n");
        return 0;
    }
    cfgval = cfg[5] ? cfg[5] : 1;
    if (!usb_set_config(cfgval))
        return 0;
    if (usb_host == USB_HOST_XHCI &&
        !xhci_configure_endpoints(usb_xhci_hcd, 0,
                      usb_ep_in, usb_ep_in_mps,
                                  usb_ep_in_burst, usb_ep_out,
                                  usb_ep_out_mps, usb_ep_out_burst))
        return 0;
    printf("usb: MSC endpoints in=%d out=%d\n", usb_ep_in, usb_ep_out);
    usb_scsi_inquiry();
    if (!usb_scsi_ready())
        printf("usb: TEST UNIT READY failed (continuing)\n");
    if (!usb_scsi_capacity(&usb_drive.total_blocks, &usb_drive.block_size)) {
        printf("usb: READ CAPACITY failed\n");
        return 0;
    }
    if (usb_drive.block_size != 512) {
        printf("usb: unsupported block size %u\n",
               (unsigned)usb_drive.block_size);
        return 0;
    }
    printf("usb: %u blocks, %u bytes/block\n",
           (unsigned)usb_drive.total_blocks,
           (unsigned)usb_drive.block_size);
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
    usb_xhci_hcd->enumerating = 1;
    usb_xhci_hcd->recovery_needed = 0;
    for (port = 1; port <= usb_xhci_hcd->max_ports && port <= 32; port++) {
        if (!(xhci_port_ccs_mask(usb_xhci_hcd) & xhci_ccs_bit(port)))
            continue;
        printf("xhci: trying port %u\n", port);
        if (!xhci_claim_port(usb_xhci_hcd, port))
            continue;
        if (usb_enumerate_msc()) {
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
    if (xhci_bulk_in_try(usb_xhci_hcd, USB_CDC_DEV, usb_cdc_ep_in,
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
    DWORD head, tail, n, i, cap;
    unsigned long flags;
    if (!usb_cdc_ready || usb_cdc_pumping || usb_host != USB_HOST_XHCI)
        return 0;
    if (usb_cdc_bulk_io_quiesced || usb_io_lock.locked)
        return 0;
    usb_cdc_pumping = 1;
    usb_io_lock_acquire();
    __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    tail = usb_cdc_tx_tail;
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
        if (!xhci_bulk(usb_xhci_hcd, USB_CDC_DEV, usb_cdc_ep_out, 0,
                       usb_cdc_dma_buf, (int)n)) {
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
    if (xhci_bulk_in_try(usb_xhci_hcd, USB_CDC_DEV, usb_cdc_ep_in,
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
        (void)xhci_bulk_in_try(usb_xhci_hcd, USB_CDC_DEV, usb_cdc_ep_in,
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
        if (xhci_bulk_in_try(usb_xhci_hcd, USB_CDC_DEV, usb_cdc_ep_in,
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
            if (xhci_bulk_in_try(usb_xhci_hcd, USB_CDC_DEV, usb_cdc_ep_in,
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
    DWORD msc_port;

    usb_cdc_reset_state();
    if (!usb_xhci_hcd)
        return 0;
    msc_port = usb_xhci_hcd->usbdevs[0].port;
    usb_xhci_hcd->enumerating = 1;
    usb_xhci_hcd->recovery_needed = 0;
    for (port = 1; port <= usb_xhci_hcd->max_ports && port <= 32; port++) {
        if (port == msc_port)
            continue;
        if (!(xhci_port_ccs_mask(usb_xhci_hcd) & xhci_ccs_bit(port)))
            continue;
        printf("xhci: trying CDC port %u\n", port);
        if (!xhci_claim_port_dev(usb_xhci_hcd, port, USB_CDC_DEV))
            continue;
        if (usb_enumerate_cdc()) {
            char ver[192];
            int vn;
            usb_xhci_hcd->enumerating = 0;
            usb_cdc_ready = 1;
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

static int usb_xhci_recover(void)
{
    usb_xhci_recovering = 1;
    usb_cdc_reset_state();
    xhci_stop_hcd(usb_xhci_hcd);
    usb_xhci_hcd->recovery_needed = 0;
    if (!xhci_init_hcd(usb_xhci_hcd) || !usb_xhci_bind_msc()) {
        printf("xhci: controller recovery failed\n");
        xhci_stop_hcd(usb_xhci_hcd);
        usb_xhci_hcd->recovery_needed = 1;
        usb_drive.present = 0;
        usb_invalidate_storage_cache();
        usb_xhci_recovering = 0;
        usb_cdc_reset_state();
        return 0;
    }
    usb_xhci_bind_cdc();
    usb_xhci_recovery_count++;
    usb_xhci_hcd->recovery_needed = 0;
    usb_drive.present = 1;
    usb_xhci_recovering = 0;
    printf("xhci: controller recovery complete count=%d\n",
           usb_xhci_recovery_count);
    return 1;
}

static int usb_xhci_reconnect(void)
{
    usb_media_identity replacement_identity;
    u64 old_blocks = usb_drive.total_blocks;
    DWORD old_block_size = usb_drive.block_size;
    /* "First attach ever" is signalled by no previously-enumerated media,
       not by publish state: the reconnect self-tests enumerate the device
       without publishing it, yet must still verify the replacement. */
    int initial_attach = (old_blocks == 0);
    usb_xhci_recovering = 1;
    usb_cdc_reset_state();
    xhci_stop_hcd(usb_xhci_hcd);
    usb_xhci_hcd->recovery_needed = 0;
    if (!xhci_init_hcd(usb_xhci_hcd) || !usb_xhci_bind_msc())
        goto fail;
    usb_xhci_bind_cdc();
    if (!initial_attach) {
        if (usb_drive.total_blocks != old_blocks ||
            usb_drive.block_size != old_block_size)
            goto fail;
        if (!usb_expected_identity.valid ||
            !usb_capture_media_identity(&replacement_identity) ||
            !usb_media_identity_equal(&usb_expected_identity,
                                      &replacement_identity)) {
            printf("usb: replacement volume identity mismatch\n");
            goto fail;
        }
    }
    usb_drive.present = 1;
    usb_xhci_recovering = 0;
    printf("xhci: device re-enumerated\n");
    return 1;
fail:
    printf("xhci: device reconnect failed\n");
    xhci_stop_hcd(usb_xhci_hcd);
    usb_drive.total_blocks = old_blocks;
    usb_drive.block_size = old_block_size;
    usb_drive.present = 0;
    usb_xhci_recovering = 0;
    return 0;
}

static void usb_xhci_hotplug_monitor(void)
{
    int attach_samples = 0;
    int reconnect_blocked = 0;
    int offline_reported = !usb_media_established;
    for (;;) {
        usb_cdc_pump();
        delay(xhci_cdc_hotplug_delay(usb_cdc_ready));
        if (usb_host != USB_HOST_XHCI)
            continue;
        if (usb_drive.present) {
            attach_samples = 0;
            reconnect_blocked = 0;
            offline_reported = 0;
            if (!xhci_usbdev_connected(usb_xhci_hcd, 0) &&
                !usb_io_lock.locked &&
                __sync_bool_compare_and_swap(&usb_hotplug_transition,0,1)) {
                if (!usb_io_lock.locked && usb_drive.present &&
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
        if (reconnect_blocked || ++attach_samples < 3)
            continue;
        attach_samples = 0;
        if (!__sync_bool_compare_and_swap(&usb_hotplug_transition,0,1))
            continue;
        if (usb_xhci_reconnect() && usb_publish_storage_devices()) {
            serial_puts("XHCI_HOTPLUG_RECONNECT_OK\n");
        } else {
            if (usb_drive.present)
                usb_xhci_disconnect_offline();
            reconnect_blocked = 1;
            serial_puts("XHCI_HOTPLUG_RECONNECT_REJECTED\n");
        }
        __sync_lock_release(&usb_hotplug_transition);
    }
}

static int usb_xhci_recovery_selftest(void)
{
    int before = usb_xhci_recovery_count;
    int pass;
    if (!usb_scsi_rw(0, 0, 1, (char *)usb_recovery_before))
        goto fail;
    usb_xhci_hcd->fault_drop_next = 1;
    if (!usb_scsi_rw(0, 0, 1, (char *)usb_recovery_after))
        goto fail;
    if (usb_xhci_recovery_count != before + 1 ||
        memcmp(usb_recovery_before, usb_recovery_after, 512) != 0)
        goto fail;
    usb_xhci_hcd->fault_drop_next = 1;
    if (!usb_scsi_rw(0, 0, 1, (char *)usb_recovery_after) ||
        usb_xhci_recovery_count != before + 2 ||
        memcmp(usb_recovery_before, usb_recovery_after, 512) != 0)
        goto fail;
    usb_drive.present = 1;
    usb_xhci_hcd->fault_drop_next = 1;
    usb_xhci_hcd->fault_fail_init = 1;
    pass = usb_scsi_rw(0, 0, 1, (char *)usb_recovery_after);
    if (pass || usb_drive.present ||
        usb_xhci_recovery_count != before + 2)
        goto fail;
    serial_puts("XHCI_RECOVERY_INIT_FAILURE_OK\n");
    if (!usb_xhci_recover() || !usb_drive.present ||
        usb_xhci_recovery_count != before + 3 ||
        !usb_scsi_rw(0, 0, 1, (char *)usb_recovery_after) ||
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
    if (!usb_scsi_rw(0, 0, 1, (char *)usb_recovery_before))
        goto fail;
    usb_fault_invalid_cbw = 1;
    if (!usb_scsi_rw(0, 0, 1, (char *)usb_recovery_after))
        goto fail;
    if (usb_xhci_recovery_count != controller_before ||
        usb_xhci_stall_recovery_count != stall_before + 1 ||
        memcmp(usb_recovery_before, usb_recovery_after, 512) != 0)
        goto fail;
    serial_puts("XHCI_BOT_SELECTIVE_RECOVERY_OK\n");
    usb_fault_invalid_cbw = 1;
    usb_fault_drop_stall_retry = 1;
    if (!usb_scsi_rw(0, 0, 1, (char *)usb_recovery_after) ||
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
    u64 blocks_before = usb_drive.total_blocks;
    DWORD block_size_before = usb_drive.block_size;
    if (reconnect && !usb_expected_identity.valid &&
        !usb_capture_media_identity(&usb_expected_identity))
        goto fail;
    if (reconnect &&
        !usb_scsi_rw(0, 0, 1, (char *)usb_recovery_before))
        goto fail;
    usb_drive.present = 1;
    usb_xhci_hcd->fault_disconnect_inflight = 1;
    if (usb_scsi_rw(0, 0, 1, (char *)usb_recovery_after) ||
        usb_drive.present || !usb_xhci_hcd->connection_lost ||
        usb_xhci_recovery_count != recovery_before)
        goto fail;
    if (usb_read_block_raw(0, (char *)usb_recovery_after, 1) ||
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
            if (usb_xhci_reconnect() || usb_drive.present ||
                usb_drive.total_blocks != blocks_before ||
                usb_drive.block_size != block_size_before)
                goto fail;
            if (identity_mismatch)
                serial_puts("XHCI_RECONNECT_IDENTITY_MISMATCH_OK\n");
            else
                serial_puts("XHCI_RECONNECT_MISMATCH_OK\n");
            return 1;
        }
        if (!usb_xhci_reconnect() ||
            !usb_read_block_raw(0, (char *)usb_recovery_after, 1) ||
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
        partdev_first_child(usb_drive.deviceid) < 0)
        goto fail;
    partition = partdev_first_child(usb_drive.deviceid);
    if (!blkcache_read(partition, 0, 1, cached))
        goto fail;
    if (!blkcache_write(partition, 0, 1, cached))
        goto fail;
    usb_disconnect_dirty_pages = 0;
    usb_xhci_hcd->fault_disconnect_inflight = 1;
    if (usb_read_block_raw(0, (char *)transfer, 1) || usb_drive.present ||
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
            !usb_publish_storage_devices())
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
    serial_puts("XHCI_MOUNTED_DISCONNECT_CACHE_FAIL\n");
    return 0;
}

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

int usb_init(void)
{
    BYTE bus, slot, func;
    DWORD bar;
    int port;
    int found_dev = 0;

    kbd_boot_leds_raw(KBD_LED_NUM);
    memset(&usb_drive, 0, sizeof(usb_drive));
    memset(&usb_expected_identity, 0, sizeof(usb_expected_identity));
    usb_media_established = 0;
    usb_drive.deviceid = -1;

    if (!uhci_find_controller(&bus, &slot, &func)) {
        printf("usb: no UHCI controller found; probing xHCI\n");
        usb_host = USB_HOST_XHCI;
        if (!usb_xhci_probe_msc()) {
            printf("usb: no xHCI mass-storage device\n");
            return -1;
        }
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
        if (usb_enumerate_msc()) {
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
        found_dev = 1;
    }

register_device:
    if (usb_host == USB_HOST_XHCI)
        usb_xhci_bind_cdc();
    usb_drive.present = 1;
    return usb_publish_storage_devices() ? 0 : -1;
}

int usb_storage_available(void)
{
    return usb_drive.present;
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
