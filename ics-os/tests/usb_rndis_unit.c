/*
  Host TAP for USB RNDIS configuration parsing.
  Covers the QEMU usb-net RNDIS config (config value 2, bulk 64-byte data
  endpoints, vendor-protocol ACM comm interface), the CDC-ECM config
  (subclass 6) which must be rejected, an ACM-shaped config missing the data
  interface, a config with only a data interface, and truncation/overflow
  bounds. RNDIS and CDC-ACM share the descriptor shape, so the parser accepts
  both; the protocol probe is the runtime disambiguator.
 */
#include <stdio.h>
#include <string.h>

#include "kernel/hardware/usb/usb_cdc_rndis.h"

static int check(const char *name, int condition)
{
    if (!condition) {
        printf("not ok - %s\n", name);
        return 0;
    }
    printf("ok - %s\n", name);
    return 1;
}

/* QEMU usb-net RNDIS: config value 2, IAD, ACM comm IF0 (vendor protocol,
   interrupt EP 0x81), CDC Data IF1 (bulk 0x82/0x02, 64-byte MPS). */
static const unsigned char qemu_rndis[] = {
    0x09, 0x02, 0x4B, 0x00, 0x02, 0x02, 0x00, 0x80, 0x32, /* config value 2 */
    0x08, 0x0B, 0x00, 0x02, 0x02, 0x02, 0xFF, 0x00,       /* IAD */
    0x09, 0x04, 0x00, 0x00, 0x01, 0x02, 0x02, 0xFF, 0x00, /* IF0 comm ACM */
    0x05, 0x24, 0x00, 0x10, 0x01,                          /* header */
    0x05, 0x24, 0x01, 0x00, 0x01,                          /* call mgmt */
    0x04, 0x24, 0x02, 0x00,                                /* ACM */
    0x05, 0x24, 0x06, 0x00, 0x01,                          /* union */
    0x07, 0x05, 0x81, 0x03, 0x10, 0x00, 0x09,              /* EP int IN */
    0x09, 0x04, 0x01, 0x00, 0x02, 0x0A, 0x00, 0x00, 0x00,  /* IF1 data */
    0x07, 0x05, 0x82, 0x02, 0x40, 0x00, 0x00,              /* EP bulk IN */
    0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00               /* EP bulk OUT */
};

/* Same shape but CDC-ECM: comm subclass 6 (Ethernet) instead of ACM (2).
   The RNDIS parser must reject it (ECM is bound by its own binder). */
static const unsigned char qemu_ecm[] = {
    0x09, 0x02, 0x4B, 0x00, 0x02, 0x01, 0x00, 0x80, 0x32, /* config value 1 */
    0x08, 0x0B, 0x00, 0x02, 0x02, 0x06, 0x00, 0x00,       /* IAD */
    0x09, 0x04, 0x00, 0x00, 0x01, 0x02, 0x06, 0x00, 0x00, /* IF0 comm ECM */
    0x05, 0x24, 0x00, 0x10, 0x01,                          /* header */
    0x05, 0x24, 0x01, 0x00, 0x01,                          /* call mgmt */
    0x05, 0x24, 0x0F, 0x03, 0x11, 0x22,                    /* ETH + iMAC */
    0x07, 0x05, 0x81, 0x03, 0x10, 0x00, 0x09,              /* EP int IN */
    0x09, 0x04, 0x01, 0x00, 0x02, 0x0A, 0x00, 0x00, 0x00,  /* IF1 data */
    0x07, 0x05, 0x82, 0x02, 0x40, 0x00, 0x00,              /* EP bulk IN */
    0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00               /* EP bulk OUT */
};

/* ACM comm interface only: no CDC Data interface. */
static const unsigned char rndis_no_data[] = {
    0x09, 0x02, 0x1C, 0x00, 0x01, 0x02, 0x00, 0x80, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x01, 0x02, 0x02, 0xFF, 0x00,
    0x07, 0x05, 0x81, 0x03, 0x10, 0x00, 0x09
};

/* Data interface only: no ACM comm interface. */
static const unsigned char rndis_no_comm[] = {
    0x09, 0x02, 0x20, 0x00, 0x01, 0x01, 0x00, 0x80, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x02, 0x0A, 0x00, 0x00, 0x00,
    0x07, 0x05, 0x82, 0x02, 0x40, 0x00, 0x00,
    0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00
};

/* Data interface with only a bulk IN endpoint (no OUT) is rejected. */
static const unsigned char rndis_data_in_only[] = {
    0x09, 0x02, 0x20, 0x00, 0x02, 0x01, 0x00, 0x80, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x01, 0x02, 0x02, 0xFF, 0x00,
    0x09, 0x04, 0x01, 0x00, 0x01, 0x0A, 0x00, 0x00, 0x00,
    0x07, 0x05, 0x82, 0x02, 0x40, 0x00, 0x00
};

/* Config header claims more than the supplied buffer. */
static const unsigned char rndis_overflow[] = {
    0x09, 0x02, 0x4B, 0x00, 0x02, 0x02, 0x00, 0x80, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x01, 0x02, 0x02, 0xFF, 0x00
};

int main(void)
{
    usb_cdc_rndis_info info;
    int ok = 1;

    printf("TAP version 13\n1..12\n");

    memset(&info, 0, sizeof info);
    ok &= check("QEMU usb-net RNDIS parses",
                usb_parse_cdc_rndis(qemu_rndis, sizeof qemu_rndis, &info) == 1);
    ok &= check("QEMU RNDIS bulk in=2 out=2 mps=64 cfg=2",
                info.ep_in == 2 && info.ep_out == 2 &&
                info.mps_in == 64 && info.mps_out == 64 &&
                info.cfgval == 2 && info.comm_if == 0 &&
                info.data_if == 1);

    memset(&info, 0, sizeof info);
    ok &= check("RNDIS config with IAD and functional descs parses",
                usb_parse_cdc_rndis(qemu_rndis, sizeof qemu_rndis, &info) == 1);
    ok &= check("RNDIS comm IF is 0 data IF is 1",
                info.comm_if == 0 && info.data_if == 1);

    ok &= check("CDC-ECM config (subclass 6) is not RNDIS",
                usb_parse_cdc_rndis(qemu_ecm, sizeof qemu_ecm, &info) == 0);
    ok &= check("ACM comm without a data IF is rejected",
                usb_parse_cdc_rndis(rndis_no_data, sizeof rndis_no_data,
                                    &info) == 0);
    ok &= check("data IF without an ACM comm IF is rejected",
                usb_parse_cdc_rndis(rndis_no_comm, sizeof rndis_no_comm,
                                    &info) == 0);
    ok &= check("data IF with only bulk IN is rejected",
                usb_parse_cdc_rndis(rndis_data_in_only,
                                    sizeof rndis_data_in_only, &info) == 0);
    ok &= check("truncated config header is rejected",
                usb_parse_cdc_rndis(qemu_rndis, 8, &info) == 0);
    ok &= check("descriptor that overruns total is rejected",
                usb_parse_cdc_rndis(rndis_overflow, sizeof rndis_overflow,
                                    &info) == 0);
    ok &= check("NULL buffer is rejected",
                usb_parse_cdc_rndis(0, sizeof qemu_rndis, &info) == 0);
    ok &= check("NULL out info is rejected",
                usb_parse_cdc_rndis(qemu_rndis, sizeof qemu_rndis, 0) == 0);

    return ok ? 0 : 1;
}
