#ifndef ICSOS_NET_UDP_H
#define ICSOS_NET_UDP_H

#include "checksum.h"
#include "ipv4.h"
#include "net_endian.h"

#define UDP_HDR_LEN       8
#define UDP_ECHO_PORT     7
#define UDP_TEST_PORT     7777

struct udp_hdr {
    unsigned short src_port;
    unsigned short dst_port;
    unsigned short length;
    unsigned short checksum;
} __attribute__((packed));

/*
 * UDP checksum over IPv4 pseudo-header + UDP header + payload.
 * src_ip/dst_ip are host-order. Returns the value to store with htons,
 * or 0xFFFF represented as 0xFFFF (never store 0 — use 0xFFFF if sum is 0).
 */
static inline unsigned short udp_checksum(unsigned int src_ip_host,
                                          unsigned int dst_ip_host,
                                          const unsigned char *udp,
                                          unsigned int udp_len)
{
    unsigned int sum = 0;
    unsigned char ph[12];

    ph[0] = (unsigned char)((src_ip_host >> 24) & 0xFF);
    ph[1] = (unsigned char)((src_ip_host >> 16) & 0xFF);
    ph[2] = (unsigned char)((src_ip_host >> 8) & 0xFF);
    ph[3] = (unsigned char)(src_ip_host & 0xFF);
    ph[4] = (unsigned char)((dst_ip_host >> 24) & 0xFF);
    ph[5] = (unsigned char)((dst_ip_host >> 16) & 0xFF);
    ph[6] = (unsigned char)((dst_ip_host >> 8) & 0xFF);
    ph[7] = (unsigned char)(dst_ip_host & 0xFF);
    ph[8] = 0;
    ph[9] = IPV4_PROTO_UDP;
    ph[10] = (unsigned char)((udp_len >> 8) & 0xFF);
    ph[11] = (unsigned char)(udp_len & 0xFF);

    sum = net_checksum_add(ph, 12, 0);
    sum = net_checksum_add(udp, udp_len, sum);
    {
        unsigned short c = net_checksum_fold(sum);
        return c ? c : 0xFFFF;
    }
}

/* Build UDP datagram into dst; checksum field filled. Returns total bytes. */
static inline unsigned int udp_build(unsigned char *dst,
                                     unsigned short src_port,
                                     unsigned short dst_port,
                                     const unsigned char *payload,
                                     unsigned int payload_len,
                                     unsigned int src_ip_host,
                                     unsigned int dst_ip_host)
{
    struct udp_hdr *h = (struct udp_hdr *)dst;
    unsigned int i;
    unsigned int udp_len = UDP_HDR_LEN + payload_len;

    h->src_port = net_htons(src_port);
    h->dst_port = net_htons(dst_port);
    h->length = net_htons((unsigned short)udp_len);
    h->checksum = 0;
    for (i = 0; i < payload_len; i++)
        dst[UDP_HDR_LEN + i] = payload[i];
    h->checksum = net_htons(udp_checksum(src_ip_host, dst_ip_host, dst, udp_len));
    return udp_len;
}

struct netif;
struct pbuf;

void udp_input(struct netif *nif, struct pbuf *p, unsigned int ip_hdr_len);
int  udp_send(struct netif *nif, unsigned int dst_host,
              unsigned short src_port, unsigned short dst_port,
              const unsigned char *payload, unsigned int payload_len);
int  udp_echo_client(struct netif *nif, unsigned int dst_host,
                     unsigned short dst_port, unsigned int timeout_spins);

/* Deliver inbound UDP to a bound user socket (returns 1 if consumed). */
int  sock_udp_deliver(unsigned short dport, unsigned short sport,
                      unsigned int sip, const unsigned char *payload,
                      unsigned int plen);

#endif
