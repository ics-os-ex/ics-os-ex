/*
  Host TAP for TCP builders and IPv4 TCP checksum.
*/
#include <stdio.h>
#include <string.h>

#include "kernel/net/tcp.h"

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
    unsigned char payload[4] = { 't', 'c', 'p', '!' };
    unsigned int len;
    struct tcp_hdr *h;
    unsigned short csum;

    printf("TAP version 13\n1..6\n");

    len = tcp_build(buf, 41000, TCP_TEST_PORT, 1000, 0, TCP_SYN,
                    0, 0, 0x0A00020F, 0x0A000202);
    ok &= check("SYN length is header only", len == TCP_HDR_MIN);
    h = (struct tcp_hdr *)buf;
    ok &= check("SYN flag set", (h->flags & TCP_SYN) != 0);
    ok &= check("data offset 5", tcp_hdr_len(h) == TCP_HDR_MIN);

    len = tcp_build(buf, 41000, TCP_TEST_PORT, 1001, 5000,
                    TCP_PSH | TCP_ACK, payload, 4,
                    0x0A00020F, 0x0A000202);
    ok &= check("data segment length", len == TCP_HDR_MIN + 4);
    h = (struct tcp_hdr *)buf;
    ok &= check("seq/ack encoded",
                net_ntohl(h->seq) == 1001 && net_ntohl(h->ack) == 5000);

    csum = net_ntohs(h->checksum);
    h->checksum = 0;
    ok &= check("checksum recomputes",
                tcp_checksum(0x0A00020F, 0x0A000202, buf, len) == csum);

    return ok ? 0 : 1;
}
