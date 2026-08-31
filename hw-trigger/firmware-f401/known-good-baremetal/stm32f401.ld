/* stm32f401.ld — STM32F401CD (384K flash / 96K RAM) by default.
 *
 * The part is marked CDU6, which is 384K.  Note that the ROM bootloader's DFU
 * descriptor advertises 512K (04*016Kg,01*064Kg,03*128Kg) -- ST's bootloader
 * reports a family layout, not the populated size, so the marking is the more
 * trustworthy number and the smaller one is the safe default.  Overflowing
 * here is a link error; guessing high produces an image that flashes and then
 * misbehaves.  Override without editing:  make FLASH_SIZE=512K
 */

ENTRY(Reset_Handler)

FLASH_SIZE = DEFINED(FLASH_SIZE) ? FLASH_SIZE : 384K;
RAM_SIZE   = DEFINED(RAM_SIZE)   ? RAM_SIZE   : 96K;

MEMORY
{
	FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = FLASH_SIZE
	RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = RAM_SIZE
}

_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
	.isr_vector : {
		KEEP(*(.isr_vector))
	} > FLASH

	.text : {
		*(.text .text.*)
		*(.rodata .rodata.*)
		*(.glue_7 .glue_7t)
		. = ALIGN(4);
	} > FLASH

	.ARM.exidx : {
		*(.ARM.exidx* .gnu.linkonce.armexidx.*)
	} > FLASH

	. = ALIGN(4);
	_sidata = LOADADDR(.data);

	.data : {
		. = ALIGN(4);
		_sdata = .;
		*(.data .data.*)
		. = ALIGN(4);
		_edata = .;
	} > RAM AT> FLASH

	.bss : {
		. = ALIGN(4);
		_sbss = .;
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(4);
		_ebss = .;
	} > RAM

	/* Outside .bss on purpose: the `dfu` magic has to survive a software
	 * reset, so startup must not zero it. */
	.noinit (NOLOAD) : {
		. = ALIGN(4);
		*(.noinit .noinit.*)
		. = ALIGN(4);
	} > RAM

	/* Fail the link rather than the board: 1 KB of headroom for the single
	 * interrupt handler and the 64-byte command buffer. */
	.stack_guard : {
		. = ALIGN(4);
		. = . + 1024;
	} > RAM

	/DISCARD/ : { *(.ARM.attributes) }
}
