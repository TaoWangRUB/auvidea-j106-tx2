/* STM32H743VIT6 — 2 MB flash, 128 KB DTCM.
 *
 * Only bank 1 and DTCM are described: this firmware is a few kB and touches
 * neither the AXI SRAM nor bank 2, and leaving them out means a stray symbol
 * lands as a link error rather than in memory nothing initialises.
 */
MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K
  DTCM  (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

_estack = ORIGIN(DTCM) + LENGTH(DTCM);

ENTRY(Reset_Handler)

SECTIONS
{
  .isr_vector : { KEEP(*(.isr_vector)) } > FLASH

  .text :
  {
    *(.text .text.*)
    *(.rodata .rodata.*)
    . = ALIGN(4);
  } > FLASH

  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data .data.*)
    . = ALIGN(4);
    _edata = .;
  } > DTCM AT > FLASH

  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    *(.bss .bss.*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } > DTCM

  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) }
}
