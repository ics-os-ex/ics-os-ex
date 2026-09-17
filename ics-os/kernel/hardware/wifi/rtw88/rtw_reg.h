/*
 * Portions derived from Linux rtw88 (GPL-2.0 OR BSD-3-Clause), Realtek / Larry Finger et al.
 * Essential register definitions for RTL8821CE bring-up.
 */
#ifndef ICSOS_RTW_REG_H
#define ICSOS_RTW_REG_H

#ifndef BIT
#define BIT(n) (1u << (n))
#endif

#define REG_SYS_FUNC_EN          0x0002
#define BIT_FEN_CPUEN            BIT(2)
#define BIT_FEN_BB_GLB_RST       BIT(1)
#define BIT_FEN_BB_RSTB          BIT(0)
#define BIT_FEN_PCIEA            BIT(6)

#define REG_SYS_PW_CTRL          0x0004
#define BIT_PFM_WOWL             BIT(3)

#define REG_SYS_CLK_CTRL         0x0008
#define BIT_CPU_CLK_EN           BIT(14)

#define REG_EFUSE_CTRL           0x0030
#define BIT_EF_FLAG              BIT(31)
#define BIT_SHIFT_EF_ADDR        8
#define BIT_MASK_EF_ADDR         0x3ff
#define BIT_MASK_EF_DATA         0xff
#define BITS_EF_ADDR             (BIT_MASK_EF_ADDR << BIT_SHIFT_EF_ADDR)

#define REG_LDO_EFUSE_CTRL       0x0034
#define BIT_MASK_EFUSE_BANK_SEL  (BIT(8) | BIT(9))

#define REG_RSV_CTRL             0x001C
#define BIT_WLMCU_IOIF           BIT(0)

#define REG_RF_CTRL              0x001F
#define BIT_RF_SDM_RSTB          BIT(2)
#define BIT_RF_RSTB              BIT(1)
#define BIT_RF_EN                BIT(0)

#define REG_GPIO_MUXCFG          0x0040
#define BIT_FSPI_EN              BIT(19)
#define BIT_WLRFE_4_5_EN         BIT(2)

#define REG_LED_CFG              0x004C
#define BIT_LNAON_SEL_EN         BIT(26)
#define BIT_PAPE_SEL_EN          BIT(25)

#define REG_PAD_CTRL1            0x0064
#define BIT_PAPE_WLBT_SEL        BIT(29)
#define BIT_LNAON_WLBT_SEL       BIT(28)

#define REG_HCI_OPT_CTRL         0x0074
#define BIT_USB_SUS_DIS          BIT(8)

#define REG_MCUFW_CTRL           0x0080
#define BIT_BOOT_FSPI_EN         BIT(20)
#define BIT_FW_INIT_RDY          BIT(15)
#define BIT_FW_DW_RDY            BIT(14)
#define BIT_DMEM_CHKSUM_OK       BIT(6)
#define BIT_DMEM_DW_OK           BIT(5)
#define BIT_IMEM_CHKSUM_OK       BIT(4)
#define BIT_IMEM_DW_OK           BIT(3)
#define BIT_MCUFWDL_EN           BIT(0)
#define BIT_CHECK_SUM_OK         (BIT(4) | BIT(6))
#define FW_READY                 (BIT_FW_INIT_RDY | BIT_FW_DW_RDY | \
                                  BIT_IMEM_DW_OK | BIT_DMEM_DW_OK | \
                                  BIT_CHECK_SUM_OK)
#define FW_READY_MASK            0xffff

#define REG_SYS_CFG1             0x00F0
#define BIT_SHIFT_CHIP_VER       12
#define BIT_MASK_CHIP_VER        0xf
#define BIT_GET_CHIP_VER(x)      (((x) >> BIT_SHIFT_CHIP_VER) & BIT_MASK_CHIP_VER)

#define REG_WLRF1                0x00EC
#define BIT_WLRF1_BBRF_EN        (BIT(24) | BIT(25) | BIT(26))

#define REG_CR                   0x0100
#define BIT_ENSWBCN              BIT(8)
#define BIT_TXDMA_EN             BIT(2)
#define BIT_RXDMA_EN             BIT(3)
#define BIT_HCI_RXDMA_EN         BIT(1)
#define BIT_HCI_TXDMA_EN         BIT(0)
#define BIT_PROTOCOL_EN          BIT(4)
#define BIT_SCHEDULE_EN          BIT(5)
#define BIT_MACTXEN              BIT(6)
#define BIT_MACRXEN              BIT(7)
/* Match the reference exactly: all 8 MAC control bits. MACRXEN/MACTXEN
 * (bits 7/6) are the MAC receive/transmit enables; without MACRXEN the MAC
 * never processes received frames (rx_ok=0, RXBD write pointer stuck, bss=0). */
#define MAC_TRX_ENABLE           (BIT_HCI_TXDMA_EN | BIT_HCI_RXDMA_EN | \
                                  BIT_TXDMA_EN | BIT_RXDMA_EN | \
                                  BIT_PROTOCOL_EN | BIT_SCHEDULE_EN | \
                                  BIT_MACTXEN | BIT_MACRXEN)

#define REG_TXDMA_PQ_MAP         0x010C

#define REG_FIFOPAGE_CTRL_2      0x0204
#define BIT_BCN_VALID_V1         BIT(15)
#define BIT_MASK_BCN_HEAD_1_V1   0xfff

#define REG_AUTO_LLT_V1          0x0208
#define BIT_AUTO_INIT_LLT_V1     BIT(0)

#define REG_TXDMA_STATUS         0x0210
#define BTI_PAGE_OVF             BIT(2)

#define REG_RQPN_CTRL_2          0x022C
#define BIT_LD_RQPN              BIT(31)

#define REG_RXFF_BNDY            0x011C
#define REG_BCNQ_BDNY_V1         0x0424
#define REG_BCNQ1_BDNY_V1        0x0424

#define REG_FIFOPAGE_INFO_1      0x0230
#define REG_FIFOPAGE_INFO_2      0x0234
#define REG_FIFOPAGE_INFO_3      0x0238
#define REG_FIFOPAGE_INFO_4      0x023C
#define REG_FIFOPAGE_INFO_5      0x0240

#define REG_FWHW_TXQ_CTRL        0x0420
#define BIT_EN_BCNQ_DL           BIT(22)

#define REG_BCN_CTRL             0x0550
#define BIT_DIS_TSF_UDT          BIT(4)
#define BIT_EN_BCN_FUNCTION      BIT(3)
#define REG_USTIME_TSF           0x055C

#define REG_RCR                  0x0608
#define BIT_APP_PHYSTS           BIT(28)
#define BIT_CBSSID_BCN           BIT(7)

#define REG_RX_PKT_LIMIT         0x060C
#define REG_MACID                0x0610
#define REG_RX_DRVINFO_SZ        0x060F
#define REG_USTIME_EDCA          0x0638
#define REG_WMAC_TRXPTCL_CTL     0x0668
#define BIT_RFMOD                (BIT(7) | BIT(8))
#define REG_DATA_SC              0x0483
#define REG_CCK_CHECK            0x0454

#define REG_RXFLTMAP0            0x06A0
#define REG_RXFLTMAP2            0x06A4

#define REG_CPU_DMEM_CON         0x1080
#define BIT_WL_PLATFORM_RST      BIT(16)
#define BIT_DDMA_EN              BIT(8)

#define REG_CR_EXT               0x1100

#define REG_DDMA_CH0SA           0x1200
#define REG_DDMA_CH0DA           0x1204
#define REG_DDMA_CH0CTRL         0x1208
#define BIT_DDMACH0_OWN          BIT(31)
#define BIT_DDMACH0_CHKSUM_EN    BIT(29)
#define BIT_DDMACH0_CHKSUM_STS   BIT(27)
#define BIT_DDMACH0_RESET_CHKSUM_STS BIT(25)
#define BIT_DDMACH0_CHKSUM_CONT  BIT(24)
#define BIT_MASK_DDMACH0_DLEN    0x3ffff

#define REG_H2CQ_CSR             0x1330
#define BIT_H2CQ_FULL            BIT(31)

#define REG_FW_DBG7              0x00CC
#define FW_KEY_MASK              0xffffff00
#define ILLEGAL_KEY_GROUP        0xFAAAAA00

#define REG_AFE_XTAL_CTRL        0x0024
#define REG_AFE_PLL_CTRL         0x0028
#define REG_AFE_CTRL             0x002C
#define BIT_AFE_SEL_XTAL         BIT(4)
#define REG_PSEMI                0x100C
#define BIT_PSEMI_REG_SEL        BIT(20)
#define BIT_PSEMI_READ           BIT(1)
#define BIT_PSEMI_WRITE          BIT(0)
#define REG_PSEMI_DATA           0x1004
#define REG_AFE_MIMO_TX_AGC_1    0x084C
#define REG_AFE_MIMO_TX_AGC_2    0x084D
#define REG_AFE_RX_AGC1          0x0870
#define REG_AFE_RX_AGC2          0x0871
#define REG_AFE_RX_AGC3          0x0872
#define REG_AFE_RX_AGC4          0x0873
#define REG_AFE_TX_AGC1          0x086C
#define REG_AFE_TX_AGC2          0x086D
#define REG_AFE_TX_AGC3          0x086E
#define REG_AFE_TX_AGC4          0x086F
#define REG_TX_IQK               0x1010
#define BIT_TX_IQK               BIT(8)
#define REG_TX_IQK_DONE          0x1010
#define BIT_TX_IQK_DONE          BIT(15)
#define REG_TX_IQK_RSLT          0x1014
#define REG_RX_IQK               0x1018
#define BIT_RX_IQK               BIT(16)
#define REG_RX_IQK_DONE          0x1018
#define BIT_RX_IQK_DONE          BIT(17)
#define REG_RX_IQK_RSLT          0x101C
#define REG_RFE_ANA_REG0         0x2020

#define REG_RXPSEL               0x0808
#define BIT_RX_PSEL_RST          BIT(13)
#define REG_RXCCAMSK             0x0814
#define REG_CLKTRK               0x0860
#define REG_ADCCLK               0x08AC
#define REG_ADC160               0x08C4
#define REG_CHFIR                0x08F0
#define REG_ACBB0                0x0948
#define REG_ACBBRXFIR            0x094C
#define REG_ENTXCCK              0x0A80
#define REG_TXSCALE_A            0x0C1C
#define REG_TXDFIR               0x0C20

/* PCI TXBD / RXBD */
#define RTK_PCI_CTRL             0x0300
#define BIT_RST_TRXDMA_INTF      BIT(0)
#define BIT_RX_TAG_EN            BIT(13)
#define RTK_PCI_TXBD_DESA_BCNQ   0x0308
#define RTK_PCI_RXBD_DESA_MPDUQ  0x0338
#define RTK_PCI_TXBD_BCN_WORK    0x0383
#define BIT_PCI_BCNQ_FLAG        BIT(4)
#define RTK_PCI_RXBD_NUM_MPDUQ   0x0382
#define RTK_PCI_TXBD_RWPTR_CLR   0x039C
#define RTK_PCI_RXBD_IDX_MPDUQ   0x03B4
#define RTK_PCI_TXBD_OWN_OFFSET  15
#define TRX_BD_HW_IDX_MASK       0x0FFF0000u
#define TRX_BD_HW_IDX_SHIFT      16
#define TRX_BD_IDX_MASK          0x0FFFu

#define OCPBASE_TXBUF_88XX       0x18780000u
#define OCPBASE_DMEM_88XX        0x00200000u

#define RTW_8821C_SYS_FUNC_EN    0xD8
#define RTW_TX_PKT_DESC_SZ       48
#define RTW_TX_BUF_DESC_SZ       16
#define RTW_RX_PKT_DESC_SZ       24
#define RTW_RX_BUF_DESC_SZ       8
#define RTW_RX_BUF_SIZE          2048
#define RTW_RX_RING_SIZE         32

#define WLAN_RX_FILTER0          0x0FFFFFFFu
#define WLAN_RX_FILTER2          0xFFFFu
#define WLAN_RCR_CFG             0xE400220Eu
#define WLAN_RXPKT_MAX_SZ_512    0x17

#define PHY_STATUS_SIZE          4

#endif
