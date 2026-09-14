/*
  Host TAP for the ESP32 serial-bridge LCD console line buffer.
  Source under test: extras/ics-esp32-serial-bridge/src/bridge_console.c
  (compiled and linked here, so the tested logic is the logic on the board).
*/
#include <stdio.h>
#include <string.h>
#include "bridge_console.h"

static int check(const char *name, int condition)
{
    if (!condition) {
        printf("not ok - %s\n", name);
        return 0;
    }
    printf("ok - %s\n", name);
    return 1;
}

static void feed(bridge_console_t *c, const char *s)
{
    while (*s)
        bridge_console_putc(c, *s++);
}

int main(void)
{
    int ok = 1;
    bridge_console_t c;
    int i;

    printf("TAP version 13\n1..19\n");

    bridge_console_init(&c);
    ok &= check("fresh panel has all rows dirty (initial full paint)",
                bridge_console_dirty_count(&c) == BRIDGE_LOG_LINES);
    ok &= check("fresh panel not full", !bridge_console_is_full(&c));

    feed(&c, "hello\n");
    ok &= check("first line lands in row 0",
                strcmp(bridge_console_line(&c, 0), "hello") == 0);
    ok &= check("row 0 marked dirty after commit",
                bridge_console_dirty(&c, 0));
    bridge_console_row_rendered(&c, 0);
    ok &= check("rendered row no longer dirty", !bridge_console_dirty(&c, 0));
    ok &= check("dirty count drops after render",
                bridge_console_dirty_count(&c) == BRIDGE_LOG_LINES - 1);

    feed(&c, "world\n");
    ok &= check("second line lands in row 1",
                strcmp(bridge_console_line(&c, 1), "world") == 0);

    /* Backspace erases the pending partial line only. */
    feed(&c, "ab\bX");
    ok &= check("backspace removes pending char",
                strcmp(c.cur, "aX") == 0);

    /* Tab expands to four spaces. */
    feed(&c, "\t");
    ok &= check("tab expands to four spaces",
                strcmp(c.cur, "aX    ") == 0);
    feed(&c, "\n");

    /* CR is dropped. */
    feed(&c, "a\r\n");
    ok &= check("CR is ignored",
                strcmp(bridge_console_line(&c, 3), "a") == 0);

    /* Control chars below 0x20 are dropped. */
    feed(&c, "\x01\x1fZ\n");
    ok &= check("low control chars dropped",
                strcmp(bridge_console_line(&c, 4), "Z") == 0);

    /* A line longer than BRIDGE_COLS is truncated, not overflowed. */
    feed(&c, "012345678901234567890123456789EXTRA\n");
    ok &= check("overlong line truncated to BRIDGE_COLS",
                strlen(bridge_console_line(&c, 5)) == BRIDGE_COLS);
    ok &= check("truncated line keeps the prefix",
                strncmp(bridge_console_line(&c, 5), "01234567890123456789012345", 24) == 0);

    /* A bare newline commits an empty line. */
    bridge_console_init(&c);
    feed(&c, "\n");
    ok &= check("blank newline commits an empty row",
                strcmp(bridge_console_line(&c, 0), "") == 0);

    /* Fill the panel and force a scroll. */
    bridge_console_init(&c);
    for (i = 0; i < BRIDGE_LOG_LINES; i++) {
        char line[BRIDGE_COLS + 1];
        snprintf(line, sizeof line, "row%02d", i);
        feed(&c, line);
        feed(&c, "\n");
    }
    ok &= check("panel reports full once last row is filled",
                bridge_console_is_full(&c));
    ok &= check("bottom row holds the last line",
                strcmp(bridge_console_line(&c, BRIDGE_LOG_LINES - 1),
                       "row38") == 0);

    /* One more line scrolls everything up. */
    feed(&c, "row39\n");
    ok &= check("after scroll, row 0 holds the second line",
                strcmp(bridge_console_line(&c, 0), "row01") == 0);
    ok &= check("after scroll, bottom holds the newest line",
                strcmp(bridge_console_line(&c, BRIDGE_LOG_LINES - 1),
                       "row39") == 0);
    ok &= check("scroll dirties every row",
                bridge_console_dirty_count(&c) == BRIDGE_LOG_LINES);

    return ok ? 0 : 1;
}
