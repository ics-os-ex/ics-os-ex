/*
 * Portions derived from Linux rtw88 (GPL-2.0 OR BSD-3-Clause), Realtek / Larry Finger et al.
 *
 * File-backed PHY tables for RTL8821CE (air RX enable).
 *
 * On-disk format (linux-firmware rtw8821c_*.bin):
 *   Raw flat u32 word array, each entry is a pair { u32 addr, u32 data }.
 *   NO header.  The array may contain conditional-branch tokens (bit 31 = pos,
 *   bit 30 = neg) and per-table delay tokens in addition to plain register
 *   writes.
 *
 * Per-table writer (matches Linux rtw8821c_table.c RTW_DECL_TABLE_* wiring):
 *   mac  -> rtw_write8 (8-bit MAC registers, no conditionals)
 *   agc  -> rtw_write32 (32-bit BB registers, conditional)
 *   bb   -> rtw_write32 + delay tokens 0xf9-0xfe (conditional)
 *   rf_a -> SIPI rtw_write_rf (RF path A, conditional, delay 0xfe/0xffe)
 *
 * Conditional branch state machine (rtw_parse_tbl_phy_cond):
 *   pos bit 31: branch header (IF/ELIF sets pos_cond; ELSE/ENDIF adjust is_matched)
 *   neg bit 30: condition test — check_positive(pos_cond) sets is_matched
 *   otherwise:  register write, applied only if is_matched
 */
#include "rtw_phy.h"
#include "rtw_io.h"
#include "rtw_fw_hdr.h"
#define __RTW_PHY_TBL_NO_TYPES
#include "rtw_phy_tbl.h"
#include "rtw_mask.h"
#include "../../../vfs/vfs_core.h"

extern int printf(const char *fmt, ...);
extern void *malloc(unsigned int);
extern void *free(void *);
extern void *memset(void *s, int c, unsigned int n);
extern void *memcpy(void *d, const void *s, unsigned int n);

/* ------------------------------------------------------------------ */
/* Table file paths (long + 8.3 short names for FAT)                   */
/* ------------------------------------------------------------------ */

static const char *phy_tbl_paths_bb[] = {
    "/icsos/firmware/rtw88/rtw8821c_bb.bin",
    "/icsos/firmware/rtw88/RTW882~B.BIN",
    0
};

static const char *phy_tbl_paths_rf[] = {
    "/icsos/firmware/rtw88/rtw8821c_rf_a.bin",
    "/icsos/firmware/rtw88/RTW882~A.BIN",
    0
};

static const char *phy_tbl_paths_agc[] = {
    "/icsos/firmware/rtw88/rtw8821c_agc.bin",
    0
};

static const char *phy_tbl_paths_mac[] = {
    "/icsos/firmware/rtw88/rtw8821c_mac.bin",
    0
};

/* ------------------------------------------------------------------ */
/* Little-endian u32 reader                                            */
/* ------------------------------------------------------------------ */

static u32 rd_le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* ------------------------------------------------------------------ */
/* Load a table file into a struct rtw_phy_tbl                         */
/* ------------------------------------------------------------------ */

int rtw_phy_tbl_load_file(struct rtw_dev *rtwdev, const char *path,
                          struct rtw_phy_tbl *t)
{
    file_PCB *f;
    vfs_stat st;
    u8 *buf;
    int n;

    if (!rtwdev || !path || !t)
        return -1;
    f = openfilex((char *)path, FILE_READ);
    if (!f)
        return -1;
    if (fstat(f, &st) != 0 || st.st_size < 8 || st.st_size > (4 * 1024 * 1024) ||
        (st.st_size & 7) != 0) {
        fclose(f);
        return -1;
    }
    buf = (u8 *)malloc((unsigned int)st.st_size);
    if (!buf) {
        fclose(f);
        return -1;
    }
    n = fread((char *)buf, st.st_size, 1, f);
    fclose(f);
    if (n != st.st_size) {
        free(buf);
        return -1;
    }
    rtw_phy_tbl_free(t);
    t->data = buf;
    t->size = (u32)st.st_size;
    t->loaded = 1;
    printf("rtw88: phy table loaded %s (%d bytes, %u pairs)\n",
           path, st.st_size, (u32)(st.st_size / 8));
    return 0;
}

void rtw_phy_tbl_free(struct rtw_phy_tbl *t)
{
    if (!t)
        return;
    if (t->data) {
        free(t->data);
        t->data = 0;
    }
    t->size = 0;
    t->loaded = 0;
}

/* ------------------------------------------------------------------ */
/* Conditional-branch parser (port of rtw_parse_tbl_phy_cond)          */
/* ------------------------------------------------------------------ */

/*
 * rtw_phy_cond bitfield layout (little-endian):
 *   bits  0-7:   rfe
 *   bits  8-11:  intf
 *   bits 12-15:  pkg
 *   bits 16-19:  plat
 *   bits 20-23:  intf_rsvd
 *   bits 24-27:  cut
 *   bits 28-29:  branch (0=IF 1=ELIF 2=ELSE 3=ENDIF)
 *   bit  30:     neg
 *   bit  31:     pos
 */
#define PHY_COND_POS      0x80000000u
#define PHY_COND_NEG      0x40000000u
#define PHY_COND_BRANCH   0x30000000u
#define PHY_COND_CUT      0x0F000000u
#define PHY_COND_PLAT     0x000F0000u
#define PHY_COND_PKG      0x0000F000u
#define PHY_COND_INTF     0x00000F00u
#define PHY_COND_RFE      0x000000FFu

#define BRANCH_IF     0
#define BRANCH_ELIF   1
#define BRANCH_ELSE   2
#define BRANCH_ENDIF  3

/*
 * Port of rtw_phy_setup_phy_cond + check_positive.
 * Driver condition (our port is always PCIe):
 *   cut  = cut_version ? cut_version : 15
 *   pkg  = pkg_type ? pkg_type : 15   (pkg_type = efuse rfe bit5 ? 1 : 0)
 *   plat = 0x04
 *   intf = INTF_PCIE (1)
 *   rfe  = efuse rfe_option (& 0x1f)
 * check_positive: cut/pkg/intf guarded by "cond field non-zero";
 * rfe is ALWAYS compared (no guard).
 */
static int phy_cond_match(u32 cond_word, struct rtw_dev *rtwdev)
{
    u32 cond_cut  = (cond_word & PHY_COND_CUT)  >> 24;
    u32 cond_pkg  = (cond_word & PHY_COND_PKG)  >> 12;
    u32 cond_intf = (cond_word & PHY_COND_INTF) >> 8;
    u32 cond_rfe  = (cond_word & PHY_COND_RFE);

    u32 drv_cut  = rtwdev->cut_version ? rtwdev->cut_version : 15;
    u32 drv_pkg  = rtwdev->pkg_type ? rtwdev->pkg_type : 15;
    u32 drv_intf = 1;  /* INTF_PCIE */
    u32 drv_rfe  = rtwdev->rfe_option;

    if (cond_cut && cond_cut != drv_cut)
        return 0;
    if (cond_pkg && cond_pkg != drv_pkg)
        return 0;
    if (cond_intf && cond_intf != drv_intf)
        return 0;
    if (cond_rfe != drv_rfe)
        return 0;
    return 1;
}

/*
 * Per-table do_cfg callback types.
 * Each takes (rtwdev, addr, data) and performs the appropriate register write.
 */
typedef void (*phy_do_cfg_fn)(struct rtw_dev *rtwdev, u32 addr, u32 data);

/* MAC: 8-bit register write */
static void phy_cfg_mac(struct rtw_dev *rtwdev, u32 addr, u32 data)
{
    rtw_write8(rtwdev, addr, (u8)data);
}

/* AGC: 32-bit register write */
static void phy_cfg_agc(struct rtw_dev *rtwdev, u32 addr, u32 data)
{
    rtw_write32(rtwdev, addr, data);
}

/* BB: 32-bit register write with delay tokens */
static void phy_cfg_bb(struct rtw_dev *rtwdev, u32 addr, u32 data)
{
    if (addr == 0xfe)
        rtw_mdelay(50);
    else if (addr == 0xfd)
        rtw_mdelay(5);
    else if (addr == 0xfc)
        rtw_mdelay(1);
    else if (addr == 0xfb)
        rtw_udelay(55);
    else if (addr == 0xfa)
        rtw_udelay(5);
    else if (addr == 0xf9)
        rtw_udelay(1);
    else
        rtw_write32(rtwdev, addr, data);
}

/* RF path A: SIPI write with delay tokens */
static void phy_cfg_rf(struct rtw_dev *rtwdev, u32 addr, u32 data)
{
    if (addr == 0xffe) {
        rtw_mdelay(50);
    } else if (addr == 0xfe) {
        rtw_udelay(105);
    } else {
        /* SIPI RF write (rf_path A -> 0xc90):
         * data_and_addr = ((addr << 20) | (data & 0x000fffff)) & 0x0fffffff
         * (mask == RFREG_MASK full 20-bit register, so no read-modify-write).
         * Reference rtw_phy_write_rf_reg_sipi: write32 + udelay(13),
         * then rtw_phy_cfg_rf adds udelay(1). */
        u32 data_and_addr = (((addr & 0xff) << 20) | (data & 0x000fffff)) & 0x0fffffff;
        rtw_write32(rtwdev, 0xc90, data_and_addr); /* RF_PATH_A SIPI */
        rtw_udelay(14);
    }
}

/*
 * Generic conditional-branch table parser.
 * Walks the flat {addr, data} pair array, tracking the branch state machine
 * and calling do_cfg for each matching register write.
 */
static int phy_tbl_apply_cond(struct rtw_dev *rtwdev, const struct rtw_phy_tbl *t,
                              const char *name, phy_do_cfg_fn do_cfg)
{
    u32 n_pairs, i;
    int applied = 0, delays = 0;
    int is_matched = 1;
    int is_skipped = 0;
    u32 pos_cond = 0;

    if (!t->data || !t->loaded) {
        printf("RTL8821CE_PHY_TABLE_MISSING %s\n", name);
        return -1;
    }
    n_pairs = t->size / 8;
    for (i = 0; i < n_pairs; i++) {
        u32 addr = rd_le32(t->data + i * 8);
        u32 data = rd_le32(t->data + i * 8 + 4);

        if (addr & PHY_COND_POS) {
            /* Branch header */
            u32 branch = (addr & PHY_COND_BRANCH) >> 28;
            switch (branch) {
            case BRANCH_ENDIF:
                is_matched = 1;
                is_skipped = 0;
                break;
            case BRANCH_ELSE:
                is_matched = is_skipped ? 0 : 1;
                break;
            case BRANCH_IF:
            case BRANCH_ELIF:
            default:
                pos_cond = addr;
                break;
            }
        } else if (addr & PHY_COND_NEG) {
            /* Condition test */
            if (!is_skipped) {
                if (phy_cond_match(pos_cond, rtwdev)) {
                    is_matched = 1;
                    is_skipped = 1;
                } else {
                    is_matched = 0;
                    is_skipped = 0;
                }
            } else {
                is_matched = 0;
            }
        } else if (is_matched) {
            do_cfg(rtwdev, addr, data);
            applied++;
            if (addr >= 0xf9 && addr <= 0xffe)
                delays++;
        }
    }
    printf("RTL8821CE_PHY_TABLE_OK %s pairs=%u writes=%d delays=%d\n",
           name, n_pairs, applied, delays);
    return 0;
}

int rtw_phy_tbl_apply_bb(struct rtw_dev *rtwdev, struct rtw_phy_tbl *t)
{
    return phy_tbl_apply_cond(rtwdev, t, "bb", phy_cfg_bb);
}

int rtw_phy_tbl_apply_rf(struct rtw_dev *rtwdev, struct rtw_phy_tbl *t)
{
    return phy_tbl_apply_cond(rtwdev, t, "rf_a", phy_cfg_rf);
}

int rtw_phy_tbl_apply_agc(struct rtw_dev *rtwdev, struct rtw_phy_tbl *t)
{
    return phy_tbl_apply_cond(rtwdev, t, "agc", phy_cfg_agc);
}

int rtw_phy_tbl_apply_mac(struct rtw_dev *rtwdev, struct rtw_phy_tbl *t)
{
    return phy_tbl_apply_cond(rtwdev, t, "mac", phy_cfg_mac);
}

/* ------------------------------------------------------------------ */
/* Channel tuning (Linux rtw8821c_set_channel_rf, 20 MHz subset)       */
/* ------------------------------------------------------------------ */

static u32 phy_read_rf(struct rtw_dev *rtwdev, u8 addr, u32 mask)
{
    return rtw_read32(rtwdev, rtw_phy_rf_read_addr(RTW_PHY_RF_BASE_PATH_A,
                                                   addr)) & mask;
}

static void phy_write_rf(struct rtw_dev *rtwdev, u8 addr, u32 mask, u32 data)
{
    u32 value = data;

    if (mask != RTW_PHY_RFREG_MASK)
        value = rtw_mask_merge(phy_read_rf(rtwdev, addr, RTW_PHY_RFREG_MASK),
                               mask, data);
    rtw_write32(rtwdev, RTW_PHY_SIPA_PATH_A,
                rtw_phy_sipa_encode(addr, value));
    rtw_udelay(13);
}

static void rtw8821c_switch_rf_2g(struct rtw_dev *rtwdev)
{
    u32 reg;

    rtw_write32_set(rtwdev, 0x1080, BIT(16)); /* REG_DMEM_CTRL / BIT_WL_RST */
    rtw_write32_set(rtwdev, 0x0000, BIT(26)); /* REG_SYS_CTRL / BIT_FEN_EN */
    reg = rtw_read32(rtwdev, 0xcb8);          /* REG_RFECTL */
    if (rtw_phy_rfe_uses_btg(rtwdev->rfe_option)) {
        reg |= BIT(16);
        reg &= ~(BIT(18) | BIT(20) | BIT(21) | BIT(22) | BIT(23));
        rtw_write32_mask(rtwdev, 0xa84, 0x00ff0000u, 0x0e);
        rtw_write32_mask(rtwdev, 0xa80, 0x0000ffffu, 0xfc84);
    } else {
        reg |= BIT(20) | BIT(22) | BIT(21);
        reg &= ~(BIT(16) | BIT(18) | BIT(23));
        rtw_write32_mask(rtwdev, 0xa84, 0x00ff0000u, 0x12);
        rtw_write32_mask(rtwdev, 0xa80, 0x0000ffffu, 0x7532);
    }
    rtw_write32(rtwdev, 0xcb8, reg);
}

static void rtw8821c_set_channel_bb_20(struct rtw_dev *rtwdev)
{
    u32 value;

    rtw_write32_mask(rtwdev, REG_RXPSEL, BIT(28), 1);
    rtw_write32_mask(rtwdev, REG_CCK_CHECK, BIT(7), 0);
    rtw_write32_mask(rtwdev, REG_ENTXCCK, BIT(18), 0);
    rtw_write32_mask(rtwdev, REG_RXCCAMSK, 0x0000fc00u, 15);
    rtw_write32_mask(rtwdev, REG_TXSCALE_A, 0x00000f00u, 0);
    rtw_write32_mask(rtwdev, REG_CLKTRK, 0x1ffe0000u, 0x96a);
    value = (rtw_read32(rtwdev, REG_ADCCLK) & 0xffcffc00u) | 0x10010000u;
    rtw_write32(rtwdev, REG_ADCCLK, value);
    rtw_write32_mask(rtwdev, REG_ADC160, BIT(30), 1);
}

static void rtw8821c_set_channel_mac_20(struct rtw_dev *rtwdev)
{
    rtw_write8(rtwdev, REG_DATA_SC, 0);
    rtw_write32_clr(rtwdev, REG_WMAC_TRXPTCL_CTL, BIT_RFMOD);
    rtw_write32_mask(rtwdev, REG_AFE_XTAL_CTRL, BIT(20) | BIT(21), 0);
    rtw_write8(rtwdev, REG_USTIME_TSF, 80);
    rtw_write8(rtwdev, REG_USTIME_EDCA, 80);
    rtw_write8_clr(rtwdev, REG_CCK_CHECK, BIT(7));
}

static void rtw8821c_set_channel_rxdfir_20(struct rtw_dev *rtwdev)
{
    rtw_write32_mask(rtwdev, REG_ACBB0, BIT(29) | BIT(28), 2);
    rtw_write32_mask(rtwdev, REG_ACBBRXFIR, BIT(29) | BIT(28), 2);
    rtw_write32_mask(rtwdev, REG_TXDFIR, BIT(31), 1);
    rtw_write32_mask(rtwdev, REG_CHFIR, BIT(31), 0);
}

int rtw8821c_phy_set_channel(struct rtw_dev *rtwdev, u8 channel)
{
    u32 rf18;

    if (channel < 1 || channel > 14) {
        printf("rtw88: bad channel %u\n", channel);
        return -1;
    }
    rf18 = phy_read_rf(rtwdev, 0x18, RTW_PHY_RFREG_MASK);
    rf18 = rtw_phy_rf18_20mhz(rf18, channel);
    rtw8821c_set_channel_bb_20(rtwdev);
    rtw8821c_set_channel_mac_20(rtwdev);
    rtw8821c_switch_rf_2g(rtwdev);
    phy_write_rf(rtwdev, 0xdf, BIT(6), 1); /* RF_LUTDBG */
    phy_write_rf(rtwdev, 0x64, 0xf, 0xf);
    phy_write_rf(rtwdev, 0x18, RTW_PHY_RFREG_MASK, rf18);
    phy_write_rf(rtwdev, 0xb8, BIT(19), 0); /* RF_XTALX2 toggle */
    phy_write_rf(rtwdev, 0xb8, BIT(19), 1);
    rtw8821c_set_channel_rxdfir_20(rtwdev);
    rtwdev->channel = channel;
    printf("rtw88: channel %u RF18=0x%x\n", channel, rf18);
    return 0;
}

/* ------------------------------------------------------------------ */
/* PHY init: load + apply all four tables                              */
/* ------------------------------------------------------------------ */

int rtw_phy_init(struct rtw_dev *rtwdev)
{
    struct rtw_phy_tbl bb, rf, agc, mac;
    const char **p;
    int ret, ok = 0;
    int have_bb, have_rf, have_agc, have_mac;
    u8 crystal_cap;

    memset(&bb, 0, sizeof(bb));
    memset(&rf, 0, sizeof(rf));
    memset(&agc, 0, sizeof(agc));
    memset(&mac, 0, sizeof(mac));

    p = phy_tbl_paths_bb;
    for (ret = 0; p && p[0]; p++) {
        if (rtw_phy_tbl_load_file(rtwdev, p[0], &bb) == 0)
            break;
    }
    for (p = phy_tbl_paths_rf; p && p[0]; p++) {
        if (rtw_phy_tbl_load_file(rtwdev, p[0], &rf) == 0)
            break;
    }
    for (p = phy_tbl_paths_agc; p && p[0]; p++) {
        if (rtw_phy_tbl_load_file(rtwdev, p[0], &agc) == 0)
            break;
    }
    for (p = phy_tbl_paths_mac; p && p[0]; p++) {
        if (rtw_phy_tbl_load_file(rtwdev, p[0], &mac) == 0)
            break;
    }

    if (!bb.loaded && !rf.loaded) {
        printf("RTL8821CE_PHY_TABLES_MISSING (air RX disabled)\n");
        rtw_phy_tbl_free(&bb);
        rtw_phy_tbl_free(&rf);
        rtw_phy_tbl_free(&agc);
        rtw_phy_tbl_free(&mac);
        return -1;
    }

    have_bb = bb.loaded;
    have_rf = rf.loaded;
    have_agc = agc.loaded;
    have_mac = mac.loaded;

    /* Linux rtw_phy_load_tables order is MAC, BB, AGC, RF. */
    if (mac.loaded && rtw_phy_tbl_apply_mac(rtwdev, &mac) == 0) ok++;
    if (bb.loaded && rtw_phy_tbl_apply_bb(rtwdev, &bb) == 0) ok++;
    if (agc.loaded && rtw_phy_tbl_apply_agc(rtwdev, &agc) == 0) ok++;
    if (rf.loaded && rtw_phy_tbl_apply_rf(rtwdev, &rf) == 0) ok++;

    /* Linux post-table sequence: crystal fields, then release RXPSEL reset. */
    crystal_cap = rtwdev->crystal_cap & 0x3f;
    rtw_write32_mask(rtwdev, REG_AFE_XTAL_CTRL, 0x7e000000u, crystal_cap);
    rtw_write32_mask(rtwdev, REG_AFE_PLL_CTRL, 0x7e, crystal_cap);
    rtw_write32_set(rtwdev, REG_RXPSEL, BIT_RX_PSEL_RST);

    rtw_phy_tbl_free(&bb);
    rtw_phy_tbl_free(&rf);
    rtw_phy_tbl_free(&agc);
    rtw_phy_tbl_free(&mac);

    if (ok > 0) {
        rtwdev->flags |= RTW_FLAG_PHY_READY;
        printf("RTL8821CE_PHY_OK tables_applied=%d (mac:%d bb:%d agc:%d rf:%d) crystal=0x%x\n",
               ok, have_mac, have_bb, have_agc, have_rf, crystal_cap);
    }
    return ok > 0 ? 0 : -1;
}
