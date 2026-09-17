/*
 * Portions derived from Linux rtw88 (GPL-2.0 OR BSD-3-Clause), Realtek / Larry Finger et al.
 * Power sequences from rtw8821c.c; mac power-on from mac.c.
 */
#include "rtw_pwr.h"
#include "rtw_io.h"

extern int printf(const char *fmt, ...);
extern void taskswitch(void);

static struct rtw_pwr_seq_cmd trans_carddis_to_cardemu_8821c[] = {
    {0x0086, RTW_PWR_CUT_ALL_MSK, RTW_PWR_INTF_SDIO_MSK,
     RTW_PWR_ADDR_SDIO, RTW_PWR_CMD_WRITE, BIT(0), 0},
    {0x0086, RTW_PWR_CUT_ALL_MSK, RTW_PWR_INTF_SDIO_MSK,
     RTW_PWR_ADDR_SDIO, RTW_PWR_CMD_POLLING, BIT(1), BIT(1)},
    {0x004A, RTW_PWR_CUT_ALL_MSK, RTW_PWR_INTF_USB_MSK,
     RTW_PWR_ADDR_MAC, RTW_PWR_CMD_WRITE, BIT(0), 0},
    {0x0005, RTW_PWR_CUT_ALL_MSK, RTW_PWR_INTF_ALL_MSK,
     RTW_PWR_ADDR_MAC, RTW_PWR_CMD_WRITE, BIT(3)|BIT(4)|BIT(7), 0},
    {0x0300, RTW_PWR_CUT_ALL_MSK, RTW_PWR_INTF_PCI_MSK,
     RTW_PWR_ADDR_MAC, RTW_PWR_CMD_WRITE, 0xFF, 0},
    {0x0301, RTW_PWR_CUT_ALL_MSK, RTW_PWR_INTF_PCI_MSK,
     RTW_PWR_ADDR_MAC, RTW_PWR_CMD_WRITE, 0xFF, 0},
    {0xFFFF, RTW_PWR_CUT_ALL_MSK, RTW_PWR_INTF_ALL_MSK,
     0, RTW_PWR_CMD_END, 0, 0},
};

static struct rtw_pwr_seq_cmd trans_cardemu_to_act_8821c[] = {
    {0x0020, RTW_PWR_CUT_ALL_MSK, RTW_PWR_INTF_USB_MSK|RTW_PWR_INTF_SDIO_MSK,
     RTW_PWR_ADDR_MAC, RTW_PWR_CMD_WRITE, BIT(0), BIT(0)},
    {0x0001, RTW_PWR_CUT_ALL_MSK, RTW_PWR_INTF_USB_MSK|RTW_PWR_INTF_SDIO_MSK,
     RTW_PWR_ADDR_MAC, RTW_PWR_CMD_DELAY, 1, RTW_PWR_DELAY_MS},
    {0x0000, RTW_PWR_CUT_ALL_MSK, RTW_PWR_INTF_USB_MSK|RTW_PWR_INTF_SDIO_MSK,
     RTW_PWR_ADDR_MAC, RTW_PWR_CMD_WRITE, BIT(5), 0},
    {0x0005, RTW_PWR_CUT_ALL_MSK, RTW_PWR_INTF_ALL_MSK,
     RTW_PWR_ADDR_MAC, RTW_PWR_CMD_WRITE, (BIT(4)|BIT(3)|BIT(2)), 0},
    {0x0075, RTW_PWR_CUT_ALL_MSK, RTW_PWR_INTF_PCI_MSK,
     RTW_PWR_ADDR_MAC, RTW_PWR_CMD_WRITE, BIT(0), BIT(0)},
    {0x0006, RTW_PWR_CUT_ALL_MSK, RTW_PWR_INTF_ALL_MSK,
     RTW_PWR_ADDR_MAC, RTW_PWR_CMD_POLLING, BIT(1), BIT(1)},
    {0x0075, RTW_PWR_CUT_ALL_MSK, RTW_PWR_INTF_PCI_MSK,
     RTW_PWR_ADDR_MAC, RTW_PWR_CMD_WRITE, BIT(0), 0},
    {0x0006, RTW_PWR_CUT_ALL_MSK, RTW_PWR_INTF_ALL_MSK,
     RTW_PWR_ADDR_MAC, RTW_PWR_CMD_WRITE, BIT(0), BIT(0)},
    {0x0005, RTW_PWR_CUT_ALL_MSK, RTW_PWR_INTF_ALL_MSK,
     RTW_PWR_ADDR_MAC, RTW_PWR_CMD_WRITE, BIT(7), 0},
    {0x0005, RTW_PWR_CUT_ALL_MSK, RTW_PWR_INTF_ALL_MSK,
     RTW_PWR_ADDR_MAC, RTW_PWR_CMD_WRITE, (BIT(4)|BIT(3)), 0},
    {0x10C3, RTW_PWR_CUT_ALL_MSK, RTW_PWR_INTF_USB_MSK,
     RTW_PWR_ADDR_MAC, RTW_PWR_CMD_WRITE, BIT(0), BIT(0)},
    {0x0005, RTW_PWR_CUT_ALL_MSK, RTW_PWR_INTF_ALL_MSK,
     RTW_PWR_ADDR_MAC, RTW_PWR_CMD_WRITE, BIT(0), BIT(0)},
    {0x0005, RTW_PWR_CUT_ALL_MSK, RTW_PWR_INTF_ALL_MSK,
     RTW_PWR_ADDR_MAC, RTW_PWR_CMD_POLLING, BIT(0), 0},
    {0x0020, RTW_PWR_CUT_ALL_MSK, RTW_PWR_INTF_ALL_MSK,
     RTW_PWR_ADDR_MAC, RTW_PWR_CMD_WRITE, BIT(3), BIT(3)},
    {0x0074, RTW_PWR_CUT_ALL_MSK, RTW_PWR_INTF_PCI_MSK,
     RTW_PWR_ADDR_MAC, RTW_PWR_CMD_WRITE, BIT(5), BIT(5)},
    {0x0022, RTW_PWR_CUT_ALL_MSK, RTW_PWR_INTF_PCI_MSK,
     RTW_PWR_ADDR_MAC, RTW_PWR_CMD_WRITE, BIT(1), 0},
    {0x0062, RTW_PWR_CUT_ALL_MSK, RTW_PWR_INTF_PCI_MSK,
     RTW_PWR_ADDR_MAC, RTW_PWR_CMD_WRITE, (BIT(7)|BIT(6)|BIT(5)),
     (BIT(7)|BIT(6)|BIT(5))},
    {0x0061, RTW_PWR_CUT_ALL_MSK, RTW_PWR_INTF_PCI_MSK,
     RTW_PWR_ADDR_MAC, RTW_PWR_CMD_WRITE, (BIT(7)|BIT(6)|BIT(5)), 0},
    {0x007C, RTW_PWR_CUT_ALL_MSK, RTW_PWR_INTF_ALL_MSK,
     RTW_PWR_ADDR_MAC, RTW_PWR_CMD_WRITE, BIT(1), 0},
    {0xFFFF, RTW_PWR_CUT_ALL_MSK, RTW_PWR_INTF_ALL_MSK,
     0, RTW_PWR_CMD_END, 0, 0},
};

static const struct rtw_pwr_seq_cmd *card_enable_flow_8821c[] = {
    trans_carddis_to_cardemu_8821c,
    trans_cardemu_to_act_8821c,
    0
};

static int rtw_pwr_cmd_polling(struct rtw_dev *rtwdev,
                               const struct rtw_pwr_seq_cmd *cmd)
{
    u32 offset = cmd->offset;
    u32 i;
    u8 value;

    for (i = 0; i < 20000; i++) {
        if ((rtw_read8(rtwdev, offset) & cmd->mask) ==
            (cmd->value & cmd->mask))
            return 0;
        rtw_udelay(50);
        if ((i & 0x3F) == 0)
            taskswitch();
    }

    /* PCI: toggle BIT_PFM_WOWL and retry once (Linux mac.c). */
    value = rtw_read8(rtwdev, REG_SYS_PW_CTRL);
    rtw_write8(rtwdev, REG_SYS_PW_CTRL, (u8)(value | BIT_PFM_WOWL));
    rtw_write8(rtwdev, REG_SYS_PW_CTRL, (u8)(value & ~BIT_PFM_WOWL));

    for (i = 0; i < 20000; i++) {
        if ((rtw_read8(rtwdev, offset) & cmd->mask) ==
            (cmd->value & cmd->mask))
            return 0;
        rtw_udelay(50);
        if ((i & 0x3F) == 0)
            taskswitch();
    }

    printf("rtw88: pwr poll fail offset=0x%x mask=0x%x val=0x%x\n",
           offset, cmd->mask, cmd->value);
    return -1;
}

static int rtw_sub_pwr_seq_parser(struct rtw_dev *rtwdev, u8 intf_mask,
                                  u8 cut_mask,
                                  const struct rtw_pwr_seq_cmd *cmd)
{
    const struct rtw_pwr_seq_cmd *cur;
    u32 offset;
    u8 value;

    for (cur = cmd; cur->cmd != RTW_PWR_CMD_END; cur++) {
        if (!(cur->intf_mask & intf_mask) || !(cur->cut_mask & cut_mask))
            continue;
        switch (cur->cmd) {
        case RTW_PWR_CMD_WRITE:
            offset = cur->offset;
            value = rtw_read8(rtwdev, offset);
            value = (u8)((value & ~cur->mask) | (cur->value & cur->mask));
            rtw_write8(rtwdev, offset, value);
            break;
        case RTW_PWR_CMD_POLLING:
            if (rtw_pwr_cmd_polling(rtwdev, cur))
                return -1;
            break;
        case RTW_PWR_CMD_DELAY:
            if (cur->value == RTW_PWR_DELAY_US)
                rtw_udelay(cur->offset);
            else
                rtw_mdelay(cur->offset);
            break;
        case RTW_PWR_CMD_READ:
            break;
        default:
            return -1;
        }
    }
    return 0;
}

static int rtw_pwr_seq_parser(struct rtw_dev *rtwdev,
                              const struct rtw_pwr_seq_cmd **cmd_seq)
{
    u8 cut_mask = (u8)cut_version_to_mask(rtwdev->cut_version);
    u8 intf_mask = RTW_PWR_INTF_PCI_MSK;
    u32 idx = 0;
    const struct rtw_pwr_seq_cmd *cmd;
    int ret;

    for (;;) {
        cmd = cmd_seq[idx];
        if (!cmd)
            break;
        ret = rtw_sub_pwr_seq_parser(rtwdev, intf_mask, cut_mask, cmd);
        if (ret)
            return ret;
        idx++;
    }
    return 0;
}

static int rtw_mac_pre_system_cfg(struct rtw_dev *rtwdev)
{
    u32 value32;
    u8 value8;

    rtw_write8(rtwdev, REG_RSV_CTRL, 0);

    /* PCI path only (Linux rtw_mac_pre_system_cfg). */
    rtw_write32_set(rtwdev, REG_HCI_OPT_CTRL, BIT_USB_SUS_DIS);

    value32 = rtw_read32(rtwdev, REG_PAD_CTRL1);
    value32 |= BIT_PAPE_WLBT_SEL | BIT_LNAON_WLBT_SEL;
    rtw_write32(rtwdev, REG_PAD_CTRL1, value32);

    value32 = rtw_read32(rtwdev, REG_LED_CFG);
    value32 &= ~(BIT_PAPE_SEL_EN | BIT_LNAON_SEL_EN);
    rtw_write32(rtwdev, REG_LED_CFG, value32);

    value32 = rtw_read32(rtwdev, REG_GPIO_MUXCFG);
    value32 |= BIT_WLRFE_4_5_EN;
    rtw_write32(rtwdev, REG_GPIO_MUXCFG, value32);

    value8 = rtw_read8(rtwdev, REG_SYS_FUNC_EN);
    value8 = (u8)(value8 & ~(BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST));
    rtw_write8(rtwdev, REG_SYS_FUNC_EN, value8);

    value8 = rtw_read8(rtwdev, REG_RF_CTRL);
    value8 = (u8)(value8 & ~(BIT_RF_SDM_RSTB | BIT_RF_RSTB | BIT_RF_EN));
    rtw_write8(rtwdev, REG_RF_CTRL, value8);

    value32 = rtw_read32(rtwdev, REG_WLRF1);
    value32 &= ~BIT_WLRF1_BBRF_EN;
    rtw_write32(rtwdev, REG_WLRF1, value32);
    return 0;
}

static int rtw_mac_init_system_cfg(struct rtw_dev *rtwdev)
{
    u8 value8;
    u32 value, tmp;

    value = rtw_read32(rtwdev, REG_CPU_DMEM_CON);
    value |= BIT_WL_PLATFORM_RST | BIT_DDMA_EN;
    rtw_write32(rtwdev, REG_CPU_DMEM_CON, value);

    rtw_write8_set(rtwdev, REG_SYS_FUNC_EN + 1, rtwdev->sys_func_en);
    value8 = (u8)((rtw_read8(rtwdev, REG_CR_EXT + 3) & 0xF0) | 0x0C);
    rtw_write8(rtwdev, REG_CR_EXT + 3, value8);

    tmp = rtw_read32(rtwdev, REG_MCUFW_CTRL);
    if (tmp & BIT_BOOT_FSPI_EN) {
        rtw_write32(rtwdev, REG_MCUFW_CTRL, tmp & ~BIT_BOOT_FSPI_EN);
        value = rtw_read32(rtwdev, REG_GPIO_MUXCFG) & ~BIT_FSPI_EN;
        rtw_write32(rtwdev, REG_GPIO_MUXCFG, value);
    }
    return 0;
}

static int rtw_mac_power_switch_on(struct rtw_dev *rtwdev)
{
    int ret;

    /* REG_CR == 0xea means powered off in Linux rtw88. */
    if (rtw_read8(rtwdev, REG_CR) != 0xea) {
        rtwdev->flags |= RTW_FLAG_POWERON;
        return 0; /* already on */
    }

    ret = rtw_pwr_seq_parser(rtwdev, card_enable_flow_8821c);
    if (!ret)
        rtwdev->flags |= RTW_FLAG_POWERON;
    return ret;
}

int rtw_mac_power_on(struct rtw_dev *rtwdev)
{
    int ret;

    ret = rtw_mac_pre_system_cfg(rtwdev);
    if (ret)
        goto err;

    ret = rtw_mac_power_switch_on(rtwdev);
    if (ret)
        goto err;

    ret = rtw_mac_init_system_cfg(rtwdev);
    if (ret)
        goto err;
    return 0;

err:
    printf("rtw88: mac power on failed\n");
    return -1;
}
