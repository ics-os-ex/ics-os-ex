/*
  Host TAP for PCI slot function-count policy and Wi-Fi class helpers.
  Regression: virtio-blk probed functions 1-7 of every slot, including
  empty ones, and hung on Intel N150 config-space timeouts.
*/
#include <stdio.h>
#include <string.h>

#include "kernel/hardware/pci_scan.h"

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
    int ok = 1;

    printf("TAP version 13\n1..15\n");
    ok &= check("empty slot probes no functions",
                pci_slot_fn_count(0xFFFF, 0) == 0);
    ok &= check("empty slot ignores a set MF bit",
                pci_slot_fn_count(0xFFFF, 0x800000) == 0);
    ok &= check("single-function Intel device probes fn 0 only",
                pci_slot_fn_count(0x8086, 0x000000) == 1);
    ok &= check("multi-function device probes 8 functions",
                pci_slot_fn_count(0x8086, 0x800000) == 8);
    ok &= check("vendor 0 with no MF bit is still one function",
                pci_slot_fn_count(0, 0) == 1);
    ok &= check("no COM1 refuses virtio pci scan",
                pci_scan_virtio_allowed(0) == 0);
    ok &= check("COM1 allows virtio pci scan",
                pci_scan_virtio_allowed(1) == 1);
    ok &= check("ethernet class is network",
                pci_class_is_network(0x0200) == 1);
    ok &= check("wifi-other class is network",
                pci_class_is_network(0x0280) == 1);
    ok &= check("xhci class is not network",
                pci_class_is_network(0x0C03) == 0);
    ok &= check("0x0D is wireless controller",
                pci_class_is_wireless_ctrl(0x0D10) == 1);
    ok &= check("wifi interest includes 0x0280",
                pci_class_is_wifi_interest(0x0280) == 1);
    ok &= check("wifi interest includes ethernet",
                pci_class_is_wifi_interest(0x0200) == 1);
    ok &= check("wifi interest excludes VGA",
                pci_class_is_wifi_interest(0x0300) == 0);
    ok &= check("net subclass other/wifi name",
                strcmp(pci_net_subclass_name(0x80), "other/wifi?") == 0);
    return ok ? 0 : 1;
}
