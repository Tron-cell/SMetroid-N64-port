ENTRY(_start)
SECTIONS {
  . = 0x80001000;
  .text : { *(.text.start) *(.text .text.*) }
  .rodata : { *(.rodata .rodata.* .rdata) }
  .data : { *(.data .data.*) }
  . = ALIGN(16);
  _gp = . + 0x8000;
  .sdata : { *(.sdata .sdata.* .lit8 .lit4) }
  . = ALIGN(8);
  __bss_start = .;
  .sbss : { *(.sbss .sbss.* .scommon) }
  .bss : { *(.bss .bss.* COMMON) }
  . = ALIGN(8);
  __bss_end = .;
  __heap_start = .;
  __stack_top = 0x80FFFFF0;
  /DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.pdr) *(.comment) }
}
