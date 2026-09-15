#ifndef ICSOS_NET_ETHERNET_H
#define ICSOS_NET_ETHERNET_H

#include "net_endian.h"

#define ETH_ADDR_LEN     6
#define ETH_HDR_LEN      14
#define ETH_TYPE_ARP     0x0806
#define ETH_TYPE_IPV4    0x0800
#define ETH_MTU          1500
#define ETH_MAX_FRAME    (ETH_HDR_LEN + ETH_MTU)

struct eth_hdr {
    unsigned char dst[ETH_ADDR_LEN];
    unsigned char src[ETH_ADDR_LEN];
    unsigned short ethertype; /* network order */
} __attribute__((packed));

struct pbuf; /* forward */
struct netif;

void ethernet_input(struct netif *nif, struct pbuf *p);
int  ethernet_output(struct netif *nif, struct pbuf *p,
                     const unsigned char *dst_mac, unsigned short ethertype);

#endif
