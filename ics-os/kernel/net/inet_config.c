#include "inet_config.h"

extern char *strstr(const char *haystack, const char *needle);

static int parse_u8(const char **pp, unsigned int *out)
{
    const char *p = *pp;
    unsigned int v = 0;
    int digits = 0;

    while (*p >= '0' && *p <= '9') {
        v = v * 10u + (unsigned int)(*p - '0');
        if (v > 255)
            return 0;
        p++;
        digits = 1;
    }
    if (!digits)
        return 0;
    *out = v;
    *pp = p;
    return 1;
}

static int parse_dotted_ip(const char *s, unsigned int *out)
{
    unsigned int a, b, c, d;
    const char *p = s;

    if (!parse_u8(&p, &a) || *p++ != '.')
        return 0;
    if (!parse_u8(&p, &b) || *p++ != '.')
        return 0;
    if (!parse_u8(&p, &c) || *p++ != '.')
        return 0;
    if (!parse_u8(&p, &d))
        return 0;
    *out = (a << 24) | (b << 16) | (c << 8) | d;
    return 1;
}

static int parse_kv_ip(const char *cmdline, const char *key, unsigned int *out)
{
    const char *p;
    unsigned int len;

    if (!cmdline || !key)
        return 0;
    p = strstr(cmdline, key);
    if (!p)
        return 0;
    len = 0;
    while (key[len])
        len++;
    p += len;
    return parse_dotted_ip(p, out);
}

void inet_config_defaults(struct inet_config *cfg)
{
    cfg->ip = NET_SLIRP_IP;
    cfg->netmask = NET_SLIRP_MASK;
    cfg->gateway = NET_SLIRP_GATEWAY;
}

void inet_config_from_cmdline(struct inet_config *cfg, const char *cmdline)
{
    unsigned int v;

    inet_config_defaults(cfg);
    if (parse_kv_ip(cmdline, "ip=", &v))
        cfg->ip = v;
    if (parse_kv_ip(cmdline, "mask=", &v))
        cfg->netmask = v;
    if (parse_kv_ip(cmdline, "gw=", &v))
        cfg->gateway = v;
}
