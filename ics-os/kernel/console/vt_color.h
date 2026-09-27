#ifndef ICSOS_VT_COLOR_H
#define ICSOS_VT_COLOR_H

/*
 * Pure color + SGR logic for the VT100/xterm interpreter (console/tty_vt.c).
 *
 * These functions carry no kernel dependencies so the exact logic linked into
 * the kernel can be exercised by a host-native TAP (tests/vt_color_unit.c).
 *
 * The cell attribute byte is: low nibble = foreground, high nibble =
 * background, bit 7 = underline. It can represent the 16-color VGA palette
 * only; 256-color and truecolor SGR requests are mapped to the nearest of
 * those 16 palette entries.
 */

#define VT_PARAM_MAX    1000000
#define VT_DEFAULT_ATTR 0x07   /* default rendition: light-gray on black */

/* Map a 256-color xterm index (0-255) to a 16-color VGA palette index. */
int vt_color256(int idx);

/* Map a truecolor RGB triple (each 0-255) to the nearest 16-color index. */
int vt_color_rgb(int r, int g, int b);

/* Parse "n1;n2;..." into vals (at most max); returns the count, -1 on bad. */
int vt_parse_params(char *s, int *vals, int max);

/* Apply an already-parsed SGR parameter list to a cell attribute; returns
   the new attribute. Handles 0/1/4/7/21/22/24/27/39/49, 30-37, 40-47,
   90-97, 100-107, and the extended 38;5;n / 48;5;n / 38;2;r;g;b / 48;2;r;g;b. */
int vt_sgr_apply(int sgr, const int *vals, int n);

#endif
