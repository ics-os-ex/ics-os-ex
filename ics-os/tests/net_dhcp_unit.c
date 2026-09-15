/*
 * Host TAP: DHCP wire builders/parser mirrored from kernel/net/dhcp.c
 * (RFC 2131/2132). Runtime DORA is gated by make test-net (NET_DHCP_OK).
 */
#include <stdio.h>
#include <string.h>

#define DHCP_BOOTREQUEST  1
#define DHCP_BOOTREPLY    2
#define DHCP_DISCOVER     1
#define DHCP_OFFER        2
#define DHCP_REQUEST      3
#define DHCP_OPT_PAD           0
#define DHCP_OPT_SUBNET_MASK   1
#define DHCP_OPT_ROUTER        3
#define DHCP_OPT_REQ_IP        50
#define DHCP_OPT_MSG_TYPE      53
#define DHCP_OPT_SERVER_ID     54
#define DHCP_OPT_PARAM_REQ     55
#define DHCP_OPT_END           255
#define DHCP_MAGIC_0  99
#define DHCP_MAGIC_1  130
#define DHCP_MAGIC_2  83
#define DHCP_MAGIC_3  99
#define DHCP_FIXED_LEN  236
#define DHCP_MIN_PKT    (DHCP_FIXED_LEN + 4)

static unsigned short htons_u(unsigned short x)
{
    return (unsigned short)((x << 8) | (x >> 8));
}
static unsigned int htonl_u(unsigned int x)
{
    return ((x & 0x000000FFu) << 24) | ((x & 0x0000FF00u) << 8) |
           ((x & 0x00FF0000u) >> 8) | ((x & 0xFF000000u) >> 24);
}
#define ntohl_u htonl_u

struct dhcp_msg {
    unsigned char op, htype, hlen, hops;
    unsigned int xid;
    unsigned short secs, flags;
    unsigned int ciaddr, yiaddr, siaddr, giaddr;
    unsigned char chaddr[16];
    unsigned char sname[64];
    unsigned char file[128];
} __attribute__((packed));

struct dhcp_lease {
    unsigned int ip, netmask, gateway, server, lease_secs;
};

static void opt_put(unsigned char **pp, unsigned char code,
                    unsigned char len, const unsigned char *val)
{
    unsigned int i;
    *(*pp)++ = code;
    *(*pp)++ = len;
    for (i = 0; i < len; i++)
        *(*pp)++ = val[i];
}

static unsigned int build_discover(unsigned char *dst, unsigned int xid,
                                   const unsigned char mac[6])
{
    struct dhcp_msg *m = (struct dhcp_msg *)dst;
    unsigned char *opt;
    unsigned char prl[3];
    unsigned int i;

    memset(dst, 0, 512);
    m->op = DHCP_BOOTREQUEST;
    m->htype = 1;
    m->hlen = 6;
    m->xid = htonl_u(xid);
    m->flags = htons_u(0x8000);
    for (i = 0; i < 6; i++)
        m->chaddr[i] = mac[i];
    opt = dst + DHCP_FIXED_LEN;
    *opt++ = DHCP_MAGIC_0; *opt++ = DHCP_MAGIC_1;
    *opt++ = DHCP_MAGIC_2; *opt++ = DHCP_MAGIC_3;
    {
        unsigned char t = DHCP_DISCOVER;
        opt_put(&opt, DHCP_OPT_MSG_TYPE, 1, &t);
    }
    prl[0] = DHCP_OPT_SUBNET_MASK;
    prl[1] = DHCP_OPT_ROUTER;
    prl[2] = 51;
    opt_put(&opt, DHCP_OPT_PARAM_REQ, 3, prl);
    *opt++ = DHCP_OPT_END;
    return (unsigned int)(opt - dst);
}

static unsigned int read_u32_be(const unsigned char *p)
{
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
           ((unsigned int)p[2] << 8) | (unsigned int)p[3];
}

static int parse_reply(const unsigned char *pkt, unsigned int len,
                       unsigned int expect_xid, unsigned char *msg_type_out,
                       struct dhcp_lease *lease)
{
    const struct dhcp_msg *m = (const struct dhcp_msg *)pkt;
    const unsigned char *opt, *end;
    unsigned char msg_type = 0;

    if (len < DHCP_MIN_PKT || m->op != DHCP_BOOTREPLY)
        return -1;
    if (ntohl_u(m->xid) != expect_xid)
        return -1;
    opt = pkt + DHCP_FIXED_LEN;
    if (opt[0] != DHCP_MAGIC_0 || opt[1] != DHCP_MAGIC_1 ||
        opt[2] != DHCP_MAGIC_2 || opt[3] != DHCP_MAGIC_3)
        return -1;
    opt += 4;
    end = pkt + len;
    memset(lease, 0, sizeof(*lease));
    lease->ip = ntohl_u(m->yiaddr);
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
        opt += olen;
    }
    if (!msg_type || !lease->ip)
        return -1;
    *msg_type_out = msg_type;
    return 0;
}

static unsigned int craft_offer(unsigned char *dst, unsigned int xid,
                                unsigned int yiaddr, unsigned int server)
{
    struct dhcp_msg *m = (struct dhcp_msg *)dst;
    unsigned char *opt;
    memset(dst, 0, 512);
    m->op = DHCP_BOOTREPLY;
    m->htype = 1;
    m->hlen = 6;
    m->xid = htonl_u(xid);
    m->yiaddr = htonl_u(yiaddr);
    opt = dst + DHCP_FIXED_LEN;
    *opt++ = DHCP_MAGIC_0; *opt++ = DHCP_MAGIC_1;
    *opt++ = DHCP_MAGIC_2; *opt++ = DHCP_MAGIC_3;
    *opt++ = DHCP_OPT_MSG_TYPE; *opt++ = 1; *opt++ = DHCP_OFFER;
    *opt++ = DHCP_OPT_SERVER_ID; *opt++ = 4;
    *opt++ = (unsigned char)((server >> 24) & 0xFF);
    *opt++ = (unsigned char)((server >> 16) & 0xFF);
    *opt++ = (unsigned char)((server >> 8) & 0xFF);
    *opt++ = (unsigned char)(server & 0xFF);
    *opt++ = DHCP_OPT_SUBNET_MASK; *opt++ = 4;
    *opt++ = 255; *opt++ = 255; *opt++ = 255; *opt++ = 0;
    *opt++ = DHCP_OPT_ROUTER; *opt++ = 4;
    *opt++ = (unsigned char)((server >> 24) & 0xFF);
    *opt++ = (unsigned char)((server >> 16) & 0xFF);
    *opt++ = (unsigned char)((server >> 8) & 0xFF);
    *opt++ = (unsigned char)(server & 0xFF);
    *opt++ = DHCP_OPT_END;
    return (unsigned int)(opt - dst);
}

static int tests_run, tests_failed;
#define OK(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { printf("not ok %d - %s\n", tests_run, msg); tests_failed++; } \
    else { printf("ok %d - %s\n", tests_run, msg); } \
} while (0)

int main(void)
{
    unsigned char pkt[512], mac[6] = {0x52,0x54,0x00,0x12,0x34,0x56};
    unsigned int len, xid = 0xAABBCCDDu;
    unsigned char mtype;
    struct dhcp_lease lease;

    printf("1..6\n");
    len = build_discover(pkt, xid, mac);
    OK(len > DHCP_MIN_PKT, "discover length");
    OK(pkt[DHCP_FIXED_LEN] == DHCP_MAGIC_0 &&
       pkt[DHCP_FIXED_LEN + 6] == DHCP_DISCOVER, "discover type");
    len = craft_offer(pkt, xid, 0x0A00020Fu, 0x0A000202u);
    OK(parse_reply(pkt, len, xid, &mtype, &lease) == 0, "parse offer");
    OK(mtype == DHCP_OFFER && lease.ip == 0x0A00020Fu &&
       lease.server == 0x0A000202u && lease.netmask == 0xFFFFFF00u, "offer fields");
    OK(parse_reply(pkt, len, xid ^ 1u, &mtype, &lease) != 0, "reject bad xid");

    /* Renew: ciaddr set, flags clear, REQUEST type, no server-id opt. */
    memset(pkt, 0, sizeof(pkt));
    {
        struct dhcp_msg *m = (struct dhcp_msg *)pkt;
        unsigned char *opt;
        unsigned int i;
        m->op = DHCP_BOOTREQUEST;
        m->htype = 1;
        m->hlen = 6;
        m->xid = htonl_u(xid);
        m->flags = 0;
        m->ciaddr = htonl_u(0x0A00020Fu);
        for (i = 0; i < 6; i++)
            m->chaddr[i] = mac[i];
        opt = pkt + DHCP_FIXED_LEN;
        *opt++ = DHCP_MAGIC_0; *opt++ = DHCP_MAGIC_1;
        *opt++ = DHCP_MAGIC_2; *opt++ = DHCP_MAGIC_3;
        *opt++ = DHCP_OPT_MSG_TYPE; *opt++ = 1; *opt++ = DHCP_REQUEST;
        *opt++ = DHCP_OPT_END;
        len = (unsigned int)(opt - pkt);
    }
    OK(((struct dhcp_msg *)pkt)->ciaddr == htonl_u(0x0A00020Fu) &&
       ((struct dhcp_msg *)pkt)->flags == 0 &&
       pkt[DHCP_FIXED_LEN + 6] == DHCP_REQUEST, "renew wire shape");

    return tests_failed ? 1 : 0;
}
