/*
  Name: serial.h
  Description: Basic COM1 serial output for diagnostics
*/
#ifndef SERIAL_H
#define SERIAL_H

#define SERIAL_COM1 0x3F8

void serial_init();
void serial_putchar(char c);
void serial_write(const char *s);
void serial_printf(const char *fmt, ...);

#endif
