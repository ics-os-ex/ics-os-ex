#include "udp.h"
#include "netif.h"
#include "pbuf.h"
#include "ipv4.h"
#include "net_endian.h"

extern void *memcpy(void *d, const void *s, unsigned int n);
extern int memcmp(const void *s1, const void *s2, unsigned int n);
extern int printf(const char *fmt, ...);

static volatile int udp_wait;
static volatile unsigned short udp_wait_sport;
static volatile unsigned short udp_wait_dport;
static unsigned char udp_wait_payload[64];
static unsigned int udp_wait_plen;

int udp_send(struct netif *nif, unsigned int dst_host,
             unsigned short src_port, unsigned short dst_port,
             const unsigned char *payload, unsigned int payload_len)
{
    struct pbuf *p;
    unsigned int udp_len;

    if (!nif || !nif->configured || !payload)
        return -1;
    if (UDP_HDR_LEN + payload_len > PBUF_SIZE)
        return -1;

    p = pbuf_alloc((u16)(UDP_HDR_LEN + payload_len));
    if (!p)
        return -1;
    udp_len = udp_build(p->data, src_port, dst_port, payload, payload_len,
                        nif->ip, dst_host);
    p->len = (u16)udp_len;
    return ipv4_output(nif, p, dst_host, IPV4_PROTO_UDP);
}

void udp_input(struct netif *nif, struct pbuf *p, unsigned int ip_hdr_len)
{
    struct ipv4_hdr *ip;
    struct udp_hdr *uh;
    unsigned int udp_len, payload_len;
    unsigned short sport, dport;
    const unsigned char *payload;

    if (!nif || !p || p->len < ip_hdr_len + UDP_HDR_LEN) {
        pbuf_free(p);
        return;
    }
    ip = (struct ipv4_hdr *)p->data;
    uh = (struct udp_hdr *)(p->data + ip_hdr_len);
    udp_len = net_ntohs(uh->length);
    if (udp_len < UDP_HDR_LEN || ip_hdr_len + udp_len > p->len) {
        pbuf_free(p);
        return;
    }
    sport = net_ntohs(uh->src_port);
    dport = net_ntohs(uh->dst_port);
    payload = p->data + ip_hdr_len + UDP_HDR_LEN;
    payload_len = udp_len - UDP_HDR_LEN;

    /* Guest echo server (port 7). */
    if (dport == UDP_ECHO_PORT && nif->configured) {
        unsigned int peer = net_ntohl(ip->src);
        (void)udp_send(nif, peer, UDP_ECHO_PORT, sport, payload, payload_len);
        pbuf_free(p);
        return;
    }

    /* Outstanding echo-client reply. */
    if (udp_wait && dport == udp_wait_sport && sport == udp_wait_dport &&
        payload_len == udp_wait_plen &&
        memcmp(payload, udp_wait_payload, payload_len) == 0) {
        udp_wait = 0;
    }

    pbuf_free(p);
}

int udp_echo_client(struct netif *nif, unsigned int dst_host,
                    unsigned short dst_port, unsigned int timeout_spins)
{
    static const unsigned char payload[] = "ICS-UDP!";
    unsigned short local_port = 40000;
    unsigned int spins;

    if (!nif || !nif->configured)
        return -1;

    udp_wait_plen = sizeof(payload) - 1;
    memcpy(udp_wait_payload, payload, udp_wait_plen);
    udp_wait_sport = local_port;
    udp_wait_dport = dst_port;
    udp_wait = 1;

    if (udp_send(nif, dst_host, local_port, dst_port, payload, udp_wait_plen) != 0) {
        udp_wait = 0;
        return -1;
    }

    spins = 0;
    while (udp_wait && spins++ < timeout_spins) {
        netif_poll(nif);
        __asm__ volatile ("pause");
    }
    if (udp_wait) {
        udp_wait = 0;
        return -1;
    }
    return 0;
}
