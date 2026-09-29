/*
 * klog.c — kernel log (dmesg-style) first version.
 *
 * Captures kernel printf() output as timestamped ring-buffer records (see
 * klog.h), gated by a live console log-level threshold. The capture happens
 * in console/dexio.c (printf begin/end + per-char hook in putcEX). This file
 * is compiled into the kernel unity build (kernel32.c #includes it).
 */
#include <stdarg.h>

#include "klog.h"
#include "klog_ring.h"

/* Kernel facilities (defined elsewhere in the unity build). */
extern unsigned int ticks;              /* ~100 Hz global tick counter */
extern void  putcEX(char x);            /* console char sink (serial + DDL) */
extern int   vsprintf(char *buffer, const char *fmt, va_list args);

static klog_ring_t   the_klog;
static unsigned char klog_console_max = KLOG_CONSOLE_DEFAULT; /* echo level <= this */
static int           klog_ready = 0;

/* Per-printf capture state. */
static int           klog_capturing = 0;
static unsigned int  klog_cur_tick = 0;
static int           klog_cur_level = KLOG_INFO;
static char          klog_linebuf[KLOG_LINE_MAX];
static unsigned int  klog_linelen = 0;

/* --- small decimal formatter (avoids printf recursion) ------------------ */
static int klog_fmt_u32(char *buf, unsigned int v)
{
    char tmp[12];
    int n = 0;
    int i;
    if (v == 0) { buf[0] = '0'; buf[1] = 0; return 1; }
    while (v > 0) { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    for (i = 0; i < n; i++)
        buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
    return n;
}

static int klog_pad_u32(char *out, int off, unsigned int v, int width)
{
    char num[12];
    int n;
    int i;
    n = klog_fmt_u32(num, v);
    if (n > width)
        width = n;
    for (i = 0; i < width - n; i++)
        out[off++] = '0';
    for (i = 0; i < n; i++)
        out[off++] = num[i];
    return off;
}

/* --- lifecycle ---------------------------------------------------------- */
void klog_init(void)
{
    klog_ring_init(&the_klog);
    klog_console_max = KLOG_CONSOLE_DEFAULT;
    klog_ready = 1;
}

/* --- capture hooks (called from dexio.c) -------------------------------- */
void klog_line_begin(int level)
{
    if (!klog_ready)
        return;
    if (level < 0)
        level = 0;
    if (level > KLOG_DEBUG)
        level = KLOG_DEBUG;
    klog_capturing = 1;
    klog_cur_tick = ticks;
    klog_cur_level = level;
    klog_linelen = 0;
}

void klog_line_char(char c)
{
    if (!klog_capturing)
        return;
    if (klog_linelen < KLOG_LINE_MAX - 1)
        klog_linebuf[klog_linelen++] = c;
}

void klog_line_end(void)
{
    if (!klog_capturing)
        return;
    klog_capturing = 0;
    klog_linebuf[klog_linelen] = 0;
    klog_ring_push(&the_klog, klog_cur_tick, (unsigned char)klog_cur_level,
                   klog_linebuf);
}

int klog_capturing_active(void) { return klog_capturing; }
int klog_current_level(void)    { return klog_cur_level; }

/* --- public log function ----------------------------------------------- */
void klog(int level, const char *fmt, ...)
{
    char buf[KLOG_LINE_MAX];
    va_list args;
    int len;
    int i;
    if (!klog_ready)
        return;
    if (level < 0)
        level = 0;
    if (level > KLOG_DEBUG)
        level = KLOG_DEBUG;
    va_start(args, fmt);
    len = vsprintf(buf, fmt, args);
    va_end(args);
    if (len < 0)
        len = 0;
    if (len > KLOG_LINE_MAX - 1)
        len = KLOG_LINE_MAX - 1;
    buf[len] = 0;
    klog_ring_push(&the_klog, ticks, (unsigned char)level, buf);
    if (klog_console_echo(level, klog_console_max)) {
        for (i = 0; i < len; i++)
            putcEX(buf[i]);
    }
}

/* --- console log-level threshold --------------------------------------- */
unsigned char klog_console_max_get(void) { return klog_console_max; }
void klog_console_max_set(unsigned char lvl)
{
    if (lvl > KLOG_DEBUG)
        lvl = KLOG_DEBUG;
    klog_console_max = lvl;
}

/* --- buffer management / inspection ------------------------------------ */
void klog_clear(void) { klog_ring_clear(&the_klog); }
unsigned int klog_count(void) { return klog_ring_count(&the_klog); }

void klog_dump(int level_filter)
{
    int oldest;
    unsigned int i;
    if (!klog_ready)
        return;
    oldest = klog_ring_oldest(&the_klog);
    if (oldest < 0)
        return;
    for (i = 0; i < the_klog.count; i++) {
        const klog_rec_t *rec;
        char line[24 + KLOG_LINE_MAX];
        int off;
        int t;
        int e;
        unsigned int slot = (unsigned int)(oldest + (int)i) % KLOG_COUNT;
        rec = klog_ring_at(&the_klog, slot);
        if (level_filter >= 0 && (int)rec->level > level_filter)
            continue;
        off = 0;
        line[off++] = '[';
        line[off++] = ' ';
        line[off++] = '+';
        off = klog_pad_u32(line, off, rec->tick / 100, 6);
        line[off++] = '.';
        off = klog_pad_u32(line, off, (rec->tick % 100) * 10, 3);
        line[off++] = ' ';
        line[off++] = ']';
        line[off++] = ' ';
        for (t = 0; t < (int)rec->len; t++)
            line[off++] = rec->text[t];
        line[off++] = '\n';
        line[off] = 0;
        for (e = 0; e < off; e++)
            putcEX(line[e]);
    }
}
