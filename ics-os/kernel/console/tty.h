#ifndef ICSOS_TTY_H
#define ICSOS_TTY_H

#define TTY_IBUF      256
#define TTY_CANON_MAX 256
#define TTY_MAX       8

#define TTY_ECHO     0x01
#define TTY_ICANON   0x02
#define TTY_ISIG     0x04
#define TTY_SERIAL    0x08

#define SIGINT       2

#define VT_COLS      80
#define VT_ROWS      25

struct _dex32_direct_device_hdl;
struct _PCB386;

/* Per-tty VT100/xterm parser state (see console/tty_vt.c). */
typedef struct _vt_state {
   int state;             /* 0 ground, 1 esc, 2 csi, 3 osc */
   int csi_n;
   char csi[24];
   int sgr;               /* current VGA cell attribute */
   int savx, savy;
   int savsgr;
   int stb_top, stb_bot;  /* DECSTBM region, 0-based inclusive */
   int curhidden;
   int alt;               /* alternate screen active */
   unsigned char *altbuf; /* 80x25x2 shadow of the primary screen */
} vt_state_t;

typedef struct _tty {
   int id;
   int flags;
   struct _dex32_direct_device_hdl *ddl;
   int session;
   int pgrp;
   volatile int in_head, in_tail;
   unsigned char in_buf[TTY_IBUF];
   int canon_len;
   int canon_off;
   char canon[TTY_CANON_MAX];
   volatile int line_ready;
   int canon_esc;         /* canonical-mode CSI swallow: 1 saw ESC, 2 in CSI */
   vt_state_t vt;
} tty_t;

void tty_init(void);
tty_t *tty_alloc(struct _dex32_direct_device_hdl *ddl, int flags);
tty_t *tty_get(int id);
tty_t *tty_fg(void);
void tty_set_fg(tty_t *t);
void tty_attach_proc(PCB386 *p, tty_t *t);
void tty_input(tty_t *t, int c);
void tty_input_fg(int c);
int tty_read(tty_t *t, char *buf, int n);
int tty_write(tty_t *t, const char *buf, int n);
void tty_signal_int(tty_t *t);
tty_t *tty_serial(void);
int tty_input_ready(tty_t *t);
int tty_flush_input(tty_t *t);
int tty_inject(tty_t *t, int c);
int sys_kcmd(char *cmd);

/* VT100/xterm screen interpreter, fed by tty output (console/tty_vt.c). */
void vt_init(vt_state_t *v);
void vt_feed(tty_t *t, int c);
void vt_screen_clear(vt_state_t *v, struct _dex32_direct_device_hdl *ddl, int attr);
void vt_cursor_set_visible(struct _dex32_direct_device_hdl *ddl, int visible);

#endif
