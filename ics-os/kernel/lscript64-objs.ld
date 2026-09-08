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
  /DISCARD/ : {
    *(.comment)
    *(.note*)
    *(.eh_frame*)
    *(.gnu*)
  }
}
/* bssEnd must stay below the 4MiB user-ELF base (TinyCC ELF_START_ADDR).
   The former 256KiB "frame stack" after bssEnd was replaced by the global
   E820-seeded frame allocator (see dexmem.c), so only a small guard is kept. */
ASSERT(bssEnd <= 0x003F0000, "kernel BSS collided with 4MiB user ELF window");
OUTPUT(Kernel64.bin)
