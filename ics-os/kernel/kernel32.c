/*

  Name: DEX-OS 1.0 Beta Kernel Main file
  Copyright: 
  Author: Joseph Emmanuel Dayo
  Date: 13/03/04 06:20
  Description: This is the kernel main file that gets called after startup.asm.
  
   
    DEX educational extensible operating system 1.0 Beta
    Copyright (C) 2004  Joseph Emmanuel DL Dayo

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. 
*/

#define NULL 0
#define DEBUGX
#define USE_CONSOLEDDL


/*some defines that are used for debugging purposes*/

//#define DEBUG_FLUSHMGR
//#define DEBUG_COFF
//#define DDL_DEBUG
//#define DEBUG_FORK
#define FULLSCREENERROR
//#define DEBUG_KSBRK
//#define DEBUG_FAT12
#define DEBUG_STARTUP
#define DEBUG_EXTENSION
//#define DEBUG_USER_PROCESS
//#define MODULE_DEBUG
//#define DEBUG_PEMODULE
//#define DEBUG_VFSREAD
//#define DEBUG_MEMORY
//#define USE_DIRECTFLOPPY
//#define DEBUG_VFS
//#define DEBUG_IOREADWRITE
//#define DEBUG_IOREADWRITE2
//#define WRITE_DEBUG2
//#define WRITE_DEBUG
//#define DEBUG_READ
//#define DEBUG_BRIDGE



//timer set to switch to new task (see time.h or time.c)
int context_switch_rate=100; 

//pointer to vga mem
char *scr_debug = (char*)0xb8000;

int op_success;

//points to the location of the multiboot header defined in startup.asm
extern unsigned long multiboothdr;
extern unsigned int multiboot_magic; 

//defined in asmlib.asm
extern void textcolor(unsigned char c);


//order is important for some include files, DO NOT CHANGE!
#include "stdarg.h"
#include "limits.h"

#include "build.h"
#include "version.h"
#include "dextypes.h"
#include "process/sync.h"
#include "stdlib/time.h"
#include "stdlib/dexstdlib.h"
#include "startup/multiboot.h"
#include "memory/dexmem.h"
#include "console/dex_DDL.h"
#include "vfs/vfs_core.h"
#include "vfs/posixfd.h"
#include "process/process.h"
#include "process/pdispatch.h"
#include "devmgr/dex32_devmgr.h"
#include "devmgr/devmgr_error.h"
#include "partition/gpt.h"
#include "partition/partdev.h"
#include "console/dexio.h"
#include "hardware/keyboard/keyboard.h"
#include "hardware/keyboard/kbd_boot_leds.h"
#include "hardware/keyboard/mouse.h"
#include "hardware/hardware.h"
#include "hardware/chips/serial.h"
#include "memory/kheap.h"
#include "build_info.h"
#include "hardware/chips/ports.c"
#include "hardware/chips/serial.c"
#include "hardware/vga/dexvga.c"
#include "hardware/vga/fbconsole.h"
#include "stdlib/qsort.c"
#include "hardware/floppy/floppy.h"
#include "hardware/ATA/ataio.h"
#include "hardware/exceptions.h"
#include "hardware/chips/speaker.h"
#include "dexapi/dex32API.h"
#include "filesystem/fat12.h"
#include "filesystem/iso9660.h"
#include "filesystem/devfs.h"
#include "filesystem/ramdisk.h"
#include "filesystem/ext4.h"
#include "process/event.h"
#include "devmgr/extensions.h"
#include "process/environment.h"
#include "console/foreground.h"
#include "console/console.h"
#include "console/tty.h"
#include "iomgr/blkcache.h"
#include "stdlib/stdlib.h"
#include "devmgr/bridges.h"
#include "process/scheduler.h"
#include "console/script.h"
#include "vfs/vfs_aux.h"
#include "hardware/usb/usb.h"
#include "iomgr/iosched.h"
#include "hardware/virtio/virtio_blk.h"
#include "hardware/virtio/virtio_net.h"
#include "net/sock.h"
#include "kexec.h"

//structure to hold the boot info
typedef struct _kernel_sysinfo {
   int boot_device;
	int part[3];
} kernel_sysinfo;

kernel_sysinfo kernel_systeminfo;

//This stores the current virtual console the kernel will use
DEX32_DDL_INFO *consoleDDL;

char kernel_cmdline[256] = {0};
int kernel_kexeced = 0;

//forward declarations.needed in process.c so must be here first 
void dex_init();
extern void smp_enable_scheduling(void);
extern int cpu_count;

/*I know there are some disadvantages to directly including files
  in the source code instead of using object files, but it simplifies
  compilation without the use of a makefile*/

#include "console/dex_DDL.c"
#include "hardware/vga/fbconsole.c"
#include "console/tty.c"
#include "console/tty_vt.c"
#include "console/tty_tc.c"
#include "hardware/dexapm.c"
#include "hardware/chips/irqhandlers.c"
#ifdef __x86_64__
#define INTERNAL_SIZE_T unsigned long
#endif
#include "memory/dlmalloc.c"
#include "memory/bsdmallo.c"
#include "stdlib/time.c"
#include "hardware/floppy/floppy.c"
#include "vfs/vfs_core.c"
#include "module/module.c"
#include "process/pdispatch.c"
#include "console/console.c"
#include "console/dexio.c"
#include "console/klog.c"
#include "stdlib/stdlib.c"
#include "process/dex_taskmgr.c"
#include "hardware/keyboard/keyboard.c"
#include "hardware/keyboard/mouse.c"
//testing network
#include "hardware/pcibus/dexpci2.c"
//#include "hardware/pcibus/i386-ports.c"
//#include "hardware/pcibus/access.c"
//#include "hardware/pcibus/generic.c"
//#include "hardware/pcibus/jachpci.c"
//#include "hardware/rtl8139/pci.c"
//#include "hardware/rtl8139/rtl8139.c"
//-----------------------------------
//#include "hardware/pcibus/i386-ports.c"
//#include "hardware/pcibus/access.c"
//#include "hardware/pcibus/generic.c"
//#include "hardware/pcibus/jachpci.c"
#include "hardware/exceptions.c"
#include "hardware/hardware.c"
#include "hardware/chips/speaker.c"
#include "devmgr/dex32_devmgr.c"
#include "devmgr/extension.c"
#include "process/environment.c"
#include "console/foreground.c"
#include "devmgr/bridges.c"
#include "process/sync.c"
#include "console/script.c"
#include "process/process.c"
#include "dexapi/dex32API.c"
#include "hardware/ATA/ide.c"
#include "hardware/usb/uhci.c"
#include "partition/crc32.c"
#include "partition/gpt.c"
#include "partition/partdev.c"
#include "vfs/vfs_aux.c"
#include "memory/kheap.c"
#include "memory/dexmem.c"
#include "memory/dexmalloc.c"
#include "vmm/vmm.c"
#include "kexec.c"

//another set of forward declarations
void dex32_startup(); 
extern startup();

fg_processinfo *fg_kernel = 0;

//holds the name of the device that booted the kernel
char boot_device_name[255]="";

/*the start of the main kernel-- The task here is to setup the memory
  so that we could use it, we also enable some devices like the keyboard 
  and the floppy disk etc.
  
  Assumptions:
  DEX assumes that at this point the following should be true:
  
  * Protected Mode is enabled
  * paging is disabled
  * interrupts are disabled
  * The CS,DS,SS,ESP must already be set up, meaning that the GDT should already be present, see startup.asm
  
  ORDER is important when starting up the kernel modules!!*/

multiboot_header *mbhdr = 0;
static BYTE acpi_rsdp[36];
static DWORD acpi_rsdp_length;


//here we go!
void main(){
   char temp[255];
   static multiboot_header mb1_compat;
    
   /* Multiboot1 or Multiboot2 info from GRUB */
   mbhdr = 0;
   memory_map = 0;
   map_length = 0;
   acpi_rsdp_length = 0;

   /* Caps Lock on: reached kernel C. Must run before serial_init so a
      missing UART cannot hide the first breadcrumb on a laptop. */
   kbd_boot_leds(0);
   serial_init();
   serial_puts("ICS-OS: serial console ready (x86_64)\n");
   kernel_kexeced = 0;
   kernel_cmdline[0] = 0;

   /* Prefer values stashed at 0x9000 by the 32-bit trampoline. */
   {
      unsigned int magic32 = *(volatile unsigned int *)0x9000;
      unsigned int info32 = *(volatile unsigned int *)0x9004;
      if (magic32 == MULTIBOOT2_MAGIC || magic32 == MULTIBOOT1_MAGIC) {
         multiboot_magic = magic32;
         multiboothdr = info32;
      }
   }

   if (multiboot_magic == MULTIBOOT2_MAGIC) {
       mb2_info *info = (mb2_info *)(uintptr)multiboothdr;
       mb2_tag *tag = (mb2_tag *)((char *)info + 8);
       static mmap mb2map[32];
       int mb2map_n = 0;
       memset(&mb1_compat, 0, sizeof(mb1_compat));
       mb1_compat.boot_device = 0xE0000000; /* default CD for Multiboot2/ISO */
       while (tag->type != 0) {
          if (tag->type == 5) { /* BIOS boot device */
             DWORD *p = (DWORD *)((char *)tag + 8);
             /* Multiboot2: p[0]=biosdev, p[1]=partition, p[2]=sub_partition.
                Pack like Multiboot1: drive in top byte. */
             mb1_compat.boot_device = (p[0] << 24)
                                   | ((p[1] & 0xFF) << 16)
                                   | ((p[2] & 0xFF) << 8);
          }
          if (tag->type == 6) { /* E820 memory map (multiboot2 mmap tag) */
             const mb2_mmap_tag *mm = (const mb2_mmap_tag *)tag;
             unsigned int es = mm->entry_size ? mm->entry_size : 24;
             unsigned int ne = (tag->size >= 16) ? (tag->size - 16) / es : 0;
             unsigned int j;
             const unsigned char *ep = (const unsigned char *)tag + 16;
             for (j = 0; j < ne && j < 32; j++) {
                const unsigned char *e = ep + j * es;
                unsigned long long base, len;
                unsigned int etype;
                int k;
                base = 0;
                for (k = 0; k < 8; k++)
                   base |= (unsigned long long)(unsigned int)e[k] << (8 * k);
                len = 0;
                for (k = 0; k < 8; k++)
                   len |= (unsigned long long)(unsigned int)e[8 + k] << (8 * k);
                etype = (unsigned int)e[16] | ((unsigned int)e[17] << 8)
                      | ((unsigned int)e[18] << 16) | ((unsigned int)e[19] << 24);
                if (etype != 1)
                   continue; /* keep RAM only */
                if (mb2map_n < 32) {
                   mb2map[mb2map_n].size = sizeof(mmap) - 4;
                   mb2map[mb2map_n].base_addr_low = (DWORD)(base & 0xFFFFFFFFu);
                   mb2map[mb2map_n].base_addr_high = (DWORD)(base >> 32);
                   mb2map[mb2map_n].length_low = (DWORD)(len & 0xFFFFFFFFu);
                   mb2map[mb2map_n].length_high = (DWORD)(len >> 32);
                   mb2map[mb2map_n].type = 1;
                   mb2map_n++;
                }
             }
           }
           if (tag->type == MB2_TAG_FRAMEBUFFER && tag->size >= 38) {
               const mb2_fb_tag *fb = (const mb2_fb_tag *)tag;
               fbconsole_boot_init(fb->addr, fb->pitch, fb->width, fb->height,
                                   fb->bpp, fb->ftype,
                                   fb->r_shift, fb->r_size,
                                   fb->g_shift, fb->g_size,
                                   fb->b_shift, fb->b_size);
            }
           if ((tag->type == 15 || (tag->type == 14 && !acpi_rsdp_length)) &&
               tag->size > 8) {
             DWORD length = tag->size - 8;
             if (length > sizeof(acpi_rsdp))
                length = sizeof(acpi_rsdp);
             memcpy(acpi_rsdp, (char *)tag + 8, length);
             acpi_rsdp_length = length;
          }
          if (tag->type == 1) { /* command line */
            unsigned n = tag->size > 8 ? tag->size - 8 : 0;
            /* A real GRUB/kexec cmdline is short. Huge sizes mean we landed
               on a misaligned tag and would memcpy kernel .rodata (which
               contains the string "kexeced") into the buffer. */
            if (tag->size < 8 || tag->size > 8 + 255)
               n = 0;
            if (n > 255) n = 255;
            memcpy(kernel_cmdline, (char *)tag + 8, n);
            kernel_cmdline[n] = 0;
            /* Exact token, not substring — a random blob can contain
               the letters "kexeced". */
            if (n && (strcmp(kernel_cmdline, "kexeced") == 0
                      || strncmp(kernel_cmdline, "kexeced ", 8) == 0))
               kernel_kexeced = 1;
         }
         tag = (mb2_tag *)(((uintptr)tag + tag->size + 7) & ~7ULL);
       }
       mbhdr = &mb1_compat;
       if (mb2map_n > 0) {
          memory_map = mb2map;
          map_length = mb2map_n * sizeof(mmap);
       } else {
          memory_map = 0;
          map_length = 0;
       }
       if (kernel_cmdline[0]) {
         serial_puts("ICS-OS: cmdline=");
         serial_puts(kernel_cmdline);
         serial_puts("\n");
      }
      if (kernel_kexeced)
         serial_puts("ICS-OS: booted via kexec\n");
   } else {
      mbhdr = (multiboot_header *)(uintptr)multiboothdr;
      if (mbhdr) {
         memory_map = (mmap *)(uintptr)mbhdr->mmap_addr;
         map_length = mbhdr->mmap_length;
      }
   }

   if (!memory_map || map_length == 0) {
      /* Fallback: invent a simple free region so mem_detectmemory works */
      static mmap fallback[1];
      fallback[0].size = sizeof(mmap) - 4;
      fallback[0].base_addr_low = 0x00300000;
      fallback[0].base_addr_high = 0;
      fallback[0].length_low = 0x0D000000;
      fallback[0].length_high = 0;
      fallback[0].type = 1;
      memory_map = fallback;
      map_length = sizeof(fallback);
      serial_puts("ICS-OS: using fallback memory map\n");
   }
    
   /* Enable the keyboard IRQ,Timer IRQ and the Floppy Disk IRQ.As more devices that uses IRQs get supported, we should OR more of them here*/
   //program8259(IRQ_TIMER | IRQ_KEYBOARD | IRQ_FDC | IRQ_MOUSE | IRQ_CASCADE); 
   program8259(IRQ_TIMER | IRQ_KEYBOARD | IRQ_FDC | IRQ_CASCADE); 

   //sets up the default interrupt handlers, like the PF handler,GPF handler
    setdefaulthandlers();
    fbdbg_stage(1, "IDT installed");

    /*and some device handlers like the keyboard handler
      initializes the keyboard*/
    installkeyboard();

    //obtain the device which booted this operating system         
   kernel_systeminfo.boot_device = mbhdr->boot_device >> 24;
   {
      char msg[96];
      sprintf(msg, "ICS-OS: multiboot_magic=0x%x boot_device=0x%x\n",
              multiboot_magic, (unsigned)mbhdr->boot_device);
      serial_puts(msg);
   }

   /* Floppy (BIOS 0) is unreliable under Multiboot2/QEMU ISO; prefer CD.
      Real Multiboot1 floppy boots can still force fd0 via an explicit image.
      Note: under UEFI GRUB also leaves this field unset, so a USB/disk UEFI
      boot reports the "cds0" default here; the root-mount scan still prefers
      the USB mass-storage device, so the actual root is identified correctly. */
   if (kernel_systeminfo.boot_device == 0 || kernel_systeminfo.boot_device >= 0xE0){
      strcpy(boot_device_name,"cds0");
   }else{ //hard disk or USB presented as a BIOS disk
      kernel_systeminfo.part[0] =    (mbhdr->boot_device >> 16) & 0xFF;
      kernel_systeminfo.part[1] =    (mbhdr->boot_device >> 8) & 0xFF;
      kernel_systeminfo.part[2] =    (mbhdr->boot_device & 0xFF);
      int n=kernel_systeminfo.boot_device - 0x80;
      if (n < 0)
         n = 0;
      sprintf(boot_device_name,"hdp%dp%d",n,kernel_systeminfo.part[0]);
   }
   {
      char msg[64];
      sprintf(msg, "ICS-OS: boot_device_name=%s\n", boot_device_name);
      serial_puts(msg);
   }

   //obtain information about the memory configuration
   /* memory_map / map_length already set above */
        
   
   /*
    DEX stores the free physical pages as a stack of free pages, therefore
    when a physical page of memory is needed, DEX just pops it off the stack.
    If DEX recovers used memory, it is pushed to the stack.
    The createstack() function creates the physical pages stack.
    See dexmem.c for details*/
    
   {
      char msg[96];
      sprintf(msg, "ICS-OS: mem detect begin map=%p len=%d\n",
              memory_map, map_length);
      serial_puts(msg);
   }
   memamount = mem_detectmemory(memory_map, map_length);
   current_process = &sPCB;
   {
      extern DWORD totalpages;
      serial_puts("ICS-OS: mem detect done\n");
      printf("Memory: %d KB, free pages=%d\n", memamount/1024, totalpages);
      mem_layout_dump();
   }

    
   /*The mem_init() function first sets up the page table/directories which
     is used by the MMU of the CPU to map vitual memory locations to physical 
     memory locations. Basically the first 3MB of physical memory is mapped
     one-to-one (meaning virtual memory location = physical memory location.
     Finally it assigns the the location of the page directory to the CR3
     register and then enables paging.
      
     NOtE: DEX uses the flat memory model and all segment registers used by
     DEX has a base equal to zero*/
 mem_init();
    /* A framebuffer above 4GiB needs the full paging setup that mem_init()
       completes; below 4GiB boot_init already mapped it uncacheable. */
    fbconsole_deferred_init();
     fbdbg_stage(2, "mem_init + fb deferred");

    /*The default values of the current_process variable, which is the kernel
      PCB (also seeded before the first printf above). */
    current_process = &sPCB;

   //Program the Timer to context switch n times a second	
   dex32_set_timer(context_switch_rate);

   //initialize the bridge manager, see bridges.c for details
   bridges_init();
    
   //initialize the virtual console manager
   fg_init();
   tty_init();
    
   //Create a virtual console that the kernel will send its output to
   consoleDDL = Dex32CreateDDL();
   /* Seed the kernel PCB before the first VGA putc. current_process is a
      per-CPU slot; sPCB.outdev is otherwise 0/garbage and GetProcessDevice
      must not follow a non-canonical pointer. */
   current_process = &sPCB;
   sPCB.outdev = consoleDDL;
   fg_kernel = fg_register(consoleDDL, 0);
   fg_setforeground(fg_kernel);
    fbdbg_stage(3, "console up");

    /* Verify the framebuffer renderer against the live panel before any
       user output; no-op (serial note) when the legacy text path is in use. */
    fbconsole_selftest();
    /* Num only: left stage 3, about to printf. Caps+Num still means we
       never entered startup (CreateDDL/selftest). */
    kbd_boot_leds_raw(KBD_LED_NUM);

    /* Preliminary initializaation complete, start up the operating system*/
   dex32_startup(); 
};

//next stage
void dex32_startup(){
    
   /*At this point, memory accesses should already be safe, and
     until the scheduler starts, the interrupts must be disabled*/

   //Display some output for introductory purposes :)
   //clrscr();

   /*show parameter information sent by the multiboot compliant bootloader.*/
   //printf("Bootloader name : %s\n", mbhdr->boot_loader_name);
    
   //obtain CPU information using the CPUID instruction
   printf("Obtaining CPU information...\n");
    /* Startup walk (overrides fbdbg_stage LEDs so 4 and 5 are not
       both Caps). Last pair is the step that hung. */
    kbd_boot_leds_hold(KBD_LED_CAPS);
  hardware_getcpuinfo(&hardware_mycpu);
    kbd_boot_leds_hold(KBD_LED_NUM);
    if (serial_com1_present())
       hardware_printinfo(&hardware_mycpu);
    else
       printf("CPU %s\n", hardware_mycpu.manufacturer);
    kbd_boot_leds_hold(KBD_LED_CAPS | KBD_LED_NUM);
     fbdbg_stage(4, "CPU info");
    kbd_boot_leds_hold(KBD_LED_CAPS | KBD_LED_NUM);

    printf("Available memory: %d KB\n", memamount/1024);

   //Initialize the extension manager
 printf("Initializing the extension manager...");
    extension_init();
    printf("[OK]\n");
    fbdbg_stage(5, "ext mgr");
    kbd_boot_leds_hold(KBD_LED_CAPS);

   //initialize the device manager
printf("Initializing the device manager...");
    devmgr_init();
    printf("[OK]\n");
    fbdbg_stage(6, "dev mgr");
    kbd_boot_leds_hold(KBD_LED_NUM);

   //register the memory manager
   printf("Registering the memory manager and the memory allocator...");
   mem_register();

   //register the different memory allocators
   bsdmalloc_init();       //BSD malloc
   dlmalloc_init();        //Doug Lea's malloc
   dexmalloc_init();       //Joseph Dayo's (*poor*) first fit malloc function
    
   /* initialize the malloc server, place the device name of the malloc
      function you wish to use as the paramater*/
   alloc_init("dl_malloc");
    printf("[OK]\n");
    fbdbg_stage(7, "alloc");

   {
      extern int vtd_discover(const void *rsdp, unsigned int length);
      printf("Discovering VT-d hardware...\n");
      if (acpi_rsdp_length)
         vtd_discover(acpi_rsdp, acpi_rsdp_length);
      else
          printf("vtd: ACPI RSDP unavailable\n");
       printf("[OK]\n");
    }
    fbdbg_stage(8, "vtd");

    //register the hardware ports manager
printf("Initializing ports...");
    ports_init();
    printf("[OK]\n");
    fbdbg_stage(9, "ports");

   //Initialize the PCI bus driver
   printf("Initializing PCI devices...");
   //show_pci();
   //icsos_pci_init();
   printf("[OK]\n");
printf("Initializing rtl8139 NIC...");
    //rtl8139_init();
    printf("[OK]\n");
    fbdbg_stage(10, "pci/nic");
    kbd_boot_leds_hold(KBD_LED_NUM);
   //delay(400/80);
				
   //initialize the API module
   printf("Initializing kernel API...");		  
api_init();
    printf("[OK]\n");
    fbdbg_stage(11, "api");
    kbd_boot_leds_hold(KBD_LED_CAPS | KBD_LED_NUM);

   //initialize the keyboard device driver
   printf("Initializing keyboard and mouse drivers...");
   irq_init();
    kbd_boot_leds_hold(KBD_LED_CAPS);
   init_keyboard();
    kbd_boot_leds_hold(KBD_LED_NUM);
    if (serial_com1_present())
       installmouse();
    init_mouse();
    printf("[OK]\n");
    fbdbg_stage(12, "kbd/mouse");
    kbd_boot_leds_hold(KBD_LED_CAPS | KBD_LED_NUM);

   /* x86_64: LAPIC probe early; AP bring-up after process manager is ready. */
   {
      extern void lapic_init(void);
      extern void smp_init(void);
      printf("Initializing LAPIC/SMP...");
    kbd_boot_leds_hold(KBD_LED_CAPS);
lapic_init();
    kbd_boot_leds_hold(KBD_LED_NUM);
       smp_init();
       printf("[OK]\n");
    }
    fbdbg_stage(13, "lapic/smp");
    kbd_boot_leds_hold(KBD_LED_CAPS | KBD_LED_NUM);
   
   //Initialize the process manager and the initial
   //processes
   printf("Initializing the process manager...");
process_init();    //defined in process.c
    printf("[OK]\n");
    fbdbg_stage(14, "process mgr");
    kbd_boot_leds_hold(KBD_LED_NUM);

   /* Start APs after process manager exists, but park them until root is mounted.
      (LAPIC timer on APs is deferred until smp_enable_scheduling.) */
   {
      extern void smp_start_aps(void);
      DWORD flags;
      storeflags(&flags);
      stopints();
      printf("Starting application processors...");
       /* A stage-1 self-host kernel must leave APs in reset while BSP kexec
          replaces the shared kernel image. The generated kernel receives the
          normal "kexeced" command line and performs full SMP bring-up.
          The parallel self-host closure is the exception: it boots the APs so
          make/gcc can spread across CPUs, then parks them again before kexec. */
       if (!serial_com1_present())
          printf("skipped (no COM1, BSP only)");
       else if (strcmp(kernel_cmdline, "selfhost-stage1") == 0)
          printf("deferred for self-host kexec");
       else {
          extern volatile int user_procs_smp;
          extern volatile int selfhost_cooperative_ready;
          extern volatile int sched_coop_user_force;
          if (strcmp(kernel_cmdline, "selfhost-stage1-parallel") == 0 ||
              strcmp(kernel_cmdline, "user-smp") == 0 ||
              strcmp(kernel_cmdline, "coop-smp") == 0) {
             user_procs_smp = 1;
             if (strcmp(kernel_cmdline, "user-smp") == 0 ||
                 strcmp(kernel_cmdline, "coop-smp") == 0)
                selfhost_cooperative_ready = 1;
             /* "coop-smp" additionally reproduces the stage-1 closure's
                cooperative user scheduling for QA without kexec. */
             if (strcmp(kernel_cmdline, "coop-smp") == 0)
                sched_coop_user_force = 1;
          }
          smp_start_aps();
       }
       printf("[OK]\n");
        restoreflags(flags);
    }
    fbdbg_stage(15, "APs started");
    kbd_boot_leds_hold(KBD_LED_CAPS | KBD_LED_NUM);

    /* BSP keeps PIT/LAPIC timer for scheduling; enable after AP probe. */
   {
      extern void lapic_timer_init(unsigned int hz);
      lapic_timer_init(context_switch_rate);
   }

  //process manager is ready, pass execution to the taskswitcher
    fbdbg_stage(16, "taskswitcher");
    kbd_boot_leds_hold(KBD_LED_CAPS);
    {
        int late = fbconsole_late_init();
        /* After scheduler Caps: both = mapped+painted, Num = no GOP tag
           or hung in the high map, Caps = still refused (too large). */
        if (late == FBCONSOLE_LATE_OK)
            kbd_boot_leds_hold(KBD_LED_CAPS | KBD_LED_NUM);
        else if (late == FBCONSOLE_LATE_NO_TAG)
            kbd_boot_leds_hold(KBD_LED_NUM);
        else
            kbd_boot_leds_hold(KBD_LED_CAPS);
    }
    taskswitcher();      //defined in process.h

    //============ we should not reach this point at all =================
   while (1)
      ;
};

#define STARTUP_DELAY 400

/* Mount a host-formatted FAT volume on virtio-blk at /work when present.
   Zeroed disks (test-virtio / test-posixio) and ATA-only boots skip. */
static void work_mount_vblk(void)
 {
    unsigned char bpb[512];
    unsigned char sb[512];
    int n;

    if (!virtio_blk_present())
       return;

    /* Probe sector 2 for an ext4 superblock (magic 0xEF53 at sb+0x38). */
    memset(sb, 0, sizeof(sb));
    n = virtio_blk_rw(0, 1024, sb, 512);
    if (n > 0 && sb[0x38] == 0x53 && sb[0x39] == 0xEF) {
       if (vfs_mount_device("ext4", "vblk", "work") == -1)
          printf("work: ext4 mount failed\n");
       else
          printf("work: mounted ext4\n");
       return;
    }

    /* Fall back to FAT. */
    memset(bpb, 0, sizeof(bpb));
    n = virtio_blk_rw(0, 0, bpb, 512);
    if (n < 0) {
       printf("work: vblk read failed (skipped)\n");
       return;
    }
    if (bpb[11] != 0x00 || bpb[12] != 0x02 || bpb[13] == 0 || bpb[16] == 0) {
       printf("work: no FAT on vblk (skipped)\n");
       return;
    }
    if (vfs_mount_device("fat", "vblk", "work") == -1)
       printf("work: mount failed\n");
    else
       printf("work: mounted\n");
 }

/*This function is the first function that is called by the taskswitcher
 see process/process.c
 incidentally it is also the first process that gets run
 it is the equivalent of the init() process in *nix systems
*/
void dex_init(){
   char temp[255],spk;
   int consolepid,i,baremode = 0;
   int delay_val =  STARTUP_DELAY / 80;
   devmgr_block_desc *myblock;
   dex32_datetime date;
    
   /* Start capturing the kernel log (dmesg) before any further console I/O. */
    klog_init();
    textcolor(GREEN);
    printf("\n");
    printf("\t\t");printf(OS_NAME);printf(" ");printf(OS_VERSION);
    printf(" (Build: %s)\n",build_id);
    printf("  release=%s build=%s%s %s\n",ICSOS_RELEASE,ICSOS_GIT_HASH,
           (ICSOS_GIT_DIRTY[0]=='1')?"-dirty":"",ICSOS_BUILD_TS);
    printf("\n");
    textcolor(WHITE);
    printf("Starting dex_init()...\n");
   printf("Press space to skip autoexec.bat processing\n");

   printf("dex_init address: 0x%x\n",(unsigned)dex_init);

   //At this point, the kernel has fininshed setting up memory and the process scheduler.
   //More importantly, interrupts are already operational, which means we can now set up
   //devices that require IRQs like the floppy disk driver 
      
    
   //add some hotkeys to the keyboard
   //kb_addhotkey(KEY_F6+CTRL_ALT, 0xFF, fg_next);
   //kb_addhotkey(KEY_F5+CTRL_ALT, 0xFF, fg_prev);
   //kb_addhotkey('\t', KBD_META_ALT, fg_toggle);
   kb_addhotkey(KEY_F12, 0xFF, fg_next); //move accross the consoles
   kb_addhotkey(KEY_F11, 0xFF, fg_prev);
   kb_addhotkey(KEY_F2, 0xFF, console_new);
   kb_addhotkey('\t', KBD_META_ALT, fg_toggle);
    
   keyboardflush();
    
   /*Now that the timer is active we can now use time based functions.
     Delay for two seconds in order to see previous messages */  
    
   textbackground(GREEN);
   for (i=0 ;i < 79; i++){
      printf(" ");  
      if (kb_ready()){
         if (getch() ==' ') {
            baremode = 1;
            break;
         };        
         delay( delay_val );
      };
   }
   textbackground(BLACK);
   printf("\n");  

   printf("Getting date and time...");
   getdatetime(&date);
   getmonthname(date.month,temp);
   printf("[OK]\n");   

   //Install the built-in floppy disk driver (only when booting from floppy)
   if (strcmp(boot_device_name,"fd0") == 0) {
      printf("Installing floppy driver...");
      floppy_install("fd0"); 
      printf("[OK]\n");
   } else {
      printf("Skipping floppy driver (boot device is %s)\n", boot_device_name);
   }
    
   /*Install the IDE, ATA-2/4 compliant driver in order to be able to
      use CD-ROMS and harddisks. This will also create logical drives from
      the partition tables if needed.*/
   printf("Initializing IDE drivers...\n");
   ide_init();
   printf("[OK]\n");

   printf("Initializing virtio-blk...\n");
   virtio_blk_init();

   printf("Initializing virtio-net...\n");
   virtio_net_init();

   printf("Initializing USB mass-storage drivers...\n");
   usb_init();
   usb_cdc_pump();
   kbd_boot_leds_raw(KBD_LED_CAPS | KBD_LED_NUM);
   if (usb_storage_available())
      printf("[OK]\n");
   else
      printf("[none]\n");   

   /*Install the VGA driver*/
   printf("Loading VGA driver...");
   vga_init();
   printf("[OK]\n");   
 
   //initialize the I/O manager
   iomgr_init();

   /* Recalibrate/seek on a missing floppy takes many seconds and is not
      needed when the system booted from USB or a hard disk. */
   if (strcmp(boot_device_name,"fd0") == 0) {
      myblock = (devmgr_block_desc*)devmgr_devlist[floppy_deviceid];
      myblock->init_device();
   }
   //initialize the file tables (Initialize the VFS)
   printf("Initializing the Virtual File System...");
   vfs_init();
   printf("[OK]\n");   

   //set the current directory of the init process to the vfs root
   current_process->workdir= vfs_root;
    
   //Initialize the task manager - a module program that monitors processes
   //for the user's convenience, as kernel thread
   printf("Initializing the task manager...");
   /* ACCESS_SYS stays on this stack through timer C.  3500 overflowed
      into neighbour heap / idle BSS (torn 64-bit slots, UD64 cpus[]). */
   tm_pid=createkthread((void*)dex32_tm_updateinfo,"task_mgr",65536);

   printf("[OK]\n");   


   //create the IO manager thread which handles all I/O to and from
   //block devices like the hard disk, floppy, CD-ROM etc. see iosched.c
   printf("Initializing the disk manager...");
   {
      DWORD dpid = createkthread((void*)iomgr_diskmgr,"disk_mgr",200000);
      if (dpid) {
         iomgr_register_diskmgr(dpid);
         ps_set_affinity((int)dpid, 0);
      }
   }
   printf("[OK]\n");   

   
   //Install a null block device
   printf("Initializng the null block device...");
   devfs_initnull();
   printf("[OK]\n");

   printf("Initializing the RAM disk...");
   ramdisk_init();
   printf("[OK]\n");
    
   printf("Initializing the filesystem driver...");
   //install and initialize the Device Filesystem driver
   devfs_init();
    
   //install and initialize the fat12 filesystem driver
   fat_register("fat");
    
   //initialize the CDFS (ISO9660/Joliet) filesystem
    iso9660_init();

    //register the ext4 filesystem driver
    ext4_register("ext4");
    printf("[OK]\n");

   printf("Mounting boot device %s...\n", boot_device_name);
   {
      int mounted = 0;

      /* Prefer a USB thumb drive when the UHCI driver found one. */
      if (!mounted && usb_storage_available()) {
         if (devmgr_finddevice("usb0p0") != -1)
            mounted = (vfs_mount_device("fat","usb0p0","icsos") != -1);
         if (!mounted && devmgr_finddevice("usb0") != -1)
            mounted = (vfs_mount_device("fat","usb0","icsos") != -1);
         if (mounted)
            printf("Root filesystem is the USB mass-storage device.\n");
      }

      /* Prefer CD when Multiboot2 boot device is the CD-ROM. */
      if (!mounted && (strcmp(boot_device_name,"cds0") == 0
                       || (boot_device_name[0] == 0
                           && devmgr_finddevice("cds0") != -1
                           && !usb_storage_available()))) {
         if (devmgr_finddevice("cds0") != -1) {
            mounted = (vfs_mount_device("cdfs","cds0","icsos") != -1);
            if (mounted)
               printf("Root filesystem is the CD-ROM (cdfs).\n");
         }
      }

      /* USB stick / hard disk presented as BIOS IDE (not CD). */
      if (!mounted && boot_device_name[0]
          && strcmp(boot_device_name,"fd0") != 0
          && strcmp(boot_device_name,"cds0") != 0) {
         if (devmgr_finddevice(boot_device_name) != -1)
            mounted = (vfs_mount_device("fat",boot_device_name,"icsos") != -1);
      }

      if (!mounted && devmgr_finddevice("hdp0p0") != -1)
         mounted = (vfs_mount_device("fat","hdp0p0","icsos") != -1);

      /* Live CD fallback if boot_device was unset / ambiguous. */
      if (!mounted && devmgr_finddevice("cds0") != -1) {
         mounted = (vfs_mount_device("cdfs","cds0","icsos") != -1);
         if (mounted)
            printf("Root filesystem is the CD-ROM (cdfs).\n");
      }

      /* Floppy image (legacy) — only if named fd0 and device exists. */
      if (!mounted && strcmp(boot_device_name,"fd0") == 0
          && devmgr_finddevice("fd0") != -1) {
         mounted = (vfs_mount_device("fat",boot_device_name,"icsos") != -1);
      }

      if (!mounted)
         printf("Warning: no root filesystem mounted (continuing).\n");
      else
         printf("Root mount [OK]\n");

        if (mounted &&
           (strstr(kernel_cmdline, "xhci-mounted-disconnect-test") ||
            strstr(kernel_cmdline, "xhci-mounted-reconnect-test") ||
            strstr(kernel_cmdline, "xhci-mounted-remount-test"))) {
         int usb_test_ok = usb_xhci_mounted_disconnect_selftest();
         if (strstr(kernel_cmdline, "xhci-mounted-remount-test")) {
            vfs_node *saved_workdir = current_process->workdir;
            vfs_node *busy_workdir = vfs_searchname("icsos/boot");
            int busy_rejected = 0;
            if (busy_workdir) {
               current_process->workdir = busy_workdir;
               busy_rejected =
                  vfs_remount_device("fat","usb0p0","icsos") == -1;
               current_process->workdir = saved_workdir;
            }
            if (busy_rejected)
               printf("VFS_REMOUNT_BUSY_REJECT_OK\n");
            if (usb_test_ok && busy_rejected &&
                vfs_remount_device("fat","usb0p0","icsos") != -1 &&
                vfs_searchname("icsos/vmdex"))
               printf("XHCI_MOUNTED_REMOUNT_OK\n");
            else
               printf("XHCI_MOUNTED_REMOUNT_FAIL\n");
         }
      }

      /* Writable scratch FS for in-OS compiles (selfhost / tccboot).
         Start the console before xHCI hotplug/CDC pump: Intel ADL-N can
         block forever in usb_cdc_pump after Root mount, which used to
         leave GOP stuck there with no prompt. */
      if (mounted)
         ramdisk_mount();
      work_mount_vblk();

      /* Unpark APs after root mount; console remains BSP-pinned. */
      if (mounted && cpu_count > 1) {
         smp_enable_scheduling();
         printf("SMP: %d CPUs online; AP scheduling enabled\n", cpu_count);
      }
   }

   //setup the initial executable loaders (So we could run .EXEs,.b32,coff and elfs)
   printf("Initializing first module loader(s) [EXE][COFF][ELF][DEX B32]...");
   dex32_initloader();
   printf("[OK]\n");   

   /*Supposed to initialize the Advanced Power Management Interface
     so that I could do a "software" shutdown **IN PROGRESS** */
   //dex32apm_init();

   printf("Running foreground manager thread\n");
    
   //create the foreground manager
   if (strcmp(kernel_cmdline, "selfhost-stage1") != 0) {
      fg_pid = createkthread_on_cpu((void*)fg_updateinfo,"fg_mgr",20000,0);
   } else {
      printf("foreground manager deferred for self-host\n");
   }
    
   if (baremode) 
      console_first++;
   printf("dex32_startup(): Running console thread\n");

    /* Advertise an xterm-compatible terminal so full-screen apps (vim)
       select the right terminfo and emit the CSI sequences the kernel VT
       interpreter understands. */
    env_setenv("TERM", "xterm", 1);
    env_setenv("COLUMNS", "80", 1);
    env_setenv("LINES", "25", 1);

   //Create a new console instance
    consolepid = console_new();
    ps_set_affinity(consolepid, 0);
    printf("console thread started pid=%d\n", consolepid);
    serial_puts("CONSOLE_THREAD_OK\n");
    kbd_boot_leds_raw(0);

    /* Wait until the first console has painted CONSOLE_READY (or time out)
       before the hotplug thread starts usb_cdc_pump. Intel ADL-N can wedge
       MSC while CDC is pumped on the init path; racing autoexec copies is
       the same failure mode. */
    {
       int spins;
       for (spins = 0; console_first == 0 && spins < 200; spins++)
          delay(1);
       if (console_first == 0)
          printf("console: timed out waiting for CONSOLE_READY\n");
    }

    /* Hotplug/CDC after the console exists so a wedged xHCI pump cannot
       hide the GOP prompt. The monitor thread does its own usb_cdc_pump. */
    if (usb_start_hotplug_monitor() == -1)
       printf("xhci: failed to start hotplug monitor\n");

    /* COM2 (0x2F8) interactive terminal/shell: live commands + introspection
       without a reboot. Attach via a second QEMU -serial chardev (telnet or
       unix socket). Output is mirrored to COM2 only while shell2 is active. */
    {
       extern void serial2_init(void);
       extern DWORD shell2_start(void);
       serial2_init();
       if (shell2_start())
          serial_puts("shell2: COM2 interactive shell online (0x2F8)\n");
    }

   /* After console exists, prove AP can run a pinned migratable kthread. */
   if (cpu_count > 1) {
      extern void smp_start_ap_work_smoke(void);
      smp_start_ap_work_smoke();
   }


   /*beep the computer just in case a screen problem occured, at least
     we know it reaches this part*/
   spk=inportb(0x61);
   spk=spk|3;
   outportb(0x61,spk);
   delay(1);
   spk=inportb(0x61);
   spk=spk&252;
   outportb(0x61,spk);

   //set the console for this process
   Dex32SetProcessDDL(consoleDDL, getprocessid());

   /* Run the process dispatcher.
      The process dispatcher is responsible for running new modules/process.
      It is the only one that could disable paging without crashing the system since
      its stack, data and code segments are located in virtual memory that is at the
      same location as the physical memory
      see pdispatch.c/pdispatch.h for details
   */
   process_dispatcher();   // defined in kernel/process/pdispatch.c
    ;
};

void end_func()
{
};
