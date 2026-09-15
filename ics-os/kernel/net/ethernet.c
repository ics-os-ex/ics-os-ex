#include "ethernet.h"
#include "arp.h"
#include "ipv4.h"
#include "netif.h"
#include "pbuf.h"
#include "net_endian.h"

extern void *memcpy(void *d, const void *s, unsigned int n);

void ethernet_input(struct netif *nif, struct pbuf *p)
{
    struct eth_hdr *eh;
    unsigned short etype;
    struct pbuf *payload;

    if (!nif || !p || p->len < ETH_HDR_LEN) {
        pbuf_free(p);
        return;
    }
    eh = (struct eth_hdr *)p->data;
    etype = net_ntohs(eh->ethertype);

    payload = pbuf_alloc((u16)(p->len - ETH_HDR_LEN));
    if (!payload) {
        pbuf_free(p);
        return;
    }
    memcpy(payload->data, p->data + ETH_HDR_LEN, p->len - ETH_HDR_LEN);
    pbuf_free(p);

    if (etype == ETH_TYPE_ARP)
        arp_input(nif, payload);
    else if (etype == ETH_TYPE_IPV4)
        ipv4_input(nif, payload);
    else
        pbuf_free(payload);
}

/* Consumes p on all paths (success or failure). */
int ethernet_output(struct netif *nif, struct pbuf *p,
                    const unsigned char *dst_mac, unsigned short ethertype)
{
    struct pbuf *frame;
    struct eth_hdr *eh;
    unsigned int i;

    if (!nif || !p || !dst_mac) {
        pbuf_free(p);
        return -1;
    }
    if ((unsigned int)p->len + ETH_HDR_LEN > PBUF_SIZE) {
        pbuf_free(p);
        return -1;
    }

    frame = pbuf_alloc((u16)(p->len + ETH_HDR_LEN));
    if (!frame) {
        pbuf_free(p);
        return -1;
    }
    eh = (struct eth_hdr *)frame->data;
    for (i = 0; i < ETH_ADDR_LEN; i++) {
        eh->dst[i] = dst_mac[i];
        eh->src[i] = nif->mac[i];
    }
    eh->ethertype = net_htons(ethertype);
    memcpy(frame->data + ETH_HDR_LEN, p->data, p->len);
    pbuf_free(p);

    if (netif_output(nif, frame) != 0) {
        pbuf_free(frame);
        return -1;
    }
    pbuf_free(frame);
    return 0;
}
