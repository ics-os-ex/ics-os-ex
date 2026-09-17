/*
 * Portions derived from Linux rtw88 (GPL-2.0 OR BSD-3-Clause), Realtek / Larry Finger et al.
 * Pure firmware-header helpers (host unit + kernel).
 */
#ifndef ICSOS_RTW_FW_HDR_H
#define ICSOS_RTW_FW_HDR_H

#include "../../../types.h"

#define RTW_FW_HDR_SIZE          64
#define RTW_FW_HDR_CHKSUM_SIZE   8
#define RTW_FW_SIGNATURE_8821C   0x8821

#pragma pack(push, 1)
struct rtw_fw_hdr {
    u16 signature;       /* 0x00 LE */
    u8  category;
    u8  function;
    u16 version;         /* 0x04 */
    u8  subversion;
    u8  subindex;
    u32 rsvd;            /* 0x08 */
    u32 feature;         /* 0x0C */
    u8  month;           /* 0x10 */
    u8  day;
    u8  hour;
    u8  min;
    u16 year;            /* 0x14 */
    u16 rsvd3;
    u8  mem_usage;       /* 0x18 */
    u8  rsvd4[3];
    u16 h2c_fmt_ver;     /* 0x1C */
    u16 rsvd5;
    u32 dmem_addr;       /* 0x20 */
    u32 dmem_size;
    u32 rsvd6;
    u32 rsvd7;
    u32 imem_size;       /* 0x30 */
    u32 emem_size;
    u32 emem_addr;
    u32 imem_addr;
};
#pragma pack(pop)

/*
 * rtw8821c PHY register table blobs (bb / rf / agc / mac):
 * raw flat u32 word array, each entry a { u32 addr, u32 data } pair.
 * No header.  Entries may include conditional-branch tokens (bit 31 = pos,
 * bit 30 = neg) and per-table delay tokens.  Parsing and per-table writers
 * live in rtw_phy.c (see rtw_phy_tbl_apply_*).
 */
static inline u32 rtw_phy_tbl_pair_count(u32 file_size)
{
    if ((file_size & 7) != 0)
        return 0;
    return file_size / 8;
}

static inline u16 rtw_fw_hdr_signature(const struct rtw_fw_hdr *h)
{
    return h ? h->signature : 0;
}

static inline int rtw_fw_hdr_valid(const void *data, u32 size)
{
    const struct rtw_fw_hdr *h;
    u32 dmem_size, imem_size, emem_size, real_size;

    if (!data || size < RTW_FW_HDR_SIZE)
        return 0;
    h = (const struct rtw_fw_hdr *)data;
    if (h->signature != RTW_FW_SIGNATURE_8821C)
        return 0;

    dmem_size = h->dmem_size;
    imem_size = h->imem_size;
    emem_size = (h->mem_usage & (1u << 4)) ? h->emem_size : 0;
    dmem_size += RTW_FW_HDR_CHKSUM_SIZE;
    imem_size += RTW_FW_HDR_CHKSUM_SIZE;
    if (emem_size)
        emem_size += RTW_FW_HDR_CHKSUM_SIZE;
    real_size = RTW_FW_HDR_SIZE + dmem_size + imem_size + emem_size;
    return real_size == size;
}

#endif
