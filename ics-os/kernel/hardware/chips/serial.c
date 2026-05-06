/*
  Name: serial.c
  Description: Basic COM1 serial output for diagnostics
*/

#include "serial.h"
#include "../../dextypes.h"
#include "../../console/dexio.h"

extern unsigned char inportb(unsigned int port);
extern void outportb(unsigned int port,unsigned char value);

static int serial_ready = 0;

static int serial_is_transmit_empty()
{
	return (inportb(SERIAL_COM1 + 5) & 0x20);
}

void serial_init()
{
	/* Disable interrupts */
	outportb(SERIAL_COM1 + 1, 0x00);
	/* Enable DLAB */
	outportb(SERIAL_COM1 + 3, 0x80);
	/* Set divisor to 1 (115200 baud) */
	outportb(SERIAL_COM1 + 0, 0x01);
	outportb(SERIAL_COM1 + 1, 0x00);
	/* 8 bits, no parity, one stop bit */
	outportb(SERIAL_COM1 + 3, 0x03);
	/* Enable FIFO, clear them, 14-byte threshold */
	outportb(SERIAL_COM1 + 2, 0xC7);
	/* IRQs enabled, RTS/DSR set */
	outportb(SERIAL_COM1 + 4, 0x0B);

	serial_ready = 1;
}

void serial_putchar(char c)
{
	if (!serial_ready)
		return;
	while (!serial_is_transmit_empty()) {}
	outportb(SERIAL_COM1, (unsigned char)c);
}

int serial_putchar_nb(char c)
{
	if (!serial_ready)
		return 0;
	if (!serial_is_transmit_empty())
		return 0;
	outportb(SERIAL_COM1, (unsigned char)c);
	return 1;
}

void serial_write(const char *s)
{
	if (!serial_ready || !s)
		return;
	while (*s) {
		if (*s == '\n')
			serial_putchar('\r');
		serial_putchar(*s++);
	}
}

void serial_printf(const char *fmt, ...)
{
	char buf[512];
	va_list args;

	if (!serial_ready)
		return;
	va_start(args, fmt);
	vsprintf(buf, fmt, args);
	va_end(args);
	serial_write(buf);
}
