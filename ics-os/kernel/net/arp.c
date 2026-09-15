#include "arp.h"
#include "netif.h"
#include "pbuf.h"
#include "ethernet.h"

extern void *memcpy(void *d, const void *s, unsigned int n);
extern void *memset(void *s, int c, unsigned int n);
extern int printf(const char *fmt, ...);
extern unsigned int ticks;

static const unsigned char eth_broadcast[ETH_ADDR_LEN] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};
static const unsigned char eth_zero[ETH_ADDR_LEN] = { 0, 0, 0, 0, 0, 0 };

void arp_init(struct netif *nif)
{
    if (nif)
        arp_cache_init(&nif->arp);
}

void arp_input(struct netif *nif, struct pbuf *p)
{
    unsigned short op;
    unsigned char sha[ETH_ADDR_LEN], tha[ETH_ADDR_LEN];
    unsigned int spa, tpa;
    struct pbuf *reply;
    unsigned char pkt[ARP_PKT_LEN];

    if (!nif || !p) {
        pbuf_free(p);
        return;
    }
    if (!arp_parse(p->data, p->len, &op, sha, &spa, tha, &tpa)) {
        pbuf_free(p);
        return;
    }

    /* Learn sender. */
    if (spa)
        arp_cache_insert(&nif->arp, spa, sha);

    if (op == ARP_OP_REQUEST && nif->configured && tpa == nif->ip) {
        arp_build(pkt, ARP_OP_REPLY, nif->mac, nif->ip, sha, spa);
        reply = pbuf_alloc(ARP_PKT_LEN);
        if (reply) {
            memcpy(reply->data, pkt, ARP_PKT_LEN);
            (void)ethernet_output(nif, reply, sha, ETH_TYPE_ARP);
        }
    }
    pbuf_free(p);
}

int arp_resolve(struct netif *nif, unsigned int ip_host,
                unsigned char mac_out[ETH_ADDR_LEN], unsigned int timeout_ticks)
{
    unsigned char pkt[ARP_PKT_LEN];
    struct pbuf *req;
    unsigned int start;
    unsigned int target;
    unsigned int spins;

    if (!nif || !mac_out)
        return -1;
    target = net_ip_route(nif, ip_host);
    if (arp_cache_lookup(&nif->arp, target, mac_out))
        return 0;

    arp_build(pkt, ARP_OP_REQUEST, nif->mac, nif->ip, eth_zero, target);
    req = pbuf_alloc(ARP_PKT_LEN);
    if (!req)
        return -1;
    memcpy(req->data, pkt, ARP_PKT_LEN);
    if (ethernet_output(nif, req, eth_broadcast, ETH_TYPE_ARP) != 0)
        return -1;

    start = ticks;
    spins = 0;
    while (1) {
        netif_poll(nif);
        if (arp_cache_lookup(&nif->arp, target, mac_out))
            return 0;
        if (ticks != start && ticks - start >= timeout_ticks)
            break;
        if (++spins > 2000000)
            break;
        __asm__ volatile ("pause");
    }
    return -1;
}
