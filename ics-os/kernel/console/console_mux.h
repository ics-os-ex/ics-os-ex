/*
  Tmux-style console multiplexer helpers: status-line formatting and
  per-screen scrollback. Host-testable (no kernel types).
*/
#ifndef CONSOLE_MUX_H
#define CONSOLE_MUX_H

#define FG_STATUS_COLS      80
#define FG_STATUS_NAME_MAX  12

/*
  Map a tmux prefix command to a lowercase letter. ICS-OS keyboard
  encoding is Ctrl-A=0 .. Ctrl-Z=25; Caps Lock sends 'A'..'Z'. The N150
  boot LED breadcrumb leaves Caps lit, so C-b C must still create a
  window (same as C-b c and C-b Ctrl-C).
*/
static inline int fg_mux_letter(int c)
{
    if (c >= 0 && c < 26)
        return 'a' + c;
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 'a';
    return c;
}
#define CONSOLE_HIST_COLS   80
#define CONSOLE_HIST_CELL   2
#define CONSOLE_HIST_LINES  128
#define CONSOLE_VIEW_ROWS   24

typedef struct {
    unsigned char lines[CONSOLE_HIST_LINES][CONSOLE_HIST_COLS * CONSOLE_HIST_CELL];
    unsigned int head;
    unsigned int count;
} console_hist_t;

static void console_hist_init(console_hist_t *h)
{
    if (!h)
        return;
    h->head = 0;
    h->count = 0;
}

static void console_hist_push(console_hist_t *h, const unsigned char *row)
{
    unsigned int i;
    unsigned char *dst;
    if (!h || !row)
        return;
    dst = h->lines[h->head];
    for (i = 0; i < CONSOLE_HIST_COLS * CONSOLE_HIST_CELL; i++)
        dst[i] = row[i];
    h->head = (h->head + 1) % CONSOLE_HIST_LINES;
    if (h->count < CONSOLE_HIST_LINES)
        h->count++;
}

static unsigned int console_hist_clamp_off(unsigned int off, unsigned int count)
{
    if (off > count)
        return count;
    return off;
}

static unsigned int console_hist_add_off(unsigned int off, int delta,
                                         unsigned int count)
{
    int v = (int)off + delta;
    if (v < 0)
        v = 0;
    if (v > (int)count)
        v = (int)count;
    return (unsigned int)v;
}

/*
  Map view row y (0..live_rows-1) with scrollback offset view_off
  (0 = live tail) onto the combined document (oldest hist ... live rows).
  Returns hist ring slot, or -1 if the row is live (*live_row set).
*/
static int console_hist_view_slot(const console_hist_t *h,
                                  unsigned int view_off, unsigned int y,
                                  unsigned int live_rows,
                                  unsigned int *live_row)
{
    unsigned int combined;
    unsigned int oldest;
    unsigned int count;
    if (!h || y >= live_rows)
        return -1;
    count = h->count;
    view_off = console_hist_clamp_off(view_off, count);
    combined = (count - view_off) + y;
    if (combined < count) {
        oldest = (h->head + CONSOLE_HIST_LINES - count) % CONSOLE_HIST_LINES;
        return (int)((oldest + combined) % CONSOLE_HIST_LINES);
    }
    if (live_row)
        *live_row = combined - count;
    return -1;
}

static unsigned int fg_status_append(char *out, unsigned int n,
                                     unsigned int cap, const char *s)
{
    if (!out || cap == 0)
        return 0;
    if (!s) {
        if (n < cap)
            out[n] = 0;
        return n;
    }
    while (n + 1 < cap && *s)
        out[n++] = *s++;
    if (n < cap)
        out[n] = 0;
    return n;
}

static unsigned int fg_status_append_n(char *out, unsigned int n,
                                       unsigned int cap, const char *s,
                                       unsigned int maxn)
{
    unsigned int k = 0;
    if (!out || cap == 0)
        return 0;
    if (!s) {
        if (n < cap)
            out[n] = 0;
        return n;
    }
    while (n + 1 < cap && *s && k < maxn) {
        out[n++] = *s++;
        k++;
    }
    if (n < cap)
        out[n] = 0;
    return n;
}

static unsigned int fg_status_uint(char *out, unsigned int n,
                                   unsigned int cap, unsigned int v)
{
    char b[11];
    int k = 0;
    unsigned int t = v;
    do {
        b[k++] = (char)('0' + (t % 10));
        t /= 10;
    } while (t && k < 10);
    while (k > 0 && n + 1 < cap)
        out[n++] = b[--k];
    if (n < cap)
        out[n] = 0;
    return n;
}

/*
  Build "[cur] *id:name  id:name [hint]". Always NUL-terminates and zeros
  the rest of out so a 80-column painter cannot leak stack garbage.
*/
static unsigned int fg_format_status(char *out, unsigned int cap,
                                     int current, int nwin,
                                     const int *ids, const char *const *names,
                                     const char *hint)
{
    unsigned int n = 0;
    unsigned int i;
    if (!out || cap == 0)
        return 0;
    for (i = 0; i < cap; i++)
        out[i] = 0;
    n = fg_status_append(out, n, cap, "[");
    n = fg_status_uint(out, n, cap, current < 0 ? 0 : (unsigned int)current);
    n = fg_status_append(out, n, cap, "]");
    for (i = 0; i < (unsigned int)nwin && ids && names; i++) {
        if (n + 6 >= cap)
            break;
        n = fg_status_append(out, n, cap,
                             ids[i] == current ? " *" : "  ");
        n = fg_status_uint(out, n, cap, (unsigned int)ids[i]);
        n = fg_status_append(out, n, cap, ":");
        n = fg_status_append_n(out, n, cap,
                               names[i] ? names[i] : "?",
                               FG_STATUS_NAME_MAX);
    }
    if (hint && hint[0] && n + 2 < cap) {
        n = fg_status_append(out, n, cap, " ");
        n = fg_status_append(out, n, cap, hint);
    }
    return n;
}

#endif
