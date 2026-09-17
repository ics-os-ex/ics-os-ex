#ifndef ICSOS_RTL8139_H
#define ICSOS_RTL8139_H

/* Realtek RTL8139C (PCI 10EC:8139) C-mode driver. */
void rtl8139_init(void);
int  rtl8139_present(void);
void rtl8139_poll(void);

#endif
