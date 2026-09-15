#ifndef ICSOS_NET_DNS_H
#define ICSOS_NET_DNS_H

#include "netif.h"

#define DNS_PORT          53
#define DNS_TEST_PORT     5353
#define DNS_MAX_NAME      256
#define DNS_MAX_PKT       512

/* Pure helpers (unit-testable). */
unsigned int dns_build_query(unsigned char *dst, unsigned int dst_max,
                             unsigned short id, const char *name);
int dns_parse_a(const unsigned char *pkt, unsigned int len,
                unsigned short expect_id, unsigned int *ip_out);

/* UDP query to server_host:server_port for an A record. */
int dns_query_a(struct netif *nif, unsigned int server_host,
                unsigned short server_port, const char *name,
                unsigned int *ip_out, unsigned int timeout_spins);
/* Deliver inbound DNS reply to an outstanding query. */
int dns_udp_deliver(unsigned short dport, const unsigned char *payload,
                    unsigned int plen);

#endif
