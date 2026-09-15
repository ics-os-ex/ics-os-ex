#include "netif.h"
#include "pbuf.h"
#include "ethernet.h"

extern void *memset(void *s, int c, unsigned int n);
extern void *memcpy(void *d, const void *s, unsigned int n);

static struct netif *g_default_netif;

void netif_set_default(struct netif *nif)
{
    g_default_netif = nif;
}

struct netif *netif_default(void)
{
    return g_default_netif;
}

void netif_init(struct netif *nif, struct netdev *dev)
{
    memset(nif, 0, sizeof(*nif));
    nif->dev = dev;
    nif->mtu = ETH_MTU;
    arp_cache_init(&nif->arp);
    if (dev && dev->ops && dev->ops->get_mac)
        dev->ops->get_mac(dev->drv, nif->mac);
}

void netif_set_addr(struct netif *nif, unsigned int ip, unsigned int mask,
                    unsigned int gw)
{
    nif->ip = ip;
    nif->netmask = mask;
    nif->gateway = gw;
    nif->configured = 1;
}

void netif_set_up(struct netif *nif)
{
    nif->link_up = 1;
}

int net_ip_on_link(struct netif *nif, unsigned int ip_host)
{
    return (ip_host & nif->netmask) == (nif->ip & nif->netmask);
}

unsigned int net_ip_route(struct netif *nif, unsigned int dst_host)
{
    if (net_ip_on_link(nif, dst_host))
        return dst_host;
    return nif->gateway;
}

int netif_output(struct netif *nif, struct pbuf *p)
{
    if (!nif || !nif->dev || !nif->dev->ops || !nif->dev->ops->transmit)
        return -1;
    if (!nif->link_up)
        return -1;
    return nif->dev->ops->transmit(nif->dev->drv, p);
}

void netif_input(struct netif *nif, struct pbuf *p)
{
    if (!nif || !p)
        return;
    ethernet_input(nif, p);
}

void netif_poll(struct netif *nif)
{
    if (nif && nif->dev && nif->dev->ops && nif->dev->ops->poll)
        nif->dev->ops->poll(nif->dev->drv);
}
