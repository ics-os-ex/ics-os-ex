#ifndef ICSOS_NET_ENDIAN_H
#define ICSOS_NET_ENDIAN_H

static inline unsigned short net_htons(unsigned short x)
{
    return (unsigned short)((x << 8) | (x >> 8));
}

static inline unsigned short net_ntohs(unsigned short x)
{
    return net_htons(x);
}

static inline unsigned int net_htonl(unsigned int x)
{
    return ((x & 0x000000FFu) << 24) |
           ((x & 0x0000FF00u) << 8) |
           ((x & 0x00FF0000u) >> 8) |
           ((x & 0xFF000000u) >> 24);
}

static inline unsigned int net_ntohl(unsigned int x)
{
    return net_htonl(x);
}

#endif
