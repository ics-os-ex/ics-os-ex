/*
  Host TAP for xHCI laptop vs QEMU IRQ policy, port CCS bit numbers,
  and control TD flags (Setup Chain is RsvdZ on Intel).
*/
#include <stdio.h>

#include "kernel/hardware/usb/xhci_policy.h"

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
    unsigned int setup_in = xhci_ctrl_setup_flags(1, 1);
    unsigned int setup_out = xhci_ctrl_setup_flags(1, 0);
    unsigned int setup_none = xhci_ctrl_setup_flags(0, 0);
    unsigned int data_in = xhci_ctrl_data_flags(1);
    unsigned int status_in = xhci_ctrl_status_flags(1, 1);
    unsigned int status_none = xhci_ctrl_status_flags(0, 0);
    int ok = 1;

    printf("TAP version 13\n1..23\n");

    ok &= check("QEMU COM1 + xAPIC allows MSI-X",
                xhci_allow_msix(1, 0) == 1);
    ok &= check("N150 no COM1 refuses MSI-X",
                xhci_allow_msix(0, 0) == 0);
    ok &= check("x2APIC refuses MSI-X even with COM1",
                xhci_allow_msix(1, 1) == 0);
    ok &= check("N150 no COM1 + x2APIC refuses MSI-X",
                xhci_allow_msix(0, 1) == 0);
    ok &= check("port 1 CCS bit is 0x1",
                xhci_ccs_bit(1) == 1u);
    ok &= check("port 4 CCS bit is 0x8",
                xhci_ccs_bit(4) == 8u);
    ok &= check("port 0 and 33 are not representable",
                xhci_ccs_bit(0) == 0 && xhci_ccs_bit(33) == 0);
    ok &= check("port settle spins are bounded and nonzero",
                XHCI_PORT_SETTLE_SPINS > 0 &&
                XHCI_PORT_SETTLE_SPINS <= 4000000u);
    ok &= check("Setup IN has IDT and type 2",
                (setup_in & XHCI_CTRL_TRB_IDT) != 0 &&
                ((setup_in >> 10) & 0x3F) == 2);
    ok &= check("Setup IN Transfer Type is 3",
                ((setup_in >> 16) & 3) == 3);
    ok &= check("Setup TRB never sets Chain",
                (setup_in & XHCI_CTRL_TRB_CHAIN) == 0 &&
                (setup_out & XHCI_CTRL_TRB_CHAIN) == 0 &&
                (setup_none & XHCI_CTRL_TRB_CHAIN) == 0);
    ok &= check("Setup no-data Transfer Type is 0",
                ((setup_none >> 16) & 3) == 0);
    ok &= check("Setup OUT Transfer Type is 2",
                ((setup_out >> 16) & 3) == 2);
    ok &= check("Data IN has DIR_IN and ISP, no Chain",
                (data_in & XHCI_CTRL_TRB_DIR_IN) != 0 &&
                (data_in & XHCI_CTRL_TRB_ISP) != 0 &&
                (data_in & XHCI_CTRL_TRB_CHAIN) == 0);
    ok &= check("Status after IN data is IOC without DIR_IN",
                (status_in & XHCI_CTRL_TRB_IOC) != 0 &&
                (status_in & XHCI_CTRL_TRB_DIR_IN) == 0 &&
                (status_none & XHCI_CTRL_TRB_DIR_IN) != 0);
    ok &= check("CDC IN spins cover a gadget write window",
                XHCI_CDC_IN_SPINS >= 40000u &&
                XHCI_CDC_IN_SPINS < XHCI_PORT_SETTLE_SPINS);
    ok &= check("CDC hotplug delay is 1 tick while bound",
                xhci_cdc_hotplug_delay(1) == 1 &&
                xhci_cdc_hotplug_delay(0) == 100);
    ok &= check("CDC IN TRB sets ISP and IOC",
                (xhci_cdc_in_trb_flags() & XHCI_CTRL_TRB_ISP) != 0 &&
                (xhci_cdc_in_trb_flags() & XHCI_CTRL_TRB_IOC) != 0);
    ok &= check("empty CDC IN poll leaves the TRB posted",
                xhci_cdc_in_stop_on_timeout() == 0);
    ok &= check("MSC waits do not Stop a posted CDC IN",
                xhci_cdc_in_cancel_for_msc() == 0);
    ok &= check("CDC extra-port mask excludes the mounted MSC port",
                xhci_cdc_extra_ccs(0x98u, 4) == 0x90u);
    ok &= check("disconnected bound CDC requires teardown",
                xhci_cdc_should_drop(1, 0) == 1);
    ok &= check("unbound CDC rebinds when a non-MSC port has CCS",
                xhci_cdc_should_rebind(0, 0x10u) == 1 &&
                xhci_cdc_should_rebind(0, 0) == 0);

    return ok ? 0 : 1;
}
