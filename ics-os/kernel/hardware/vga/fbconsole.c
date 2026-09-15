/*
  Name: fbconsole
  Description:
  ==========================================================================
  Renders the 80x25 DDL text grid into the linear framebuffer supplied by
  the multiboot2 framebuffer tag (UEFI GOP or BIOS VBE). The DDL keeps its
  per-device text shadow; this module blits cells into the framebuffer so
  the on-screen console works on firmware without a legacy VGA text mode.

  Pixel formats: RGB (tag type 1) at 16/24/32 bpp using the tag's
  shift/size fields (little-endian pixel storage).
  ==========================================================================
*/

#include "hardware/vga/fbconsole_geom.h"
#include "hardware/keyboard/kbd_boot_leds.h"
#include "memory/memlayout.h"

/* Classic VGA 16-color palette (r,g,b), index = attribute color nibble. */
static const unsigned char fb_palette[16][3] = {
    {   0,   0,   0}, {   0,   0, 170}, {   0, 170,   0}, {   0, 170, 170},
    { 170,   0,   0}, { 170,   0, 170}, { 170,  85,   0}, { 170, 170, 170},
    {  85,  85,  85}, {  85,  85, 255}, {  85, 255,  85}, {  85, 255, 255},
    { 255,  85,  85}, { 255,  85, 255}, { 255, 255,  85}, { 255, 255, 255}
};

static unsigned int fb_have_tag;
static unsigned int fb_enabled;
static unsigned int fb_ready;
static unsigned long long fb_phys;
static unsigned int fb_pitch, fb_width, fb_height, fb_bpp, fb_ftype;
static unsigned int fb_rshift, fb_rsize, fb_gshift, fb_gsize, fb_bshift, fb_bsize;
static unsigned char *fb_base;
static int fb_cx = -1;
static int fb_cy = -1;
static unsigned int fb_zoom_x = 1;
static unsigned int fb_zoom_y = 1;
static unsigned int fb_offx;
static unsigned int fb_offy;

/* Live per-cell framebuffer blitting is OFF by default. Each cell render is
   128 MMIO writes (and a full screen refresh is 256,000), which in QEMU are
   slow VM-exits; doing them on the console hot path while a process holds a
   device lock deadlocks the I/O path (see development_blog.md 2026-09-09).
   The DDL already keeps a per-device text shadow; a deferred blit from a
   low-priority context is the intended long-term path. The boot selftest
   (fbconsole_selftest) turns this ON to validate the renderer directly. */
static int fb_live_render = 0;

void fbconsole_set_live_render(int on)
{
    fb_live_render = on ? 1 : 0;
}

static unsigned int fb_scale(unsigned int v, unsigned int size)
{
    if (size >= 8)
        return v;
    return (v >> (8 - size)) & ((1u << size) - 1u);
}

static unsigned int fb_color(unsigned int idx)
{
    const unsigned char *p = fb_palette[idx & 0x0Fu];
    unsigned int r = fb_scale(p[0], fb_rsize);
    unsigned int g = fb_scale(p[1], fb_gsize);
    unsigned int b = fb_scale(p[2], fb_bsize);
    return (r << fb_rshift) | (g << fb_gshift) | (b << fb_bshift);
}

static void fb_put_pixel(unsigned int x, unsigned int y, unsigned int v)
{
    unsigned char *p;
    if (x >= fb_width || y >= fb_height || !fb_base)
        return;
    p = fb_base + (y * fb_pitch) + (x * (fb_bpp >> 3));
    /* movnti bypasses the CPU cache so the iGPU sees the store even
       when the identity map is still write-back. Requires 4-byte
       alignment (32 bpp + 4K-aligned GOP). */
    if (fb_bpp == 32 && !serial_com1_present()
        && (((unsigned long)(uintptr)p) & 3ul) == 0) {
        __asm__ __volatile__("movnti %1, %0"
                             : "=m"(*(unsigned int *)(void *)p)
                             : "r"(v)
                             : "memory");
        return;
    }
    if (fb_bpp == 32) {
        p[0] = (unsigned char)(v & 0xFFu);
        p[1] = (unsigned char)((v >> 8) & 0xFFu);
        p[2] = (unsigned char)((v >> 16) & 0xFFu);
        p[3] = (unsigned char)((v >> 24) & 0xFFu);
    } else if (fb_bpp == 24) {
        p[0] = (unsigned char)(v & 0xFFu);
        p[1] = (unsigned char)((v >> 8) & 0xFFu);
        p[2] = (unsigned char)((v >> 16) & 0xFFu);
    } else {
        p[0] = (unsigned char)(v & 0xFFu);
        p[1] = (unsigned char)((v >> 8) & 0xFFu);
    }
}

static unsigned int fb_get_pixel(unsigned int x, unsigned int y)
{
    const unsigned char *p = fb_base + (y * fb_pitch) + (x * (fb_bpp >> 3));
    unsigned int v = 0;
    int k;
    for (k = (fb_bpp >> 3) - 1; k >= 0; k--)
        v = (v << 8) | p[k];
    return v;
}

/* Laptop GOP is write-combining (PAT PA4) plus movnti. sfence drains
   the WC buffers; a full-panel UC fill rebooted the N150, so do not
   mark this range PCD|PWT. */
static void fb_flush_rect(unsigned int x, unsigned int y,
                          unsigned int w, unsigned int h)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    if (!fb_base || serial_com1_present())
        return;
    __asm__ __volatile__("sfence" ::: "memory");
}

static void fb_flush_glyph(unsigned int px, unsigned int py)
{
    unsigned int zx = fb_zoom_x ? fb_zoom_x : 1u;
    unsigned int zy = fb_zoom_y ? fb_zoom_y : 1u;
    fb_flush_rect(px, py, 8u * zx, 16u * zy);
}

static void fb_draw_glyph(const unsigned char *glyph, unsigned int px,
                          unsigned int py, unsigned int fg, unsigned int bg)
{
    unsigned int row, col, zy, zx;
    unsigned int zx_n = fb_zoom_x ? fb_zoom_x : 1u;
    unsigned int zy_n = fb_zoom_y ? fb_zoom_y : 1u;
    for (row = 0; row < 16; row++) {
        unsigned int bits = glyph[row];
        for (col = 0; col < 8; col++) {
            unsigned int pix = (bits >> (7 - col)) & 1u ? fg : bg;
            for (zy = 0; zy < zy_n; zy++)
                for (zx = 0; zx < zx_n; zx++)
                    fb_put_pixel(px + col * zx_n + zx,
                                 py + row * zy_n + zy, pix);
        }
    }
    fb_flush_glyph(px, py);
}

int fbconsole_boot_init(unsigned long long addr, unsigned int pitch,
                        unsigned int width, unsigned int height,
                        unsigned int bpp, unsigned int ftype,
                        unsigned int rshift, unsigned int rsize,
                        unsigned int gshift, unsigned int gsize,
                        unsigned int bshift, unsigned int bsize)
{
    char msg[96];
    if (!fbconsole_format_supported(pitch, width, height, bpp, ftype)) {
        serial_puts("FBCONSOLE: unsupported framebuffer format, using VGA text\n");
        return 0;
    }
    fb_phys = addr;
    fb_pitch = pitch;
    fb_width = width;
    fb_height = height;
    fb_bpp = bpp;
    fb_ftype = ftype;
    fb_rshift = rshift;
    fb_rsize = rsize;
    fb_gshift = gshift;
    fb_gsize = gsize;
    fb_bshift = bshift;
    fb_bsize = bsize;
    fb_base = 0;
    fb_ready = 0;
    fbconsole_glyph_scale_xy(width, height, &fb_zoom_x, &fb_zoom_y);
    /* Record only. Mapping or painting GOP before the IDT and mem_init
       triple-faults on N150 (stolen graphics is not safe MMIO yet). */
    fb_have_tag = 1;
    fb_enabled = 1;
    sprintf(msg, "FBCONSOLE: %ux%u bpp=%u pitch=%u zoom=%ux%u addr=0x%llx type=%u (deferred)\n",
            (unsigned)width, (unsigned)height, (unsigned)bpp, (unsigned)pitch,
            (unsigned)fb_zoom_x, (unsigned)fb_zoom_y,
            (unsigned long long)addr, (unsigned)ftype);
    serial_puts(msg);
    return fb_enabled;
}

void fbconsole_deferred_init(void)
{
    unsigned long long bytes;
    if (!fb_enabled || fb_ready)
        return;
    /* N150: any GOP store/clflush at stage 2/3 rebooted the box (blank
       panel, Caps+Num). Leave the panel unused until a later safe WC
       map exists. QEMU has COM1 and still maps for FBCONSOLE_PASS. */
    if (!serial_com1_present()) {
        serial_puts("FBCONSOLE: skipping GOP MMIO (no COM1)\n");
        fb_enabled = 0;
        fb_base = 0;
        fb_ready = 0;
        return;
    }
    bytes = (unsigned long long)fb_pitch * fb_height;
    if (!bytes)
        return;
    if (fb_phys < 0x100000000ULL && fb_phys + bytes <= 0x100000000ULL) {
        /* QEMU selftest wants UC so readback bypasses the host cache.
           Real Intel GOP must stay WB: UC of 1920x1200 reboots the N150. */
        if (serial_com1_present()) {
            if (!mmio_mark_uncacheable(fb_phys, bytes)) {
                serial_puts("FBCONSOLE: could not map framebuffer, using VGA text\n");
                fb_enabled = 0;
                return;
            }
        }
        fb_base = (unsigned char *)(uintptr)fb_phys;
    } else {
        fb_base = (unsigned char *)(uintptr)
            (unsigned long)mmio_map(fb_phys, bytes);
        if (!fb_base) {
            serial_puts("FBCONSOLE: high framebuffer mapping failed, using VGA text\n");
            fb_enabled = 0;
            return;
        }
    }
    fb_ready = 1;
    {
        char fbl[96];
        sprintf(fbl, "FB %ux%u zoom=%ux%u", fb_width, fb_height,
                fb_zoom_x, fb_zoom_y);
        fbdbg_info(fbl);
    }
}

/* IA32_PAT: keep PA0=WB for RAM, set PA4=WC (0x01). 2MiB GOP PDEs then
   use PAT bit 12 so stores are write-combining instead of write-back. */
static void fb_pat_enable_wc(void)
{
    unsigned int lo, hi;
    unsigned long long pat;

    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x277));
    pat = ((unsigned long long)hi << 32) | lo;
    pat &= ~(0xFFULL << 32);
    pat |= (0x01ULL << 32);
    lo = (unsigned int)pat;
    hi = (unsigned int)(pat >> 32);
    __asm__ __volatile__("wbinvd" ::: "memory");
    __asm__ __volatile__("wrmsr" :: "c"(0x277), "a"(lo), "d"(hi) : "memory");
    __asm__ __volatile__("wbinvd" ::: "memory");
}

static void fb_tlb_reload(void)
{
    unsigned long cr3;

    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

static void fb_mark_identity_wc(unsigned long long phys, unsigned long long len)
{
    extern unsigned long long boot_pd0[], boot_pd1[], boot_pd2[], boot_pd3[];
    unsigned long long *pds[4];
    unsigned long long addr, end;

    pds[0] = boot_pd0;
    pds[1] = boot_pd1;
    pds[2] = boot_pd2;
    pds[3] = boot_pd3;
    end = phys + len;
    addr = phys & ~0x1FFFFFULL;
    for (; addr < end; addr += 0x200000ULL) {
        unsigned pd = (unsigned)(addr >> 30);
        unsigned idx = (unsigned)((addr >> 21) & 0x1FF);
        if (pd > 3)
            continue;
        pds[pd][idx] = fbconsole_pde_mark_wc(pds[pd][idx]);
    }
    fb_tlb_reload();
}

static unsigned long long fb_high_pd[512] __attribute__((aligned(4096)));

static unsigned char *fb_map_high_wc(unsigned long long phys,
                                     unsigned long long len)
{
    extern unsigned long long boot_pdpt_high[];
    unsigned long long aligned, offset, pages, i;

    if (!len || len > (unsigned long long)KFB_SIZE)
        return 0;
    aligned = phys & ~0x1FFFFFULL;
    offset = phys - aligned;
    pages = (offset + len + 0x1FFFFFULL) >> 21;
    if (!pages || pages > 512)
        return 0;
    for (i = 0; i < 512; i++)
        fb_high_pd[i] = 0;
    for (i = 0; i < pages; i++)
        fb_high_pd[i] = fbconsole_pde_mark_wc((aligned + (i << 21)) | 0x83ULL);
    boot_pdpt_high[5] = (unsigned long long)(uintptr)fb_high_pd | 3ULL;
    fb_tlb_reload();
    return (unsigned char *)(uintptr)(KFB_BASE + offset);
}

static unsigned int fb_cell_px(int x)
{
    return fb_offx + (unsigned int)x * 8u * (fb_zoom_x ? fb_zoom_x : 1u);
}

static unsigned int fb_cell_py(int y)
{
    return fb_offy + (unsigned int)y * 16u * (fb_zoom_y ? fb_zoom_y : 1u);
}

static void fb_fill_panel(unsigned int v)
{
    unsigned int y, x;
    if (!fb_base)
        return;
    for (y = 0; y < fb_height; y++)
        for (x = 0; x < fb_width; x++)
            fb_put_pixel(x, y, v);
    __asm__ __volatile__("sfence" ::: "memory");
}

static void fb_blit_grid(void)
{
    DEX32_DDL_INFO *d = ActiveDDL;
    unsigned char *t;
    int x, y;
    if (!d || !d->active || d->bufmode)
        return;
    t = (unsigned char *)d->hdw_ptr;
    for (y = 0; y < 25; y++)
        for (x = 0; x < 80; x++)
            fbconsole_cell_render(x, y, t[(y * 80 + x) * 2],
                                  (unsigned char)t[(y * 80 + x) * 2 + 1]);
    fbconsole_cursor_to(d->curx, d->cury);
}

static void fb_late_present(void)
{
    fbconsole_grid_origin_xy(fb_width, fb_height, fb_zoom_x, fb_zoom_y,
                             &fb_offx, &fb_offy);
    fb_fill_panel(fb_color(0x00u));
    fb_blit_grid();
    __asm__ __volatile__("sfence; wbinvd" ::: "memory");
}

int fbconsole_late_init(void)
{
    unsigned long long bytes;
    int reason;
    unsigned long flags;
    unsigned char *base;

    bytes = (unsigned long long)fb_pitch * fb_height;
    reason = fbconsole_late_map_reason(fb_have_tag ? 1 : 0, fb_ready ? 1 : 0,
                                       fb_phys, bytes);
    if (reason != FBCONSOLE_LATE_OK && reason != FBCONSOLE_LATE_HIGH)
        return reason;
    if (reason == FBCONSOLE_LATE_HIGH && bytes > (unsigned long long)KFB_SIZE)
        return FBCONSOLE_LATE_BAD;

    /* Num while mapping: if this hangs, Caps-only was the old HIGH skip. */
    kbd_boot_leds_raw(KBD_LED_NUM);

    __asm__ __volatile__("pushfq; pop %0; cli" : "=r"(flags));
    fb_pat_enable_wc();
    if (reason == FBCONSOLE_LATE_HIGH)
        base = fb_map_high_wc(fb_phys, bytes);
    else {
        fb_mark_identity_wc(fb_phys, bytes);
        base = (unsigned char *)(uintptr)fb_phys;
    }
    if (!base) {
        __asm__ __volatile__("push %0; popfq" :: "r"(flags) : "memory", "cc");
        return FBCONSOLE_LATE_BAD;
    }
    fb_enabled = 1;
    fb_base = base;
    fb_ready = 1;
    fb_live_render = 1;
    fb_late_present();
    __asm__ __volatile__("push %0; popfq" :: "r"(flags) : "memory", "cc");
    printf("FB %ux%u pitch=%u zoom=%ux%u\n", fb_width, fb_height, fb_pitch,
           fb_zoom_x, fb_zoom_y);
    return FBCONSOLE_LATE_OK;
}

int fbconsole_active(void)
{
    return fb_enabled && fb_ready;
}

int fbconsole_have_tag(void)
{
    return fb_have_tag ? 1 : 0;
}

int fbconsole_geom(unsigned int *width, unsigned int *height,
                   unsigned int *bpp)
{
    if (!fbconsole_active() || !fb_base)
        return 0;
    if (width)
        *width = fb_width;
    if (height)
        *height = fb_height;
    if (bpp)
        *bpp = fb_bpp;
    return 1;
}

int fbconsole_rgb_at(unsigned int x, unsigned int y,
                     unsigned char *r, unsigned char *g, unsigned char *b)
{
    unsigned int v, rv, gv, bv, rmax, gmax, bmax;
    if (!fbconsole_active() || !fb_base || x >= fb_width || y >= fb_height)
        return 0;
    v = fb_get_pixel(x, y);
    rmax = fb_rsize ? ((1u << fb_rsize) - 1u) : 0;
    gmax = fb_gsize ? ((1u << fb_gsize) - 1u) : 0;
    bmax = fb_bsize ? ((1u << fb_bsize) - 1u) : 0;
    rv = fb_rsize ? ((v >> fb_rshift) & rmax) : 0;
    gv = fb_gsize ? ((v >> fb_gshift) & gmax) : 0;
    bv = fb_bsize ? ((v >> fb_bshift) & bmax) : 0;
    if (r)
        *r = (unsigned char)(rmax ? (rv * 255u) / rmax : 0);
    if (g)
        *g = (unsigned char)(gmax ? (gv * 255u) / gmax : 0);
    if (b)
        *b = (unsigned char)(bmax ? (bv * 255u) / bmax : 0);
    return 1;
}

void fbconsole_cell_render(int x, int y, unsigned char c, unsigned char attr)
{
    if (!fbconsole_active() || !fb_live_render)
        return;
    if (x < 0 || x >= 80 || y < 0 || y >= 25)
        return;
    fb_draw_glyph(&g_8x16_font[(unsigned int)(unsigned char)c * 16u],
                  fb_cell_px(x), fb_cell_py(y),
                  fb_color(attr & 0x0Fu), fb_color((attr >> 4) & 0x0Fu));
}

/* Read a cell from the active DDL shadow buffer. */
static int fb_shadow_cell(int x, int y, unsigned char *c, unsigned char *attr)
{
    DEX32_DDL_INFO *d = ActiveDDL;
    unsigned char *t;
    int i;
    if (!d || !d->active || d->bufmode)
        return 0;
    t = (unsigned char *)d->hdw_ptr;
    i = (y * 80 + x) * 2;
    *c = t[i];
    *attr = (unsigned char)t[i + 1];
    return 1;
}

static void fb_reblit_cell(int x, int y)
{
    unsigned char c, attr;
    if (fb_shadow_cell(x, y, &c, &attr))
        fbconsole_cell_render(x, y, c, attr);
}

void fbconsole_screen_refresh(void)
{
    DEX32_DDL_INFO *d;
    unsigned char *t;
    int x, y;
    if (!fbconsole_active() || !fb_live_render)
        return;
    d = ActiveDDL;
    if (!d || !d->active || d->bufmode)
        return;
    t = (unsigned char *)d->hdw_ptr;
    /* QEMU COM1: a full 80x25 blit at 3x is a flood of MMIO VM-exits.
       The laptop WC path blits the whole grid. */
    if ((fb_zoom_x > 1 || fb_zoom_y > 1) && serial_com1_present()) {
        y = d->cury;
        if (y < 0)
            y = 0;
        if (y > 24)
            y = 24;
        for (x = 0; x < 80; x++)
            fbconsole_cell_render(x, y, t[(y * 80 + x) * 2],
                                  (unsigned char)t[(y * 80 + x) * 2 + 1]);
        fbconsole_cursor_to(d->curx, d->cury);
        return;
    }
    fb_blit_grid();
}

void fbconsole_clear_screen(void)
{
    int x, y;
    if (!fbconsole_active())
        return;
    if ((fb_zoom_x > 1 || fb_zoom_y > 1) && serial_com1_present()) {
        fb_cx = -1;
        fb_cy = -1;
        fbconsole_cursor_to(0, 0);
        return;
    }
    for (y = 0; y < 25; y++)
        for (x = 0; x < 80; x++)
            fbconsole_cell_render(x, y, ' ', 0x07);
    fbconsole_cursor_to(0, 0);
}

void fbconsole_cursor_to(int x, int y)
{
    unsigned int row, col;
    unsigned int white;
    if (!fbconsole_active() || !fb_live_render)
        return;
    if (x < 0) x = 0;
    if (x >= 80) x = 79;
    if (y < 0) y = 0;
    if (y >= 25) y = 24;
    if (fb_cx == x && fb_cy == y)
        return;
    if (fb_cx >= 0)
        fb_reblit_cell(fb_cx, fb_cy);
    fb_cx = x;
    fb_cy = y;
    white = fb_color(0x0Fu);
    for (row = 0; row < 16u * (fb_zoom_y ? fb_zoom_y : 1u); row++)
        for (col = 0; col < 8u * (fb_zoom_x ? fb_zoom_x : 1u); col++)
            fb_put_pixel(fb_cell_px(x) + col, fb_cell_py(y) + row, white);
    fb_flush_glyph(fb_cell_px(x), fb_cell_py(y));
}

void fbconsole_selftest(void)
{
    unsigned int expect, fg, bg, white, row, col;
    const unsigned char *glyph;
    unsigned int fails = 0;
    int i;

    if (!fbconsole_active())
        return;

    /* Enable live blitting for this one-shot validation so the renderer
       actually writes the framebuffer. QEMU keeps it off afterwards (COM1
       is present; live GOP MMIO on the console hot path can deadlock I/O).
       Laptops with no UART keep it on so the panel stays the console. */
    fb_live_render = 1;

    /* Pixel readback is QEMU-only (1x zoom + COM1). Laptop GOP readback
       can bus-hang, and 3x glyphs are not at the unscaled coordinates. */
    if (fb_zoom_x == 1 && fb_zoom_y == 1 && serial_com1_present()) {
    /* 1) absolute pixel check: red-on-black space at cell (0,0). Every
       pixel of the 8x16 cell must equal the packed palette[4] value. */
    expect = fb_color(0x04);
    fbconsole_cell_render(0, 0, ' ', 0x40);
    for (row = 0; row < 16; row++)
        for (col = 0; col < 8; col++)
            if (fb_get_pixel(col, row) != expect)
                fails++;

    /* 2) glyph check: 'M' attr 0x1F at cell (1,0); every pixel must match
       the font bit expanded through the palette. */
    glyph = &g_8x16_font[0x4Du * 16u];
    fg = fb_color(0x0Fu);
    bg = fb_color(0x01u);
    fbconsole_cell_render(1, 0, 'M', 0x1F);
    for (row = 0; row < 16; row++)
        for (col = 0; col < 8; col++) {
        expect = (glyph[row] >> (7 - col)) & 1u ? fg : bg;
        if (fb_get_pixel(8u + col, row) != expect)
            fails++;
    }

    /* 3) cursor check: block cursor covers cell (2,0) in white, and
       moving it away restores the cell from the shadow buffer. */
    white = fb_color(0x0Fu);
    fbconsole_cursor_to(2, 0);
    for (row = 0; row < 16; row++)
        for (col = 0; col < 8; col++)
            if (fb_get_pixel(16u + col, row) != white)
                fails++;
    fbconsole_cursor_to(3, 0);
    for (row = 0; row < 16; row++)
        for (col = 0; col < 8; col++)
            if (fb_get_pixel(16u + col, row) != fb_color(0x00u))
                fails++;

    /* Restore the cells the test overwrote. */
    for (i = 0; i < 4; i++)
        fbconsole_cell_render(i, 0, ' ', 0x07);
    fbconsole_cursor_to(0, 0);
    } else {
        fbconsole_cell_render(0, 0, 'I', 0x1F);
        fbconsole_cell_render(1, 0, 'C', 0x1F);
        fbconsole_cell_render(2, 0, 'S', 0x1F);
    }

    if (fails == 0)
        serial_puts("FBCONSOLE_PASS\n");
    else {
        char msg[64];
        sprintf(msg, "FBCONSOLE_FAIL %u pixel mismatches\n", (unsigned)fails);
        serial_puts(msg);
    }

    /* Do not blit the whole 80x25 grid at 3x here: that is ~2 million
       GOP stores and rebooted the N150. Later putc() paints one glyph. */
    fb_live_render = serial_com1_present() ? 0 : 1;
}

void fbconsole_export_tag(unsigned char *buf)
{
    if (!fbconsole_active()) {
        buf[0] = 0;
        buf[1] = 0;
        buf[2] = 8;
        buf[3] = 0;
        return;
    }
    buf[0] = MB2_TAG_FRAMEBUFFER & 0xFFu;
    buf[1] = (MB2_TAG_FRAMEBUFFER >> 8) & 0xFFu;
    buf[2] = 40;
    buf[3] = 0;
    buf[4] = (unsigned char)(fb_phys & 0xFFu);
    buf[5] = (unsigned char)((fb_phys >> 8) & 0xFFu);
    buf[6] = (unsigned char)((fb_phys >> 16) & 0xFFu);
    buf[7] = (unsigned char)((fb_phys >> 24) & 0xFFu);
    buf[8] = (unsigned char)((fb_phys >> 32) & 0xFFu);
    buf[9] = (unsigned char)((fb_phys >> 40) & 0xFFu);
    buf[10] = (unsigned char)((fb_phys >> 48) & 0xFFu);
    buf[11] = (unsigned char)((fb_phys >> 56) & 0xFFu);
    buf[12] = (unsigned char)(fb_pitch & 0xFFu);
    buf[13] = (unsigned char)((fb_pitch >> 8) & 0xFFu);
    buf[14] = (unsigned char)((fb_pitch >> 16) & 0xFFu);
    buf[15] = (unsigned char)((fb_pitch >> 24) & 0xFFu);
    buf[16] = (unsigned char)(fb_width & 0xFFu);
    buf[17] = (unsigned char)((fb_width >> 8) & 0xFFu);
    buf[18] = (unsigned char)((fb_width >> 16) & 0xFFu);
    buf[19] = (unsigned char)((fb_width >> 24) & 0xFFu);
    buf[20] = (unsigned char)(fb_height & 0xFFu);
    buf[21] = (unsigned char)((fb_height >> 8) & 0xFFu);
    buf[22] = (unsigned char)((fb_height >> 16) & 0xFFu);
    buf[23] = (unsigned char)((fb_height >> 24) & 0xFFu);
    buf[24] = (unsigned char)fb_bpp;
    buf[25] = (unsigned char)fb_ftype;
    buf[26] = 0;
    buf[27] = 0;
    buf[28] = (unsigned char)fb_rshift;
    buf[29] = (unsigned char)fb_rsize;
    buf[30] = (unsigned char)fb_gshift;
    buf[31] = (unsigned char)fb_gsize;
    buf[32] = (unsigned char)fb_bshift;
    buf[33] = (unsigned char)fb_bsize;
    buf[34] = 0;
    buf[35] = 0;
    buf[36] = 0;
    buf[37] = 0;
}

/* ==========================================================================
   Low-level direct-framebuffer diagnostics (early-boot crash localization).

   These paint straight into the linear framebuffer using the validated fb
   state (fb_base/fb_pitch/fb_bpp), bypassing the DDL/console entirely. They
   are safe to call from kernel fault handlers: read-only 8x16 font, no
   allocation, and the wrapper has already disabled interrupts. Every entry
   no-ops when the framebuffer is not ready (fb_base == 0), so the legacy
   0xB8000 text path is left completely untouched.
   ========================================================================== */

static void fbdbg_cell(int cx, int cy, char c, unsigned char attr)
{
    if (!fb_base)
        return;
    if (cx < 0 || cx >= 80 || cy < 0 || cy >= 25)
        return;
    fb_draw_glyph(&g_8x16_font[(unsigned int)(unsigned char)c * 16u],
                  fb_cell_px(cx), fb_cell_py(cy),
                  fb_color(attr & 0x0Fu), fb_color((attr >> 4) & 0x0Fu));
}

static int fbdbg_str(int cx, int cy, const char *s, unsigned char attr)
{
    while (*s && cx < 80) {
        if (*s == '\n') { if (cy < 24) cy++; cx = 0; }
        else { fbdbg_cell(cx, cy, *s, attr); cx++; }
        s++;
    }
    return cx;
}

static void fbdbg_row(int cy, unsigned char attr)
{
    int x;
    if (!fb_base || cy < 0 || cy >= 25)
        return;
    for (x = 0; x < 80; x++)
        fbdbg_cell(x, cy, ' ', attr);
}

/* Record a boot stage on the bottom row (0x5F = white on magenta). Overwritten
   each call, so the last visible badge is the stage where boot stopped. Bottom
   placement keeps it clear of the top-down console text. */
void fbdbg_stage(int n, const char *name)
{
    char line[72];
    /* Keyboard LEDs work even when GOP is unmapped. */
    if (n < 0)
        n = 0;
    kbd_boot_leds((unsigned int)n);
    if (!fb_base)
        return;
    fbdbg_row(24, 0x5F);
    sprintf(line, " STAGE %02d: %s", n, name);
    fbdbg_str(0, 24, line, 0x5F);
}

/* One-shot info line on row 0 (0x1E = yellow on blue). Used right after the
   multiboot2 framebuffer tag is parsed so the fb console state is visible on
   the panel before the console init clears the screen. */
void fbdbg_info(const char *s)
{
    if (!fb_base)
        return;
    fbdbg_row(0, 0x1E);
    fbdbg_str(0, 0, s, 0x1E);
}

/* Full-panel fault banner (0x4F = white on red), rows 0-6. Painted by the
   kernel fault handlers so the crash is visible on the panel (the N150 has no
   serial). Shows the vector, name, faulting RIP and CR2; the RIP resolves
   against Kernel64.sym. */
void fbdbg_fault(int vec, const char *name,
                 unsigned long long rip, unsigned long long cr2)
{
    static volatile int shown = 0;
    char line[64];
    int x, y;
    kbd_boot_leds_raw(0);
    /* Full-panel GOP fill from a fault nested #PF / rebooted the N150.
       Keep LEDs (both off) as the laptop oracle. */
    if (!serial_com1_present() || !fb_base || shown)
        return;
    shown = 1;
    for (y = 0; y < 7; y++)
        for (x = 0; x < 80; x++)
            fbdbg_cell(x, y, ' ', 0x40);
    fbdbg_str(0, 0, "### KERNEL FAULT ###", 0x4F);
    sprintf(line, "vector %d  (%s)", vec, name);
    fbdbg_str(0, 1, line, 0x4F);
    sprintf(line, "rip = 0x%llx", rip);
    fbdbg_str(0, 2, line, 0x4F);
    sprintf(line, "cr2 = 0x%llx", cr2);
    fbdbg_str(0, 3, line, 0x4F);
    fbdbg_str(0, 5, "system halted", 0x4F);
}
