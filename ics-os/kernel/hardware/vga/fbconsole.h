/*
  Name: fbconsole
  Description:
  ==========================================================================
  Linear-framebuffer text console for the 80x25 DDL grid.

  When the bootloader supplies a multiboot2 framebuffer tag (UEFI GOP or
  BIOS VBE), the DDL "hardware" buffer becomes a per-device 80x25 text
  shadow and this module renders it into the linear framebuffer. Without a
  framebuffer tag the kernel keeps the legacy 0xB8000 text-mode path and
  every entry point here is a no-op.
  ==========================================================================
*/
#ifndef FBCONSOLE_H
#define FBCONSOLE_H

/* Record the multiboot2 framebuffer tag. Returns 1 when the console is
   usable now, 0 when it must be retried after paging is fully up
   (framebuffer above 4GiB) or is unusable. */
int fbconsole_boot_init(unsigned long long addr, unsigned int pitch,
                        unsigned int width, unsigned int height,
                        unsigned int bpp, unsigned int ftype,
                        unsigned int rshift, unsigned int rsize,
                        unsigned int gshift, unsigned int gsize,
                        unsigned int bshift, unsigned int bsize);

/* Complete deferred setup for a framebuffer above 4GiB. Call after
   mem_init(); before the first DDL is created. */
void fbconsole_deferred_init(void);

int fbconsole_active(void);

/* Render one 8x16 cell (VGA attribute: low nibble fg, high nibble bg). */
void fbconsole_cell_render(int x, int y, unsigned char c, unsigned char attr);

/* Re-render the whole 80x25 grid from the active DDL shadow buffer. */
void fbconsole_screen_refresh(void);

/* Fill the screen with spaces (attr 0x07) and reset the cursor. */
void fbconsole_clear_screen(void);

/* Move the on-screen block cursor; restores the cell it leaves. */
void fbconsole_cursor_to(int x, int y);

/* Guest selftest: absolute pixel, glyph, and cursor checks. Prints
   FBCONSOLE_PASS / FBCONSOLE_FAIL on the serial console. */
void fbconsole_selftest(void);

/* Fill buf (>= 40 bytes) with the framebuffer info tag so a kexeced
    kernel can reuse the same framebuffer. */
 void fbconsole_export_tag(unsigned char *buf);

 /* ---- Direct-framebuffer crash diagnostics (early-boot localization) ----
    Paint straight to the linear framebuffer, bypassing the DDL/console. Safe
    to call from kernel fault handlers; no-op when the framebuffer is not
    ready (legacy VGA path unaffected). */

 /* Record a boot stage on the bottom row; the last badge is where boot stopped. */
 void fbdbg_stage(int n, const char *name);

 /* One-shot info line on row 0 (fb console state, right after tag parse). */
 void fbdbg_info(const char *s);

 /* Full-panel red fault banner: vector, name, faulting RIP and CR2. */
 void fbdbg_fault(int vec, const char *name,
                  unsigned long long rip, unsigned long long cr2);

 #endif
