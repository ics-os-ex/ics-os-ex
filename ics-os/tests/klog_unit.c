/*
  Name: klog_unit.c
  Description: Host unit tests for the kernel-log ring buffer
  (kernel/console/klog_ring.h). The ring is pure C with no kernel dependencies,
  so this runs on the host: init/empty, append, per-record metadata (tick/
  level), wrap-around eviction with the capacity cap, text truncation, clear,
  and the console-echo policy (default threshold keeps KLOG_DEBUG off the
  live console).
*/
#include <stdio.h>
#include <string.h>

#include "kernel/console/klog_ring.h"

static int g_ok = 1;
static int g_n = 0;

static void check(const char *name, int cond)
{
    g_n++;
    if (!cond) {
        printf("not ok %d - %s\n", g_n, name);
        g_ok = 0;
    } else {
        printf("ok %d - %s\n", g_n, name);
    }
}

int main(void)
{
    klog_ring_t r;
    unsigned int i;

    printf("TAP version 13\n1..24\n");

    /* --- init / empty -------------------------------------------------- */
    klog_ring_init(&r);
    check("init: empty ring reports count 0", klog_ring_count(&r) == 0u);
    check("init: oldest of empty ring is -1", klog_ring_oldest(&r) == -1);

    /* --- append (not yet full) ---------------------------------------- */
    klog_ring_push(&r, 100u, 3u, "boot");
    check("append: count is 1 after one push", klog_ring_count(&r) == 1u);
    check("append: oldest slot is 0", klog_ring_oldest(&r) == 0);
    check("append: text stored at slot 0",
          strcmp(klog_ring_at(&r, 0u)->text, "boot") == 0);
    check("append: tick and level are recorded",
          klog_ring_at(&r, 0u)->tick == 100u &&
          klog_ring_at(&r, 0u)->level == 3u);
    klog_ring_push(&r, 101u, 5u, "second");
    check("append: count is 2 after two pushes", klog_ring_count(&r) == 2u);
    check("append: oldest is still slot 0", klog_ring_oldest(&r) == 0);
    check("append: second record is at slot 1",
          strcmp(klog_ring_at(&r, 1u)->text, "second") == 0);

    /* --- wrap-around eviction (fresh ring) ---------------------------- */
    klog_ring_init(&r);
    for (i = 0; i < KLOG_COUNT + 5u; i++) {
        char name[24];
        sprintf(name, "r%u", i);
        klog_ring_push(&r, 1000u + i, 1u, name);
    }
    check("wrap: count caps at KLOG_COUNT", klog_ring_count(&r) == KLOG_COUNT);
    check("wrap: oldest advanced to slot 5", klog_ring_oldest(&r) == 5);
    check("wrap: slot 5 holds the first surviving record (r5)",
          strcmp(klog_ring_at(&r, 5u)->text, "r5") == 0);
    {
        char newest[24];
        sprintf(newest, "r%u", KLOG_COUNT + 4u);
        check("wrap: slot just before head holds the newest",
              strcmp(klog_ring_at(&r, 4u)->text, newest) == 0);
    }
    check("wrap: a middle slot is intact (r6)",
          strcmp(klog_ring_at(&r, 6u)->text, "r6") == 0);
    check("wrap: tick follows its record",
          klog_ring_at(&r, 5u)->tick == 1005u);

    /* --- truncation (fresh ring) -------------------------------------- */
    klog_ring_init(&r);
    {
        static char longtxt[KLOG_LINE_MAX + 6];
        int n;
        for (n = 0; n < KLOG_LINE_MAX + 5; n++)
            longtxt[n] = 'a';
        longtxt[KLOG_LINE_MAX + 5] = 0;
        klog_ring_push(&r, 200u, 2u, longtxt);
    }
    check("truncate: stored length is capped at LINE_MAX-1",
          klog_ring_at(&r, 0u)->len == (unsigned short)(KLOG_LINE_MAX - 1));
    check("truncate: text is NUL-terminated at the cap",
          klog_ring_at(&r, 0u)->text[KLOG_LINE_MAX - 1] == 0);

    /* --- clear --------------------------------------------------------- */
    klog_ring_clear(&r);
    check("clear: count returns to 0", klog_ring_count(&r) == 0u);
    check("clear: oldest is -1 again", klog_ring_oldest(&r) == -1);

    /* --- console echo policy ------------------------------------------ */
    /* Regression: the default threshold must keep KLOG_DEBUG out of the
     * live console (it corrupted user TUIs, e.g. "usb: cache synchronized"
     * mid-NetHack) while bare printf (KLOG_INFO) still echoes. */
    check("policy: default console threshold is KLOG_INFO",
          KLOG_CONSOLE_DEFAULT == KLOG_INFO);
    check("policy: KLOG_DEBUG is NOT echoed at the default threshold",
          !klog_console_echo(KLOG_DEBUG, KLOG_CONSOLE_DEFAULT));
    check("policy: bare printf (KLOG_INFO) IS echoed at the default threshold",
          klog_console_echo(KLOG_INFO, KLOG_CONSOLE_DEFAULT));
    check("policy: NOTICE/WARN/ERR are echoed at the default threshold",
          klog_console_echo(KLOG_NOTICE, KLOG_CONSOLE_DEFAULT) &&
          klog_console_echo(KLOG_WARN, KLOG_CONSOLE_DEFAULT) &&
          klog_console_echo(KLOG_ERR, KLOG_CONSOLE_DEFAULT));
    check("policy: threshold KLOG_DEBUG echoes everything (dmesg -n 7)",
          klog_console_echo(KLOG_DEBUG, KLOG_DEBUG) &&
          klog_console_echo(KLOG_EMERG, KLOG_DEBUG));

    (void)g_n;
    return g_ok ? 0 : 1;
}
