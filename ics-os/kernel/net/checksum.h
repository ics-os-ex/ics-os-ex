#ifndef ICSOS_NET_CHECKSUM_H
#define ICSOS_NET_CHECKSUM_H

/*
 * Internet checksum (RFC 1071). Host-testable; no kernel deps beyond
 * unsigned types.
 */

static inline unsigned short net_checksum_fold(unsigned int sum)
{
    while (sum >> 16)
        sum = (sum & 0xFFFFu) + (sum >> 16);
    return (unsigned short)(~sum & 0xFFFFu);
}

/* Ones'-complement sum of 16-bit words; odd trailing byte is padded. */
static inline unsigned int net_checksum_add(const void *data, unsigned int len,
                                            unsigned int sum)
{
    const unsigned char *p = (const unsigned char *)data;
    unsigned int i;

    for (i = 0; i + 1 < len; i += 2)
        sum += (unsigned int)p[i] << 8 | p[i + 1];
    if (i < len)
        sum += (unsigned int)p[i] << 8;
    return sum;
}

static inline unsigned short net_checksum(const void *data, unsigned int len)
{
    return net_checksum_fold(net_checksum_add(data, len, 0));
}

#endif
