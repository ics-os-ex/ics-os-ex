/*
  Host TAP for Internet checksum (RFC 1071).
*/
#include <stdio.h>
#include <string.h>

#include "kernel/net/checksum.h"

static int check(const char *name, int condition)
{
    if (!condition) {
        printf("not ok - %s\n", name);
        return 0;
    }
    printf("ok - %s\n", name);
    return 1;
}

int main(void)
{
    int ok = 1;
    unsigned char words[4] = { 0x00, 0x01, 0xf2, 0x03 };
    unsigned char odd[3] = { 0x00, 0x01, 0xF0 };
    unsigned short csum;

    printf("TAP version 13\n1..5\n");

    ok &= check("empty buffer checksum is 0xFFFF",
                net_checksum((const void *)0, 0) == 0xFFFF);

    /* Classic RFC 1071 example fragment: 0x0001 + 0xf203 = 0xf204 -> ~ = 0x0DFB */
    csum = net_checksum(words, 4);
    ok &= check("two-word example", csum == 0x0DFB);

    csum = net_checksum(odd, 3);
    ok &= check("odd-length pads high byte",
                csum == net_checksum_fold(0x0001u + 0xF000u));

    {
        unsigned int sum = net_checksum_add(words, 4, 0);
        ok &= check("add then fold matches checksum",
                    net_checksum_fold(sum) == net_checksum(words, 4));
    }

    {
        unsigned char ip[20];
        memset(ip, 0, sizeof(ip));
        ip[0] = 0x45;
        ip[2] = 0x00;
        ip[3] = 0x14;
        ip[8] = 0x40;
        ip[9] = 0x01;
        ip[12] = 0x0A;
        ip[13] = 0x00;
        ip[14] = 0x02;
        ip[15] = 0x0F;
        ip[16] = 0x0A;
        ip[17] = 0x00;
        ip[18] = 0x02;
        ip[19] = 0x02;
        csum = net_checksum(ip, 20);
        ip[10] = (unsigned char)(csum >> 8);
        ip[11] = (unsigned char)(csum & 0xFF);
        ok &= check("IP header verifies to 0",
                    net_checksum(ip, 20) == 0);
    }

    return ok ? 0 : 1;
}
