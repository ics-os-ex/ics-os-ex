/*
 * Host TAP: AF_INET sockaddr pack helpers matching SDK htons/inet_addr.
 */
#include <stdio.h>

static int tests_run;
static int tests_failed;

#define OK(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        printf("not ok %d - %s\n", tests_run, msg); \
        tests_failed++; \
    } else { \
        printf("ok %d - %s\n", tests_run, msg); \
    } \
} while (0)

static unsigned short htons_u(unsigned short x)
{
    return (unsigned short)((x << 8) | (x >> 8));
}

static unsigned int htonl_u(unsigned int x)
{
    return ((x & 0x000000FFu) << 24) |
           ((x & 0x0000FF00u) << 8) |
           ((x & 0x00FF0000u) >> 8) |
           ((x & 0xFF000000u) >> 24);
}

static unsigned int inet_addr_u(const char *cp)
{
    unsigned int a = 0, b = 0, c = 0, d = 0, i = 0, v = 0;
    const char *p = cp;
    for (;;) {
        if (*p >= '0' && *p <= '9') {
            v = v * 10u + (unsigned int)(*p - '0');
            if (v > 255)
                return (unsigned int)-1;
            p++;
            continue;
        }
        if (*p == '.' || *p == '\0') {
            if (i == 0) a = v;
            else if (i == 1) b = v;
            else if (i == 2) c = v;
            else if (i == 3) d = v;
            else return (unsigned int)-1;
            v = 0;
            i++;
            if (*p == '\0')
                break;
            p++;
            continue;
        }
        return (unsigned int)-1;
    }
    if (i != 4)
        return (unsigned int)-1;
    return htonl_u((a << 24) | (b << 16) | (c << 8) | d);
}

int main(void)
{
    unsigned int ip;
    printf("1..5\n");
    OK(htons_u(7778) == 0x621E, "htons 7778");
    OK(htonl_u(htonl_u(0x0A000202u)) == 0x0A000202u, "htonl involution");
    ip = inet_addr_u("10.0.2.2");
    OK(ip == htonl_u(0x0A000202u), "inet_addr 10.0.2.2");
    OK(inet_addr_u("256.0.0.1") == (unsigned int)-1, "inet_addr reject octet");
    OK(inet_addr_u("10.0.2") == (unsigned int)-1, "inet_addr reject short");
    if (tests_failed) {
        printf("# %d failed of %d\n", tests_failed, tests_run);
        return 1;
    }
    return 0;
}
