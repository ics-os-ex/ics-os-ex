#ifndef ICSOS_PCI_SCAN_H
#define ICSOS_PCI_SCAN_H

/*
  How many PCI functions to probe at a slot.

  vendor is the vendor id from function 0, offset 0.
  hdr_type_dword is config dword 0x0C (header type in bits 16-23;
  bit 23 means multi-function).

  Empty slots (vendor 0xFFFF) must not probe functions 1-7: Intel PCH
  devices can hang or master-abort-timeout those accesses. Match the
  xHCI/UHCI slot walk.
*/
static inline unsigned int pci_slot_fn_count(unsigned int vendor,
                                             unsigned int hdr_type_dword)
{
    if ((vendor & 0xFFFFu) == 0xFFFFu)
        return 0;
    if (hdr_type_dword & 0x800000u)
        return 8;
    return 1;
}

/*
  Whether a full virtio PCI bus walk is safe. No-COM1 laptops (N150) have
  no virtio-blk; probing bus 0 alone can still hang on PCH config reads.
  QEMU keeps COM1 and may place virtio behind a bridge.
*/
static inline int pci_scan_virtio_allowed(int com1_present)
{
    return com1_present != 0;
}

/*
  class_code is the 16-bit PCI class/subclass (high byte class, low subclass),
  e.g. 0x0280 for network/other (typical Wi-Fi).
*/
static inline int pci_class_is_network(unsigned int class_code)
{
    return ((class_code >> 8) & 0xFFu) == 0x02u;
}

static inline int pci_class_is_wireless_ctrl(unsigned int class_code)
{
    return ((class_code >> 8) & 0xFFu) == 0x0Du;
}

/* True for devices we want in a Wi-Fi bring-up dump. */
static inline int pci_class_is_wifi_interest(unsigned int class_code)
{
    unsigned int base = (class_code >> 8) & 0xFFu;
    unsigned int sub = class_code & 0xFFu;
    if (base == 0x02u && (sub == 0x00u || sub == 0x80u))
        return 1;
    if (base == 0x0Du)
        return 1;
    return 0;
}

static inline const char *pci_net_subclass_name(unsigned int subclass)
{
    switch (subclass & 0xFFu) {
    case 0x00: return "ethernet";
    case 0x01: return "token-ring";
    case 0x02: return "fddi";
    case 0x03: return "atm";
    case 0x04: return "isdn";
    case 0x05: return "worldfip";
    case 0x06: return "picmg";
    case 0x07: return "infiniband";
    case 0x08: return "fabric";
    case 0x80: return "other/wifi?";
    default:   return "network";
    }
}

static inline const char *pci_wireless_subclass_name(unsigned int subclass)
{
    switch (subclass & 0xFFu) {
    case 0x00: return "irda";
    case 0x01: return "consumer-ir";
    case 0x10: return "rf";
    case 0x11: return "bluetooth";
    case 0x12: return "broadband";
    case 0x20: return "802.11a";
    case 0x21: return "802.11b";
    case 0x80: return "other";
    default:   return "wireless";
    }
}

#endif
