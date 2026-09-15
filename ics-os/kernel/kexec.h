#ifndef _KEXEC_H
#define _KEXEC_H

/* Load an ELF64 kernel from `path` into a high staging area.
   Returns 0 on success. */
int kexec_load(const char *path);
int kexec_load_mem(const void *img, unsigned int sz);

/* Copy the staged kernel over 0x100000, drop to 32-bit protected mode,
   and jump to its Multiboot2 entry. Does not return. */
void kexec_reboot(void);

#endif
