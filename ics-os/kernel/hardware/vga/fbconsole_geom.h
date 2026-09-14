#ifndef FBCONSOLE_GEOM_H
#define FBCONSOLE_GEOM_H

/*
  Validate a Multiboot2 RGB framebuffer for the 80x25 GOP/VBE console.

  Intel GOP commonly pads pitch past width*(bpp/8) (64/128/4096-byte row
  alignment). Packed pitch is the minimum; extra bytes are padding. A pitch
  smaller than packed cannot store a scanline and is rejected.
*/
#define FBCONSOLE_PITCH_MAX 65536u
#define FBCONSOLE_COLS 80u
#define FBCONSOLE_ROWS 25u
#define FBCONSOLE_CELL_W 8u
#define FBCONSOLE_CELL_H 16u

static inline int fbconsole_format_supported(unsigned int pitch,
                                             unsigned int width,
                                             unsigned int height,
                                             unsigned int bpp,
                                             unsigned int ftype)
{
    unsigned int packed;

    if (ftype != 1) /* MB2_FB_TYPE_RGB */
        return 0;
    if (bpp != 16 && bpp != 24 && bpp != 32)
        return 0;
    if (!width || !height || !pitch)
        return 0;
    packed = width * (bpp / 8);
    if (pitch < packed)
        return 0;
    if (pitch > FBCONSOLE_PITCH_MAX)
        return 0;
    return 1;
}

/* Integer zoom per axis so 80x25 fills as much of the panel as possible.
   1920x1200 is 3x3. 1920x1080 is 3x2 (full width, letterbox top/bottom).
   The old min() zoom left 1920x1080 at 2x2 (1280x800, black on all sides). */
static inline void fbconsole_glyph_scale_xy(unsigned int width,
                                            unsigned int height,
                                            unsigned int *sx,
                                            unsigned int *sy)
{
    unsigned int x, y;

    if (!sx || !sy)
        return;
    x = width / (FBCONSOLE_COLS * FBCONSOLE_CELL_W);
    y = height / (FBCONSOLE_ROWS * FBCONSOLE_CELL_H);
    if (x < 1)
        x = 1;
    if (y < 1)
        y = 1;
    *sx = x;
    *sy = y;
}

static inline unsigned int fbconsole_glyph_scale(unsigned int width,
                                                 unsigned int height)
{
    unsigned int sx, sy;

    fbconsole_glyph_scale_xy(width, height, &sx, &sy);
    return sx < sy ? sx : sy;
}

static inline void fbconsole_grid_origin_xy(unsigned int width,
                                            unsigned int height,
                                            unsigned int zoom_x,
                                            unsigned int zoom_y,
                                            unsigned int *ox, unsigned int *oy)
{
    unsigned int gw, gh;

    if (!ox || !oy)
        return;
    if (!zoom_x)
        zoom_x = 1;
    if (!zoom_y)
        zoom_y = 1;
    gw = FBCONSOLE_COLS * FBCONSOLE_CELL_W * zoom_x;
    gh = FBCONSOLE_ROWS * FBCONSOLE_CELL_H * zoom_y;
    *ox = width > gw ? (width - gw) / 2u : 0;
    *oy = height > gh ? (height - gh) / 2u : 0;
}

/* Uniform-zoom origin (host TAP). Production uses grid_origin_xy. */
static inline void fbconsole_grid_origin(unsigned int width, unsigned int height,
                                         unsigned int zoom,
                                         unsigned int *ox, unsigned int *oy)
{
    fbconsole_grid_origin_xy(width, height, zoom, zoom, ox, oy);
}

/* Legacy VGA text (0xB8000 + CRTC 0x3D4) only when there is no framebuffer
   tag and COM1 is present (QEMU/Bochs). A GOP tag we chose not to map, or
   a COM1-less laptop, has no VGA; those I/O ports hang or reset Intel PCH. */
static inline int fbconsole_use_legacy_vga(int have_fb_tag, int com1_present,
                                           int fb_active)
{
    if (fb_active)
        return 0;
    if (have_fb_tag)
        return 0;
    if (!com1_present)
        return 0;
    return 1;
}

/* fbconsole_late_init() return / skip codes (also the post-scheduler LED). */
#define FBCONSOLE_LATE_OK      0 /* identity <4GiB; mapped+painted → Caps+Num */
#define FBCONSOLE_LATE_NO_TAG  1 /* GRUB gave no GOP tag; Num stay */
#define FBCONSOLE_LATE_READY   2 /* already mapped (QEMU deferred) */
#define FBCONSOLE_LATE_HIGH    3 /* phys above/crossing 4GiB; map via KFB */
#define FBCONSOLE_LATE_BAD     4 /* missing phys/size, wrap, or too large */

/* 2MiB PDE PAT is bit 12. Combined with PA4=WC in IA32_PAT (not PCD|PWT
   UC, which rebooted the N150 on a full-panel fill). Do not use on 4KiB
   PTEs: those use bit 7 as PAT. */
#define FBCONSOLE_PDE_PAT_LARGE 0x1000ULL
#define FBCONSOLE_PTE_PWT       0x8ULL
#define FBCONSOLE_PTE_PCD       0x10ULL

static inline unsigned long long fbconsole_pde_mark_wc(unsigned long long pde)
{
    pde &= ~(FBCONSOLE_PTE_PWT | FBCONSOLE_PTE_PCD);
    pde |= FBCONSOLE_PDE_PAT_LARGE;
    return pde;
}

/* Identity WC only fits below 4 GiB. Crossing or above uses KFB 2MiB pages. */
static inline int fbconsole_late_fits_identity(unsigned long long phys,
                                               unsigned long long bytes)
{
    if (!phys || !bytes)
        return 0;
    if (phys + bytes < phys)
        return 0;
    if (phys >= 0x100000000ULL || phys + bytes > 0x100000000ULL)
        return 0;
    return 1;
}

static inline int fbconsole_late_map_reason(int have_tag, int already_ready,
                                            unsigned long long phys,
                                            unsigned long long bytes)
{
    if (already_ready)
        return FBCONSOLE_LATE_READY;
    if (!have_tag)
        return FBCONSOLE_LATE_NO_TAG;
    if (!phys || !bytes)
        return FBCONSOLE_LATE_BAD;
    if (phys + bytes < phys)
        return FBCONSOLE_LATE_BAD;
    if (!fbconsole_late_fits_identity(phys, bytes))
        return FBCONSOLE_LATE_HIGH;
    return FBCONSOLE_LATE_OK;
}

static inline int fbconsole_late_map_ok(int have_tag, int already_ready,
                                        unsigned long long phys,
                                        unsigned long long bytes)
{
    int r = fbconsole_late_map_reason(have_tag, already_ready, phys, bytes);
    return r == FBCONSOLE_LATE_OK || r == FBCONSOLE_LATE_HIGH;
}

#endif
