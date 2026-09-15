/*
  Name: usb.h
  Description: USB UHCI/xHCI host and Mass Storage Class (Bulk-Only) support.
               Used to boot ICS-OS from a USB thumb drive and mount that
               drive as the root filesystem.
*/

#ifndef ICSOS_USB_H
#define ICSOS_USB_H

int usb_init(void);
int usb_storage_available(void);
int usb_start_hotplug_monitor(void);
int usb_xhci_mounted_disconnect_selftest(void);

void usb_cdc_putc(int c);
int usb_cdc_present(void);
int usb_cdc_pump(void);
int usb_cdc_write_raw(const void *p, int n);
/* Drop posted CDC IN for a USB-root ELF stream load; re-arm when done. */
void usb_cdc_bulk_io_begin(void);
void usb_cdc_bulk_io_end(void);

#endif
