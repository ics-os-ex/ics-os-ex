#ifndef ICSOS_NET_NETDEV_H
#define ICSOS_NET_NETDEV_H

#include "../types.h"
#include "ethernet.h"

struct pbuf;
struct netif;

struct netdev_ops {
    int (*transmit)(void *drv, struct pbuf *p);
    int (*link_up)(void *drv);
    void (*poll)(void *drv);
    void (*get_mac)(void *drv, unsigned char mac[ETH_ADDR_LEN]);
};

struct netdev {
    void *drv;
    const struct netdev_ops *ops;
    char name[8];
};

#endif
