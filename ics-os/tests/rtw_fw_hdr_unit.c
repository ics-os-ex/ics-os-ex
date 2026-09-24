/*
  Host TAP for rtw8821c firmware header validation helpers.
*/
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Host build: provide types.h stand-ins via the kernel header path. */
#include "kernel/hardware/wifi/rtw88/rtw_fw_hdr.h"

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
    struct rtw_fw_hdr hdr;
    unsigned char buf[RTW_FW_HDR_SIZE + 8 + 8];

    printf("TAP version 13\n1..5\n");

    ok &= check("null data rejected", rtw_fw_hdr_valid(0, 100) == 0);
    ok &= check("too small rejected",
                rtw_fw_hdr_valid(buf, RTW_FW_HDR_SIZE - 1) == 0);

    memset(&hdr, 0, sizeof(hdr));
    hdr.signature = RTW_FW_SIGNATURE_8821C;
    hdr.dmem_size = 0;
    hdr.imem_size = 0;
    hdr.emem_size = 0;
    hdr.mem_usage = 0;
    /* real_size = 64 + 8 + 8 = 80 */
    memset(buf, 0, sizeof(buf));
    memcpy(buf, &hdr, sizeof(hdr));
    ok &= check("empty sections valid at 80 bytes",
                rtw_fw_hdr_valid(buf, 80) == 1);
    ok &= check("wrong size rejected",
                rtw_fw_hdr_valid(buf, 64) == 0);

    hdr.signature = 0x1234;
    memcpy(buf, &hdr, sizeof(hdr));
    ok &= check("bad signature rejected",
                rtw_fw_hdr_valid(buf, 80) == 0);

    return ok ? 0 : 1;
}
