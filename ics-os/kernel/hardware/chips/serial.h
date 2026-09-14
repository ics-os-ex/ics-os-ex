#ifndef ICSOS_SERIAL_H
#define ICSOS_SERIAL_H

/* COM1 (0x3F8): headless oracle console. */
void serial_init(void);
int serial_com1_present(void);
void serial_putc(char c);
void serial_puts(const char *s);
void serial_write(const char *s, int n);
int serial_getc(void);

/* COM2 (0x2F8): optional interactive terminal / shell (live introspection). */
void serial2_init(void);
void serial2_putc(char c);
void serial2_puts(const char *s);
int serial2_getc(void);

/* Output routing: when enabled, putcEX() also mirrors kernel/user console
   output to COM2 so the shell2 terminal sees command results. */
void serial2_mirror_set(int on);
int  serial2_mirror_get(void);

/* Register-only COM1/COM2 TX (no uart_dev* on the C stack). */
void uart_com1_putc(unsigned int c);
void uart_com2_putc(unsigned int c);

#endif
