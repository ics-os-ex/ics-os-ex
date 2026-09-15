/*
  Host TAP for ARP builders, parsers, and cache.
*/
#include <stdio.h>
#include <string.h>

#include "kernel/net/arp.h"

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
    unsigned char pkt[ARP_PKT_LEN];
    unsigned char sha[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    unsigned char tha[6] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    unsigned char mac[6];
    unsigned short op;
    unsigned int spa, tpa;
    struct arp_cache cache;

    printf("TAP version 13\n1..8\n");

    ok &= check("build request length",
                arp_build(pkt, ARP_OP_REQUEST, sha, 0x0A00020F, tha, 0x0A000202) ==
                    ARP_PKT_LEN);

    ok &= check("parse request",
                arp_parse(pkt, ARP_PKT_LEN, &op, mac, &spa, tha, &tpa) == 1);
    ok &= check("op is request", op == ARP_OP_REQUEST);
    ok &= check("spa preserved", spa == 0x0A00020F);
    ok &= check("tpa preserved", tpa == 0x0A000202);
    ok &= check("sha preserved", memcmp(mac, sha, 6) == 0);

    arp_cache_init(&cache);
    arp_cache_insert(&cache, 0x0A000202, sha);
    ok &= check("cache lookup hit", arp_cache_lookup(&cache, 0x0A000202, mac) == 1);
    ok &= check("cache miss", arp_cache_lookup(&cache, 0x0A000201, mac) == 0);

    return ok ? 0 : 1;
}
