/*
  Minimal 16550 UART driver for COM1 and COM2.

  Used so ICS-OS can be tested on modern PCs and in QEMU without a VGA
  window (qemu -display none -serial stdio).

  COM1 (0x3F8) is the headless oracle console (serial_* API).
  COM2 (0x2F8) is an optional interactive terminal / shell (serial2_* API)
  used for live command execution and introspection without a reboot.
*/

#include "../../cpu/spinlock.h"

extern void uart_com1_putc(unsigned int c);
extern void uart_com2_putc(unsigned int c);

#define SERIAL_COM1 0x3F8
#define SERIAL_COM2 0x2F8

typedef struct uart_dev {
    unsigned int base;
    volatile int ready;
    spinlock_t lock;
    volatile int owner;
} uart_dev;

static uart_dev uart1;
static uart_dev uart2;

/* When set, console putcEX() also mirrors output to COM2 so the shell2
   terminal shows command results. Off by default to avoid serial overhead
   in headless runs; the shell2 thread turns it on. */
static volatile int com2_mirror = 0;

void serial2_mirror_set(int on) { com2_mirror = on; }
int  serial2_mirror_get(void)   { return com2_mirror; }

typedef struct serial_guard {
    spin_irq_flags_t flags;
    int locked;
} serial_guard;

extern unsigned int lapic_get_id(void);
extern volatile unsigned int *lapic_mmio;
extern int smp_cpu_id(void);

static serial_guard uart_guard_acquire(uart_dev *u)
{
    serial_guard guard;
    int cpu;
    unsigned spins = 0;

    __asm__ __volatile__("pushfq; popq %0; cli"
                         : "=r"(guard.flags) : : "memory");
    /* Never walk LAPIC MMIO here: ps_switchto may call serial_puts while
       CR3 is still a user PML4, and 0xFEE00000 is not mapped there.
       TSC_AUX / smp_cpu_id() is CR3-safe. */
    cpu = smp_cpu_id();
    if (cpu < 0)
        cpu = 0;
    guard.locked = 0;
    if (u->lock.locked && u->owner == cpu)
        return guard;
    while (!__sync_bool_compare_and_swap(&u->lock.locked, 0, 1)) {
        if (++spins > 100000)
            return guard;
        __asm__ __volatile__("pause");
    }
    u->owner = cpu;
    guard.locked = 1;
    return guard;
}

static void uart_guard_release(uart_dev *u, serial_guard guard)
{
    if (guard.locked) {
        u->owner = -1;
        spin_unlock(&u->lock);
    }
    if (guard.flags & (1ULL << 9))
        __asm__ __volatile__("sti" : : : "memory");
}

static void uart_putc_raw(uart_dev *u, char c)
{
    if (u == &uart2)
        uart_com2_putc((unsigned char)c);
    else
        uart_com1_putc((unsigned char)c);
}

static int uart_getc_raw(uart_dev *u)
{
    if (!u->ready)
        return -1;
    if ((inportb(u->base + 5) & 1) == 0)
        return -1;
    return (int)inportb(u->base);
}

/* 16550 scratch-register presence test. N150-class laptops have no Super I/O
   UART; floating 0x3F8 reads 0xFF and the scratch byte does not stick. */
static int uart_port_present(unsigned int base)
{
    if (inportb(base + 5) == 0xFF)
        return 0;
    outportb(base + 7, 0x5A);
    if (inportb(base + 7) != 0x5A)
        return 0;
    outportb(base + 7, 0xA5);
    if (inportb(base + 7) != 0xA5)
        return 0;
    return 1;
}

static void uart_hw_init(uart_dev *u, unsigned int base)
{
    u->base = base;
    u->ready = 0;
    u->owner = -1;
    spin_init(&u->lock);
    if (!uart_port_present(base))
        return;
    outportb(base + 1, 0x00);    /* disable UART interrupts */
    outportb(base + 3, 0x80);    /* enable DLAB */
    outportb(base + 0, 0x01);    /* 115200 baud */
    outportb(base + 1, 0x00);
    outportb(base + 3, 0x03);    /* 8N1 */
    outportb(base + 2, 0xC7);    /* enable FIFO */
    outportb(base + 4, 0x0B);    /* IRQs enabled, RTS/DSR set */
    u->ready = 1;
}

void serial_init(void)
{
    uart_hw_init(&uart1, SERIAL_COM1);
};

int serial_com1_present(void)
{
    return uart1.ready;
}

void serial_putc(char c)
{
    serial_guard guard;

    if (!uart1.ready)
        return;

    guard = uart_guard_acquire(&uart1);
    if (c == '\n')
        uart_putc_raw(&uart1, '\r');
    uart_putc_raw(&uart1, c);
    uart_guard_release(&uart1, guard);
};

void serial_puts(const char *s)
{
    serial_guard guard;

    if (s == 0)
        return;
    if (!uart1.ready)
        return;
    guard = uart_guard_acquire(&uart1);
    while (*s) {
        if (*s == '\n')
            uart_putc_raw(&uart1, '\r');
        uart_putc_raw(&uart1, *s++);
    }
    uart_guard_release(&uart1, guard);
};

void serial_write(const char *s, int n)
{
    serial_guard guard;
    int i;

    if (s == 0 || n <= 0)
        return;
    if (!uart1.ready)
        return;
    guard = uart_guard_acquire(&uart1);
    for (i = 0; i < n; i++) {
        if (s[i] == '\n')
            uart_putc_raw(&uart1, '\r');
        uart_putc_raw(&uart1, s[i]);
    }
    uart_guard_release(&uart1, guard);
};

int serial_getc(void)
{
    return uart_getc_raw(&uart1);
};

void serial2_init(void)
{
    uart_hw_init(&uart2, SERIAL_COM2);
};

void serial2_putc(char c)
{
    serial_guard guard;

    if (!uart2.ready)
        return;

    guard = uart_guard_acquire(&uart2);
    if (c == '\n')
        uart_putc_raw(&uart2, '\r');
    uart_putc_raw(&uart2, c);
    uart_guard_release(&uart2, guard);
};

void serial2_puts(const char *s)
{
    serial_guard guard;

    if (s == 0)
        return;
    if (!uart2.ready)
        return;
    guard = uart_guard_acquire(&uart2);
    while (*s) {
        if (*s == '\n')
            uart_putc_raw(&uart2, '\r');
        uart_putc_raw(&uart2, *s++);
    }
    uart_guard_release(&uart2, guard);
};

int serial2_getc(void)
{
    return uart_getc_raw(&uart2);
};
