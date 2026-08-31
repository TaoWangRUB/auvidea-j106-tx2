/* startup.c — vector table, reset entry and ROM-bootloader entry for the
 * STM32F401 trigger build.
 *
 * Hand-written rather than taken from CubeMX so this tree stays free of
 * vendor files: the whole firmware is two sources and a linker script, and
 * `make` needs nothing but arm-none-eabi-gcc.
 */

#include <stdint.h>

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;

int  main(void);
void TIM2_IRQHandler(void);

/* Set by the `dfu` command, then survives the software reset.
 *
 * It lives in .noinit, which the linker places outside .bss and which the
 * loop below therefore never zeroes -- that is the whole mechanism.  The
 * H7 build uses the same trick for the same reason. */
__attribute__((section(".noinit"), used))
uint32_t g_boot_magic;

#define BOOT_MAGIC	0xB00710ADu

/* STM32F401 ROM bootloader.  RM0368 table 3: system memory sits at
 * 0x1FFF0000, and SYSCFG_MEMRMP = 1 aliases it to address 0 so the vector
 * fetches land in the right place. */
#define SYSMEM_BASE	0x1FFF0000u
#define RCC_APB2ENR	(*(volatile uint32_t *)0x40023844u)
#define SYSCFG_MEMRMP	(*(volatile uint32_t *)0x40013800u)

/* Anything that is not TIM2 parks here.  A spin, not a reset: if the board
 * ever does take an unexpected fault, the cameras keep getting whatever the
 * timer was last programmed with (the timer runs in hardware, independent of
 * the core), and a debugger finds the CPU sitting at a known address instead
 * of in a reboot loop that erases the evidence. */
static void Default_Handler(void)
{
	for (;;)
		;
}

/* Entered before .data/.bss init and before any peripheral is configured,
 * which is exactly why the jump happens here rather than inside the command
 * handler: the bootloader gets a machine in reset state, with no running
 * timer, no enabled interrupt and no half-configured clock tree to trip over. */
static void maybe_enter_bootloader(void)
{
	uint32_t sp, pc;

	if (g_boot_magic != BOOT_MAGIC)
		return;
	g_boot_magic = 0;		/* one shot: never loop into DFU */

	RCC_APB2ENR |= (1u << 14);	/* SYSCFGEN */
	SYSCFG_MEMRMP = 1u;		/* alias system memory to 0x00000000 */

	sp = *(volatile uint32_t *)(SYSMEM_BASE);
	pc = *(volatile uint32_t *)(SYSMEM_BASE + 4u);

	/* A blank or unexpected system-memory vector would send us into the
	 * weeds; falling through to main() instead leaves a board that still
	 * triggers cameras and can still be reached over UART. */
	if ((pc & 1u) == 0u || pc < SYSMEM_BASE || pc >= SYSMEM_BASE + 0x8000u)
		return;

	__asm volatile ("msr msp, %0" :: "r" (sp) : );
	((void (*)(void))pc)();
}

void Reset_Handler(void)
{
	uint32_t *src, *dst;

	maybe_enter_bootloader();

	for (src = &_sidata, dst = &_sdata; dst < &_edata; )
		*dst++ = *src++;
	for (dst = &_sbss; dst < &_ebss; )
		*dst++ = 0;

	main();
	for (;;)
		;
}

typedef void (*vector_t)(void);

/* 16 system entries then the external interrupts.  TIM2 is IRQ 28, so it
 * lands at index 16 + 28 = 44.  The F401 has 85 maskable interrupts. */
__attribute__((section(".isr_vector"), used))
const vector_t g_vectors[] = {
	(vector_t)&_estack,
	Reset_Handler,
	Default_Handler,	/* NMI          */
	Default_Handler,	/* HardFault    */
	Default_Handler,	/* MemManage    */
	Default_Handler,	/* BusFault     */
	Default_Handler,	/* UsageFault   */
	0, 0, 0, 0,
	Default_Handler,	/* SVCall       */
	Default_Handler,	/* DebugMonitor */
	0,
	Default_Handler,	/* PendSV       */
	Default_Handler,	/* SysTick      */

	[16 + 28] = TIM2_IRQHandler,
	[16 + 84] = Default_Handler,	/* size the table to a full F401 map */
};

/* GCC may synthesise calls to these for aggregate copies and zero-fills even
 * when the source never names them, and -nostdlib means nothing else provides
 * them.  Tiny, correct, and never on a hot path here. */
void *memset(void *d, int c, unsigned long n)
{
	unsigned char *p = d;

	while (n--)
		*p++ = (unsigned char)c;
	return d;
}

void *memcpy(void *d, const void *s, unsigned long n)
{
	unsigned char *p = d;
	const unsigned char *q = s;

	while (n--)
		*p++ = *q++;
	return d;
}
