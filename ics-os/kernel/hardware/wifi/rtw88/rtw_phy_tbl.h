/*
 * Portions derived from Linux rtw88 (GPL-2.0 OR BSD-3-Clause), Realtek /
 * Larry Finger et al.
 *
 * Pure logic for parsing rtw8821c PHY table blobs — shared between the
 * kernel (rtw_phy.c) and the host TAP test (tests/rtw_phy_unit.c).
 * No device / VFS dependencies.
 *
 * On-disk format (linux-firmware rtw8821c_{mac,agc,bb,rf_a}.bin):
 *   Raw flat little-endian u32 word array; each entry is a pair
 *   { u32 addr, u32 data }.  NO header, NO 16-byte command records.
 *   In addition to plain register writes, entries may be:
 *     - conditional-branch tokens  (bit 31 pos / bit 30 neg)
 *     - per-table delay tokens     (bb: 0xf9..0xfe, rf: 0xfe, 0xffe)
 *
 * rtw_phy_cond bitfield (little-endian, per Linux struct rtw_phy_cond):
 *   bits  0-7:   rfe      (always compared by check_positive)
 *   bits  8-11:  intf     (guarded: cond non-zero)
 *   bits 12-15:  pkg      (guarded)
 *   bits 16-19:  plat
 *   bits 20-23:  intf_rsvd
 *   bits 24-27:  cut      (guarded)
 *   bits 28-29:  branch   (0=IF 1=ELIF 2=ELSE 3=ENDIF)
 *   bit  30:     neg
 *   bit  31:     pos
 */
#ifndef ICSOS_RTW_PHY_TBL_H
#define ICSOS_RTW_PHY_TBL_H

#ifndef __RTW_PHY_TBL_NO_TYPES
#include <stdint.h>
typedef uint8_t  rtw_phy_u8;
typedef uint16_t rtw_phy_u16;
typedef uint32_t rtw_phy_u32;
typedef int      rtw_phy_s32;
#else
typedef u8  rtw_phy_u8;
typedef u16 rtw_phy_u16;
typedef u32 rtw_phy_u32;
typedef s32 rtw_phy_s32;
#endif

#define RTW_PHY_COND_POS       0x80000000u
#define RTW_PHY_COND_NEG       0x40000000u
#define RTW_PHY_COND_BRANCH    0x30000000u
#define RTW_PHY_COND_CUT       0x0F000000u
#define RTW_PHY_COND_PLAT      0x000F0000u
#define RTW_PHY_COND_PKG       0x0000F000u
#define RTW_PHY_COND_INTF      0x00000F00u
#define RTW_PHY_COND_RFE       0x000000FFu

#define RTW_PHY_BRANCH_IF      0
#define RTW_PHY_BRANCH_ELIF    1
#define RTW_PHY_BRANCH_ELSE    2
#define RTW_PHY_BRANCH_ENDIF   3

#define RTW_PHY_INTF_PCIE      1
#define RTW_PHY_INTF_USB       2
#define RTW_PHY_INTF_SDIO      4

#define RTW_PHY_RFREG_MASK     0xfffffu
#define RTW_PHY_INV_RF_DATA    0xffffffffu

/* Driver condition (Linux rtw_phy_setup_phy_cond). */
struct rtw_phy_drv_cond {
    rtw_phy_u32 cut;   /* hal->cut_version ? : 15 */
    rtw_phy_u32 pkg;   /* hal->pkg_type ? : 15 */
    rtw_phy_u32 intf;  /* RTW_PHY_INTF_PCIE on PCI */
    rtw_phy_u32 rfe;   /* efuse rfe & 0x1f — always compared */
};

/*
 * check_positive: field-by-field compare. cut/pkg/intf are guarded by
 * "condition field non-zero" (wildcard); rfe is ALWAYS compared.
 */
static int rtw_phy_cond_match(rtw_phy_u32 cond_word,
                              const struct rtw_phy_drv_cond *drv)
{
    rtw_phy_u32 cond_cut  = (cond_word & RTW_PHY_COND_CUT)  >> 24;
    rtw_phy_u32 cond_pkg  = (cond_word & RTW_PHY_COND_PKG)  >> 12;
    rtw_phy_u32 cond_intf = (cond_word & RTW_PHY_COND_INTF) >> 8;
    rtw_phy_u32 cond_rfe  = (cond_word & RTW_PHY_COND_RFE);

    if (cond_cut && cond_cut != drv->cut)
        return 0;
    if (cond_pkg && cond_pkg != drv->pkg)
        return 0;
    if (cond_intf && cond_intf != drv->intf)
        return 0;
    if (cond_rfe != drv->rfe)
        return 0;
    return 1;
}

/* Branch state machine (Linux rtw_parse_tbl_phy_cond). */
struct rtw_phy_cond_state {
    int matched;   /* current branch accepted so far */
    int skipped;   /* a branch was already accepted in this IF group */
    rtw_phy_u32 pos_cond; /* last IF/ELIF header word */
};

static void rtw_phy_cond_state_init(struct rtw_phy_cond_state *st)
{
    st->matched = 1;
    st->skipped = 0;
    st->pos_cond = 0;
}

/*
 * Feed one table entry (addr word) into the state machine.
 * Returns 1 if a plain register write should be applied, 0 if skipped.
 * Delay tokens (low addresses like 0xf9..0xfe) also return 1 so the
 * per-table writer can dispatch them.
 */
static int rtw_phy_cond_step(struct rtw_phy_cond_state *st,
                             rtw_phy_u32 addr,
                             const struct rtw_phy_drv_cond *drv)
{
    if (addr & RTW_PHY_COND_POS) {
        rtw_phy_u32 branch = (addr & RTW_PHY_COND_BRANCH) >> 28;
        switch (branch) {
        case RTW_PHY_BRANCH_ENDIF:
            st->matched = 1;
            st->skipped = 0;
            break;
        case RTW_PHY_BRANCH_ELSE:
            st->matched = st->skipped ? 0 : 1;
            break;
        case RTW_PHY_BRANCH_IF:
        case RTW_PHY_BRANCH_ELIF:
        default:
            st->pos_cond = addr;
            break;
        }
        return 0;
    }
    if (addr & RTW_PHY_COND_NEG) {
        if (!st->skipped) {
            if (rtw_phy_cond_match(st->pos_cond, drv)) {
                st->matched = 1;
                st->skipped = 1;
            } else {
                st->matched = 0;
            }
        } else {
            st->matched = 0;
        }
        return 0;
    }
    return st->matched;
}

/*
 * SIPI RF write encoding (Linux rtw_phy_write_rf_reg_sipi):
 *   data_and_addr = ((addr << 20) | (data & 0x000fffff)) & 0x0fffffff
 * Path A SIPI control register is 0xc90; path B is 0xe90.
 */
#define RTW_PHY_SIPA_PATH_A    0xc90u
#define RTW_PHY_SIPA_PATH_B    0xe90u

static rtw_phy_u32 rtw_phy_sipa_encode(rtw_phy_u32 addr, rtw_phy_u32 data)
{
    return (((addr & 0xff) << 20) | (data & RTW_PHY_RFREG_MASK)) & 0x0fffffffu;
}

/* Direct RF register read address (Linux rtw_phy_read_rf):
 * base[path] + (addr << 2).  Path A base is 0x2800. */
#define RTW_PHY_RF_BASE_PATH_A 0x2800u
#define RTW_PHY_RF_BASE_PATH_B 0x2c00u

static rtw_phy_u32 rtw_phy_rf_read_addr(rtw_phy_u32 base, rtw_phy_u32 addr)
{
    return base + ((addr & 0xff) << 2);
}

#define RTW_PHY_RF18_BAND_MASK    ((1u << 16) | (1u << 9) | (1u << 8))
#define RTW_PHY_RF18_CHANNEL_MASK 0xffu
#define RTW_PHY_RF18_RFSI_MASK    ((1u << 18) | (1u << 17))
#define RTW_PHY_RF18_BW_MASK      ((1u << 11) | (1u << 10))
#define RTW_PHY_RF18_BW_20M       ((1u << 11) | (1u << 10))

static rtw_phy_u32 rtw_phy_rf18_20mhz(rtw_phy_u32 old, rtw_phy_u8 channel)
{
    rtw_phy_u32 clear = RTW_PHY_RF18_BAND_MASK | RTW_PHY_RF18_CHANNEL_MASK |
                        RTW_PHY_RF18_RFSI_MASK | RTW_PHY_RF18_BW_MASK;
    return (old & ~clear) | (channel & RTW_PHY_RF18_CHANNEL_MASK) |
           RTW_PHY_RF18_BW_20M;
}

static int rtw_phy_rfe_uses_btg(rtw_phy_u8 rfe)
{
    return rfe == 4 || rfe == 7 || rfe == 10 || rfe == 12 || rfe == 15;
}

/*
 * Iterate a flat {u32 addr, u32 data} pair array through the conditional
 * state machine, calling do_cfg for each accepted entry.
 * data must be N*8 bytes; returns number of pairs, or -1 on bad size.
 */
static int rtw_phy_tbl_apply(const void *data, rtw_phy_s32 size,
                             const struct rtw_phy_drv_cond *drv,
                             void (*do_cfg)(void *ctx,
                                            rtw_phy_u32 addr,
                                            rtw_phy_u32 data),
                             void *ctx)
{
    struct rtw_phy_cond_state st;
    rtw_phy_s32 n_pairs, i;
    const rtw_phy_u8 *p;

    if (!data || size < 0 || (size & 7) != 0 || size < 8)
        return -1;
    p = (const rtw_phy_u8 *)data;
    n_pairs = size / 8;
    rtw_phy_cond_state_init(&st);
    for (i = 0; i < n_pairs; i++) {
        rtw_phy_u32 addr = (rtw_phy_u32)p[i*8]   | ((rtw_phy_u32)p[i*8+1] << 8)
                         | ((rtw_phy_u32)p[i*8+2] << 16) | ((rtw_phy_u32)p[i*8+3] << 24);
        rtw_phy_u32 dat  = (rtw_phy_u32)p[i*8+4] | ((rtw_phy_u32)p[i*8+5] << 8)
                         | ((rtw_phy_u32)p[i*8+6] << 16) | ((rtw_phy_u32)p[i*8+7] << 24);
        if (rtw_phy_cond_step(&st, addr, drv))
            do_cfg(ctx, addr, dat);
    }
    return n_pairs;
}

#endif /* ICSOS_RTW_PHY_TBL_H */
