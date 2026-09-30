#ifndef USB_CDC_RNDIS_H
#define USB_CDC_RNDIS_H

/*
  Host-testable RNDIS (CDC-ACM-shaped) config parser.

  RNDIS presents a Communications Class interface (class 2, subclass 2 = ACM)
  with a Union functional descriptor, plus a CDC Data interface (class 10)
  with a bulk IN and a bulk OUT endpoint. Unlike CDC-ECM there is NO Ethernet
  functional descriptor: the MAC and MTU are negotiated at runtime via the
  RNDIS control protocol (OID_802_3_PERMANENT_ADDRESS /
  OID_GEN_MAXIMUM_FRAME_SIZE) carried over the CDC
  SEND/GET_ENCAPSULATED_COMMAND/RESPONSE class requests, not in the
  descriptor.

  The descriptor shape is byte-for-byte identical to a plain CDC-ACM serial
  gadget. The driver therefore does NOT rely on the descriptor to decide a
  device is RNDIS: usb_enumerate_rndis() issues an RNDIS INITIALIZE probe over
  the encapsulation control endpoint after SET_CONFIGURATION and only commits
  to the NIC if the device answers with INITIALIZE_CMPLT (a real ACM serial
  STALLs SET_ENCAPSULATED_COMMAND). This parser only identifies the
  candidate shape.

  Constant names are USB_RNDIS_* to avoid clashing with usb_cdc_ecm.h (which
  uses USB_CDC_*) when both are included in the same translation unit.
*/

#define USB_RNDIS_DESC_CONFIG     2
#define USB_RNDIS_DESC_INTERFACE  4
#define USB_RNDIS_DESC_ENDPOINT   5
#define USB_RNDIS_DESC_SS_COMP    48
#define USB_RNDIS_DESC_FUNC       0x24

#define USB_RNDIS_CLASS_COMM      2
#define USB_RNDIS_SUBCLASS_ACM    2
#define USB_RNDIS_CLASS_DATA      10

#define USB_RNDIS_FUNC_UNION      0x06

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
} usb_cdc_rndis_info;

static inline int usb_parse_cdc_rndis(const unsigned char *cfg,
                                      unsigned int total,
                                      usb_cdc_rndis_info *out)
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
        if (type == USB_RNDIS_DESC_CONFIG && len >= 9 && off == 0) {
            out->cfgval = cfg[off + 5] ? cfg[off + 5] : 1;
        } else if (type == USB_RNDIS_DESC_INTERFACE && len >= 9) {
            unsigned char ifnum = cfg[off + 2];
            unsigned char classid = cfg[off + 5];
            unsigned char sub = cfg[off + 6];
            in_data = 0;
            last_ep = 0;
            if (classid == USB_RNDIS_CLASS_COMM &&
                sub == USB_RNDIS_SUBCLASS_ACM) {
                have_comm = 1;
                out->comm_if = ifnum;
            } else if (classid == USB_RNDIS_CLASS_DATA) {
                in_data = 1;
                out->data_if = ifnum;
                out->ep_in = 0;
                out->ep_out = 0;
                out->mps_in = 64;
                out->mps_out = 64;
                out->burst_in = 0;
                out->burst_out = 0;
            }
        } else if (in_data && type == USB_RNDIS_DESC_ENDPOINT && len >= 7) {
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
        } else if (in_data && type == USB_RNDIS_DESC_SS_COMP && len >= 6 &&
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

#endif
