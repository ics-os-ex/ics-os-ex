/*
  Host TAP for tmux status-line formatting and console scrollback mapping.
  Regression: the 80-column painter used to read past the status NUL into
  stack garbage after names like "3:console(0)".
*/
#include <stdio.h>
#include <string.h>

#include "kernel/console/console_mux.h"

static int check(const char *name, int condition)
{
    if (!condition) {
        printf("not ok - %s\n", name);
        return 0;
    }
    printf("ok - %s\n", name);
    return 1;
}

int main(void)
{
    char line[FG_STATUS_COLS + 1];
    const char *names[4];
    int ids[4];
    unsigned int n, i;
    unsigned int live_row;
    int slot;
    int zero_tail;
    console_hist_t hist;
    console_hist_t empty;
    unsigned char row[CONSOLE_HIST_COLS * CONSOLE_HIST_CELL];
    char tiny[8];
    int ok = 1;

    printf("TAP version 13\n1..16\n");

    ok &= check("mux letter Ctrl-C is c", fg_mux_letter(2) == 'c');
    ok &= check("mux letter Caps C is c", fg_mux_letter('C') == 'c');
    ok &= check("mux letter lowercase c stays", fg_mux_letter('c') == 'c');
    ok &= check("mux letter digit unchanged", fg_mux_letter('3') == '3');

    ids[0] = 0;
    names[0] = "console(0)";
    ids[1] = 1;
    names[1] = "console(1)";
    ids[2] = 2;
    names[2] = "console(2)";
    ids[3] = 3;
    names[3] = "console(3)";
    n = fg_format_status(line, sizeof line, 3, 4, ids, names,
                         "C-b  c:new n:next p:prev l:last w:list x:kill ?:help");
    ok &= check("status is NUL-terminated within 80",
                n < sizeof line && line[n] == 0);
    ok &= check("status contains 3:console(3)",
                strstr(line, "3:console(3)") != 0);
    zero_tail = 1;
    for (i = n + 1; i < sizeof line; i++)
        if (line[i] != 0)
            zero_tail = 0;
    ok &= check("bytes after NUL are zero (no stack garbage)", zero_tail);
    ok &= check("long hint does not overflow the buffer",
                strlen(line) < sizeof line);

    names[0] = "this-name-is-way-too-long-for-the-bar";
    fg_format_status(line, sizeof line, 0, 1, ids, names, 0);
    ok &= check("process names truncate to 12 characters",
                strstr(line, "this-name-is-way-too-long") == 0 &&
                strstr(line, "this-name-is") != 0);

    console_hist_init(&hist);
    memset(row, 0, sizeof row);
    for (i = 0; i < CONSOLE_HIST_LINES + 5; i++) {
        row[0] = (unsigned char)('A' + (i % 26));
        console_hist_push(&hist, row);
    }
    ok &= check("hist count saturates at cap",
                hist.count == CONSOLE_HIST_LINES);
    live_row = 99;
    ok &= check("view off=0 row 0 is live 0",
                console_hist_view_slot(&hist, 0, 0, CONSOLE_VIEW_ROWS,
                                       &live_row) == -1 &&
                live_row == 0);
    slot = console_hist_view_slot(&hist, hist.count, 0, CONSOLE_VIEW_ROWS,
                                  &live_row);
    ok &= check("fully scrolled row 0 is a hist slot",
                slot >= 0 && slot < (int)CONSOLE_HIST_LINES);
    ok &= check("add_off clamps to count",
                console_hist_add_off(0, 10000, 10) == 10 &&
                console_hist_add_off(3, -10, 10) == 0);
    console_hist_init(&empty);
    live_row = 99;
    ok &= check("empty hist view is live rows",
                console_hist_view_slot(&empty, 0, 5, 24, &live_row) == -1 &&
                live_row == 5);
    n = fg_format_status(tiny, sizeof tiny, 0, 4, ids, names, "HINT");
    ok &= check("tiny cap still NUL-terminates",
                n < sizeof tiny && tiny[sizeof tiny - 1] == 0);
    ok &= check("formatted status starts with [",
                line[0] == '[');

    return ok ? 0 : 1;
}
