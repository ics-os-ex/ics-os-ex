#include "icmp.h"
#include "netif.h"
#include "pbuf.h"
#include "ipv4.h"
#include "arp.h"
#include "ethernet.h"
#include "net_endian.h"
#include "net_sync.h"

extern void *memcpy(void *d, const void *s, unsigned int n);
extern int printf(const char *fmt, ...);
extern unsigned int ticks;

static volatile int ping_waiting;
static volatile unsigned short ping_id;
static volatile unsigned short ping_seq;
static volatile unsigned int ping_src; /* host order, who replied */

void icmp_input(struct netif *nif, struct pbuf *p, unsigned int ip_hdr_len)
{
    struct ipv4_hdr *ip;
    struct icmp_echo_hdr *ic;
    unsigned int icmp_len;
    struct pbuf *reply_payload;
    unsigned int dst;

    if (!nif || !p || p->len < ip_hdr_len + ICMP_HDR_LEN) {
        pbuf_free(p);
        return;
    }
    ip = (struct ipv4_hdr *)p->data;
    ic = (struct icmp_echo_hdr *)(p->data + ip_hdr_len);
    icmp_len = p->len - ip_hdr_len;

    if (ic->type == ICMP_TYPE_ECHO_REQUEST && ic->code == 0 &&
        nif->configured) {
        /* Reply using transform on a copy sized to the IP datagram. */
        if (!icmp_echo_reply_transform(p->data, p->len)) {
            pbuf_free(p);
            return;
        }
        /* Send the transformed IP packet via ethernet (skip ipv4_output). */
        {
            unsigned char dmac[ETH_ADDR_LEN];
            dst = net_ntohl(ip->dst); /* after swap, dst is original requester */
            /* After transform: src=us, dst=requester. Destination for ARP is
               the requester (current ip->dst). */
            (void)dst;
            dst = net_ntohl(((struct ipv4_hdr *)p->data)->dst);
            if (arp_resolve(nif, dst, dmac, 200) != 0) {
                pbuf_free(p);
                return;
            }
            /* ethernet_output always consumes p. */
            (void)ethernet_output(nif, p, dmac, ETH_TYPE_IPV4);
            return;
        }
    }

    if (ic->type == ICMP_TYPE_ECHO_REPLY && ic->code == 0) {
        if (ping_waiting &&
            net_ntohs(ic->id) == ping_id &&
            net_ntohs(ic->seq) == ping_seq) {
            ping_src = net_ntohl(ip->src);
            ping_waiting = 0;
        }
        pbuf_free(p);
        return;
    }

    (void)reply_payload;
    (void)icmp_len;
    pbuf_free(p);
}

int icmp_ping(struct netif *nif, unsigned int dst_host, unsigned int timeout_ticks)
{
    static const unsigned char payload[8] = {
        'I', 'C', 'S', '-', 'O', 'S', '!', '!'
    };
    unsigned char buf[ICMP_HDR_LEN + 8];
    struct pbuf *p;
    unsigned int icmp_len;
    unsigned int start;
    unsigned int spins;
    int ret = -1;

    net_lock();
    if (!nif || !nif->configured)
        goto out;

    ping_id = 0x1C50;
    ping_seq++;
    ping_waiting = 1;
    ping_src = 0;

    icmp_len = icmp_build_echo_request(buf, ping_id, ping_seq, payload, 8);
    p = pbuf_alloc((u16)icmp_len);
    if (!p) {
        ping_waiting = 0;
        goto out;
    }
    memcpy(p->data, buf, icmp_len);
    if (ipv4_output(nif, p, dst_host, IPV4_PROTO_ICMP) != 0) {
        ping_waiting = 0;
        goto out;
    }

    start = ticks;
    spins = 0;
    while (ping_waiting) {
        netif_poll(nif);
        if (ticks != start && ticks - start >= timeout_ticks)
            break;
        if (++spins > 2000000)
            break;
        __asm__ volatile ("pause");
    }
    if (ping_waiting) {
        ping_waiting = 0;
        goto out;
    }
    (void)ping_src;
    ret = 0;
out:
    net_unlock();
    return ret;
}
