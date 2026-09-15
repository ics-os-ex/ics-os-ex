#include "ipv4.h"
#include "icmp.h"
#include "netif.h"
#include "pbuf.h"
#include "ethernet.h"
#include "arp.h"
#include "net_endian.h"

extern void *memcpy(void *d, const void *s, unsigned int n);
extern int printf(const char *fmt, ...);

static u16 ipv4_id_counter;

void ipv4_input(struct netif *nif, struct pbuf *p)
{
    struct ipv4_hdr *ip;
    unsigned int ihl, total;

    if (!nif || !p || p->len < IPV4_HDR_MIN) {
        pbuf_free(p);
        return;
    }
    ip = (struct ipv4_hdr *)p->data;
    if ((ip->ver_ihl >> 4) != 4) {
        pbuf_free(p);
        return;
    }
    ihl = ipv4_hdr_len(ip);
    if (ihl < IPV4_HDR_MIN || p->len < ihl) {
        pbuf_free(p);
        return;
    }
    total = net_ntohs(ip->total_len);
    if (total < ihl || total > p->len)
        total = p->len;

    /* Accept unicast to us, limited broadcast, or subnet broadcast. */
    {
        unsigned int dst = net_ntohl(ip->dst);
        unsigned int bcast = nif->ip | ~nif->netmask;
        if (nif->configured && dst != nif->ip && dst != 0xFFFFFFFFu &&
            dst != bcast) {
            pbuf_free(p);
            return;
        }
    }

    if (ip->proto == IPV4_PROTO_ICMP)
        icmp_input(nif, p, ihl);
    else
        pbuf_free(p);
}

int ipv4_output(struct netif *nif, struct pbuf *payload, unsigned int dst_host,
                unsigned char proto)
{
    struct pbuf *pkt;
    struct ipv4_hdr *ip;
    unsigned char dmac[ETH_ADDR_LEN];
    unsigned int total;

    if (!nif || !payload || !nif->configured)
        return -1;
    total = IPV4_HDR_MIN + payload->len;
    if (total > PBUF_SIZE)
        return -1;

    pkt = pbuf_alloc((u16)total);
    if (!pkt)
        return -1;
    ip = (struct ipv4_hdr *)pkt->data;
    ip->ver_ihl = IPV4_VERSION_IHL;
    ip->tos = 0;
    ip->total_len = net_htons((unsigned short)total);
    ip->id = net_htons(ipv4_id_counter++);
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->proto = proto;
    ip->src = net_htonl(nif->ip);
    ip->dst = net_htonl(dst_host);
    ipv4_set_checksum(ip);
    memcpy(pkt->data + IPV4_HDR_MIN, payload->data, payload->len);
    pbuf_free(payload);

    if (arp_resolve(nif, dst_host, dmac, 200) != 0) {
        pbuf_free(pkt);
        return -1;
    }
    return ethernet_output(nif, pkt, dmac, ETH_TYPE_IPV4);
}
