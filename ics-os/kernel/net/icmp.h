#ifndef ICSOS_NET_ICMP_H
#define ICSOS_NET_ICMP_H

#include "checksum.h"
#include "ipv4.h"
#include "net_endian.h"

#define ICMP_TYPE_ECHO_REPLY    0
#define ICMP_TYPE_ECHO_REQUEST  8
#define ICMP_HDR_LEN            8

struct icmp_echo_hdr {
    unsigned char  type;
    unsigned char  code;
    unsigned short checksum;
    unsigned short id;
    unsigned short seq;
} __attribute__((packed));

/*
 * Transform an IPv4+ICMP echo request into an echo reply in place.
 * pkt points at the IPv4 header; len is the full IP datagram length.
 * Returns 1 on success.
 */
static inline int icmp_echo_reply_transform(unsigned char *pkt, unsigned int len)
{
    struct ipv4_hdr *ip;
    struct icmp_echo_hdr *ic;
    unsigned int ihl, icmp_len;
    unsigned int tmp;
    unsigned int i;

    if (!pkt || len < IPV4_HDR_MIN + ICMP_HDR_LEN)
        return 0;
    ip = (struct ipv4_hdr *)pkt;
    if ((ip->ver_ihl >> 4) != 4)
        return 0;
    ihl = ipv4_hdr_len(ip);
    if (ihl < IPV4_HDR_MIN || len < ihl + ICMP_HDR_LEN)
        return 0;
    if (ip->proto != IPV4_PROTO_ICMP)
        return 0;
    ic = (struct icmp_echo_hdr *)(pkt + ihl);
    if (ic->type != ICMP_TYPE_ECHO_REQUEST || ic->code != 0)
        return 0;

    /* Swap IP addresses. */
    tmp = ip->src;
    ip->src = ip->dst;
    ip->dst = tmp;
    ip->ttl = 64;
    ipv4_set_checksum(ip);

    ic->type = ICMP_TYPE_ECHO_REPLY;
    ic->checksum = 0;
    icmp_len = len - ihl;
    ic->checksum = net_htons(net_checksum(ic, icmp_len));
    (void)i;
    return 1;
}

/* Build echo request ICMP header + payload into dst. Returns total ICMP bytes. */
static inline unsigned int icmp_build_echo_request(unsigned char *dst,
                                                  unsigned short id,
                                                  unsigned short seq,
                                                  const unsigned char *payload,
                                                  unsigned int payload_len)
{
    struct icmp_echo_hdr *ic = (struct icmp_echo_hdr *)dst;
    unsigned int i;

    ic->type = ICMP_TYPE_ECHO_REQUEST;
    ic->code = 0;
    ic->checksum = 0;
    ic->id = net_htons(id);
    ic->seq = net_htons(seq);
    for (i = 0; i < payload_len; i++)
        dst[ICMP_HDR_LEN + i] = payload[i];
    ic->checksum = net_htons(net_checksum(dst, ICMP_HDR_LEN + payload_len));
    return ICMP_HDR_LEN + payload_len;
}

struct netif;
struct pbuf;

void icmp_input(struct netif *nif, struct pbuf *p, unsigned int ip_hdr_len);
int  icmp_ping(struct netif *nif, unsigned int dst_host, unsigned int timeout_ticks);

#endif
