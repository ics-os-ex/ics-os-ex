/*
  Host TAP for GOP/VBE pitch acceptance. Intel GOP pads scanlines; the old
  `pitch > width*(bpp/8)` check rejected a 1920x1200x32 panel with pitch 8192.
*/
#include <stdio.h>

#include "kernel/hardware/vga/fbconsole_geom.h"

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
    int ok = 1;

    printf("TAP version 13\n1..35\n");

    ok &= check("packed 1024x768x32 (QEMU VBE) is accepted",
                fbconsole_format_supported(4096, 1024, 768, 32, 1));
    ok &= check("packed 1920x1200x32 (N150 native) is accepted",
                fbconsole_format_supported(7680, 1920, 1200, 32, 1));
    ok &= check("padded 1920x1200x32 pitch 8192 is accepted",
                fbconsole_format_supported(8192, 1920, 1200, 32, 1));
    ok &= check("padded 1920x1080x32 pitch 8192 is accepted",
                fbconsole_format_supported(8192, 1920, 1080, 32, 1));
    ok &= check("pitch smaller than packed scanline is rejected",
                !fbconsole_format_supported(7679, 1920, 1200, 32, 1));
    ok &= check("zero pitch is rejected",
                !fbconsole_format_supported(0, 1920, 1200, 32, 1));
    ok &= check("indexed palette framebuffer is rejected",
                !fbconsole_format_supported(7680, 1920, 1200, 32, 0));
    ok &= check("8 bpp is rejected",
                !fbconsole_format_supported(1920, 1920, 1200, 8, 1));
    ok &= check("pitch above 64KiB is rejected",
                !fbconsole_format_supported(65537, 1920, 1200, 32, 1));
    ok &= check("24 bpp packed 1920-wide is accepted",
                fbconsole_format_supported(5760, 1920, 1200, 24, 1));
    ok &= check("1920x1200 uses 3x glyphs (fills the N150 panel)",
                fbconsole_glyph_scale(1920, 1200) == 3);
    ok &= check("1024x768 stays 1x (QEMU VBE)",
                fbconsole_glyph_scale(1024, 768) == 1);
    ok &= check("undersize 320x200 stays 1x",
                fbconsole_glyph_scale(320, 200) == 1);
    ok &= check("QEMU VGA-only (no tag, COM1) uses 0xB8000",
                fbconsole_use_legacy_vga(0, 1, 0) == 1);
    ok &= check("N150 GOP tag skipped (no COM1) never uses VGA",
                fbconsole_use_legacy_vga(1, 0, 0) == 0);
    ok &= check("N150 no GOP tag and no COM1 never uses VGA",
                fbconsole_use_legacy_vga(0, 0, 0) == 0);
    ok &= check("mapped GOP never uses VGA CRTC",
                fbconsole_use_legacy_vga(1, 1, 1) == 0);
    ok &= check("late map: N150 stolen GOP below 4GiB is allowed",
                fbconsole_late_map_ok(1, 0, 0x80000000ULL, 1920ull * 1200u * 4u) == 1);
    ok &= check("late map: already mapped (QEMU deferred) is a no-op",
                fbconsole_late_map_ok(1, 1, 0xE0000000ULL, 4096ull * 768u) == 0);
    ok &= check("late map: missing tag is refused",
                fbconsole_late_map_ok(0, 0, 0x80000000ULL, 4096) == 0);
    ok &= check("late map: GOP above 4GiB is HIGH but mappable",
                fbconsole_late_map_ok(1, 0, 0x100000000ULL, 4096) == 1);
    ok &= check("late map: wraparound length is refused",
                fbconsole_late_map_ok(1, 0, 0xFFFFFFFFFFFFFF00ULL, 512) == 0);
    ok &= check("late reason: missing tag is NO_TAG",
                fbconsole_late_map_reason(0, 0, 0x80000000ULL, 4096)
                == FBCONSOLE_LATE_NO_TAG);
    ok &= check("late reason: GOP above 4GiB is HIGH",
                fbconsole_late_map_reason(1, 0, 0x100000000ULL, 4096)
                == FBCONSOLE_LATE_HIGH);
    ok &= check("late map: GOP crossing 4GiB is HIGH but mappable",
                fbconsole_late_map_ok(1, 0, 0xFFFF0000ULL, 0x20000ULL) == 1);
    ok &= check("late reason: already mapped is READY",
                fbconsole_late_map_reason(1, 1, 0xE0000000ULL, 4096)
                == FBCONSOLE_LATE_READY);
    ok &= check("late reason: N150 stolen GOP is OK",
                fbconsole_late_map_reason(1, 0, 0x80000000ULL,
                                          1920ull * 1200u * 4u)
                == FBCONSOLE_LATE_OK);
    ok &= check("2MiB PDE 0x83 becomes PAT-WC 0x1083",
                fbconsole_pde_mark_wc(0x83ULL) == 0x1083ULL);
    ok &= check("2MiB PDE UC bits are cleared for PAT-WC",
                fbconsole_pde_mark_wc(0x83ULL | 0x18ULL) == 0x1083ULL);
    ok &= check("1920x1080 uses 2x glyphs",
                fbconsole_glyph_scale(1920, 1080) == 2);
    {
        unsigned int ox = 99, oy = 99;
        fbconsole_grid_origin(1920, 1200, 3, &ox, &oy);
        ok &= check("1920x1200 3x grid origin is 0,0", ox == 0 && oy == 0);
        fbconsole_grid_origin(1920, 1080, 2, &ox, &oy);
        ok &= check("1920x1080 2x grid is centered (320,140)",
                    ox == 320 && oy == 140);
        {
            unsigned int sx = 0, sy = 0;
            fbconsole_glyph_scale_xy(1920, 1080, &sx, &sy);
            ok &= check("1920x1080 per-axis zoom is 3x2", sx == 3 && sy == 2);
            fbconsole_glyph_scale_xy(1920, 1200, &sx, &sy);
            ok &= check("1920x1200 per-axis zoom is 3x3", sx == 3 && sy == 3);
            fbconsole_grid_origin_xy(1920, 1080, 3, 2, &ox, &oy);
            ok &= check("1920x1080 3x2 origin is 0,140", ox == 0 && oy == 140);
        }
    }

    return ok ? 0 : 1;
}
