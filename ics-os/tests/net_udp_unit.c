/*
  Host TAP for UDP builders and IPv4 UDP checksum.
*/
#include <stdio.h>
#include <string.h>

#include "kernel/net/udp.h"

static int check(const char *name, int condition)
{
    if (!condition) {
        printf("not ok - %s\n", name);
        return 0;
    }
    printf("ok - %s\n", name);
    return 1;
}

int main(void)
{
    int ok = 1;
    unsigned char buf[64];
    unsigned char payload[4] = { 'p', 'i', 'n', 'g' };
    unsigned int len;
    struct udp_hdr *h;
    unsigned short csum;

    printf("TAP version 13\n1..5\n");

    len = udp_build(buf, 40000, UDP_TEST_PORT, payload, 4,
                    0x0A00020F, 0x0A000202);
    ok &= check("udp_build length", len == UDP_HDR_LEN + 4);
    h = (struct udp_hdr *)buf;
    ok &= check("ports encoded",
                net_ntohs(h->src_port) == 40000 &&
                net_ntohs(h->dst_port) == UDP_TEST_PORT);
    ok &= check("length field", net_ntohs(h->length) == len);
    ok &= check("checksum nonzero", h->checksum != 0);

    /* Zero checksum field and recompute must match stored value. */
    csum = net_ntohs(h->checksum);
    h->checksum = 0;
    ok &= check("checksum recomputes",
                udp_checksum(0x0A00020F, 0x0A000202, buf, len) == csum);

    return ok ? 0 : 1;
}
