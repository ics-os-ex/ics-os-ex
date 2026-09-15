#ifndef ICSOS_NET_ARP_H
#define ICSOS_NET_ARP_H

/*
 * ARP packet builders/parsers and a tiny host-testable cache.
 */

#include "ethernet.h"
#include "net_endian.h"

#define ARP_HW_ETHER     1
#define ARP_PROTO_IPV4   0x0800
#define ARP_OP_REQUEST   1
#define ARP_OP_REPLY     2
#define ARP_PKT_LEN      28
#define ARP_CACHE_SIZE   8

struct arp_hdr {
    unsigned short hw_type;
    unsigned short proto_type;
    unsigned char  hw_len;
    unsigned char  proto_len;
    unsigned short op;
    unsigned char  sha[ETH_ADDR_LEN];
    unsigned char  spa[4];
    unsigned char  tha[ETH_ADDR_LEN];
    unsigned char  tpa[4];
} __attribute__((packed));

struct arp_cache_entry {
    unsigned int ip; /* host order */
    unsigned char mac[ETH_ADDR_LEN];
    unsigned char valid;
};

struct arp_cache {
    struct arp_cache_entry e[ARP_CACHE_SIZE];
};

static inline void arp_cache_init(struct arp_cache *c)
{
    unsigned int i;
    for (i = 0; i < ARP_CACHE_SIZE; i++)
        c->e[i].valid = 0;
}

static inline int arp_cache_lookup(const struct arp_cache *c, unsigned int ip,
                                   unsigned char mac_out[ETH_ADDR_LEN])
{
    unsigned int i;
    for (i = 0; i < ARP_CACHE_SIZE; i++) {
        if (c->e[i].valid && c->e[i].ip == ip) {
            unsigned int j;
            for (j = 0; j < ETH_ADDR_LEN; j++)
                mac_out[j] = c->e[i].mac[j];
            return 1;
        }
    }
    return 0;
}

static inline void arp_cache_insert(struct arp_cache *c, unsigned int ip,
                                    const unsigned char mac[ETH_ADDR_LEN])
{
    unsigned int i, slot = ARP_CACHE_SIZE;
    for (i = 0; i < ARP_CACHE_SIZE; i++) {
        if (c->e[i].valid && c->e[i].ip == ip) {
            slot = i;
            break;
        }
        if (!c->e[i].valid && slot == ARP_CACHE_SIZE)
            slot = i;
    }
    if (slot == ARP_CACHE_SIZE)
        slot = 0;
    c->e[slot].ip = ip;
    for (i = 0; i < ETH_ADDR_LEN; i++)
        c->e[slot].mac[i] = mac[i];
    c->e[slot].valid = 1;
}

static inline void arp_ip_to_bytes(unsigned int ip_host, unsigned char out[4])
{
    out[0] = (unsigned char)((ip_host >> 24) & 0xFF);
    out[1] = (unsigned char)((ip_host >> 16) & 0xFF);
    out[2] = (unsigned char)((ip_host >> 8) & 0xFF);
    out[3] = (unsigned char)(ip_host & 0xFF);
}

static inline unsigned int arp_bytes_to_ip(const unsigned char in[4])
{
    return ((unsigned int)in[0] << 24) | ((unsigned int)in[1] << 16) |
           ((unsigned int)in[2] << 8) | (unsigned int)in[3];
}

/* Build an ARP request or reply into dst[ARP_PKT_LEN]. Returns length. */
static inline unsigned int arp_build(unsigned char *dst, unsigned short op,
                                     const unsigned char sha[ETH_ADDR_LEN],
                                     unsigned int spa_host,
                                     const unsigned char tha[ETH_ADDR_LEN],
                                     unsigned int tpa_host)
{
    struct arp_hdr *h = (struct arp_hdr *)dst;
    unsigned int i;

    h->hw_type = net_htons(ARP_HW_ETHER);
    h->proto_type = net_htons(ARP_PROTO_IPV4);
    h->hw_len = ETH_ADDR_LEN;
    h->proto_len = 4;
    h->op = net_htons(op);
    for (i = 0; i < ETH_ADDR_LEN; i++) {
        h->sha[i] = sha[i];
        h->tha[i] = tha ? tha[i] : 0;
    }
    arp_ip_to_bytes(spa_host, h->spa);
    arp_ip_to_bytes(tpa_host, h->tpa);
    return ARP_PKT_LEN;
}

/* Parse ARP; returns 1 if valid IPv4/Ethernet ARP. */
static inline int arp_parse(const unsigned char *pkt, unsigned int len,
                            unsigned short *op_out,
                            unsigned char sha_out[ETH_ADDR_LEN],
                            unsigned int *spa_out,
                            unsigned char tha_out[ETH_ADDR_LEN],
                            unsigned int *tpa_out)
{
    const struct arp_hdr *h = (const struct arp_hdr *)pkt;
    unsigned int i;

    if (!pkt || len < ARP_PKT_LEN)
        return 0;
    if (net_ntohs(h->hw_type) != ARP_HW_ETHER)
        return 0;
    if (net_ntohs(h->proto_type) != ARP_PROTO_IPV4)
        return 0;
    if (h->hw_len != ETH_ADDR_LEN || h->proto_len != 4)
        return 0;
    if (op_out)
        *op_out = net_ntohs(h->op);
    if (sha_out) {
        for (i = 0; i < ETH_ADDR_LEN; i++)
            sha_out[i] = h->sha[i];
    }
    if (tha_out) {
        for (i = 0; i < ETH_ADDR_LEN; i++)
            tha_out[i] = h->tha[i];
    }
    if (spa_out)
        *spa_out = arp_bytes_to_ip(h->spa);
    if (tpa_out)
        *tpa_out = arp_bytes_to_ip(h->tpa);
    return 1;
}

struct netif;
struct pbuf;

void arp_init(struct netif *nif);
void arp_input(struct netif *nif, struct pbuf *p);
int  arp_resolve(struct netif *nif, unsigned int ip_host,
                 unsigned char mac_out[ETH_ADDR_LEN], unsigned int timeout_ticks);

#endif
