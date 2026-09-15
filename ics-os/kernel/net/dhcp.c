#include "dhcp.h"
#include "udp.h"
#include "ipv4.h"
#include "netif.h"
#include "pbuf.h"
#include "ethernet.h"
#include "net_endian.h"
#include "net_sync.h"

extern void *memcpy(void *d, const void *s, unsigned int n);
extern void *memset(void *s, int c, unsigned int n);
extern int printf(const char *fmt, ...);

static const unsigned char eth_broadcast[6] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

static volatile int dhcp_waiting;
static unsigned char dhcp_rx[576];
static volatile unsigned int dhcp_rx_len;

static void opt_put(unsigned char **pp, unsigned char code,
                    unsigned char len, const unsigned char *val)
{
    unsigned char *p = *pp;
    unsigned int i;
    *p++ = code;
    *p++ = len;
    for (i = 0; i < len; i++)
        *p++ = val[i];
    *pp = p;
}

static void opt_put_u8(unsigned char **pp, unsigned char code, unsigned char v)
{
    opt_put(pp, code, 1, &v);
}

static void opt_put_u32_be(unsigned char **pp, unsigned char code,
                           unsigned int host)
{
    unsigned char v[4];
    v[0] = (unsigned char)((host >> 24) & 0xFF);
    v[1] = (unsigned char)((host >> 16) & 0xFF);
    v[2] = (unsigned char)((host >> 8) & 0xFF);
    v[3] = (unsigned char)(host & 0xFF);
    opt_put(pp, code, 4, v);
}

static unsigned int dhcp_build_common(unsigned char *dst, unsigned int dst_max,
                                      unsigned int xid,
                                      const unsigned char mac[6],
                                      unsigned char **opt_out)
{
    struct dhcp_msg *m;
    unsigned char *opt;
    unsigned int i;

    if (!dst || dst_max < DHCP_MIN_PKT + 16 || !mac)
        return 0;
    memset(dst, 0, dst_max);
    m = (struct dhcp_msg *)dst;
    m->op = DHCP_BOOTREQUEST;
    m->htype = 1;
    m->hlen = 6;
    m->xid = net_htonl(xid);
    m->flags = net_htons(0x8000); /* broadcast */
    for (i = 0; i < 6; i++)
        m->chaddr[i] = mac[i];

    opt = dst + DHCP_FIXED_LEN;
    *opt++ = DHCP_MAGIC_0;
    *opt++ = DHCP_MAGIC_1;
    *opt++ = DHCP_MAGIC_2;
    *opt++ = DHCP_MAGIC_3;
    *opt_out = opt;
    return DHCP_FIXED_LEN + 4;
}

unsigned int dhcp_build_discover(unsigned char *dst, unsigned int dst_max,
                                 unsigned int xid,
                                 const unsigned char mac[6])
{
    unsigned char *opt;
    unsigned int base;
    unsigned char prl[3];

    base = dhcp_build_common(dst, dst_max, xid, mac, &opt);
    if (!base)
        return 0;
    opt_put_u8(&opt, DHCP_OPT_MSG_TYPE, DHCP_DISCOVER);
    prl[0] = DHCP_OPT_SUBNET_MASK;
    prl[1] = DHCP_OPT_ROUTER;
    prl[2] = DHCP_OPT_LEASE_TIME;
    opt_put(&opt, DHCP_OPT_PARAM_REQ, 3, prl);
    *opt++ = DHCP_OPT_END;
    return (unsigned int)(opt - dst);
}

unsigned int dhcp_build_request(unsigned char *dst, unsigned int dst_max,
                                unsigned int xid,
                                const unsigned char mac[6],
                                unsigned int req_ip_host,
                                unsigned int server_host)
{
    unsigned char *opt;
    unsigned int base;
    unsigned char prl[3];

    base = dhcp_build_common(dst, dst_max, xid, mac, &opt);
    if (!base)
        return 0;
    opt_put_u8(&opt, DHCP_OPT_MSG_TYPE, DHCP_REQUEST);
    opt_put_u32_be(&opt, DHCP_OPT_REQ_IP, req_ip_host);
    opt_put_u32_be(&opt, DHCP_OPT_SERVER_ID, server_host);
    prl[0] = DHCP_OPT_SUBNET_MASK;
    prl[1] = DHCP_OPT_ROUTER;
    prl[2] = DHCP_OPT_LEASE_TIME;
    opt_put(&opt, DHCP_OPT_PARAM_REQ, 3, prl);
    *opt++ = DHCP_OPT_END;
    return (unsigned int)(opt - dst);
}

static unsigned int read_u32_be(const unsigned char *p)
{
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
           ((unsigned int)p[2] << 8) | (unsigned int)p[3];
}

int dhcp_parse_reply(const unsigned char *pkt, unsigned int len,
                     unsigned int expect_xid,
                     unsigned char *msg_type_out,
                     struct dhcp_lease *lease)
{
    const struct dhcp_msg *m;
    const unsigned char *opt, *end;
    unsigned char msg_type = 0;

    if (!pkt || len < DHCP_MIN_PKT || !msg_type_out || !lease)
        return -1;
    m = (const struct dhcp_msg *)pkt;
    if (m->op != DHCP_BOOTREPLY || m->htype != 1 || m->hlen != 6)
        return -1;
    if (net_ntohl(m->xid) != expect_xid)
        return -1;
    opt = pkt + DHCP_FIXED_LEN;
    if (opt[0] != DHCP_MAGIC_0 || opt[1] != DHCP_MAGIC_1 ||
        opt[2] != DHCP_MAGIC_2 || opt[3] != DHCP_MAGIC_3)
        return -1;
    opt += 4;
    end = pkt + len;
    memset(lease, 0, sizeof(*lease));
    lease->ip = net_ntohl(m->yiaddr);

    while (opt < end) {
        unsigned char code = *opt++;
        unsigned char olen;
        if (code == DHCP_OPT_PAD)
            continue;
        if (code == DHCP_OPT_END)
            break;
        if (opt >= end)
            return -1;
        olen = *opt++;
        if (opt + olen > end)
            return -1;
        if (code == DHCP_OPT_MSG_TYPE && olen == 1)
            msg_type = opt[0];
        else if (code == DHCP_OPT_SUBNET_MASK && olen == 4)
            lease->netmask = read_u32_be(opt);
        else if (code == DHCP_OPT_ROUTER && olen >= 4)
            lease->gateway = read_u32_be(opt);
        else if (code == DHCP_OPT_SERVER_ID && olen == 4)
            lease->server = read_u32_be(opt);
        else if (code == DHCP_OPT_LEASE_TIME && olen == 4)
            lease->lease_secs = read_u32_be(opt);
        opt += olen;
    }
    if (!msg_type || !lease->ip)
        return -1;
    *msg_type_out = msg_type;
    return 0;
}

int dhcp_udp_deliver(unsigned short dport, const unsigned char *payload,
                     unsigned int plen)
{
    if (!dhcp_waiting || dport != DHCP_CLIENT_PORT || !payload || !plen)
        return 0;
    if (dhcp_rx_len)
        return 1;
    if (plen > sizeof(dhcp_rx))
        plen = sizeof(dhcp_rx);
    memcpy(dhcp_rx, payload, plen);
    dhcp_rx_len = plen;
    return 1;
}

/* Broadcast UDP without requiring nif->configured / ARP. */
static int dhcp_xmit(struct netif *nif, const unsigned char *payload,
                     unsigned int plen)
{
    struct pbuf *udp, *pkt;
    struct ipv4_hdr *ip;
    unsigned int udp_len, total;
    unsigned char dmac[6];
    unsigned int i;

    if (!nif || !payload || !plen)
        return -1;
    if (UDP_HDR_LEN + plen > PBUF_SIZE)
        return -1;

    udp = pbuf_alloc((u16)(UDP_HDR_LEN + plen));
    if (!udp)
        return -1;
    udp_len = udp_build(udp->data, DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
                        payload, plen, 0 /* src */, 0xFFFFFFFFu);
    udp->len = (u16)udp_len;

    total = IPV4_HDR_MIN + udp->len;
    pkt = pbuf_alloc((u16)total);
    if (!pkt) {
        pbuf_free(udp);
        return -1;
    }
    ip = (struct ipv4_hdr *)pkt->data;
    ip->ver_ihl = IPV4_VERSION_IHL;
    ip->tos = 0;
    ip->total_len = net_htons((unsigned short)total);
    ip->id = net_htons(1);
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->proto = IPV4_PROTO_UDP;
    ip->src = 0;
    ip->dst = net_htonl(0xFFFFFFFFu);
    ipv4_set_checksum(ip);
    memcpy(pkt->data + IPV4_HDR_MIN, udp->data, udp->len);
    pbuf_free(udp);

    for (i = 0; i < 6; i++)
        dmac[i] = eth_broadcast[i];
    return ethernet_output(nif, pkt, dmac, ETH_TYPE_IPV4);
}

static int dhcp_wait_reply(struct netif *nif, unsigned int xid,
                           unsigned char want_type, struct dhcp_lease *lease,
                           unsigned int timeout_spins)
{
    unsigned int spins = 0;
    unsigned char mtype;

    dhcp_rx_len = 0;
    dhcp_waiting = 1;
    while (spins++ < timeout_spins) {
        netif_poll(nif);
        if (dhcp_rx_len) {
            if (dhcp_parse_reply(dhcp_rx, dhcp_rx_len, xid, &mtype, lease) == 0 &&
                mtype == want_type) {
                dhcp_waiting = 0;
                dhcp_rx_len = 0;
                return 0;
            }
            dhcp_rx_len = 0;
        }
        __asm__ volatile ("pause");
    }
    dhcp_waiting = 0;
    return -1;
}

int dhcp_client(struct netif *nif, struct inet_config *cfg,
                unsigned int timeout_spins)
{
    unsigned char pkt[512];
    unsigned int len, xid;
    struct dhcp_lease offer, ack;
    int ret = -1;

    if (!nif || !cfg || !nif->mac)
        return -1;

    net_lock();
    xid = 0xC0DE0000u ^ ((unsigned int)nif->mac[4] << 8) ^ nif->mac[5];
    if (!xid)
        xid = 0x12345678u;

    len = dhcp_build_discover(pkt, sizeof(pkt), xid, nif->mac);
    if (!len || dhcp_xmit(nif, pkt, len) != 0)
        goto out;
    if (dhcp_wait_reply(nif, xid, DHCP_OFFER, &offer, timeout_spins) != 0)
        goto out;
    if (!offer.server)
        offer.server = NET_SLIRP_GATEWAY;

    len = dhcp_build_request(pkt, sizeof(pkt), xid, nif->mac,
                             offer.ip, offer.server);
    if (!len || dhcp_xmit(nif, pkt, len) != 0)
        goto out;
    if (dhcp_wait_reply(nif, xid, DHCP_ACK, &ack, timeout_spins) != 0)
        goto out;

    cfg->ip = ack.ip;
    cfg->netmask = ack.netmask ? ack.netmask : NET_SLIRP_MASK;
    cfg->gateway = ack.gateway ? ack.gateway : offer.server;
    ret = 0;
out:
    net_unlock();
    return ret;
}
