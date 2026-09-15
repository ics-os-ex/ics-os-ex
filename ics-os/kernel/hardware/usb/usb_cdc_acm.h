#ifndef USB_CDC_ACM_H
#define USB_CDC_ACM_H

/*
  Host-testable CDC-ACM config parser.

  Finds a Communications Class ACM interface (class 2, subclass 2) plus a
  CDC Data interface (class 10) with bulk IN and bulk OUT. Device class
  may be 2, 0 (interface-defined), or 0xEF with an IAD; those live in the
  device descriptor, so this walker only sees the configuration blob.
*/

#define USB_CDC_DESC_CONFIG     2
#define USB_CDC_DESC_INTERFACE  4
#define USB_CDC_DESC_ENDPOINT   5
#define USB_CDC_DESC_IAD        11
#define USB_CDC_DESC_SS_COMP    48

#define USB_CDC_CLASS_COMM      2
#define USB_CDC_SUBCLASS_ACM    2
#define USB_CDC_CLASS_DATA      10

#define USB_CDC_REQ_SET_LINE_CODING         0x20
#define USB_CDC_REQ_SET_CONTROL_LINE_STATE  0x22
#define USB_CDC_LINE_DTR                    0x01
#define USB_CDC_LINE_RTS                    0x02

typedef struct {
    unsigned char comm_if;
    unsigned char data_if;
    unsigned char ep_in;
    unsigned char ep_out;
    unsigned short mps_in;
    unsigned short mps_out;
    unsigned char burst_in;
    unsigned char burst_out;
    unsigned char cfgval;
} usb_cdc_acm_info;

static inline int usb_parse_cdc_acm(const unsigned char *cfg,
                                    unsigned int total,
                                    usb_cdc_acm_info *out)
{
    unsigned int off = 0;
    int in_data = 0;
    int have_comm = 0;
    unsigned char last_ep = 0;

    if (!cfg || !out || total < 9)
        return 0;
    out->comm_if = 0;
    out->data_if = 0;
    out->ep_in = 0;
    out->ep_out = 0;
    out->mps_in = 64;
    out->mps_out = 64;
    out->burst_in = 0;
    out->burst_out = 0;
    out->cfgval = 1;
    while (off + 2 <= total) {
        unsigned char len = cfg[off];
        unsigned char type = cfg[off + 1];
        if (len < 2 || off + (unsigned int)len > total)
            return 0;
        if (type == USB_CDC_DESC_CONFIG && len >= 9 && off == 0) {
            out->cfgval = cfg[off + 5] ? cfg[off + 5] : 1;
        } else if (type == USB_CDC_DESC_INTERFACE && len >= 9) {
            unsigned char ifnum = cfg[off + 2];
            unsigned char classid = cfg[off + 5];
            unsigned char sub = cfg[off + 6];
            in_data = 0;
            last_ep = 0;
            if (classid == USB_CDC_CLASS_COMM &&
                sub == USB_CDC_SUBCLASS_ACM) {
                have_comm = 1;
                out->comm_if = ifnum;
            } else if (classid == USB_CDC_CLASS_DATA) {
                in_data = 1;
                out->data_if = ifnum;
                out->ep_in = 0;
                out->ep_out = 0;
                out->mps_in = 64;
                out->mps_out = 64;
                out->burst_in = 0;
                out->burst_out = 0;
            }
        } else if (in_data && type == USB_CDC_DESC_ENDPOINT && len >= 7) {
            unsigned char addr = cfg[off + 2];
            unsigned char attr = cfg[off + 3];
            unsigned short mps =
                (unsigned short)(cfg[off + 4] | (cfg[off + 5] << 8));
            if ((attr & 3) == 2) {
                if (addr & 0x80) {
                    out->ep_in = (unsigned char)(addr & 0x0F);
                    out->mps_in = mps ? mps : 64;
                } else {
                    out->ep_out = (unsigned char)(addr & 0x0F);
                    out->mps_out = mps ? mps : 64;
                }
                last_ep = addr;
            }
        } else if (in_data && type == USB_CDC_DESC_SS_COMP && len >= 6 &&
                   last_ep) {
            if (cfg[off + 2] > 15)
                return 0;
            if (last_ep & 0x80)
                out->burst_in = cfg[off + 2];
            else
                out->burst_out = cfg[off + 2];
            last_ep = 0;
        } else {
            last_ep = 0;
        }
        off += len;
    }
    return have_comm && out->ep_in && out->ep_out;
}

static inline int usb_parse_vendor_bulk_serial(const unsigned char *cfg,
                                               unsigned int total,
                                               usb_cdc_acm_info *out)
{
    unsigned int off = 0;
    int in_vendor = 0;
    unsigned char last_ep = 0;

    if (!cfg || !out || total < 9)
        return 0;
    out->comm_if = 0;
    out->data_if = 0;
    out->ep_in = 0;
    out->ep_out = 0;
    out->mps_in = 64;
    out->mps_out = 64;
    out->burst_in = 0;
    out->burst_out = 0;
    out->cfgval = 1;
    while (off + 2 <= total) {
        unsigned char len = cfg[off];
        unsigned char type = cfg[off + 1];
        if (len < 2 || off + (unsigned int)len > total)
            return 0;
        if (type == USB_CDC_DESC_CONFIG && len >= 9 && off == 0) {
            out->cfgval = cfg[off + 5] ? cfg[off + 5] : 1;
        } else if (type == USB_CDC_DESC_INTERFACE && len >= 9) {
            unsigned char ifnum = cfg[off + 2];
            unsigned char classid = cfg[off + 5];
            in_vendor = 0;
            last_ep = 0;
            /* QEMU usb-serial is FTDI: vendor class 0xFF, two bulk EPs. */
            if (classid == 0xFF) {
                in_vendor = 1;
                out->comm_if = ifnum;
                out->data_if = ifnum;
                out->ep_in = 0;
                out->ep_out = 0;
                out->mps_in = 64;
                out->mps_out = 64;
                out->burst_in = 0;
                out->burst_out = 0;
            }
        } else if (in_vendor && type == USB_CDC_DESC_ENDPOINT && len >= 7) {
            unsigned char addr = cfg[off + 2];
            unsigned char attr = cfg[off + 3];
            unsigned short mps =
                (unsigned short)(cfg[off + 4] | (cfg[off + 5] << 8));
            if ((attr & 3) == 2) {
                if (addr & 0x80) {
                    out->ep_in = (unsigned char)(addr & 0x0F);
                    out->mps_in = mps ? mps : 64;
                } else {
                    out->ep_out = (unsigned char)(addr & 0x0F);
                    out->mps_out = mps ? mps : 64;
                }
                last_ep = addr;
            }
        } else if (in_vendor && type == USB_CDC_DESC_SS_COMP && len >= 6 &&
                   last_ep) {
            if (cfg[off + 2] > 15)
                return 0;
            if (last_ep & 0x80)
                out->burst_in = cfg[off + 2];
            else
                out->burst_out = cfg[off + 2];
            last_ep = 0;
        } else {
            last_ep = 0;
        }
        off += len;
    }
    return out->ep_in && out->ep_out;
}

static inline int usb_parse_usb_console(const unsigned char *cfg,
                                        unsigned int total,
                                        usb_cdc_acm_info *out)
{
    if (usb_parse_cdc_acm(cfg, total, out))
        return 1;
    return usb_parse_vendor_bulk_serial(cfg, total, out);
}

#endif
