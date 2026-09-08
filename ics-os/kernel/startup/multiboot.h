/*
  Name: multiboot
  Copyright: 
  Author: Joseph Emmanuel DL Dayo
  Date: 21/01/04 20:07
  Description: This defines the functions necessary for obtaining information
               from the multiboot header. Multiboot is a specification for bootloaders
               so that it could be provided with information on how to load a particular
               operating system kernel.
*/
#ifndef _MULTIBOOT_H_
#define _MULTIBOOT_H_


#define MB_FLAGS_CMDLINE 4
#define MULTIBOOT1_MAGIC 0x2BADB002
#define MULTIBOOT2_MAGIC 0x36D76289

typedef struct _multiboot_header {
    DWORD flags;
    DWORD mem_lower, mem_upper;
    DWORD boot_device;
    DWORD cmdline;
    DWORD mods_count;
    DWORD mods_addr;
    DWORD syms[4];
    DWORD mmap_length;
    DWORD mmap_addr;
    DWORD drives_length;
    DWORD drives_addr;
    DWORD config_table;
    DWORD boot_loader_name;
    DWORD apm_table;
    DWORD vbe_control_info;
    DWORD vbe_mode_info;
    DWORD vbe_mode;
    DWORD vbe_interface_seg;
    DWORD vbe_interface_off;
    DWORD vbe_interface_len;
} multiboot_header;

typedef struct _mmap {
DWORD   size;
DWORD   base_addr_low;     
DWORD   base_addr_high;
DWORD   length_low;
DWORD   length_high;
DWORD   type;
} mmap;

/* Multiboot2 tags (simplified) */
typedef struct __attribute__((packed)) {
    DWORD total_size;
    DWORD reserved;
} mb2_info;

typedef struct __attribute__((packed)) {
    DWORD type;
    DWORD size;
} mb2_tag;

typedef struct __attribute__((packed)) {
    DWORD type;
    DWORD size;
    DWORD entry_size;
    DWORD entry_version;
} mb2_mmap_tag;

/* Multiboot2 info tag types used by the kernel. */
#define MB2_TAG_FRAMEBUFFER 8

/* Multiboot2 framebuffer info tag (tag type 8). */
#define MB2_FB_TYPE_INDEXED 0
#define MB2_FB_TYPE_RGB     1
#define MB2_FB_TYPE_EGA     2

typedef struct __attribute__((packed)) {
    DWORD type;
    DWORD size;
    unsigned long long addr;
    DWORD pitch;
    DWORD width;
    DWORD height;
    BYTE bpp;
    BYTE ftype;
    WORD reserved;
    BYTE r_shift, r_size, g_shift, g_size, b_shift, b_size;
} mb2_fb_tag;

DWORD map_length = 0;
mmap *memory_map;
extern unsigned int multiboot_magic;
extern unsigned long multiboothdr;

/* Filled from Multiboot2 tag type 1. "kexeced" means we jumped here via kexec. */
extern char kernel_cmdline[256];
extern int kernel_kexeced;

#endif
