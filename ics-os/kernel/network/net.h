// Basic minimal network stack interfaces for ICS-OS
// Provides Ethernet frame parsing, ARP handling, IPv4 + ICMP echo reply.

#ifndef ICSOS_NET_H
#define ICSOS_NET_H

#include "../dextypes.h"

#define NET_ETH_TYPE_ARP 0x0806
#define NET_ETH_TYPE_IP  0x0800

// Our (static) network configuration for now (QEMU user net typical subnet)
extern uint8_t net_mac_addr[6];
extern uint32_t net_ip_addr;      // host order
extern uint32_t net_ip_gateway;   // host order
extern uint32_t net_ip_netmask;   // host order

void net_set_mac(const uint8_t *mac);
void net_init();
void ethernet_handle_packet(uint8_t *data, unsigned len);
void net_periodic();

// Debug utilities
void net_dump_mac(const uint8_t *m);

#endif
