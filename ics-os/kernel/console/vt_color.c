/*
 * Pure color + SGR logic for the VT100/xterm interpreter. See vt_color.h.
 * Compiled into the kernel (via kernel32.c) and by the host-native TAP in
 * tests/vt_color_unit.c so the tested logic is exactly the linked logic.
 */

#include "vt_color.h"

/* Classic VGA 16-color palette (r,g,b); index = attribute color nibble.
   Must match fb_palette[] in hardware/vga/fbconsole.c. */
static const unsigned char vt_pal16[16][3] = {
    {   0,   0,   0}, {   0,   0, 170}, {   0, 170,   0}, {   0, 170, 170},
    { 170,   0,   0}, { 170,   0, 170}, { 170,  85,   0}, { 170, 170, 170},
    {  85,  85,  85}, {  85,  85, 255}, {  85, 255,  85}, {  85, 255, 255},
    { 255,  85,  85}, { 255,  85, 255}, { 255, 255,  85}, { 255, 255, 255}
};

/* ANSI SGR color order (0 black,1 red,2 green,3 yellow,4 blue,5 magenta,
   6 cyan,7 white) -> VGA/PC palette index. The framebuffer palette (fbconsole
   fb_palette / vt_pal16) uses the classic PC order (1 blue, 2 green, 3 cyan,
   4 red, 5 magenta, 6 yellow, 7 white), while SGR 30-37 / 90-97 use the ANSI
   order. Without this permutation SGR 31 (red) would render as palette 1
   (blue). */
static const unsigned char vt_ansi2vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

/* Nearest palette entry to an RGB triple by squared Euclidean distance. */
static int vt_nearest16(int r, int g, int b)
{
    int best = 0, bestd = 0x7fffffff;
    int i, dr, dg, db, d;
    for (i = 0; i < 16; i++) {
        dr = r - (int)vt_pal16[i][0];
        dg = g - (int)vt_pal16[i][1];
        db = b - (int)vt_pal16[i][2];
        d = dr * dr + dg * dg + db * db;
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

int vt_color_rgb(int r, int g, int b)
{
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return vt_nearest16(r, g, b);
}

int vt_color256(int idx)
{
    if (idx < 0) idx = 0;
    if (idx > 255) idx = 255;
    if (idx < 16)
        /* base 16: the 256-cube uses the ANSI order, so permute to the
           VGA/PC palette (idx 0-7 base, 8-15 bright). */
        return idx < 8 ? vt_ansi2vga[idx] : vt_ansi2vga[idx - 8] + 8;
    if (idx < 232) {
        int m = idx - 16;
        int cr = m / 36, cg = (m / 6) % 6, cb = m % 6;
        int r = cr == 0 ? 0 : cr * 40 + 55;
        int g = cg == 0 ? 0 : cg * 40 + 55;
        int b = cb == 0 ? 0 : cb * 40 + 55;
        return vt_nearest16(r, g, b);
    }
    {   int v = (idx - 232) * 10 + 8;   /* grayscale ramp 8..238 */
        return vt_nearest16(v, v, v);
    }
}

int vt_parse_params(char *s, int *vals, int max)
{
    int n = 0, cur = -1, i;
    for (i = 0; s[i] && n < max; i++) {
        if (s[i] >= '0' && s[i] <= '9') {
            if (cur < 0)
                cur = 0;
            if (cur < VT_PARAM_MAX) {
                cur = cur * 10 + (s[i] - '0');
                if (cur > VT_PARAM_MAX)
                    cur = VT_PARAM_MAX;
            }
        } else if (s[i] == ';') {
            if (n < max)
                vals[n++] = cur < 0 ? 0 : cur;
            cur = -1;
        } else {
            return -1;
        }
    }
    if (n < max)
        vals[n++] = cur < 0 ? 0 : cur;
    return n;
}

int vt_sgr_apply(int sgr, const int *vals, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        int p = vals[i];
        switch (p) {
        case 0:
            sgr = VT_DEFAULT_ATTR;
            break;
        case 1:
            sgr = (sgr & 0xF0) | 0x0F;
            break;
        case 4:
            sgr |= 0x80;
            break;
        case 7: {
            int fg = sgr & 0x0F, bg = (sgr >> 4) & 0x0F;
            sgr = (sgr & 0x80) | (fg << 4) | bg;
            break;
        }
        case 21: case 22:
            sgr = (sgr & 0xF0) | (sgr & 0x07);
            break;
        case 24:
            sgr &= ~0x80;
            break;
        case 27: {
            int fg = sgr & 0x0F, bg = (sgr >> 4) & 0x0F;
            sgr = (sgr & 0x80) | fg | (bg << 4);
            break;
        }
        case 39:
            sgr = (sgr & 0xF0) | 0x07;
            break;
        case 49:
            sgr &= 0x0F;
            break;
        case 38:
            if (i + 1 < n && vals[i + 1] == 5 && i + 2 < n) {
                sgr = (sgr & 0xF0) | vt_color256(vals[i + 2]);
                i += 2;
            } else if (i + 1 < n && vals[i + 1] == 2 && i + 4 < n) {
                sgr = (sgr & 0xF0) |
                      vt_color_rgb(vals[i + 2], vals[i + 3], vals[i + 4]);
                i += 4;
            }
            break;
        case 48:
            if (i + 1 < n && vals[i + 1] == 5 && i + 2 < n) {
                sgr = (sgr & 0x0F) | (vt_color256(vals[i + 2]) << 4);
                i += 2;
            } else if (i + 1 < n && vals[i + 1] == 2 && i + 4 < n) {
                sgr = (sgr & 0x0F) |
                      (vt_color_rgb(vals[i + 2], vals[i + 3], vals[i + 4]) << 4);
                i += 4;
            }
            break;
        default:
            if (p >= 30 && p <= 37)
                sgr = (sgr & 0xF0) | vt_ansi2vga[p - 30];
            else if (p >= 40 && p <= 47)
                sgr = (sgr & 0x0F) | (vt_ansi2vga[p - 40] << 4);
            else if (p >= 90 && p <= 97)
                sgr = (sgr & 0xF0) | (vt_ansi2vga[p - 90] + 8);
            else if (p >= 100 && p <= 107)
                sgr = (sgr & 0x0F) | ((vt_ansi2vga[p - 100] + 8) << 4);
            break;
        }
    }
    return sgr;
}
