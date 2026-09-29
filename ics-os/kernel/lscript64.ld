OUTPUT_FORMAT("elf64-x86-64")
ENTRY(startup)
SECTIONS {
  . = 0x00100000;
  .multiboot : {
    *(.multiboot)
  }
  .text : {
    *(.text)
    *(.text.*)
  }
  textEnd = .;
  .rodata : {
    *(.rodata)
    *(.rodata.*)
  }
  .data : {
    *(.data)
    *(.data.*)
  }
  dataEnd = .;
  .bss : {
    *(.common)
    *(.bss)
    *(.bss.*)
  }
  bssEnd = .;
  /* Boot page tables (PML4/PDPT/PD), 4096-aligned and set up by the boot
     code (CR3 -> boot_pml4).  Kept in their own section so .bss does not
     inherit 4096-byte alignment, which would otherwise force .bss to jump a
     full page for every ~4KiB of .text growth.  Placed after .bss but still
     within the frame-reserved [MEM_KERNEL_LOAD, MEM_KERNEL_LIMIT) range. */
  . = ALIGN(4096);
  .bootpt (NOLOAD) : {
    *(.bootpt)
  }
  bootptEnd = .;
  /DISCARD/ : {
    *(.comment)
    *(.note*)
    *(.eh_frame*)
    *(.gnu*)
  }
}
/* bssEnd must stay below the 4MiB user-ELF base (TinyCC ELF_START_ADDR).
   The former 256KiB "frame stack" after bssEnd was replaced by the global
   E820-seeded frame allocator (see dexmem.c), which reserves the fixed
   [MEM_KERNEL_LOAD, MEM_KERNEL_LIMIT) range regardless of bssEnd, so the
   gap between bssEnd and 4MiB is inert slack and only a small guard is
   kept.  Raised to 0x3FA000 for the RTL8821CE plus the current network
   stack, then to 0x3FB000 (16 KiB guard) when the ELF64 stream loader's
   batched 128KiB reads crossed the 0x1be000 .bss alignment boundary. */
ASSERT(bssEnd <= 0x003FB000, "kernel BSS collided with 4MiB user ELF window");
/* The boot page tables (.bootpt) sit after bssEnd; the whole kernel image
   (bssEnd + boot page tables) must still end below the 4MiB user-ELF base. */
ASSERT(bootptEnd <= 0x00400000, "boot page tables collided with 4MiB user ELF window");
INPUT(startup.o asmlib.o context.o ap_trampoline.o lapic.o smp.o
      kernel32.o scheduler.o iomgr.o blkcache.o posixfd.o fat.o ext4.o iso9660.o ramdisk.o
      devfs.o irqwrap.o devmgr_error.o kexec.o irq_lifecycle.o vtd.o pci_probe.o
      virtio_blk.o virtio_net.o rtl8139.o
      wifi.o rtw_io.o rtw_pwr.o rtw_pci.o rtw_fw.o rtw_efuse.o rtw_mac.o rtw_phy.o rtw8821ce.o
      net_pbuf.o net_netif.o net_ethernet.o net_arp.o net_ipv4.o net_icmp.o net_udp.o net_tcp.o net_sock.o net_sync.o net_dhcp.o net_inet_config.o net_dns.o net_softnet.o net_netcfg.o)
OUTPUT(Kernel64.bin)
