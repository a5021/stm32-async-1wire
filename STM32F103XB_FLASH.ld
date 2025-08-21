/*
*****************************************************************************
**
**  File        : STM32F103XB_FLASH.ld
**
**  Abstract    : Improved linker script for STM32F103xB Device
**                (128KByte FLASH, 20KByte RAM)
**
**                - Supports C and C++ (constructors/destructors)
**                - Ensures heap/stack do not overlap with data sections
**                - Provides useful symbols for debugging and runtime
**                - Compatible with standard STM32 startup files
**
*****************************************************************************
*/

/* Entry Point */
ENTRY(Reset_Handler)

/* Highest address of the user mode stack */
_estack = ORIGIN(RAM) + LENGTH(RAM);    /* End of RAM (0x20005000 for 20K) */

/* Minimum heap and stack sizes (can be overridden at link time) */
_Min_Heap_Size  = 0x200;      /* 512 bytes heap (adjust as needed) */
_Min_Stack_Size = 0x400;      /* 1KB stack (adjust as needed) */

/* Specify the memory areas */
MEMORY
{
  FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 128K
  RAM   (xrw): ORIGIN = 0x20000000, LENGTH = 20K
}

/* Define output sections */
SECTIONS
{
  /* The startup code goes first into FLASH */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector)) /* Startup code */
    . = ALIGN(4);
  } >FLASH

  /* The program code and other data goes into FLASH */
  .text :
  {
    . = ALIGN(4);
    *(.text)           /* .text sections (code) */
    *(.text*)          /* .text* sections (code) */
    *(.glue_7)         /* glue arm to thumb code */
    *(.glue_7t)        /* glue thumb to arm code */
    *(.eh_frame)
    KEEP(*(.init))
    KEEP(*(.fini))
    . = ALIGN(4);
    _etext = .;        /* End of code */
  } >FLASH

  /* Constant data goes into FLASH */
  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)         /* .rodata sections (constants, strings, etc.) */
    *(.rodata*)        /* .rodata* sections (constants, strings, etc.) */
    . = ALIGN(4);
  } >FLASH

  /* ARM unwind and exception handling sections */
  .ARM.extab   : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
  .ARM.exidx   :
  {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  /* C++ support: constructors/destructors */
  .preinit_array :
  {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } >FLASH

  .init_array :
  {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
  } >FLASH

  .fini_array :
  {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } >FLASH

  /* Used by the startup to initialize data */
  _sidata = LOADADDR(.data);

  /* Initialized data sections go into RAM, load LMA copy after code */
  .data : 
  {
    . = ALIGN(4);
    _sdata = .;        /* RAM start address of .data */
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;        /* RAM end address of .data */
  } >RAM AT> FLASH

  /* Uninitialized data section */
  . = ALIGN(4);
  .bss :
  {
    _sbss = .;         /* BSS start */
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;         /* BSS end */
    __bss_end__ = _ebss;
  } >RAM

  /* Heap and stack section, ensures they do not overlap with data/bss */
  ._heap :
  {
    . = ALIGN(8);
    _heap_start = .;
    . = . + _Min_Heap_Size;
    _heap_end = .;
  } >RAM

  ._stack :
  {
    . = ALIGN(8);
    _stack_start = .;
    . = . + _Min_Stack_Size;
    _stack_end = .;
  } >RAM

  /* Provide symbols for end of RAM usage */
  PROVIDE(end = .);
  PROVIDE(_end = .);

  /* Remove information from the standard libraries */
  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}

/* Useful symbols for debugging and runtime checks */
_ram_start   = ORIGIN(RAM);
_ram_end     = ORIGIN(RAM) + LENGTH(RAM);
_flash_start = ORIGIN(FLASH);
_flash_end   = ORIGIN(FLASH) + LENGTH(FLASH);

/* Calculate used/available RAM */
_ram_data_bss_used = _ebss - _ram_start;
_ram_heap_stack_used = _stack_end - _heap_start;
_ram_total_used = _stack_end - _ram_start;
_ram_free = _ram_end - _stack_end;
