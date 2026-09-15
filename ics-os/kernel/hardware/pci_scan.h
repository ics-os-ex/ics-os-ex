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

#endif
