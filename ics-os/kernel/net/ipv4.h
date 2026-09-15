#ifndef ICSOS_NET_IPV4_H
#define ICSOS_NET_IPV4_H

#include "checksum.h"
#include "net_endian.h"

#define IPV4_HDR_MIN     20
#define IPV4_PROTO_ICMP  1
#define IPV4_PROTO_UDP   17
#define IPV4_VERSION_IHL 0x45

struct ipv4_hdr {
    unsigned char  ver_ihl;
    unsigned char  tos;
    unsigned short total_len;
    unsigned short id;
    unsigned short frag_off;
    unsigned char  ttl;
    unsigned char  proto;
    unsigned short checksum;
    unsigned int   src;
    unsigned int   dst;
} __attribute__((packed));

static inline unsigned int ipv4_hdr_len(const struct ipv4_hdr *h)
{
    return (unsigned int)(h->ver_ihl & 0x0F) * 4u;
}

static inline void ipv4_set_checksum(struct ipv4_hdr *h)
{
    h->checksum = 0;
    h->checksum = net_htons(net_checksum(h, ipv4_hdr_len(h)));
}

struct netif;
struct pbuf;

void ipv4_input(struct netif *nif, struct pbuf *p);
int  ipv4_output(struct netif *nif, struct pbuf *p, unsigned int dst_host,
                 unsigned char proto);

#endif
