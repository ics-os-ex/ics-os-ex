#include "netcfg.h"
#include "netif.h"
#include "netdev.h"
#include "../types.h"

extern void *memset(void *s, int c, unsigned int n);
extern void *memcpy(void *d, const void *s, unsigned int n);

#define EINVAL 22
#define ENODEV 19
#define EFAULT 14

long sys_netcfg(int op, unsigned long a1, unsigned long a2, unsigned long a3,
                unsigned long a4)
{
    struct netif *nif = netif_default();
    struct netcfg_info *out;
    unsigned int i;

    if (!nif || !nif->dev)
        return -ENODEV;

    switch (op) {
    case NETCFG_GET:
        out = (struct netcfg_info *)(uintptr)a1;
        if (!out)
            return -EFAULT;
        memset(out, 0, sizeof(*out));
        out->ip = nif->ip;
        out->netmask = nif->netmask;
        out->gateway = nif->gateway;
        out->link_up = nif->link_up;
        out->configured = nif->configured;
        for (i = 0; i < 6; i++)
            out->mac[i] = nif->mac[i];
        if (nif->dev->name[0]) {
            for (i = 0; i < sizeof(out->name) - 1 && nif->dev->name[i]; i++)
                out->name[i] = nif->dev->name[i];
            out->name[i] = 0;
        } else {
            out->name[0] = 'e';
            out->name[1] = 't';
            out->name[2] = 'h';
            out->name[3] = '0';
            out->name[4] = 0;
        }
        return 0;

    case NETCFG_SET_ADDR:
        /* a1=ip a2=mask a3=gw (host order); 0 means "leave unchanged". */
        {
            unsigned int ip = a1 ? (unsigned int)a1 : nif->ip;
            unsigned int mask = a2 ? (unsigned int)a2 : nif->netmask;
            unsigned int gw = a3 ? (unsigned int)a3 : nif->gateway;
            netif_set_addr(nif, ip, mask, gw);
            if (nif->link_up)
                netif_set_up(nif);
        }
        return 0;

    case NETCFG_SET_UP:
        netif_set_up(nif);
        return 0;

    case NETCFG_SET_DOWN:
        nif->link_up = 0;
        return 0;

    case NETCFG_SET_GW:
        if (!a1)
            return -EINVAL;
        nif->gateway = (unsigned int)a1;
        return 0;

    default:
        return -EINVAL;
    }
}
