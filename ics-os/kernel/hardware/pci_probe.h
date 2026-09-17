#ifndef ICSOS_PCI_PROBE_H
#define ICSOS_PCI_PROBE_H

/*
  On-demand PCI dumps for bring-up (N150 Wi-Fi capture, etc.).
  Safe slot walk: empty vendors skipped via pci_slot_fn_count().
*/

/* Print every present function (class/vendor/BARs/caps). */
void pci_dump_all(void);

/* Print PCI network (class 0x02) and wireless (class 0x0D) controllers. */
void pci_dump_network(void);

#endif
