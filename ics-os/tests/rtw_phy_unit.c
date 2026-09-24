/* Host TAP for the shared RTL8821C PHY-table parser and RF helpers. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "kernel/hardware/wifi/rtw88/rtw_phy_tbl.h"
#include "kernel/hardware/wifi/rtw88/rtw_mask.h"

static int ntest;
static int nfail;

static void check(const char *name, int condition)
{
    ntest++;
    printf("%s %d - %s\n", condition ? "ok" : "not ok", ntest, name);
    if (!condition)
        nfail++;
}

struct capture {
    uint32_t addr[16];
    uint32_t data[16];
    int count;
};

static void capture_write(void *opaque, uint32_t addr, uint32_t data)
{
    struct capture *cap = (struct capture *)opaque;
    if (cap->count < 16) {
        cap->addr[cap->count] = addr;
        cap->data[cap->count] = data;
    }
    cap->count++;
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static int apply_file(const char *path, const struct rtw_phy_drv_cond *drv,
                      struct capture *cap, long *size_out)
{
    FILE *f;
    uint8_t *data;
    long size;
    int ret;

    f = fopen(path, "rb");
    if (!f)
        return -2;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -2;
    }
    size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -2;
    }
    data = (uint8_t *)malloc((size_t)size);
    if (!data) {
        fclose(f);
        return -2;
    }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return -2;
    }
    fclose(f);
    ret = rtw_phy_tbl_apply(data, (int)size, drv, capture_write, cap);
    free(data);
    *size_out = size;
    return ret;
}

int main(void)
{
    struct rtw_phy_drv_cond rfe6 = { 15, 15, RTW_PHY_INTF_PCIE, 6 };
    struct rtw_phy_drv_cond rfe7 = { 15, 15, RTW_PHY_INTF_PCIE, 7 };
    struct capture cap = {{0}, {0}, 0};
    uint8_t table[8 * 10] = {0};
    static const char *paths[] = {
        "base/firmware/rtw88/rtw8821c_mac.bin",
        "base/firmware/rtw88/rtw8821c_agc.bin",
        "base/firmware/rtw88/rtw8821c_bb.bin",
        "base/firmware/rtw88/rtw8821c_rf_a.bin"
    };
    static const int pairs[] = {138, 1200, 1680, 2712};
    int i;

    printf("TAP version 13\n");
    check("NULL table rejected", rtw_phy_tbl_apply(0, 8, &rfe6, capture_write, &cap) == -1);
    check("short table rejected", rtw_phy_tbl_apply(table, 7, &rfe6, capture_write, &cap) == -1);
    check("misaligned table rejected", rtw_phy_tbl_apply(table, 9, &rfe6, capture_write, &cap) == -1);

    check("condition matches N150 rfe", rtw_phy_cond_match(6, &rfe6));
    check("rfe is compared unconditionally", !rtw_phy_cond_match(0, &rfe6));
    check("zero cut/pkg/intf fields are wildcards", rtw_phy_cond_match(6, &rfe6));
    check("Linux bitfield positions match PCI/package/cut condition",
          rtw_phy_cond_match(6u | (1u << 8) | (15u << 12) | (15u << 24),
                             &rfe6));
    check("SIPI encoding", rtw_phy_sipa_encode(0x18, 0x12345) == 0x01812345u);
    check("RF path-A direct read address", rtw_phy_rf_read_addr(RTW_PHY_RF_BASE_PATH_A, 0x18) == 0x2860u);
    check("RF18 20MHz channel replaces stale band/channel/bw bits",
          rtw_phy_rf18_20mhz(RTW_PHY_RFREG_MASK, 11) == 0x0008fc0bu);
    check("N150 rfe=6 uses WLG rather than BTG switch",
          !rtw_phy_rfe_uses_btg(6));
    check("reference BTG rfe option is recognized",
          rtw_phy_rfe_uses_btg(7));
    check("masked writes shift field values to mask LSB",
          rtw_mask_merge(0xa5u, 0xf0u, 1u) == 0x15u);

    /* IF rfe=6, write A; ELIF rfe=7, write B; ELSE write C; ENDIF, write D. */
    put32(table + 0, RTW_PHY_COND_POS | 6); put32(table + 4, 0);
    put32(table + 8, RTW_PHY_COND_NEG); put32(table + 12, 0);
    put32(table + 16, 0x100); put32(table + 20, 0xaaaa);
    put32(table + 24, RTW_PHY_COND_POS | (RTW_PHY_BRANCH_ELIF << 28) | 7); put32(table + 28, 0);
    put32(table + 32, RTW_PHY_COND_NEG); put32(table + 36, 0);
    put32(table + 40, 0x104); put32(table + 44, 0xbbbb);
    put32(table + 48, RTW_PHY_COND_POS | (RTW_PHY_BRANCH_ELSE << 28)); put32(table + 52, 0);
    put32(table + 56, 0x108); put32(table + 60, 0xcccc);
    put32(table + 64, RTW_PHY_COND_POS | (RTW_PHY_BRANCH_ENDIF << 28)); put32(table + 68, 0);
    put32(table + 72, 0x10c); put32(table + 76, 0xdddd);
    cap.count = 0;
    check("branch table pair count", rtw_phy_tbl_apply(table, sizeof(table), &rfe6,
                                                       capture_write, &cap) == 10);
    check("matched IF suppresses ELIF and ELSE", cap.count == 2 &&
          cap.addr[0] == 0x100 && cap.data[0] == 0xaaaa &&
          cap.addr[1] == 0x10c && cap.data[1] == 0xdddd);
    cap.count = 0;
    (void)rtw_phy_tbl_apply(table, sizeof(table), &rfe7, capture_write, &cap);
    check("failed IF permits matching ELIF", cap.count == 2 &&
          cap.addr[0] == 0x104 && cap.data[0] == 0xbbbb);

    for (i = 0; i < 4; i++) {
        long size = 0;
        int ret;
        cap.count = 0;
        ret = apply_file(paths[i], &rfe6, &cap, &size);
        check(paths[i], ret == pairs[i] && size == (long)pairs[i] * 8 && cap.count > 0);
    }

    printf("1..%d\n# ntest=%d nfail=%d\n", ntest, ntest, nfail);
    return nfail ? 1 : 0;
}