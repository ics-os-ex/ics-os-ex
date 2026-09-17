#ifndef ICSOS_NET_NETCFG_H
#define ICSOS_NET_NETCFG_H

/*
 * Userspace net configuration (single default NIC).
 * DEX syscall 0xCF: sys_netcfg(op, arg1, arg2, arg3, arg4)
 */

#define NETCFG_GET        1
#define NETCFG_SET_ADDR   2
#define NETCFG_SET_UP     3
#define NETCFG_SET_DOWN   4
#define NETCFG_SET_GW     5

/* Filled by NETCFG_GET; all IPv4 fields are host order. */
struct netcfg_info {
    unsigned int ip;
    unsigned int netmask;
    unsigned int gateway;
    unsigned char mac[6];
    unsigned char link_up;
    unsigned char configured;
    char name[16];
};

long sys_netcfg(int op, unsigned long a1, unsigned long a2, unsigned long a3,
                unsigned long a4);

#endif
