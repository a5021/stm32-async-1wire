/*
 * Linker script for STM32H743VI (2MB Flash, 1MB RAM)
 *
 * Memory layout:
 *   FLASH (rx)  : 0x08000000, 2048K
 *   RAM   (xrw) : 0x24000000,  512K  (AXI SRAM1)
 *
 * Everything lives in AXI SRAM1 on purpose: DTCM/ITCM are CPU-only tightly
 * coupled memories the DMA cannot reach, so a pulse or capture buffer linked
 * there would silently never transfer. SRAM2/SRAM3/SRAM4 stay unused.
 *
 * Provides symbols needed by the CMSIS startup code:
 *   _estack, _sidata, _sdata, _edata, _sbss, _ebss
 */

ENTRY(Reset_Handler)

_estack = ORIGIN(RAM) + LENGTH(RAM);
/* Stack-limit anchor for cores whose startup file programs MSPLIM (kept for
 * uniformity with the other part scripts; this H7 startup file only loads
 * _estack into SP). */
_sstack = ORIGIN(RAM);

_Min_Heap_Size  = 0x200;
_Min_Stack_Size = 0x400;

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 2048K
  RAM   (xrw) : ORIGIN = 0x24000000, LENGTH =  512K
}

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)
    KEEP(*(.init))
    KEEP(*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >FLASH

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH

  .ARM.exidx :
  {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  .preinit_array :
  {
    PROVIDE_HIDDEN(__preinit_array_start = .);
    KEEP(*(.preinit_array*))
    PROVIDE_HIDDEN(__preinit_array_end = .);
  } >FLASH

  .init_array :
  {
    PROVIDE_HIDDEN(__init_array_start = .);
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array*))
    PROVIDE_HIDDEN(__init_array_end = .);
  } >FLASH

  .fini_array :
  {
    PROVIDE_HIDDEN(__fini_array_start = .);
    KEEP(*(SORT(.fini_array.*)))
    KEEP(*(.fini_array*))
    PROVIDE_HIDDEN(__fini_array_end = .);
  } >FLASH

  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } >RAM AT>FLASH

  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM

  .noinit (NOLOAD) :
  {
    . = ALIGN(4);
    *(.noinit)
    *(.noinit*)
    . = ALIGN(4);
  } >RAM

  __heap_start = ALIGN(8);
  __heap_end   = __heap_start + _Min_Heap_Size;

  __StackTop = _estack;
  __stack = _estack;
}
