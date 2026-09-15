#ifndef _ARPA_INET_H
#define _ARPA_INET_H

#include <sys/socket.h>

static inline in_addr_t inet_addr(const char *cp)
{
    unsigned int a = 0, b = 0, c = 0, d = 0, i = 0, v = 0;
    const char *p = cp;
    if (!p)
        return (in_addr_t)-1;
    for (;;) {
        if (*p >= '0' && *p <= '9') {
            v = v * 10u + (unsigned int)(*p - '0');
            if (v > 255)
                return (in_addr_t)-1;
            p++;
            continue;
        }
        if (*p == '.' || *p == '\0') {
            if (i == 0)
                a = v;
            else if (i == 1)
                b = v;
            else if (i == 2)
                c = v;
            else if (i == 3)
                d = v;
            else
                return (in_addr_t)-1;
            v = 0;
            i++;
            if (*p == '\0')
                break;
            p++;
            continue;
        }
        return (in_addr_t)-1;
    }
    if (i != 4)
        return (in_addr_t)-1;
    return htonl((a << 24) | (b << 16) | (c << 8) | d);
}

#endif
