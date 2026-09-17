#ifndef ICSOS_PCI_CFG_H
#define ICSOS_PCI_CFG_H

/*
  Shared CF8/CFC config-space accessors for discovery dumps and drivers.
  Prefer pci_slot_fn_count() from pci_scan.h when walking slots.
*/

#define PCI_CFG_ADDR 0xCF8
#define PCI_CFG_DATA 0xCFC

static inline unsigned int pci_cfg_inl(unsigned short port)
{
    unsigned int v;
    __asm__ volatile("inl %%dx, %%eax" : "=a"(v) : "d"(port));
    return v;
}

static inline void pci_cfg_outl(unsigned short port, unsigned int val)
{
    __asm__ volatile("outl %%eax, %%dx" : : "d"(port), "a"(val));
}

static inline unsigned int pci_cfg_addr(unsigned char bus, unsigned char dev,
                                       unsigned char fn, unsigned char off)
{
    return 0x80000000u | ((unsigned int)bus << 16) |
           ((unsigned int)dev << 11) | ((unsigned int)fn << 8) |
           (off & 0xFCu);
}

static inline unsigned int pci_cfg_read32(unsigned char bus, unsigned char dev,
                                          unsigned char fn, unsigned char off)
{
    pci_cfg_outl(PCI_CFG_ADDR, pci_cfg_addr(bus, dev, fn, off));
    return pci_cfg_inl(PCI_CFG_DATA);
}

static inline unsigned short pci_cfg_read16(unsigned char bus, unsigned char dev,
                                            unsigned char fn, unsigned char off)
{
    unsigned int v = pci_cfg_read32(bus, dev, fn, (unsigned char)(off & 0xFC));
    return (unsigned short)(v >> ((off & 2) * 8));
}

static inline unsigned char pci_cfg_read8(unsigned char bus, unsigned char dev,
                                          unsigned char fn, unsigned char off)
{
    unsigned int v = pci_cfg_read32(bus, dev, fn, (unsigned char)(off & 0xFC));
    return (unsigned char)(v >> ((off & 3) * 8));
}

#endif
