/*
 * Portions derived from Linux rtw88 (GPL-2.0 OR BSD-3-Clause), Realtek / Larry Finger et al.
 * Physical + logical efuse dump; PCIE MAC at logical offset 0xd0.
 */
#include "rtw_efuse.h"
#include "rtw_io.h"

extern int printf(const char *fmt, ...);
extern void *malloc(unsigned int);
extern void free(void *);
extern void *memset(void *s, int c, unsigned int n);
extern void *memcpy(void *d, const void *s, unsigned int n);

#define RTW_EFUSE_BANK_WIFI 0

static void rtw8821c_cfg_ldo25(struct rtw_dev *rtwdev, int enable)
{
    u8 ldo = rtw_read8(rtwdev, REG_LDO_EFUSE_CTRL + 3);
    if (enable)
        ldo = (u8)(ldo | BIT(7));
    else
        ldo = (u8)(ldo & ~BIT(7));
    rtw_write8(rtwdev, REG_LDO_EFUSE_CTRL + 3, ldo);
}

static void switch_efuse_bank(struct rtw_dev *rtwdev)
{
    rtw_write32_mask(rtwdev, REG_LDO_EFUSE_CTRL, BIT_MASK_EFUSE_BANK_SEL,
                     RTW_EFUSE_BANK_WIFI);
}

static int rtw_dump_physical_efuse_map(struct rtw_dev *rtwdev, u8 *map)
{
    u32 size = RTW_EFUSE_PHY_SIZE;
    u32 efuse_ctl;
    u32 addr;
    u32 cnt;

    switch_efuse_bank(rtwdev);
    rtw8821c_cfg_ldo25(rtwdev, 0);

    efuse_ctl = rtw_read32(rtwdev, REG_EFUSE_CTRL);
    for (addr = 0; addr < size; addr++) {
        efuse_ctl &= ~(BIT_MASK_EF_DATA | BITS_EF_ADDR);
        efuse_ctl |= (addr & BIT_MASK_EF_ADDR) << BIT_SHIFT_EF_ADDR;
        rtw_write32(rtwdev, REG_EFUSE_CTRL, efuse_ctl & ~BIT_EF_FLAG);

        for (cnt = 0; cnt < 1000000; cnt++) {
            efuse_ctl = rtw_read32(rtwdev, REG_EFUSE_CTRL);
            if (efuse_ctl & BIT_EF_FLAG)
                break;
            rtw_udelay(1);
        }
        if (!(efuse_ctl & BIT_EF_FLAG)) {
            printf("rtw88: efuse read timeout addr=%u\n", addr);
            return -1;
        }
        map[addr] = (u8)(efuse_ctl & BIT_MASK_EF_DATA);
    }
    return 0;
}

static int rtw_dump_logical_efuse_map(struct rtw_dev *rtwdev, u8 *phy_map,
                                      u8 *log_map)
{
    u32 phy_idx, log_idx;
    u8 hdr1, hdr2, blk_idx, word_en;
    int i;

    (void)rtwdev;
    for (phy_idx = 0; phy_idx < RTW_EFUSE_PHY_SIZE - RTW_EFUSE_PROT_SIZE;) {
        hdr1 = phy_map[phy_idx];
        hdr2 = phy_map[phy_idx + 1];
        if (hdr1 == 0xff || (((hdr1 & 0x1f) == 0xf) && hdr2 == 0xff))
            break;

        if ((hdr1 & 0x1f) == 0xf) {
            blk_idx = (u8)((((hdr2 & 0xf0) >> 1) | ((hdr1 >> 5) & 0x07)));
            word_en = (u8)(hdr2 & 0xf);
            phy_idx += 2;
        } else {
            blk_idx = (u8)((hdr1 & 0xf0) >> 4);
            word_en = (u8)(hdr1 & 0xf);
            phy_idx += 1;
        }

        for (i = 0; i < 4; i++) {
            if ((word_en & (1u << i)) != 0)
                continue;
            log_idx = (blk_idx << 3) + (i << 1);
            if (phy_idx + 1 > RTW_EFUSE_PHY_SIZE - RTW_EFUSE_PROT_SIZE ||
                log_idx + 1 > RTW_EFUSE_LOG_SIZE)
                return -1;
            log_map[log_idx] = phy_map[phy_idx];
            log_map[log_idx + 1] = phy_map[phy_idx + 1];
            phy_idx += 2;
        }
    }
    return 0;
}

static int mac_valid(const u8 mac[6])
{
    if ((mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) == 0)
        return 0;
    if ((mac[0] & mac[1] & mac[2] & mac[3] & mac[4] & mac[5]) == 0xff)
        return 0;
    return 1;
}

int rtw_efuse_read_mac(struct rtw_dev *rtwdev, u8 mac[6])
{
    u8 *phy_map = 0;
    u8 *log_map = 0;
    int ret = -1;

    if (!rtwdev || !mac)
        return -1;
    phy_map = (u8 *)malloc(RTW_EFUSE_PHY_SIZE);
    log_map = (u8 *)malloc(RTW_EFUSE_LOG_SIZE);
    if (!phy_map || !log_map)
        goto out;

    memset(log_map, 0xff, RTW_EFUSE_LOG_SIZE);
    if (rtw_dump_physical_efuse_map(rtwdev, phy_map) != 0)
        goto out;
    if (rtw_dump_logical_efuse_map(rtwdev, phy_map, log_map) != 0)
        goto out;

    /* PCIE MAC at logical 0xd0 (struct rtw8821ce_efuse). */
    memcpy(mac, log_map + RTW_EFUSE_MAC_OFF, 6);
    rtwdev->crystal_cap = (u8)(log_map[0xb9] & 0x3f); /* xtal_k */
    rtwdev->rfe_option = (u8)(log_map[0xca] & 0x1f);  /* rfe_option (low 5 bits) */
    /* pkg_type (rtw_phy_cond.pkg) comes from bit 5 of the RAW efuse byte;
     * must be read before the & 0x1f mask discards it.
     * Reference: hal->pkg_type = map->rfe_option & BIT(5) ? 1 : 0 */
    rtwdev->pkg_type = (u8)((log_map[0xca] & BIT(5)) ? 1 : 0);

    if (!mac_valid(mac)) {
        printf("rtw88: efuse MAC invalid\n");
        goto out;
    }
    ret = 0;

out:
    if (phy_map)
        free(phy_map);
    if (log_map)
        free(log_map);
    return ret;
}
