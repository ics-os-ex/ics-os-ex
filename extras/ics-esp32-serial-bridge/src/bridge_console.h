/*
  bridge_console.h -- line buffering for the ESP32 serial bridge LCD console.

  Portable C (no Arduino/ESP dependencies) so the very logic that drives the
  172x320 panel is exercised by a host-native TAP test (see
  ics-os/tests/bridge_console_unit.c and `make test-bridge-console-unit`).

  Model: LOG_LINES full-width rows in screen order (row 0 = just under the
  status bar). Bytes are accumulated into a partial line until a newline,
  then committed to the next row; when the panel is full the whole buffer
  scrolls up one row and the new line lands at the bottom.
*/
#ifndef BRIDGE_CONSOLE_H
#define BRIDGE_CONSOLE_H

#define BRIDGE_COLS 28
#define BRIDGE_ROWS 40
#define BRIDGE_STATUS_ROWS 1
#define BRIDGE_LOG_LINES (BRIDGE_ROWS - BRIDGE_STATUS_ROWS)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char lines[BRIDGE_LOG_LINES][BRIDGE_COLS + 1];
    unsigned dirty[BRIDGE_LOG_LINES];
    int next_row;   /* next row to fill (0-based); == LOG_LINES when full */
    int full;       /* set once the panel has scrolled at least once */
    char cur[BRIDGE_COLS + 1];
    int cur_pos;
} bridge_console_t;

void bridge_console_init(bridge_console_t *c);
void bridge_console_putc(bridge_console_t *c, char ch);
const char *bridge_console_line(const bridge_console_t *c, int row);
int bridge_console_dirty(const bridge_console_t *c, int row);
/* Mark row as rendered (call after painting it). */
void bridge_console_row_rendered(bridge_console_t *c, int row);
int bridge_console_is_full(const bridge_console_t *c);
/* Number of rows currently pending a paint. */
int bridge_console_dirty_count(const bridge_console_t *c);

#ifdef __cplusplus
}
#endif

#endif /* BRIDGE_CONSOLE_H */
