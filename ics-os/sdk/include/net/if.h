#ifndef _NET_IF_H
#define _NET_IF_H

/* ICS-OS userspace net configuration (syscall 0xCF). */

#define NETCFG_GET        1
#define NETCFG_SET_ADDR   2
#define NETCFG_SET_UP     3
#define NETCFG_SET_DOWN   4
#define NETCFG_SET_GW     5

struct netcfg_info {
    unsigned int ip;
    unsigned int netmask;
    unsigned int gateway;
    unsigned char mac[6];
    unsigned char link_up;
    unsigned char configured;
    char name[16];
};

int netcfg_get(struct netcfg_info *info);
int netcfg_set_addr(unsigned int ip, unsigned int mask, unsigned int gw);
int netcfg_set_up(void);
int netcfg_set_down(void);
int netcfg_set_gw(unsigned int gw);

#endif
