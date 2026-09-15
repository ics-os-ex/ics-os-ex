/*
  Host TAP for ICMP echo request -> reply transform.
*/
#include <stdio.h>
#include <string.h>

#include "kernel/net/icmp.h"
#include "kernel/net/net_endian.h"

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
    unsigned char pkt[IPV4_HDR_MIN + ICMP_HDR_LEN + 4];
    struct ipv4_hdr *ip = (struct ipv4_hdr *)pkt;
    struct icmp_echo_hdr *ic;
    unsigned int icmp_len;
    unsigned char payload[4] = { 1, 2, 3, 4 };
    unsigned int orig_src, orig_dst;

    printf("TAP version 13\n1..7\n");

    memset(pkt, 0, sizeof(pkt));
    ip->ver_ihl = IPV4_VERSION_IHL;
    ip->total_len = net_htons((unsigned short)sizeof(pkt));
    ip->ttl = 64;
    ip->proto = IPV4_PROTO_ICMP;
    ip->src = net_htonl(0x0A000202);
    ip->dst = net_htonl(0x0A00020F);
    ipv4_set_checksum(ip);

    icmp_len = icmp_build_echo_request(pkt + IPV4_HDR_MIN, 0x1234, 7, payload, 4);
    ok &= check("echo request built", icmp_len == ICMP_HDR_LEN + 4);

    ic = (struct icmp_echo_hdr *)(pkt + IPV4_HDR_MIN);
    ok &= check("type is echo request", ic->type == ICMP_TYPE_ECHO_REQUEST);
    ok &= check("id/seq encoded",
                net_ntohs(ic->id) == 0x1234 && net_ntohs(ic->seq) == 7);

    orig_src = ip->src;
    orig_dst = ip->dst;
    ok &= check("transform succeeds",
                icmp_echo_reply_transform(pkt, sizeof(pkt)) == 1);
    ok &= check("type is echo reply", ic->type == ICMP_TYPE_ECHO_REPLY);
    ok &= check("addresses swapped",
                ip->src == orig_dst && ip->dst == orig_src);
    ok &= check("reply ICMP checksum verifies",
                net_checksum(ic, icmp_len) == 0);

    return ok ? 0 : 1;
}
