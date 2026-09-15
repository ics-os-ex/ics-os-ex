#ifndef ICSOS_NET_SYNC_H
#define ICSOS_NET_SYNC_H

/*
 * Softnet biglock: serializes protocol state (TCP PCBs, UDP socks, ARP cache,
 * ICMP/UDP wait flags, ipv4 id) against IRQ RX delivery and concurrent
 * socket syscalls on other CPUs.
 *
 * Recursive on the same CPU so tcp_input → arp_resolve → netif_poll →
 * deliver can re-enter. Outermost acquire is irqsave.
 *
 * Lock order: net_lock → virtio-net device lock (never reverse).
 * pbuf uses its own spinlock and may nest inside either.
 */

#include "../cpu/spinlock.h"

void net_lock(void);
void net_unlock(void);
int  net_lock_held(void);

#endif
