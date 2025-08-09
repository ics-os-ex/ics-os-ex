// Basic minimal network stack interfaces for ICS-OS
// Provides Ethernet frame parsing, ARP handling, IPv4 + ICMP echo reply.

#ifndef ICSOS_NET_H
#define ICSOS_NET_H

#include "../dextypes.h"

#define NET_ETH_TYPE_ARP 0x0806
#define NET_ETH_TYPE_IP  0x0800

// --- User-facing minimal syscall/event interface (experimental) ---
// Event types exposed to user space via sys_net_recv()
#define NET_EVENT_NONE            0
#define NET_EVENT_ARP_REQUEST     1
#define NET_EVENT_ARP_REPLY       2
#define NET_EVENT_ICMP_ECHO_REQ   3
#define NET_EVENT_ICMP_ECHO_REP   4
#define NET_EVENT_TCP_ESTABLISHED 5
#define NET_EVENT_TCP_DATA        6
#define NET_EVENT_TCP_CLOSED      7

// Structure returned by sys_net_info (subject to change)
struct net_info {
	uint32_t ip;        // host order
	uint32_t gateway;   // host order
	uint32_t netmask;   // host order
	uint8_t  mac[6];
	uint8_t  gateway_mac[6];
	uint32_t flags;     // bit0: gateway_mac_valid
};

// Public syscall registration (called after api_init())
void net_register_syscalls();

// Internal helper to enqueue an event (used inside net.c, exposed for future drivers)
void net_user_event(uint16_t type, const uint8_t *data, uint16_t len);

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
