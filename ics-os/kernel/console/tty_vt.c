/*
 * VT100/xterm subset interpreter for DDL-backed ttys.
 *
 * Feeds user-space console output through a small escape-sequence parser so
 * full-screen applications (vim, less, top) can position the cursor, edit
 * lines, change colors, and use the alternate screen buffer on the 80x25
 * VGA text console. Serial ttys pass bytes through raw (the remote side is
 * itself a terminal).
 *
 * Supported: C0 controls, ESC [ CSI (CUP CHA VPA CUU CUD CUF CUB CNL CPL
 * ED EL ICH DCH ECH IL DL SU SD SGR DECSTBM DSR-6, DECSC/DECRST, private
 * modes ?25 ?47 ?1047 ?1048 ?1049), ESC ] OSC (ignored), ESC 7/8,
 * ESC M (RI), ESC c (RIS).
 */

#include "../dextypes.h"
#include "tty.h"
#include "dex_DDL.h"
#include "../hardware/vga/fbconsole.h"
#include "vt_color.h"

extern void Dex32UpdateCursor(DEX32_DDL_INFO *dev, int y, int x);
extern void dd_swaptohardware(DEX32_DDL_INFO *dev);
extern void serial_putc(char c);
extern void outportb(unsigned int port, unsigned char value);
extern void *memset(void *s, int c, unsigned int n);
extern void *memcpy(void *d, const void *s, unsigned int n);
extern void *memmove(void *d, const void *s, unsigned int n);
extern void *malloc(unsigned int n);
extern void free(void *p);

enum { VT_S_GROUND = 0, VT_S_ESC, VT_S_CSI, VT_S_OSC, VT_S_OSC_ESC };

#define VT_SCREEN_SIZE (VT_COLS * VT_ROWS * 2)

static int vt_hw_live(const tty_t *t)
{
    return t && t->ddl && t->ddl == ActiveDDL &&
           t->ddl->active && !t->ddl->bufmode;
}

static void vt_refresh(tty_t *t)
{
    if (t && t->ddl && t->ddl->bufmode)
       dd_swaptohardware(t->ddl);
    if (vt_hw_live(t) && fbconsole_active())
       fbconsole_screen_refresh();
}

static void vt_clear_buffer(unsigned char *s, int attr)
{
    int i;
    memset(s, ' ', VT_SCREEN_SIZE);
    for (i = 1; i < VT_SCREEN_SIZE; i += 2)
       s[i] = (unsigned char)attr;
}

static unsigned char *vt_screen(tty_t *t)
{
   return (unsigned char *)t->ddl->buf_ptr;
}

static void vt_putcell(tty_t *t, int x, int y, char c, int attr)
{
    unsigned char *s = vt_screen(t);
    if (x < 0 || x >= VT_COLS || y < 0 || y >= VT_ROWS)
       return;
    s[(y * VT_COLS + x) * 2] = (unsigned char)c;
    s[(y * VT_COLS + x) * 2 + 1] = (unsigned char)attr;
    if (vt_hw_live(t) && fbconsole_active())
       fbconsole_cell_render(x, y, (unsigned char)c, (unsigned char)attr);
}

static void vt_clear_rect(tty_t *t, int x1, int y1, int x2, int y2, int attr)
{
   int x, y;
   if (x1 < 0) x1 = 0;
   if (y1 < 0) y1 = 0;
   if (x2 >= VT_COLS) x2 = VT_COLS - 1;
   if (y2 >= VT_ROWS) y2 = VT_ROWS - 1;
   if (x1 > x2 || y1 > y2)
      return;
   for (y = y1; y <= y2; y++)
      for (x = x1; x <= x2; x++)
         vt_putcell(t, x, y, ' ', attr);
}

/* Scroll region [top..bot] up by one row; new bottom row is blanked. */
static void vt_scroll_up(tty_t *t, int top, int bot, int attr)
{
   unsigned char *s = vt_screen(t);
   int rows = bot - top + 1;
   if (top < 0) top = 0;
   if (bot >= VT_ROWS) bot = VT_ROWS - 1;
   if (top > bot)
      return;
   memmove(s + top * VT_COLS * 2,
            s + (top + 1) * VT_COLS * 2,
            (rows - 1) * VT_COLS * 2);
    vt_clear_rect(t, 0, bot, VT_COLS - 1, bot, attr);
    vt_refresh(t);
}

static void vt_scroll_down(tty_t *t, int top, int bot, int attr)
{
   unsigned char *s = vt_screen(t);
   int rows = bot - top + 1;
   if (top < 0) top = 0;
   if (bot >= VT_ROWS) bot = VT_ROWS - 1;
   if (top > bot)
      return;
  memmove(s + (top + 1) * VT_COLS * 2,
            s + top * VT_COLS * 2,
            (rows - 1) * VT_COLS * 2);
    vt_clear_rect(t, 0, top, VT_COLS - 1, top, attr);
    vt_refresh(t);
}

static void vt_cursor_clamp(tty_t *t)
{
   if (t->ddl->curx < 0) t->ddl->curx = 0;
   if (t->ddl->curx >= VT_COLS) t->ddl->curx = VT_COLS - 1;
   if (t->ddl->cury < 0) t->ddl->cury = 0;
   if (t->ddl->cury >= VT_ROWS) t->ddl->cury = VT_ROWS - 1;
}

static void vt_cursor_sync(tty_t *t)
{
   vt_cursor_clamp(t);
   Dex32UpdateCursor((DEX32_DDL_INFO *)t->ddl, t->ddl->cury, t->ddl->curx);
}

/* Move down one line honoring the scroll region (VT "index" behavior).
   Newly exposed rows are filled with the default rendition, not the current
   SGR, so a colored SGR active at scroll time does not paint a band. */
static void vt_index(tty_t *t)
{
    vt_state_t *v = &t->vt;
    if (t->ddl->cury == v->stb_bot)
       vt_scroll_up(t, v->stb_top, v->stb_bot, VT_DEFAULT_ATTR);
    else if (t->ddl->cury < VT_ROWS - 1)
       t->ddl->cury++;
    vt_cursor_sync(t);
}

static void vt_reverse_index(tty_t *t)
{
    vt_state_t *v = &t->vt;
    if (t->ddl->cury == v->stb_top)
       vt_scroll_down(t, v->stb_top, v->stb_bot, VT_DEFAULT_ATTR);
    else if (t->ddl->cury > 0)
       t->ddl->cury--;
    vt_cursor_sync(t);
}

static void vt_putchar(tty_t *t, char c, int attr)
{
   int x = t->ddl->curx;
    if (x >= VT_COLS) {
       x = 0;
       vt_index(t);
    }
   vt_putcell(t, x, t->ddl->cury, c, attr);
   t->ddl->curx = x + 1;
   if (t->ddl->curx >= VT_COLS)
      t->ddl->curx = 0;
   vt_cursor_sync(t);
}

/* Apply an SGR parameter string to the current cell attribute. The parse +
   attribute math (including 256-color / truecolor) live in vt_color.c so the
   host-native TAP exercises the exact kernel logic. */
static void vt_apply_sgr(tty_t *t, char *params)
{
    int vals[32], n;
    n = vt_parse_params(params, vals, 32);
    if (n <= 0)
       return;
    t->vt.sgr = vt_sgr_apply(t->vt.sgr, vals, n);
}

/* Format a non-negative integer into dst (no leading zeros); return len. */
static int vt_fmt_num(char *dst, int val)
{
   char tmp[12];
   int n = 0, i = 0;
   if (val == 0) { dst[0] = '0'; return 1; }
   while (val > 0) { tmp[n++] = (char)('0' + (val % 10)); val /= 10; }
   while (n > 0) dst[i++] = tmp[--n];
   return i;
}

/* Respond to DSR-6 by injecting CSI row;col R into the tty input queue. */
 static void vt_dsr_response(tty_t *t)
 {
     char buf[24];
     int row, col, i = 0, j;
     if (t->flags & TTY_SERIAL) {
        row = t->vt.sery + 1;
        col = t->vt.serx + 1;
     } else {
        row = t->ddl->cury + 1;
        col = t->ddl->curx + 1;
     }
     buf[i++] = 0x1B; buf[i++] = '[';
     i += vt_fmt_num(buf + i, row);
     buf[i++] = ';';
     i += vt_fmt_num(buf + i, col);
     buf[i++] = 'R';
    for (j = 0; j < i; j++)
         tty_inject(t, buf[j]);
   }

static void vt_ris(tty_t *t)
{
    vt_state_t *v = &t->vt;
    v->state = VT_S_GROUND;
    v->sgr = 0x07;
    v->stb_top = 0;
    v->stb_bot = VT_ROWS - 1;
    v->savx = v->savy = 0;
    v->savsgr = 0x07;
    if (v->alt) {
       v->alt = 0;
       memcpy(vt_screen(t), t->ddl->mem_ptr, VT_SCREEN_SIZE);
    }
    vt_screen_clear(v, t->ddl, 0x07);
    t->ddl->curx = 0;
    t->ddl->cury = 0;
    t->ddl->attb = 0x07;
    vt_cursor_sync(t);
    vt_refresh(t);
}

static void vt_alt_enter(tty_t *t, int savecursor, int clear)
{
    vt_state_t *v = &t->vt;
    if (v->alt)
       return;
    if (t->ddl->bufmode)
       dd_swaptohardware(t->ddl);
    if (savecursor) {
       v->savx = t->ddl->curx;
       v->savy = t->ddl->cury;
       v->savsgr = v->sgr;
    }
    /* Save the primary screen in the DDL memory buffer and make the
       alternate screen the live DDL shadow so framebuffer refreshes show it. */
    memcpy(t->ddl->mem_ptr, vt_screen(t), VT_SCREEN_SIZE);
     if (clear)
        vt_clear_buffer(vt_screen(t), VT_DEFAULT_ATTR);
     v->alt = 1;
    vt_refresh(t);
}

void vt_alt_exit(tty_t *t, int restorecursor)
{
    vt_state_t *v = &t->vt;
    if (!v->alt)
       return;
    v->alt = 0;
    memcpy(vt_screen(t), t->ddl->mem_ptr, VT_SCREEN_SIZE);
    if (restorecursor) {
       t->ddl->curx = v->savx;
       t->ddl->cury = v->savy;
       v->sgr = v->savsgr;
    }
    vt_cursor_sync(t);
    vt_refresh(t);
}

static void vt_csi_dispatch(tty_t *t, char final)
{
    vt_state_t *v = &t->vt;
    char params[48];
    int n = v->csi_n;
    int vals[32];
    int cnt, p0, p1, private;

   private = (n > 0 && v->csi[0] == '?') ? 1 : 0;
   if (private) {
      n--;
      if (n > 0)
         memcpy(params, v->csi + 1, n);
   } else if (n > 0) {
      memcpy(params, v->csi, n);
   }
   params[n < (int)sizeof(params) ? n : (int)sizeof(params) - 1] = 0;

   cnt = vt_parse_params(params, vals, 16);
   if (cnt < 0)
      cnt = 0;
   p0 = cnt > 0 ? vals[0] : 0;
   p1 = cnt > 1 ? vals[1] : 0;

   switch (final) {
   case 'A':
      if (p0 <= 0) p0 = 1;
      t->ddl->cury -= p0;
      vt_cursor_sync(t);
      break;
   case 'B': case 'e':
      if (p0 <= 0) p0 = 1;
      t->ddl->cury += p0;
      vt_cursor_sync(t);
      break;
   case 'C':
      if (p0 <= 0) p0 = 1;
      t->ddl->curx += p0;
      vt_cursor_sync(t);
      break;
   case 'D':
      if (p0 <= 0) p0 = 1;
      t->ddl->curx -= p0;
      vt_cursor_sync(t);
      break;
   case 'E':
      if (p0 <= 0) p0 = 1;
      t->ddl->cury += p0;
      t->ddl->curx = 0;
      vt_cursor_sync(t);
      break;
   case 'F':
      if (p0 <= 0) p0 = 1;
      t->ddl->cury -= p0;
      t->ddl->curx = 0;
      vt_cursor_sync(t);
      break;
   case 'G': case '`':
      t->ddl->curx = (p0 <= 0 ? 1 : p0) - 1;
      vt_cursor_sync(t);
      break;
   case 'd':
      t->ddl->cury = (p0 <= 0 ? 1 : p0) - 1;
      vt_cursor_sync(t);
      break;
   case 'H': case 'f':
      t->ddl->cury = (p0 <= 0 ? 1 : p0) - 1;
      t->ddl->curx = (p1 <= 0 ? 1 : p1) - 1;
      vt_cursor_sync(t);
      break;
   case 'J':
       switch (p0) {
       case 0:
          vt_clear_rect(t, t->ddl->curx, t->ddl->cury, VT_COLS - 1, t->ddl->cury, v->sgr);
          vt_clear_rect(t, 0, t->ddl->cury + 1, VT_COLS - 1, VT_ROWS - 1, v->sgr);
          break;
       case 1:
          vt_clear_rect(t, 0, 0, VT_COLS - 1, t->ddl->cury - 1, v->sgr);
          vt_clear_rect(t, 0, t->ddl->cury, t->ddl->curx, t->ddl->cury, v->sgr);
          break;
       case 2:
          vt_screen_clear(v, t->ddl, v->sgr);
          vt_refresh(t);
          break;
       default:
          break;
       }
        break;
    case 'K':
       switch (p0) {
       case 0:
          vt_clear_rect(t, t->ddl->curx, t->ddl->cury, VT_COLS - 1, t->ddl->cury, v->sgr);
          break;
       case 1:
          vt_clear_rect(t, 0, t->ddl->cury, t->ddl->curx, t->ddl->cury, v->sgr);
          break;
       case 2:
          vt_clear_rect(t, 0, t->ddl->cury, VT_COLS - 1, t->ddl->cury, v->sgr);
          break;
       default:
          break;
       }
       break;
   case '@': {
      int x = t->ddl->curx, y = t->ddl->cury;
      unsigned char *s = vt_screen(t);
      int count = p0 <= 0 ? 1 : p0;
      unsigned char *dst = s + (y * VT_COLS + x) * 2;
      if (count > VT_COLS - x)
         count = VT_COLS - x;
     memmove(dst + count * 2, dst, (VT_COLS - x - count) * 2);
       vt_clear_rect(t, x, y, x + count - 1, y, v->sgr);
       vt_refresh(t);
       break;
    }
   case 'P': {
      int x = t->ddl->curx, y = t->ddl->cury;
      unsigned char *s = vt_screen(t);
      int count = p0 <= 0 ? 1 : p0;
      unsigned char *dst = s + (y * VT_COLS + x) * 2;
      if (count > VT_COLS - x)
         count = VT_COLS - x;
    memmove(dst, dst + count * 2, (VT_COLS - x - count) * 2);
       vt_clear_rect(t, VT_COLS - count, y, VT_COLS - 1, y, v->sgr);
       vt_refresh(t);
       break;
    }
   case 'X': {
      int count = p0 <= 0 ? 1 : p0;
      vt_clear_rect(t, t->ddl->curx, t->ddl->cury,
                    t->ddl->curx + count - 1, t->ddl->cury, v->sgr);
      break;
   }
   case 'L': {
      int y = t->ddl->cury, count = p0 <= 0 ? 1 : p0;
      unsigned char *s = vt_screen(t);
      if (y < v->stb_top || y > v->stb_bot)
         break;
      if (count > v->stb_bot - y + 1)
         count = v->stb_bot - y + 1;
memmove(s + (y + count) * VT_COLS * 2,
                s + y * VT_COLS * 2,
                (v->stb_bot - y + 1 - count) * VT_COLS * 2);
        vt_clear_rect(t, 0, y, VT_COLS - 1, y + count - 1, VT_DEFAULT_ATTR);
        vt_refresh(t);
        break;
     }
   case 'M': {
      int y = t->ddl->cury, count = p0 <= 0 ? 1 : p0;
      unsigned char *s = vt_screen(t);
      if (y < v->stb_top || y > v->stb_bot)
         break;
      if (count > v->stb_bot - y + 1)
         count = v->stb_bot - y + 1;
 memmove(s + y * VT_COLS * 2,
                s + (y + count) * VT_COLS * 2,
                (v->stb_bot - y + 1 - count) * VT_COLS * 2);
        vt_clear_rect(t, 0, v->stb_bot - count + 1, VT_COLS - 1, v->stb_bot, VT_DEFAULT_ATTR);
        vt_refresh(t);
        break;
     }
   case 'S': {
       int span = v->stb_bot - v->stb_top + 1;
       if (p0 <= 0) p0 = 1;
       if (p0 > span) p0 = span;
       while (p0-- > 0)
           vt_scroll_up(t, v->stb_top, v->stb_bot, VT_DEFAULT_ATTR);
        break;
     }
    case 'T': {
        int span = v->stb_bot - v->stb_top + 1;
        if (p0 <= 0) p0 = 1;
        if (p0 > span) p0 = span;
        while (p0-- > 0)
           vt_scroll_down(t, v->stb_top, v->stb_bot, VT_DEFAULT_ATTR);
        break;
     }
   case 'm':
      vt_apply_sgr(t, params);
      break;
   case 'n':
      if (p0 == 6)
         vt_dsr_response(t);
      break;
   case 'r':
      if (p0 <= 0) p0 = 1;
      if (p1 <= 0) p1 = VT_ROWS;
      if (p0 < 1 || p1 > VT_ROWS || p0 >= p1)
         break;
      v->stb_top = p0 - 1;
      v->stb_bot = p1 - 1;
      t->ddl->curx = 0;
      t->ddl->cury = 0;
      vt_cursor_sync(t);
      break;
   case 's':
      v->savx = t->ddl->curx;
      v->savy = t->ddl->cury;
      break;
   case 'u':
      t->ddl->curx = v->savx;
      t->ddl->cury = v->savy;
      vt_cursor_sync(t);
      break;
   case 'h': case 'l':
      if (!private)
         break;
      switch (p0) {
      case 25:
         vt_cursor_set_visible((DEX32_DDL_INFO *)t->ddl, final == 'h');
         v->curhidden = (final == 'l');
         break;
      case 47:
         if (final == 'h')
            vt_alt_enter(t, 0, 1);
         else
            vt_alt_exit(t, 0);
         break;
      case 1047:
         if (final == 'h')
            vt_alt_enter(t, 0, 0);
         else
            vt_alt_exit(t, 0);
         break;
      case 1048:
         if (final == 'h') {
            v->savx = t->ddl->curx;
            v->savy = t->ddl->cury;
         } else {
            t->ddl->curx = v->savx;
            t->ddl->cury = v->savy;
            vt_cursor_sync(t);
         }
         break;
      case 1049:
         if (final == 'h')
            vt_alt_enter(t, 1, 1);
         else
            vt_alt_exit(t, 1);
         break;
      default:
         break;
      }
      break;
   default:
      break;
   }
}

void vt_init(vt_state_t *v)
{
   memset(v, 0, sizeof(*v));
   v->state = VT_S_GROUND;
   v->sgr = 0x07;
   v->stb_top = 0;
   v->stb_bot = VT_ROWS - 1;
   v->altbuf = 0;
}

void vt_screen_clear(vt_state_t *v, struct _dex32_direct_device_hdl *ddl, int attr)
{
   int i;
   (void)v;
   memset(ddl->buf_ptr, ' ', VT_SCREEN_SIZE);
   for (i = 1; i < VT_SCREEN_SIZE; i += 2)
      ((unsigned char *)ddl->buf_ptr)[i] = (unsigned char)attr;
}

void vt_cursor_set_visible(struct _dex32_direct_device_hdl *ddl, int visible)
{
    if (!ddl)
       return;
    /* VGA CRTC cursor scan lines are only valid on a real 0xB8000 text
       console. A GOP/VBE framebuffer has no VGA CRTC; those ports can
       hang or reset the PCH, so use the software block cursor instead. */
    if ((unsigned long)ddl->hdw_ptr != 0xB8000UL) {
       fbconsole_cursor_visible(visible);
       return;
    }
    if (!visible) {
       outportb(0x3D4, 0x0A);
       outportb(0x3D5, 0x18);
       outportb(0x3D4, 0x0C);
       outportb(0x3D5, 0x18);
    } else {
       outportb(0x3D4, 0x0A);
       outportb(0x3D5, 0x0C);
       outportb(0x3D4, 0x0C);
       outportb(0x3D5, 0x0E);
    }
}

static void vt_serial_clamp(vt_state_t *v)
{
    if (v->serx < 0) v->serx = 0;
    if (v->serx >= VT_COLS) v->serx = VT_COLS - 1;
    if (v->sery < 0) v->sery = 0;
    if (v->sery >= VT_ROWS) v->sery = VT_ROWS - 1;
}

static int vt_csi_serial_params(vt_state_t *v, char *params, int *vals)
{
    int n = v->csi_n;
    int private, cnt;

    private = (n > 0 && v->csi[0] == '?') ? 1 : 0;
    if (private) {
       n--;
       if (n > 0)
          memcpy(params, v->csi + 1, n);
    } else if (n > 0) {
       memcpy(params, v->csi, n);
    }
    params[n < (int)sizeof(params) ? n : (int)sizeof(params) - 1] = 0;

    cnt = vt_parse_params(params, vals, 16);
    if (cnt < 0)
       cnt = 0;
    return cnt;
}

/* Serial ttys pass bytes through to COM1, but they still track a lightweight
   cursor model so DSR-6 and basic cursor-position queries work in headless
   serial tests. */
static void vt_serial_feed(tty_t *t, int c)
{
    vt_state_t *v = &t->vt;

   switch (v->state) {
    case VT_S_OSC:
        if (c == 0x07)
           v->state = VT_S_GROUND;
        else if (c == 0x1B)
           v->state = VT_S_OSC_ESC;
        return;

    case VT_S_OSC_ESC:
        if (c != 0x1B)
           v->state = VT_S_GROUND;
        return;

    case VT_S_ESC:
        if (c == '[') {
           v->state = VT_S_CSI;
           v->csi_n = 0;
        } else if (c == ']') {
           v->state = VT_S_OSC;
        } else if (c == '7') {
           v->savx = v->serx;
           v->savy = v->sery;
           v->state = VT_S_GROUND;
        } else if (c == '8') {
           v->serx = v->savx;
           v->sery = v->savy;
           vt_serial_clamp(v);
           v->state = VT_S_GROUND;
        } else {
           v->state = VT_S_GROUND;
        }
        return;

    case VT_S_CSI:
       if (c == 0x1B) {
          v->state = VT_S_ESC;
          return;
       }
      if (c >= 0x40 && c <= 0x7E) {
           char params[48];
           int vals[32], cnt, p0, p1;
           cnt = vt_csi_serial_params(v, params, vals);
          p0 = cnt > 0 ? vals[0] : 0;
          p1 = cnt > 1 ? vals[1] : 0;
          switch ((char)c) {
          case 'H': case 'f':
             v->sery = (p0 <= 0 ? 1 : p0) - 1;
             v->serx = (p1 <= 0 ? 1 : p1) - 1;
             break;
          case 'A':
             if (p0 <= 0) p0 = 1;
             v->sery -= p0;
             break;
          case 'B': case 'e':
             if (p0 <= 0) p0 = 1;
             v->sery += p0;
             break;
          case 'C':
             if (p0 <= 0) p0 = 1;
             v->serx += p0;
             break;
          case 'D':
             if (p0 <= 0) p0 = 1;
             v->serx -= p0;
             break;
          case 'E':
             if (p0 <= 0) p0 = 1;
             v->sery += p0;
             v->serx = 0;
             break;
          case 'F':
             if (p0 <= 0) p0 = 1;
             v->sery -= p0;
             v->serx = 0;
             break;
          case 'G': case '`':
             v->serx = (p0 <= 0 ? 1 : p0) - 1;
             break;
          case 'd':
              v->sery = (p0 <= 0 ? 1 : p0) - 1;
              break;
           case 's':
              v->savx = v->serx;
              v->savy = v->sery;
              break;
           case 'u':
              v->serx = v->savx;
              v->sery = v->savy;
              break;
           case 'n':
              if (p0 == 6)
                 vt_dsr_response(t);
              break;
          default:
             break;
          }
          vt_serial_clamp(v);
          v->state = VT_S_GROUND;
          return;
       }
       if (c >= 0x20 && c <= 0x3F) {
          if (v->csi_n < (int)sizeof(v->csi) - 1)
             v->csi[v->csi_n++] = (char)c;
       } else {
          v->state = VT_S_GROUND;
       }
       return;

    default:
       break;
    }

    if (c == 0x1B) {
       v->state = VT_S_ESC;
       return;
    }
    if (c == '\r') {
       v->serx = 0;
       return;
    }
    if (c == '\n') {
       v->sery++;
       vt_serial_clamp(v);
       return;
    }
    if (c == '\b') {
       if (v->serx > 0)
          v->serx--;
       return;
    }
    if (c == '\t') {
       while (v->serx < VT_COLS && (v->serx & 7) != 0)
          v->serx++;
       vt_serial_clamp(v);
       return;
    }
    if (c < 0x20)
       return;
    if (v->serx >= VT_COLS) {
       v->serx = 0;
       v->sery++;
    }
    v->serx++;
    vt_serial_clamp(v);
}

void vt_feed(tty_t *t, int c)
{
    vt_state_t *v;
    if (!t)
       return;
    if (t->flags & TTY_SERIAL) {
       serial_putc((char)c);
       vt_serial_feed(t, c);
       return;
    }
   if (!t->ddl)
      return;
   v = &t->vt;

   switch (v->state) {
    case VT_S_OSC:
       if (c == 0x07)
          v->state = VT_S_GROUND;
       else if (c == 0x1B)
          v->state = VT_S_OSC_ESC;
       return;

    case VT_S_OSC_ESC:
       if (c != 0x1B)
          v->state = VT_S_GROUND;
       return;

    case VT_S_ESC:
       if (c == '[') {
          v->state = VT_S_CSI;
          v->csi_n = 0;
       } else if (c == ']') {
          v->state = VT_S_OSC;
       } else if (c == '7') {
         v->savx = t->ddl->curx;
         v->savy = t->ddl->cury;
         v->savsgr = v->sgr;
         v->state = VT_S_GROUND;
      } else if (c == '8') {
         t->ddl->curx = v->savx;
         t->ddl->cury = v->savy;
         v->sgr = v->savsgr;
         v->state = VT_S_GROUND;
         vt_cursor_sync(t);
     } else if (c == 'M') {
          vt_reverse_index(t);
          v->state = VT_S_GROUND;
      } else if (c == 'c') {
         vt_ris(t);
      } else if (c != 0x1B) {
         v->state = VT_S_GROUND;
      }
      return;

   case VT_S_CSI:
      if (c == 0x1B) {
         v->state = VT_S_ESC;
         return;
      }
      if (c >= 0x40 && c <= 0x7E) {
         vt_csi_dispatch(t, (char)c);
         v->state = VT_S_GROUND;
         return;
      }
      if (c >= 0x20 && c <= 0x3F) {
          /* 0x20-0x2F intermediate bytes, 0x30-0x3F parameter bytes
             (digits, ';', ':', and the '?' private marker). */
          if (v->csi_n < (int)sizeof(v->csi) - 1)
             v->csi[v->csi_n++] = (char)c;
       } else {
          /* control char or overflow: abort sequence */
          v->state = VT_S_GROUND;
       }
       return;

   default:
      break;
   }

   /* ground state */
   switch (c) {
   case 0x1B:
      v->state = VT_S_ESC;
      return;
  case '\n':
       vt_index(t);
       return;
   case '\r':
      t->ddl->curx = 0;
      vt_cursor_sync(t);
      return;
   case '\b':
      if (t->ddl->curx > 0)
         t->ddl->curx--;
      vt_cursor_sync(t);
      return;
   case '\t':
      while (t->ddl->curx < VT_COLS && (t->ddl->curx & 7) != 0)
         t->ddl->curx++;
      vt_cursor_sync(t);
      return;
   case '\a':
       return;
    default:
       if (c < 0x20)
          return;
       vt_putchar(t, (char)c, v->sgr);
       return;
    }
}

/* In-kernel probe for the `colortest` builtin: feeds a CSI byte stream
   through the real DDL vt_feed path into a scratch vt_state and returns the
   resulting sgr. A zeroed dummy DDL is borrowed only to select the DDL (not
   serial) path; pure SGR sequences never reach vt_putchar, so nothing is
   rendered. */
int vt_sgr_probe(const char *seq)
{
    static struct _dex32_direct_device_hdl dummy;
    tty_t t;
    const char *p;

    memset(&dummy, 0, sizeof(dummy));
    memset(&t, 0, sizeof(t));
    t.flags = 0;                 /* not TTY_SERIAL -> DDL vt_feed path */
    t.ddl = &dummy;
    vt_init(&t.vt);
    for (p = seq; *p; p++)
        vt_feed(&t, (unsigned char)*p);
    return (int)t.vt.sgr;
}
