/*
 * Portions derived from Linux rtw88 (GPL-2.0 OR BSD-3-Clause), Realtek / Larry Finger et al.
 */
#include "rtw_fw.h"
#include "rtw_fw_hdr.h"
#include "rtw_io.h"
#include "rtw_pci.h"
#include "rtw_reg.h"
#include "../../../vfs/vfs_core.h"

extern int printf(const char *fmt, ...);
extern void *malloc(unsigned int);
extern void free(void *);
extern void *memset(void *s, int c, unsigned int n);

struct rtw_backup_info {
    u8 len;
    u32 reg;
    u32 val;
};

#define DLFW_RESTORE_REG_NUM 6

void rtw_fw_free(struct rtw_dev *rtwdev)
{
    if (rtwdev->fw_data) {
        free(rtwdev->fw_data);
        rtwdev->fw_data = 0;
        rtwdev->fw_len = 0;
    }
}

int rtw_fw_load_file(struct rtw_dev *rtwdev, const char *path)
{
    file_PCB *f;
    vfs_stat st;
    u8 *buf;
    int n;

    if (!rtwdev || !path)
        return -1;
    f = openfilex((char *)path, FILE_READ);
    if (!f)
        return -1;
    if (fstat(f, &st) != 0 || st.st_size < (int)RTW_FW_HDR_SIZE ||
        st.st_size > (8 * 1024 * 1024)) {
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
    if (!rtw_fw_hdr_valid(buf, (u32)st.st_size)) {
        printf("rtw88: firmware header invalid size=%d\n", st.st_size);
        free(buf);
        return -1;
    }
    rtw_fw_free(rtwdev);
    rtwdev->fw_data = buf;
    rtwdev->fw_len = (u32)st.st_size;
    printf("rtw88: firmware loaded %s (%d bytes)\n", path, st.st_size);
    return 0;
}

static void wlan_cpu_enable(struct rtw_dev *rtwdev, int enable)
{
    if (enable) {
        rtw_write8_set(rtwdev, REG_RSV_CTRL + 1, BIT_WLMCU_IOIF);
        rtw_write8_set(rtwdev, REG_SYS_FUNC_EN + 1, BIT_FEN_CPUEN);
    } else {
        rtw_write8_clr(rtwdev, REG_SYS_FUNC_EN + 1, BIT_FEN_CPUEN);
        rtw_write8_clr(rtwdev, REG_RSV_CTRL + 1, BIT_WLMCU_IOIF);
    }
}

static void download_firmware_reg_backup(struct rtw_dev *rtwdev,
                                         struct rtw_backup_info *bckp)
{
    u8 tmp;
    u8 idx = 0;

    bckp[idx].len = 1;
    bckp[idx].reg = REG_TXDMA_PQ_MAP + 1;
    bckp[idx].val = rtw_read8(rtwdev, REG_TXDMA_PQ_MAP + 1);
    idx++;
    tmp = (u8)(3u << 6); /* RTW_DMA_MAPPING_HIGH */
    rtw_write8(rtwdev, REG_TXDMA_PQ_MAP + 1, tmp);

    bckp[idx].len = 1;
    bckp[idx].reg = REG_CR;
    bckp[idx].val = rtw_read8(rtwdev, REG_CR);
    idx++;
    bckp[idx].len = 4;
    bckp[idx].reg = REG_H2CQ_CSR;
    bckp[idx].val = BIT_H2CQ_FULL;
    idx++;
    rtw_write8(rtwdev, REG_CR, (u8)(BIT_HCI_TXDMA_EN | BIT_TXDMA_EN));
    rtw_write32(rtwdev, REG_H2CQ_CSR, BIT_H2CQ_FULL);

    bckp[idx].len = 2;
    bckp[idx].reg = REG_FIFOPAGE_INFO_1;
    bckp[idx].val = rtw_read16(rtwdev, REG_FIFOPAGE_INFO_1);
    idx++;
    bckp[idx].len = 4;
    bckp[idx].reg = REG_RQPN_CTRL_2;
    bckp[idx].val = rtw_read32(rtwdev, REG_RQPN_CTRL_2) | BIT_LD_RQPN;
    idx++;
    rtw_write16(rtwdev, REG_FIFOPAGE_INFO_1, 0x200);
    rtw_write32(rtwdev, REG_RQPN_CTRL_2, bckp[idx - 1].val);

    tmp = rtw_read8(rtwdev, REG_BCN_CTRL);
    bckp[idx].len = 1;
    bckp[idx].reg = REG_BCN_CTRL;
    bckp[idx].val = tmp;
    idx++;
    tmp = (u8)((tmp & ~BIT_EN_BCN_FUNCTION) | BIT_DIS_TSF_UDT);
    rtw_write8(rtwdev, REG_BCN_CTRL, tmp);
    (void)idx;
}

static void download_firmware_reset_platform(struct rtw_dev *rtwdev)
{
    rtw_write8_clr(rtwdev, REG_CPU_DMEM_CON + 2, (u8)(BIT_WL_PLATFORM_RST >> 16));
    rtw_write8_clr(rtwdev, REG_SYS_CLK_CTRL + 1, (u8)(BIT_CPU_CLK_EN >> 8));
    rtw_write8_set(rtwdev, REG_CPU_DMEM_CON + 2, (u8)(BIT_WL_PLATFORM_RST >> 16));
    rtw_write8_set(rtwdev, REG_SYS_CLK_CTRL + 1, (u8)(BIT_CPU_CLK_EN >> 8));
}

static void download_firmware_reg_restore(struct rtw_dev *rtwdev,
                                          struct rtw_backup_info *bckp, u8 n)
{
    u8 i;
    for (i = 0; i < n; i++) {
        if (bckp[i].len == 1)
            rtw_write8(rtwdev, bckp[i].reg, (u8)bckp[i].val);
        else if (bckp[i].len == 2)
            rtw_write16(rtwdev, bckp[i].reg, (u16)bckp[i].val);
        else
            rtw_write32(rtwdev, bckp[i].reg, bckp[i].val);
    }
}

static int iddma_enable(struct rtw_dev *rtwdev, u32 src, u32 dst, u32 ctrl)
{
    rtw_write32(rtwdev, REG_DDMA_CH0SA, src);
    rtw_write32(rtwdev, REG_DDMA_CH0DA, dst);
    rtw_write32(rtwdev, REG_DDMA_CH0CTRL, ctrl);
    if (!rtw_check_hw_ready(rtwdev, REG_DDMA_CH0CTRL, BIT_DDMACH0_OWN, 0))
        return -1;
    return 0;
}

static int iddma_download_firmware(struct rtw_dev *rtwdev, u32 src, u32 dst,
                                   u32 len, u8 first)
{
    u32 ch0_ctrl = BIT_DDMACH0_CHKSUM_EN | BIT_DDMACH0_OWN;

    if (!rtw_check_hw_ready(rtwdev, REG_DDMA_CH0CTRL, BIT_DDMACH0_OWN, 0))
        return -1;
    ch0_ctrl |= len & BIT_MASK_DDMACH0_DLEN;
    if (!first)
        ch0_ctrl |= BIT_DDMACH0_CHKSUM_CONT;
    return iddma_enable(rtwdev, src, dst, ch0_ctrl);
}

static int check_fw_checksum(struct rtw_dev *rtwdev, u32 addr)
{
    u8 fw_ctrl = rtw_read8(rtwdev, REG_MCUFW_CTRL);

    if (rtw_read32(rtwdev, REG_DDMA_CH0CTRL) & BIT_DDMACH0_CHKSUM_STS) {
        if (addr < OCPBASE_DMEM_88XX) {
            fw_ctrl = (u8)((fw_ctrl | BIT_IMEM_DW_OK) & ~BIT_IMEM_CHKSUM_OK);
            rtw_write8(rtwdev, REG_MCUFW_CTRL, fw_ctrl);
        } else {
            fw_ctrl = (u8)((fw_ctrl | BIT_DMEM_DW_OK) & ~BIT_DMEM_CHKSUM_OK);
            rtw_write8(rtwdev, REG_MCUFW_CTRL, fw_ctrl);
        }
        printf("rtw88: invalid fw checksum\n");
        return 0;
    }
    if (addr < OCPBASE_DMEM_88XX)
        fw_ctrl = (u8)(fw_ctrl | BIT_IMEM_DW_OK | BIT_IMEM_CHKSUM_OK);
    else
        fw_ctrl = (u8)(fw_ctrl | BIT_DMEM_DW_OK | BIT_DMEM_CHKSUM_OK);
    rtw_write8(rtwdev, REG_MCUFW_CTRL, fw_ctrl);
    return 1;
}

static int send_firmware_pkt(struct rtw_dev *rtwdev, u16 pg_addr,
                             const u8 *data, u32 size)
{
    u8 bckp0, bckp1, val;
    u16 pg;
    int ret;

    /* Mirror rtw_fw_write_data_rsvd_page (11ac / PCIe). */
    pg = (u16)((pg_addr & BIT_MASK_BCN_HEAD_1_V1) | BIT_BCN_VALID_V1);
    rtw_write16(rtwdev, REG_FIFOPAGE_CTRL_2, pg);

    val = rtw_read8(rtwdev, REG_CR + 1);
    bckp0 = val;
    rtw_write8(rtwdev, REG_CR + 1, (u8)(val | (BIT_ENSWBCN >> 8)));

    val = rtw_read8(rtwdev, REG_FWHW_TXQ_CTRL + 2);
    bckp1 = val;
    rtw_write8(rtwdev, REG_FWHW_TXQ_CTRL + 2, (u8)(val & ~(BIT_EN_BCNQ_DL >> 16)));

    ret = rtw_pci_write_data_rsvd_page(rtwdev, data, size);

    rtw_write8(rtwdev, REG_FWHW_TXQ_CTRL + 2, bckp1);
    rtw_write8(rtwdev, REG_CR + 1, bckp0);
    return ret;
}

static int download_firmware_to_mem(struct rtw_dev *rtwdev, const u8 *data,
                                    u32 src, u32 dst, u32 size)
{
    u32 desc_size = RTW_TX_PKT_DESC_SZ;
    u8 first_part = 1;
    u32 mem_offset = 0;
    u32 residue = size;
    u32 pkt_size;
    u32 max_size = 0x1000;
    u32 val;
    int ret;

    val = rtw_read32(rtwdev, REG_DDMA_CH0CTRL);
    val |= BIT_DDMACH0_RESET_CHKSUM_STS;
    rtw_write32(rtwdev, REG_DDMA_CH0CTRL, val);

    while (residue) {
        pkt_size = (residue >= max_size) ? max_size : residue;
        ret = send_firmware_pkt(rtwdev, (u16)(src >> 7),
                                data + mem_offset, pkt_size);
        if (ret)
            return ret;
        ret = iddma_download_firmware(rtwdev,
                                      OCPBASE_TXBUF_88XX + src + desc_size,
                                      dst + mem_offset, pkt_size, first_part);
        if (ret)
            return ret;
        first_part = 0;
        mem_offset += pkt_size;
        residue -= pkt_size;
    }
    if (!check_fw_checksum(rtwdev, dst))
        return -1;
    return 0;
}

static int start_download_firmware(struct rtw_dev *rtwdev, const u8 *data,
                                   u32 size)
{
    const struct rtw_fw_hdr *fw_hdr = (const struct rtw_fw_hdr *)data;
    const u8 *cur;
    u16 val;
    u32 imem_size, dmem_size, emem_size, addr;
    int ret;

    (void)size;
    dmem_size = fw_hdr->dmem_size + RTW_FW_HDR_CHKSUM_SIZE;
    imem_size = fw_hdr->imem_size + RTW_FW_HDR_CHKSUM_SIZE;
    emem_size = (fw_hdr->mem_usage & (1u << 4))
                    ? (fw_hdr->emem_size + RTW_FW_HDR_CHKSUM_SIZE)
                    : 0;

    val = (u16)(rtw_read16(rtwdev, REG_MCUFW_CTRL) & 0x3800);
    val = (u16)(val | BIT_MCUFWDL_EN);
    rtw_write16(rtwdev, REG_MCUFW_CTRL, val);

    cur = data + RTW_FW_HDR_SIZE;
    addr = fw_hdr->dmem_addr & ~BIT(31);
    ret = download_firmware_to_mem(rtwdev, cur, 0, addr, dmem_size);
    if (ret)
        return ret;

    cur = data + RTW_FW_HDR_SIZE + dmem_size;
    addr = fw_hdr->imem_addr & ~BIT(31);
    ret = download_firmware_to_mem(rtwdev, cur, 0, addr, imem_size);
    if (ret)
        return ret;

    if (emem_size) {
        cur = data + RTW_FW_HDR_SIZE + dmem_size + imem_size;
        addr = fw_hdr->emem_addr & ~BIT(31);
        ret = download_firmware_to_mem(rtwdev, cur, 0, addr, emem_size);
        if (ret)
            return ret;
    }
    return 0;
}

static void download_firmware_end_flow(struct rtw_dev *rtwdev)
{
    u16 fw_ctrl;

    rtw_write32(rtwdev, REG_TXDMA_STATUS, BTI_PAGE_OVF);
    fw_ctrl = rtw_read16(rtwdev, REG_MCUFW_CTRL);
    if ((fw_ctrl & BIT_CHECK_SUM_OK) != BIT_CHECK_SUM_OK)
        return;
    fw_ctrl = (u16)((fw_ctrl | BIT_FW_DW_RDY) & ~BIT_MCUFWDL_EN);
    rtw_write16(rtwdev, REG_MCUFW_CTRL, fw_ctrl);
}

static int download_firmware_validate(struct rtw_dev *rtwdev)
{
    u32 fw_key;
    if (!rtw_check_hw_ready(rtwdev, REG_MCUFW_CTRL, FW_READY_MASK, FW_READY)) {
        fw_key = rtw_read32(rtwdev, REG_FW_DBG7) & FW_KEY_MASK;
        if (fw_key == ILLEGAL_KEY_GROUP)
            printf("rtw88: invalid fw key\n");
        printf("rtw88: FW_READY timeout MCUFW_CTRL=0x%x\n",
               rtw_read32(rtwdev, REG_MCUFW_CTRL));
        return -1;
    }
    return 0;
}

int rtw_download_firmware(struct rtw_dev *rtwdev)
{
    struct rtw_backup_info bckp[DLFW_RESTORE_REG_NUM];
    int ret;

    if (!rtwdev->fw_data || !rtwdev->fw_len)
        return -1;
    if (!rtw_fw_hdr_valid(rtwdev->fw_data, rtwdev->fw_len))
        return -1;

    memset(bckp, 0, sizeof(bckp));
    wlan_cpu_enable(rtwdev, 0);
    download_firmware_reg_backup(rtwdev, bckp);
    download_firmware_reset_platform(rtwdev);

    ret = start_download_firmware(rtwdev, rtwdev->fw_data, rtwdev->fw_len);
    if (ret)
        goto fail;

    download_firmware_reg_restore(rtwdev, bckp, DLFW_RESTORE_REG_NUM);
    download_firmware_end_flow(rtwdev);
    wlan_cpu_enable(rtwdev, 1);

    ret = download_firmware_validate(rtwdev);
    if (ret)
        goto fail;

    rtwdev->fw_ready = 1;
    rtwdev->flags |= RTW_FLAG_FW_RUNNING;
    return 0;

fail:
    wlan_cpu_enable(rtwdev, 0);
    rtwdev->fw_ready = 0;
    return -1;
}
