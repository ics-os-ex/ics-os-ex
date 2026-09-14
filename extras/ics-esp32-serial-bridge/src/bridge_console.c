/*
  bridge_console.c -- implementation of the ESP32 serial bridge LCD console
  line buffer. Compiles as C89 (host TAP test under -std=gnu89) and is
  linked into the Arduino firmware, so the tested logic is byte-for-byte the
  logic running on the board.
*/
#include <string.h>

#include "bridge_console.h"

void bridge_console_init(bridge_console_t *c)
{
    int i;
    for (i = 0; i < BRIDGE_LOG_LINES; i++) {
        c->lines[i][0] = '\0';
        c->dirty[i] = 1;      /* first paint covers the whole panel */
    }
    c->next_row = 0;
    c->full = 0;
    c->cur[0] = '\0';
    c->cur_pos = 0;
}

const char *bridge_console_line(const bridge_console_t *c, int row)
{
    if (row < 0 || row >= BRIDGE_LOG_LINES)
        return "";
    return c->lines[row];
}

int bridge_console_dirty(const bridge_console_t *c, int row)
{
    if (row < 0 || row >= BRIDGE_LOG_LINES)
        return 0;
    return c->dirty[row];
}

void bridge_console_row_rendered(bridge_console_t *c, int row)
{
    if (row >= 0 && row < BRIDGE_LOG_LINES)
        c->dirty[row] = 0;
}

/* The panel is "full" once the last row is filled (a further newline
   would trigger the first scroll). */
int bridge_console_is_full(const bridge_console_t *c)
{
    return c->next_row >= BRIDGE_LOG_LINES;
}

int bridge_console_dirty_count(const bridge_console_t *c)
{
    int i, n = 0;
    for (i = 0; i < BRIDGE_LOG_LINES; i++)
        n += (c->dirty[i] != 0);
    return n;
}

static void bridge_console_commit(bridge_console_t *c)
{
    int i, n;
    n = 0;
    while (n < BRIDGE_COLS && c->cur[n] != '\0')
        n++;
    if (c->next_row < BRIDGE_LOG_LINES) {
        i = c->next_row;
        memcpy(c->lines[i], c->cur, (size_t)n);
        c->lines[i][n] = '\0';
        c->dirty[i] = 1;
        c->next_row++;
    } else {
        /* Panel full: shift up one row, new line at the bottom. */
        memmove(c->lines, c->lines + 1,
                (size_t)(BRIDGE_LOG_LINES - 1) * sizeof c->lines[0]);
        i = BRIDGE_LOG_LINES - 1;
        memcpy(c->lines[i], c->cur, (size_t)n);
        c->lines[i][n] = '\0';
        for (i = 0; i < BRIDGE_LOG_LINES; i++)
            c->dirty[i] = 1;
        c->full = 1;
    }
    c->cur[0] = '\0';
    c->cur_pos = 0;
}

void bridge_console_putc(bridge_console_t *c, char ch)
{
    switch (ch) {
    case '\n':
        bridge_console_commit(c);
        break;
    case '\r':
        break;
    case '\b':
        if (c->cur_pos > 0)
            c->cur[--c->cur_pos] = '\0';
        break;
    case '\t':
        bridge_console_putc(c, ' ');
        bridge_console_putc(c, ' ');
        bridge_console_putc(c, ' ');
        bridge_console_putc(c, ' ');
        break;
    default:
        if ((unsigned char)ch >= 0x20 && c->cur_pos < BRIDGE_COLS) {
            c->cur[c->cur_pos++] = ch;
            c->cur[c->cur_pos] = '\0';
        }
        break;
    }
}
