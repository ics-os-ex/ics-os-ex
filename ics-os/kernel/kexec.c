/*
 * kexec: load a freshly compiled ELF64 kernel and jump to it.
 * Staging lives at MEM_KEXEC_STAGE so it does not overlap the running
 * image or the userpd pool. The trampoline at MEM_KEXEC_TRAMP survives
 * the copy over MEM_KERNEL_LOAD.
 */
#include "kexec.h"
#include "memory/memlayout.h"

#define KEXEC_STAGE   MEM_KEXEC_STAGE
#define KEXEC_TRAMP   MEM_KEXEC_TRAMP
#define KEXEC_MB2     MEM_KEXEC_MB2
#define KEXEC_MAX     MEM_KEXEC_STAGE_SIZE

extern void kexec_tramp(void);
extern char kexec_tramp_end[];

static unsigned long kexec_entry;
static unsigned long kexec_span;
static int kexec_ready;

int kexec_load(const char *path)
{
   DWORD sz = 0;
   char *img;
   unsigned char *eh;
   unsigned long phoff, phentsize, phnum, i;
   unsigned long min_va = ~0UL, max_va = 0;

   kexec_ready = 0;
   kexec_span = 0;
   kexec_entry = 0;

   img = vfs_mapfile(path, &sz);
   if (!img || sz < 64) {
      printf("kexec: cannot map %s\n", path);
      return -1;
   }
   eh = (unsigned char *)img;
   if (eh[0] != 0x7f || eh[1] != 'E' || eh[2] != 'L' || eh[3] != 'F' || eh[4] != 2) {
      printf("kexec: not ELF64\n");
      free(img);
      return -1;
   }
   kexec_entry = *(unsigned long *)(img + 24); /* e_entry */
   phoff = *(unsigned long *)(img + 32);
   phentsize = *(unsigned short *)(img + 54);
   phnum = *(unsigned short *)(img + 56);
   if (!phoff || !phnum || phentsize < 56) {
      printf("kexec: bad program headers\n");
      free(img);
      return -1;
   }

   memset((void *)(uintptr)KEXEC_STAGE, 0, KEXEC_MAX);

   for (i = 0; i < phnum; i++) {
      char *ph = img + phoff + i * phentsize;
      unsigned int type = *(unsigned int *)ph;
      unsigned long vaddr, filesz, memsz, offset;
      unsigned long dst;

      if (type != 1) /* PT_LOAD */
         continue;
      offset = *(unsigned long *)(ph + 8);
      vaddr  = *(unsigned long *)(ph + 16);
      filesz = *(unsigned long *)(ph + 32);
      memsz  = *(unsigned long *)(ph + 40);
      if (vaddr < MEM_KERNEL_LOAD) {
         printf("kexec: PT_LOAD vaddr 0x%lx below kernel load\n", vaddr);
         free(img);
         return -1;
      }
      dst = KEXEC_STAGE + (vaddr - MEM_KERNEL_LOAD);
      if ((vaddr - MEM_KERNEL_LOAD) + memsz > KEXEC_MAX) {
         printf("kexec: image too large (%lu)\n", (unsigned long)memsz);
         free(img);
         return -1;
      }
      if (filesz)
         memcpy((void *)(uintptr)dst, img + offset, (unsigned)filesz);
      if (memsz > filesz)
         memset((void *)(uintptr)(dst + filesz), 0, (unsigned)(memsz - filesz));
      if (vaddr < min_va)
         min_va = vaddr;
      if (vaddr + memsz > max_va)
         max_va = vaddr + memsz;
   }
   free(img);

   if (max_va <= min_va) {
      printf("kexec: no PT_LOAD segments\n");
      return -1;
   }
   kexec_span = max_va - MEM_KERNEL_LOAD;
   if (kexec_span < 4096)
      kexec_span = 4096;
   kexec_ready = 1;
   printf("kexec: staged %lu bytes entry=0x%lx\n",
          kexec_span, kexec_entry);
   return 0;
}

 void kexec_reboot(void)
{
    unsigned int tramp_len;
    unsigned int *mb2;
    unsigned char fbtag[40];
    void (*tramp)(void *, unsigned long, unsigned long, unsigned long);

   if (!kexec_ready || !kexec_entry) {
      printf("kexec: nothing loaded\n");
      return;
   }

   tramp_len = (unsigned int)((uintptr)kexec_tramp_end - (uintptr)kexec_tramp);
   if (tramp_len == 0 || tramp_len > 0x1000) {
      printf("kexec: bad trampoline\n");
      return;
   }
   memcpy((void *)(uintptr)KEXEC_TRAMP, (void *)kexec_tramp, tramp_len);

  /* Minimal Multiboot2 info with a cmdline tag so the new kernel can
      skip autoexec and prove it is the kexec'd image. When the current
      boot used a linear framebuffer, forward the tag so the kexec'd
      kernel keeps the on-screen console. */
    mb2 = (unsigned int *)(uintptr)KEXEC_MB2;
    memset(mb2, 0, 128);
    mb2[2] = 1;                       /* cmdline tag */
    mb2[3] = 8 + 8;                   /* size including "kexeced\0" */
    memcpy((char *)mb2 + 16, "kexeced", 8);
    if (fbconsole_active()) {
       fbconsole_export_tag(fbtag);
       memcpy((char *)mb2 + 24, fbtag, sizeof(fbtag));
       mb2[16] = 0;                   /* end tag at offset 64 */
       mb2[17] = 8;
       mb2[0] = 72;                   /* header + cmdline + fb + end */
    } else {
       mb2[6] = 0;                    /* end tag at offset 24 */
       mb2[7] = 8;
       mb2[0] = 32;                   /* header + cmdline + end */
    }

   printf("kexec: jumping to 0x%lx (KBUILD_KEXEC)\n", kexec_entry);
   serial_puts("KBUILD_KEXEC\n");
   stopints();
   tramp = (void (*)(void *, unsigned long, unsigned long, unsigned long))
           (uintptr)KEXEC_TRAMP;
   tramp((void *)(uintptr)KEXEC_STAGE, kexec_span, kexec_entry, KEXEC_MB2);
   while (1) {}
}
