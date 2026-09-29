/*
 * Host TAP for the VT color + SGR logic (kernel/console/vt_color.c). This is
 * the exact code linked into the kernel, so the 256-color / truecolor -> 16
 * palette mapping and the SGR state machine are verified off-target.
 *
 * Regression focus: vim/nethack emit SGR only when the termcap advertises Co,
 * and 256-color requests arrive as "38;5;n" / "48;5;n" (or "38;2;r;g;b") which
 * the old parser dropped, leaving the text monochrome.
 */
#include <stdio.h>

#include "kernel/console/vt_color.h"

static int check(const char *name, int condition)
{
    if (!condition) {
        printf("not ok - %s\n", name);
        return 0;
    }
    printf("ok - %s\n", name);
    return 1;
}

/* Apply an SGR param string to a starting attr and return the result. */
static int sgr_from(const char *params, int start)
{
    int vals[32], n;
    n = vt_parse_params((char *)params, vals, 32);
    if (n <= 0)
        return start;
    return vt_sgr_apply(start, vals, n);
}

int main(void)
{
    int ok = 1;

    printf("TAP version 13\n1..45\n");

    /* 256-color: base 16 permutes the ANSI order onto the VGA/PC palette. */
    ok &= check("256 idx 0 (black) maps to palette 0", vt_color256(0) == 0);
    ok &= check("256 idx 1 (ANSI red) maps to palette 4", vt_color256(1) == 4);
    ok &= check("256 idx 4 (ANSI blue) maps to palette 1", vt_color256(4) == 1);
    ok &= check("256 idx 7 maps to palette 7", vt_color256(7) == 7);
    ok &= check("256 idx 9 (ANSI bright red) maps to palette 12", vt_color256(9) == 12);
    ok &= check("256 idx 15 maps to palette 15", vt_color256(15) == 15);
    /* Cube corners. */
    ok &= check("256 idx 16 (0,0,0) maps to black", vt_color256(16) == 0);
    ok &= check("256 idx 21 (0,0,255) maps to blue", vt_color256(21) == 1);
    ok &= check("256 idx 196 (255,0,0) maps to red", vt_color256(196) == 4);
    ok &= check("256 idx 231 (255,255,255) maps to white", vt_color256(231) == 15);
    /* Grayscale ramp. */
    ok &= check("256 idx 232 (darkest gray) maps near black", vt_color256(232) == 0);
    ok &= check("256 idx 255 (lightest gray) maps to white", vt_color256(255) == 15);
    /* Out-of-range clamps. */
    ok &= check("256 idx -1 clamps to palette 0", vt_color256(-1) == 0);
    ok &= check("256 idx 999 clamps to white", vt_color256(999) == 15);

    /* Truecolor: exact palette entries map to themselves. */
    ok &= check("rgb(0,0,0) -> 0", vt_color_rgb(0, 0, 0) == 0);
    ok &= check("rgb(170,0,0) -> 4 (red)", vt_color_rgb(170, 0, 0) == 4);
    ok &= check("rgb(0,170,170) -> 3 (cyan)", vt_color_rgb(0, 170, 170) == 3);
    ok &= check("rgb(85,85,255) -> 9 (light blue)", vt_color_rgb(85, 85, 255) == 9);
    ok &= check("rgb(255,255,255) -> 15", vt_color_rgb(255, 255, 255) == 15);
    ok &= check("rgb(255,255,0) -> 14 (nearest yellow)", vt_color_rgb(255, 255, 0) == 14);
    ok &= check("rgb clamps negative to black", vt_color_rgb(-5, 0, 0) == 0);
    ok &= check("rgb clamps >255 to white", vt_color_rgb(999, 999, 999) == 15);

    /* Param parsing. */
    {
        int vals[32], n;
        n = vt_parse_params((char *)"38;5;196", vals, 32);
        ok &= check("parse 38;5;196 -> 3 values", n == 3);
        ok &= check("parse 38;5;196 values", vals[0] == 38 && vals[1] == 5 && vals[2] == 196);
        n = vt_parse_params((char *)"38;2;255;128;0", vals, 32);
        ok &= check("parse 38;2;255;128;0 -> 5 values",
                    n == 5 && vals[0] == 38 && vals[1] == 2 &&
                    vals[2] == 255 && vals[3] == 128 && vals[4] == 0);
        n = vt_parse_params((char *)"", vals, 32);
        ok &= check("parse empty -> default 0", n == 1 && vals[0] == 0);
    }

    /* SGR: basic + attribute ops. Basic colors permute the ANSI order onto the
       VGA/PC palette (31 red -> 4, 34 blue -> 1, etc.). */
    ok &= check("sgr 0 resets to default 0x07", sgr_from("0", 0x41) == 0x07);
    ok &= check("sgr 31 (ANSI red) sets fg 4", sgr_from("31", 0x07) == 0x04);
    ok &= check("sgr 34 (ANSI blue) sets fg 1", sgr_from("34", 0x07) == 0x01);
    ok &= check("sgr 41 (ANSI red bg) sets bg 4", sgr_from("41", 0x07) == 0x47);
    ok &= check("sgr 1 sets bold (fg 0x0F)", sgr_from("1", 0x07) == 0x0F);
    ok &= check("sgr 4 sets underline bit", sgr_from("4", 0x07) == 0x87);
    ok &= check("sgr 7 swaps fg/bg", sgr_from("7", 0x07) == 0x70);
    ok &= check("sgr 39 restores default fg", sgr_from("39", 0x41) == 0x47);
    ok &= check("sgr 49 clears bg", sgr_from("49", 0x41) == 0x01);

    /* SGR: 256-color and truecolor (the regression). */
    ok &= check("sgr 38;5;196 sets fg to red(4)", sgr_from("38;5;196", 0x07) == 0x04);
    ok &= check("sgr 48;5;21 sets bg to blue(1)", sgr_from("48;5;21", 0x07) == 0x17);
    ok &= check("sgr 38;5;231 sets fg to white(15)", sgr_from("38;5;231", 0x07) == 0x0F);
    ok &= check("sgr 48;5;232 sets bg to black(0)", sgr_from("48;5;232", 0x41) == 0x01);
    ok &= check("sgr 38;2;255;0;0 sets fg to red(4)", sgr_from("38;2;255;0;0", 0x07) == 0x04);
    ok &= check("sgr 48;2;0;170;170 sets bg to cyan(3)", sgr_from("48;2;0;170;170", 0x07) == 0x37);
    ok &= check("sgr 1;31 ends with fg red(4), no underline",
                (sgr_from("1;31", 0x07) & 0x0F) == 4 && (sgr_from("1;31", 0x07) & 0x80) == 0);
    ok &= check("sgr 91 (bright red) sets fg 12", sgr_from("91", 0x07) == 0x0C);
    ok &= check("sgr 94 (bright blue) sets fg 9", sgr_from("94", 0x07) == 0x09);
    ok &= check("sgr 101 (bright red bg) sets bg 12", sgr_from("101", 0x07) == 0xC7);

    return ok ? 0 : 1;
}
