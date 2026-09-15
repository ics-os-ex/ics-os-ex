#include "dns.h"
#include "udp.h"
#include "ipv4.h"
#include "pbuf.h"
#include "netif.h"
#include "net_endian.h"
#include "net_sync.h"

extern void *memcpy(void *d, const void *s, unsigned int n);
extern void *memset(void *s, int c, unsigned int n);
extern unsigned int ticks;

static volatile int dns_waiting;
static unsigned char dns_rx[DNS_MAX_PKT];
static volatile unsigned int dns_rx_len;
static unsigned short dns_wait_port;

/* Encode "www.example.com" as 3www7example3com0 */
static unsigned int dns_encode_name(unsigned char *dst, unsigned int max,
                                    const char *name)
{
    unsigned int o = 0;
    const char *p = name;

    if (!dst || !name || max < 2)
        return 0;
    while (*p) {
        const char *dot = p;
        unsigned int lab;
        while (*dot && *dot != '.')
            dot++;
        lab = (unsigned int)(dot - p);
        if (!lab || lab > 63 || o + 1 + lab + 1 > max)
            return 0;
        dst[o++] = (unsigned char)lab;
        memcpy(dst + o, p, lab);
        o += lab;
        p = *dot ? dot + 1 : dot;
    }
    dst[o++] = 0;
    return o;
}

static int dns_skip_name(const unsigned char *pkt, unsigned int len,
                         unsigned int *off)
{
    unsigned int o = *off;
    unsigned int hops = 0;

    while (o < len && hops++ < 64) {
        unsigned char c = pkt[o];
        if (c == 0) {
            *off = o + 1;
            return 0;
        }
        if ((c & 0xC0) == 0xC0) {
            if (o + 1 >= len)
                return -1;
            *off = o + 2;
            return 0;
        }
        if (o + 1 + c >= len)
            return -1;
        o += 1 + c;
    }
    return -1;
}

unsigned int dns_build_query(unsigned char *dst, unsigned int dst_max,
                             unsigned short id, const char *name)
{
    unsigned int nlen, total;

    if (!dst || dst_max < 18 || !name)
        return 0;
    memset(dst, 0, dst_max);
    dst[0] = (unsigned char)((id >> 8) & 0xFF);
    dst[1] = (unsigned char)(id & 0xFF);
    dst[2] = 0x01; /* RD */
    dst[3] = 0x00;
    dst[4] = 0x00;
    dst[5] = 0x01; /* QDCOUNT=1 */
    nlen = dns_encode_name(dst + 12, dst_max - 16, name);
    if (!nlen)
        return 0;
    dst[12 + nlen] = 0x00;
    dst[12 + nlen + 1] = 0x01; /* QTYPE A */
    dst[12 + nlen + 2] = 0x00;
    dst[12 + nlen + 3] = 0x01; /* QCLASS IN */
    total = 12 + nlen + 4;
    return total;
}

int dns_parse_a(const unsigned char *pkt, unsigned int len,
                unsigned short expect_id, unsigned int *ip_out)
{
    unsigned int off, ancount, i;
    unsigned short id, qtype, qclass, rdlen;

    if (!pkt || len < 12 || !ip_out)
        return -1;
    id = (unsigned short)((pkt[0] << 8) | pkt[1]);
    if (id != expect_id)
        return -1;
    if ((pkt[3] & 0x0F) != 0) /* RCODE */
        return -1;
    ancount = (unsigned int)((pkt[6] << 8) | pkt[7]);
    if (!ancount)
        return -1;

    off = 12;
    if (dns_skip_name(pkt, len, &off) != 0)
        return -1;
    if (off + 4 > len)
        return -1;
    off += 4; /* QTYPE+QCLASS */

    for (i = 0; i < ancount; i++) {
        if (dns_skip_name(pkt, len, &off) != 0)
            return -1;
        if (off + 10 > len)
            return -1;
        qtype = (unsigned short)((pkt[off] << 8) | pkt[off + 1]);
        qclass = (unsigned short)((pkt[off + 2] << 8) | pkt[off + 3]);
        rdlen = (unsigned short)((pkt[off + 8] << 8) | pkt[off + 9]);
        off += 10;
        if (off + rdlen > len)
            return -1;
        if (qtype == 1 && qclass == 1 && rdlen == 4) {
            *ip_out = ((unsigned int)pkt[off] << 24) |
                      ((unsigned int)pkt[off + 1] << 16) |
                      ((unsigned int)pkt[off + 2] << 8) |
                      (unsigned int)pkt[off + 3];
            return 0;
        }
        off += rdlen;
    }
    return -1;
}

int dns_udp_deliver(unsigned short dport, const unsigned char *payload,
                    unsigned int plen)
{
    if (!dns_waiting || dport != dns_wait_port || !payload || !plen)
        return 0;
    if (dns_rx_len)
        return 1;
    if (plen > sizeof(dns_rx))
        plen = sizeof(dns_rx);
    memcpy(dns_rx, payload, plen);
    dns_rx_len = plen;
    return 1;
}

int dns_query_a(struct netif *nif, unsigned int server_host,
                unsigned short server_port, const char *name,
                unsigned int *ip_out, unsigned int timeout_spins)
{
    unsigned char q[DNS_MAX_PKT];
    struct pbuf *udp;
    unsigned int qlen, udp_len, spins = 0;
    unsigned short id, sport;
    int ret = -1;

    if (!nif || !nif->configured || !name || !ip_out)
        return -1;

    net_lock();
    id = (unsigned short)(0xD500u ^ (unsigned short)ticks);
    qlen = dns_build_query(q, sizeof(q), id, name);
    if (!qlen)
        goto out;

    sport = (unsigned short)(43000u + (id & 0x3FFu));
    dns_wait_port = sport;
    dns_rx_len = 0;
    dns_waiting = 1;

    udp = pbuf_alloc((u16)(UDP_HDR_LEN + qlen));
    if (!udp)
        goto out_wait;
    udp_len = udp_build(udp->data, sport, server_port, q, qlen,
                        nif->ip, server_host);
    udp->len = (u16)udp_len;
    if (ipv4_output(nif, udp, server_host, IPV4_PROTO_UDP) != 0)
        goto out_wait;

    while (spins++ < timeout_spins) {
        netif_poll(nif);
        if (dns_rx_len) {
            if (dns_parse_a(dns_rx, dns_rx_len, id, ip_out) == 0) {
                ret = 0;
                break;
            }
            dns_rx_len = 0;
        }
        __asm__ volatile ("pause");
    }
out_wait:
    dns_waiting = 0;
    dns_rx_len = 0;
out:
    net_unlock();
    return ret;
}
