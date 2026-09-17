/*
 * Portions derived from Linux rtw88 (GPL-2.0 OR BSD-3-Clause), Realtek / Larry Finger et al.
 * Post-FW MAC TRX enable + WMAC RX filters; thin BB/RF power-on (no phy tables).
 */
#include "rtw_mac.h"
#include "rtw_io.h"

extern int printf(const char *fmt, ...);

#define RTW_TXFF_SIZE_8821C      65536u
#define RTW_RXFF_SIZE_8821C      16384u
#define RTW_RSVD_DRV_PG_8821C    8u
#define RTW_RSVD_H2C_EXTRA       24u
#define RTW_RSVD_H2C_STATIC      8u
#define RTW_RSVD_H2CQ            8u
#define RTW_RSVD_FW_TXBUF        4u
#define RTW_C2H_PKT_BUF          256u

void rtw_mac_write_addr(struct rtw_dev *rtwdev, const u8 mac[6])
{
    int i;
    for (i = 0; i < 6; i++)
        rtw_write8(rtwdev, REG_MACID + i, mac[i]);
}

static void rtw8821c_mac_init_wmac(struct rtw_dev *rtwdev)
{
    rtw_write32(rtwdev, REG_RXFLTMAP0, WLAN_RX_FILTER0);
    rtw_write16(rtwdev, REG_RXFLTMAP2, WLAN_RX_FILTER2);
    /* Promiscuous beacons for scan: clear CBSSID filter. */
    rtw_write32(rtwdev, REG_RCR, (WLAN_RCR_CFG | BIT_APP_PHYSTS) & ~BIT_CBSSID_BCN);
    rtw_write8(rtwdev, REG_RX_PKT_LIMIT, WLAN_RXPKT_MAX_SZ_512);
    rtw_write8_set(rtwdev, REG_BCN_CTRL, BIT_EN_BCN_FUNCTION);
    rtw_write8(rtwdev, REG_RX_DRVINFO_SZ, PHY_STATUS_SIZE);
}

int rtw_mac_init_post_fw(struct rtw_dev *rtwdev)
{
    u32 txdma_pq_map;
    u16 txff_pg;
    u16 rsvd_pg;
    u16 rsvd_boundary;
    u16 acq_pg;
    u16 pubq;

    /* PCIE rqpn_table_8821c[1]: VO/VI NORMAL, BE/BK LOW, MG EXTRA, HI HIGH. */
    txdma_pq_map = 0;
    txdma_pq_map |= (2u << 4);  /* VO */
    txdma_pq_map |= (2u << 6);  /* VI */
    txdma_pq_map |= (1u << 8);  /* BE */
    txdma_pq_map |= (1u << 10); /* BK */
    txdma_pq_map |= (0u << 12); /* MG */
    txdma_pq_map |= (3u << 14); /* HI */
    rtw_write16(rtwdev, REG_TXDMA_PQ_MAP, (u16)txdma_pq_map);

    rtw_write8(rtwdev, REG_CR, 0);
    rtw_write8(rtwdev, REG_CR, (u8)MAC_TRX_ENABLE);
    rtw_write32(rtwdev, REG_H2CQ_CSR, BIT_H2CQ_FULL);

    txff_pg = (u16)(RTW_TXFF_SIZE_8821C >> 7);
    rsvd_pg = (u16)(RTW_RSVD_DRV_PG_8821C + RTW_RSVD_H2C_EXTRA +
                    RTW_RSVD_H2C_STATIC + RTW_RSVD_H2CQ +
                    RTW_RSVD_FW_TXBUF);
    rsvd_boundary = (u16)(txff_pg - rsvd_pg);
    acq_pg = rsvd_boundary;
    pubq = (u16)(acq_pg - (16 + 16 + 16 + 14));

    rtw_write16(rtwdev, REG_FIFOPAGE_INFO_1, 16);  /* HQ */
    rtw_write16(rtwdev, REG_FIFOPAGE_INFO_2, 16);  /* LQ */
    rtw_write16(rtwdev, REG_FIFOPAGE_INFO_3, 16);  /* NQ */
    rtw_write16(rtwdev, REG_FIFOPAGE_INFO_4, 14);  /* EXQ */
    rtw_write16(rtwdev, REG_FIFOPAGE_INFO_5, pubq);
    rtw_write32_set(rtwdev, REG_RQPN_CTRL_2, BIT_LD_RQPN);

    rtw_write16(rtwdev, REG_FIFOPAGE_CTRL_2, rsvd_boundary);
    rtw_write16(rtwdev, REG_BCNQ_BDNY_V1, rsvd_boundary);
    rtw_write16(rtwdev, REG_FIFOPAGE_CTRL_2 + 2, rsvd_boundary);
    rtw_write16(rtwdev, REG_BCNQ1_BDNY_V1, rsvd_boundary);
    rtw_write32(rtwdev, REG_RXFF_BNDY, RTW_RXFF_SIZE_8821C - RTW_C2H_PKT_BUF - 1);

    rtw_write8_set(rtwdev, REG_AUTO_LLT_V1, BIT_AUTO_INIT_LLT_V1);
    if (!rtw_check_hw_ready(rtwdev, REG_AUTO_LLT_V1, BIT_AUTO_INIT_LLT_V1, 0)) {
        printf("RTL8821CE_MAC_LLT_FAIL\n");
        return -1;
    }
    rtw_write8(rtwdev, REG_CR + 3, 0);

    rtw8821c_mac_init_wmac(rtwdev);
    rtwdev->flags |= RTW_FLAG_MAC_READY;

    printf("RTL8821CE_MAC_OK CR=0x%x RCR=0x%x boundary=%u\n",
           rtw_read32(rtwdev, REG_CR), rtw_read32(rtwdev, REG_RCR),
           rsvd_boundary);
    return 0;
}

int rtw_phy_basic_init(struct rtw_dev *rtwdev)
{
    u8 val;

    val = rtw_read8(rtwdev, REG_SYS_FUNC_EN);
    val = (u8)(val | BIT_FEN_PCIEA);
    rtw_write8(rtwdev, REG_SYS_FUNC_EN, val);

    val = (u8)(val | BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST);
    rtw_write8(rtwdev, REG_SYS_FUNC_EN, val);
    val = (u8)(val & ~(BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST));
    rtw_write8(rtwdev, REG_SYS_FUNC_EN, val);
    val = (u8)(val | BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST);
    rtw_write8(rtwdev, REG_SYS_FUNC_EN, val);

    rtw_write8(rtwdev, REG_RF_CTRL,
               (u8)(BIT_RF_EN | BIT_RF_RSTB | BIT_RF_SDM_RSTB));
    rtw_udelay(20);
    rtw_write8(rtwdev, REG_WLRF1 + 3,
               (u8)(BIT_RF_EN | BIT_RF_RSTB | BIT_RF_SDM_RSTB));
    rtw_udelay(20);

    /* Linux rtw8821c_phy_set_param: clear before loading PHY tables. */
    rtw_write32_clr(rtwdev, REG_RXPSEL, BIT_RX_PSEL_RST);

    printf("RTL8821CE_PHY_PRE_OK rfe=%u\n", rtwdev->rfe_option);
    return 0;
}
