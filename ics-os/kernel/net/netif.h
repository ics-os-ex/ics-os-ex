#ifndef ICSOS_NET_NETIF_H
#define ICSOS_NET_NETIF_H

#include "../types.h"
#include "ethernet.h"
#include "netdev.h"
#include "arp.h"

struct pbuf;

struct netif {
    unsigned char mac[ETH_ADDR_LEN];
    unsigned int ip;      /* host order */
    unsigned int netmask;
    unsigned int gateway;
    u16 mtu;
    u8  link_up;
    u8  configured;
    struct netdev *dev;
    struct arp_cache arp;
};

void netif_init(struct netif *nif, struct netdev *dev);
void netif_set_addr(struct netif *nif, unsigned int ip, unsigned int mask,
                    unsigned int gw);
void netif_set_up(struct netif *nif);
int  netif_output(struct netif *nif, struct pbuf *p);
void netif_input(struct netif *nif, struct pbuf *p);
void netif_poll(struct netif *nif);

/* Default / primary interface (single-NIC milestone A). */
struct netif *netif_default(void);
void netif_set_default(struct netif *nif);

int net_ip_on_link(struct netif *nif, unsigned int ip_host);
unsigned int net_ip_route(struct netif *nif, unsigned int dst_host);

#endif
