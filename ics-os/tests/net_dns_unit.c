/*
 * Host TAP: DNS query builder / A-record parser mirrored from kernel/net/dns.c.
 */
#include <stdio.h>
#include <string.h>

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

static unsigned int dns_build_query(unsigned char *dst, unsigned int dst_max,
                                    unsigned short id, const char *name)
{
    unsigned int nlen, total;
    if (!dst || dst_max < 18 || !name)
        return 0;
    memset(dst, 0, dst_max);
    dst[0] = (unsigned char)((id >> 8) & 0xFF);
    dst[1] = (unsigned char)(id & 0xFF);
    dst[2] = 0x01;
    dst[5] = 0x01;
    nlen = dns_encode_name(dst + 12, dst_max - 16, name);
    if (!nlen)
        return 0;
    dst[12 + nlen + 1] = 0x01;
    dst[12 + nlen + 3] = 0x01;
    total = 12 + nlen + 4;
    return total;
}

static int dns_skip_name(const unsigned char *pkt, unsigned int len,
                         unsigned int *off)
{
    unsigned int o = *off, hops = 0;
    while (o < len && hops++ < 64) {
        unsigned char c = pkt[o];
        if (c == 0) { *off = o + 1; return 0; }
        if ((c & 0xC0) == 0xC0) {
            if (o + 1 >= len) return -1;
            *off = o + 2; return 0;
        }
        if (o + 1 + c >= len) return -1;
        o += 1 + c;
    }
    return -1;
}

static int dns_parse_a(const unsigned char *pkt, unsigned int len,
                       unsigned short expect_id, unsigned int *ip_out)
{
    unsigned int off, ancount, i;
    unsigned short id, qtype, qclass, rdlen;
    if (!pkt || len < 12 || !ip_out) return -1;
    id = (unsigned short)((pkt[0] << 8) | pkt[1]);
    if (id != expect_id) return -1;
    if ((pkt[3] & 0x0F) != 0) return -1;
    ancount = (unsigned int)((pkt[6] << 8) | pkt[7]);
    if (!ancount) return -1;
    off = 12;
    if (dns_skip_name(pkt, len, &off) != 0) return -1;
    if (off + 4 > len) return -1;
    off += 4;
    for (i = 0; i < ancount; i++) {
        if (dns_skip_name(pkt, len, &off) != 0) return -1;
        if (off + 10 > len) return -1;
        qtype = (unsigned short)((pkt[off] << 8) | pkt[off + 1]);
        qclass = (unsigned short)((pkt[off + 2] << 8) | pkt[off + 3]);
        rdlen = (unsigned short)((pkt[off + 8] << 8) | pkt[off + 9]);
        off += 10;
        if (off + rdlen > len) return -1;
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

static int tests_run, tests_failed;
#define OK(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { printf("not ok %d - %s\n", tests_run, msg); tests_failed++; } \
    else { printf("ok %d - %s\n", tests_run, msg); } \
} while (0)

int main(void)
{
    unsigned char q[128], resp[128];
    unsigned int len, ip = 0;
    unsigned short id = 0xABCD;

    printf("1..2\n");
    len = dns_build_query(q, sizeof(q), id, "icsos.test");
    OK(len > 12 && q[12] == 5 && q[13] == 'i' && q[len - 3] == 1,
       "build query name+type A");

    memset(resp, 0, sizeof(resp));
    memcpy(resp, q, len);
    resp[2] = 0x85; resp[3] = 0x80;
    resp[6] = 0; resp[7] = 1;
    {
        unsigned int o = len;
        resp[o++] = 0xC0; resp[o++] = 0x0C;
        resp[o++] = 0; resp[o++] = 1;
        resp[o++] = 0; resp[o++] = 1;
        resp[o++] = 0; resp[o++] = 0; resp[o++] = 0; resp[o++] = 60;
        resp[o++] = 0; resp[o++] = 4;
        resp[o++] = 10; resp[o++] = 0; resp[o++] = 2; resp[o++] = 2;
        OK(dns_parse_a(resp, o, id, &ip) == 0 && ip == 0x0A000202u,
           "parse A 10.0.2.2");
    }
    return tests_failed ? 1 : 0;
}
