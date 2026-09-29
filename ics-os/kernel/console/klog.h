/*
 * klog.h — kernel log (dmesg-style) public interface.
 *
 * A timestamped ring buffer that captures kernel printf() output, plus a
 * `dmesg` console command and a live console log-level threshold.
 * User-space console output is NOT captured (only kernel printf), which is
 * the first step toward segregating kernel and user logs.
 *
 * Severity and the console-echo policy live in klog_ring.h (host-testable):
 * a message is always buffered, and is echoed to the live console only if
 * klog_console_echo(level, klog_console_max) is true. Default threshold =
 * KLOG_INFO: bare printf() (captured at KLOG_INFO) still echoes, while
 * KLOG_DEBUG traces stay in the ring only — they must never corrupt a user
 * TUI. Steady-state runtime traces must use klog(KLOG_DEBUG, ...), real
 * failures klog(KLOG_ERR, ...).
 */
#ifndef KLOG_H
#define KLOG_H

#include "klog_ring.h"

/* Initialize the ring (call early in boot, before the console is busy). */
void klog_init(void);

/* Log a formatted message at the given severity (buffered + level-gated echo).
 * Does not recurse through printf(). */
void klog(int level, const char *fmt, ...);

/* Hooks used by console/dexio.c to capture a kernel printf() call as one
 * timestamped record. Begin, then per-character, then end. */
void klog_line_begin(int level);
void klog_line_char(char c);
void klog_line_end(void);

/* True while a kernel printf() is being captured (used by putcEX). */
int  klog_capturing_active(void);
/* Severity of the printf() currently being captured. */
int  klog_current_level(void);

/* Console log-level threshold. */
unsigned char klog_console_max_get(void);
void          klog_console_max_set(unsigned char lvl);

/* Buffer management / inspection. */
void klog_clear(void);
/* Dump the ring oldest-first. level_filter < 0 means "all"; otherwise only
 * records with level <= level_filter are printed. Output goes to the console
 * (not through printf, so it is not re-captured). */
void klog_dump(int level_filter);
unsigned int klog_count(void);

#endif /* KLOG_H */
