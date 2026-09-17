/*
  PCI discovery dumps for hardware bring-up (N150 laptop Wi-Fi, etc.).
  Uses the same empty-slot policy as xHCI (pci_slot_fn_count).
*/
#include "pci_cfg.h"
#include "pci_scan.h"
#include "pci_probe.h"

extern int printf(const char *fmt, ...);

static const char *pci_cap_name(unsigned char id)
{
    switch (id) {
    case 0x01: return "PM";
    case 0x05: return "MSI";
    case 0x10: return "PCIe";
    case 0x11: return "MSI-X";
    case 0x12: return "SATA";
    case 0x13: return "AF";
    default:   return "?";
    }
}

static void pci_dump_bars(unsigned char bus, unsigned char slot,
                          unsigned char func)
{
    unsigned char bir;
    for (bir = 0; bir < 6; bir++) {
        unsigned char off = (unsigned char)(0x10 + bir * 4);
        unsigned int lo = pci_cfg_read32(bus, slot, func, off);
        unsigned int hi = 0;
        unsigned long long base;
        int io;

        if (lo == 0 || lo == 0xFFFFFFFFu)
            continue;
        io = (lo & 1) != 0;
        if (!io && (lo & 6) == 4) {
            if (bir >= 5)
                break;
            hi = pci_cfg_read32(bus, slot, func, (unsigned char)(off + 4));
            base = ((unsigned long long)hi << 32) | (lo & ~0xFu);
            printf("  BAR%u mem64 0x%llx\n", bir, base);
            bir++;
            continue;
        }
        if (io)
            printf("  BAR%u io 0x%x\n", bir, lo & ~3u);
        else
            printf("  BAR%u mem32 0x%x\n", bir, lo & ~0xFu);
    }
}

static void pci_dump_caps(unsigned char bus, unsigned char slot,
                          unsigned char func)
{
    unsigned short status = pci_cfg_read16(bus, slot, func, 0x06);
    unsigned char ptr;
    int guard = 0;

    if (!(status & 0x10)) {
        printf("  caps: (none)\n");
        return;
    }
    ptr = pci_cfg_read8(bus, slot, func, 0x34);
    printf("  caps:");
    while (ptr && ptr != 0xFF && guard++ < 48) {
        unsigned char id = pci_cfg_read8(bus, slot, func, ptr);
        unsigned char next = pci_cfg_read8(bus, slot, func,
                                           (unsigned char)(ptr + 1));
        printf(" %02x(%s)@%02x", id, pci_cap_name(id), ptr);
        ptr = next;
    }
    printf("\n");
}

static void pci_dump_one(unsigned char bus, unsigned char slot,
                         unsigned char func)
{
    unsigned int id, classreg;
    unsigned short vendor, device, class_code, cmd, subven, subdev;
    unsigned char rev, prog, irq, base, sub;
    const char *kind;

    id = pci_cfg_read32(bus, slot, func, 0x00);
    vendor = (unsigned short)(id & 0xFFFFu);
    if (vendor == 0xFFFFu)
        return;
    device = (unsigned short)(id >> 16);
    classreg = pci_cfg_read32(bus, slot, func, 0x08);
    rev = (unsigned char)(classreg & 0xFFu);
    prog = (unsigned char)((classreg >> 8) & 0xFFu);
    class_code = (unsigned short)((classreg >> 16) & 0xFFFFu);
    base = (unsigned char)(class_code >> 8);
    sub = (unsigned char)(class_code & 0xFFu);
    (void)base;

    cmd = pci_cfg_read16(bus, slot, func, 0x04);
    irq = pci_cfg_read8(bus, slot, func, 0x3C);
    subven = pci_cfg_read16(bus, slot, func, 0x2C);
    subdev = pci_cfg_read16(bus, slot, func, 0x2E);

    if (pci_class_is_network(class_code))
        kind = pci_net_subclass_name(sub);
    else if (pci_class_is_wireless_ctrl(class_code))
        kind = pci_wireless_subclass_name(sub);
    else
        kind = "pci";

    printf("PCI %u:%u.%u %04x:%04x class=%04x (%s) rev=%02x prog=%02x "
           "cmd=%04x irq=%u sub=%04x:%04x\n",
           bus, slot, func, vendor, device, class_code, kind, rev, prog,
           cmd, irq, subven, subdev);
    pci_dump_bars(bus, slot, func);
    pci_dump_caps(bus, slot, func);
}

static void pci_walk(int wifi_only)
{
    unsigned char b, s, f;
    unsigned int found = 0;

    for (b = 0; b < 8; b++) {
        for (s = 0; s < 32; s++) {
            unsigned int vendor = pci_cfg_read16(b, s, 0, 0);
            unsigned int hdr = pci_cfg_read32(b, s, 0, 0x0C);
            unsigned int maxf = pci_slot_fn_count(vendor, hdr);
            for (f = 0; f < maxf; f++) {
                unsigned int id = pci_cfg_read32(b, s, f, 0x00);
                unsigned short v = (unsigned short)(id & 0xFFFFu);
                unsigned short class_code;
                unsigned int classreg;
                if (v == 0xFFFFu)
                    continue;
                classreg = pci_cfg_read32(b, s, f, 0x08);
                class_code = (unsigned short)((classreg >> 16) & 0xFFFFu);
                if (wifi_only && !pci_class_is_wifi_interest(class_code))
                    continue;
                pci_dump_one(b, s, f);
                found++;
            }
        }
    }
    if (wifi_only)
        printf("WIFI_HW devices=%u\n", found);
    else
        printf("PCI_HW devices=%u\n", found);
}

void pci_dump_all(void)
{
    printf("PCI_HW_BEGIN\n");
    pci_walk(0);
    printf("PCI_HW_END\n");
}

void pci_dump_network(void)
{
    printf("WIFI_HW_BEGIN\n");
    printf("wifi: scanning PCI class 0x02 (network) and 0x0D (wireless)\n");
    pci_walk(1);
    printf("WIFI_HW_END\n");
}
