/* STM32H743VITx — linker script for camtrig.
 *
 * Replaces the previous hand-written script, which described only bank 1 and
 * DTCM so that "a stray symbol lands as a link error rather than in memory
 * nothing initialises".  That property is deliberately given up here: the HAL,
 * the USB device library and FreeRTOS all expect the standard full memory map.
 *
 * .data/.bss/heap/stack live in DTCM.  That is safe *because* the USB core runs
 * in slave (FIFO) mode with no DMA — see design D4.  If DMA is ever enabled for
 * any peripheral, its buffers must be moved to RAM_D1 (AXI SRAM), because no
 * DMA engine can reach DTCM.
 */

ENTRY(Reset_Handler)

_estack    = ORIGIN(DTCMRAM) + LENGTH(DTCMRAM);   /* top of stack */
_Min_Heap_Size  = 0x400;    /* newlib-nano sbrk arena; FreeRTOS uses its own */
_Min_Stack_Size = 0x800;    /* MSP: ISR context only, tasks carry their own */

MEMORY
{
  DTCMRAM (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
  ITCMRAM (xrw) : ORIGIN = 0x00000000, LENGTH = 64K
  RAM_D1  (xrw) : ORIGIN = 0x24000000, LENGTH = 512K
  RAM_D2  (xrw) : ORIGIN = 0x30000000, LENGTH = 288K
  RAM_D3  (xrw) : ORIGIN = 0x38000000, LENGTH = 64K
  FLASH   (rx)  : ORIGIN = 0x08000000, LENGTH = 2048K
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
    KEEP (*(.init))
    KEEP (*(.fini))
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
  .ARM : {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

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

  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } >DTCMRAM AT> FLASH

  . = ALIGN(4);
  .bss :
  {
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >DTCMRAM

  /* Survives NVIC_SystemReset(): deliberately outside _sbss.._ebss so the
   * startup code's bss clear does not touch it.  Carries the "reboot into the
   * ROM bootloader" request across the reset. */
  .noinit (NOLOAD) :
  {
    . = ALIGN(4);
    *(.noinit .noinit.*)
    . = ALIGN(4);
  } >DTCMRAM

  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >DTCMRAM

  /* ST's template discards libc.a/libm.a/libgcc.a leftovers here. Omitted on
   * purpose: we link newlib-nano, so it would match nothing, and a DISCARD that
   * silently swallows a section is the opposite of the link-error-on-surprise
   * property this script already gives up enough of. */

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
