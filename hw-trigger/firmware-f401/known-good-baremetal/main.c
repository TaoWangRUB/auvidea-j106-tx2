/* main.c — IMX296 hardware frame trigger, STM32F401 stopgap build.
 *
 * WHY THIS EXISTS
 * ---------------
 * The WeAct MiniSTM32H7xx that normally runs this rig died: its 3.3 V rail
 * measured 0.8 ohm to ground (the 5 V input side stayed open at 15 Mohm), so
 * the LDO was current-limiting into a dead short and the MCU never had a core
 * supply.  This is a from-scratch replacement for an STM32F103 so the camera
 * rig keeps running while a new H7 board is sourced.  It is NOT a port of the
 * H7 build: that is HAL + FreeRTOS + USB CDC + TFT, none of which applies here.
 * This is bare metal, two sources, no dependencies.
 *
 * Target: STM32F401CDU6 (WeAct Black Pill), Cortex-M4 at 84 MHz, HSE 25 MHz.
 *
 * WHAT CARRIES OVER UNCHANGED
 * ---------------------------
 *   - The pins.  H7 drives TIM5_CH1..4 on PA0-PA3; F401 drives TIM2_CH1..4 on
 *     PA0-PA3 (AF1).  Same four pins.  The wiring harness in WIRING.md
 *     section 4.1 needs no change at all.
 *   - The console.  Both put USART1 on PA9 (TX) / PA10 (RX) at 115200 8N1 (AF7
 *     here), so the FT232R dongle already wired for the H7 works as-is, and
 *     tools/j106-trigctl.py talks to this firmware unmodified.
 *   - The command grammar and the sensor timing model (below), copied from
 *     Core/Inc/camtrig.h and Core/Src/camtrig.c so behaviour matches.
 *
 *   - Pulse-width resolution.  TIM2 on the F401 is a 32-bit counter, exactly
 *     like the H7's TIM5, so the prescaler stays at 1 for every rate in range
 *     and one tick is 11.9 ns at 84 MHz (the H7 managed 8.33 ns at 120 MHz).
 *     An earlier draft of this firmware targeted an F103, where a 16-bit ARR
 *     forced ~514 ns; moving to the F4 removes that compromise entirely.
 *   - The `dfu` command.  The F401's ROM bootloader does speak USB DFU, so the
 *     H7's no-BOOT0 reflash trick works here too -- see request_bootloader().
 *
 * WHAT IS DIFFERENT
 * -----------------
 *   - No `bl` command: there is no on-board display.
 *   - USB is used only by the ROM bootloader.  This firmware does not drive
 *     the USB peripheral, so USB-C supplies power and DFU, nothing else.
 *     Control is over USART1.
 *
 * Everything else -- help, status, start, stop, fps, period, exp, pol, skew,
 * burst -- behaves as it does on the H7.
 */

#include <stdint.h>

#include "usb_cdc.h"

/* ------------------------------------------------------------------ *
 * Register map.  Declared here rather than pulled from CMSIS so this tree
 * has no vendor headers to vendor and no download to reproduce a build.
 * Reference: RM0368 rev 5 (STM32F401xB/C/D/E).
 *
 * Note how little of this looks like the F1: on the F4 the GPIO block is
 * MODER/OSPEEDR/AFR rather than CRL/CRH, RCC moves to 0x40023800, and USART1
 * is at 0x40011000 (0x40013800 is SYSCFG here, which on the F1 was USART1 --
 * an easy and silent way to get this wrong).
 * ------------------------------------------------------------------ */
#define REG(a)		(*(volatile uint32_t *)(a))

#define RCC_BASE	0x40023800u
#define RCC_CR		REG(RCC_BASE + 0x00)
#define RCC_PLLCFGR	REG(RCC_BASE + 0x04)
#define RCC_CFGR	REG(RCC_BASE + 0x08)
#define RCC_AHB1ENR	REG(RCC_BASE + 0x30)
#define RCC_APB1ENR	REG(RCC_BASE + 0x40)
#define RCC_APB2ENR	REG(RCC_BASE + 0x44)

#define FLASH_ACR	REG(0x40023C00u + 0x00)

#define GPIOC_BASE	0x40020800u
#define GPIOC_MODER	REG(GPIOC_BASE + 0x00)
#define GPIOC_BSRR	REG(GPIOC_BASE + 0x18)

#define GPIOA_BASE	0x40020000u
#define GPIOA_MODER	REG(GPIOA_BASE + 0x00)
#define GPIOA_OSPEEDR	REG(GPIOA_BASE + 0x08)
#define GPIOA_PUPDR	REG(GPIOA_BASE + 0x0C)
#define GPIOA_AFRL	REG(GPIOA_BASE + 0x20)
#define GPIOA_AFRH	REG(GPIOA_BASE + 0x24)

#define USART1_BASE	0x40011000u
#define USART1_SR	REG(USART1_BASE + 0x00)
#define USART1_DR	REG(USART1_BASE + 0x04)
#define USART1_BRR	REG(USART1_BASE + 0x08)
#define USART1_CR1	REG(USART1_BASE + 0x0C)

#define TIM2_BASE	0x40000000u
#define TIM2_CR1	REG(TIM2_BASE + 0x00)
#define TIM2_DIER	REG(TIM2_BASE + 0x0C)
#define TIM2_SR		REG(TIM2_BASE + 0x10)
#define TIM2_EGR	REG(TIM2_BASE + 0x14)
#define TIM2_CCMR1	REG(TIM2_BASE + 0x18)
#define TIM2_CCMR2	REG(TIM2_BASE + 0x1C)
#define TIM2_CCER	REG(TIM2_BASE + 0x20)
#define TIM2_CNT	REG(TIM2_BASE + 0x24)
#define TIM2_PSC	REG(TIM2_BASE + 0x28)
#define TIM2_ARR	REG(TIM2_BASE + 0x2C)
#define TIM2_CCR(i)	REG(TIM2_BASE + 0x34 + 4u * (i))	/* i = 0..3 */

#define NVIC_ISER0	REG(0xE000E100u)
#define SCB_AIRCR	REG(0xE000ED0Cu)
#define TIM2_IRQn	28

/* HSE fitted on a WeAct Black Pill.  Override at build time for a board with
 * a different crystal:  make HSE_HZ=8000000 */
#ifndef HSE_HZ
#define HSE_HZ		25000000u
#endif

#define SYSCLK_HZ	84000000u	/* F401 maximum */

/* ------------------------------------------------------------------ *
 * Sensor limits.  Verbatim from Core/Inc/camtrig.h -- these are datasheet
 * constants (IMX296LQR-C_Fulldatasheet_Awin.pdf) in physical units, kept in
 * physical units so they can be checked against the datasheet rather than
 * against a pre-baked tick count.
 * ------------------------------------------------------------------ */
#define IMX296_1H_NS		14815u		/* HMAX 1100 / 74.25 MHz          */
#define IMX296_TTGPD_H		1126u		/* fast trigger, all-pixel readout */
#define IMX296_MIN_PERIOD_NS	(IMX296_1H_NS * IMX296_TTGPD_H)	/* 16.68 ms */
#define IMX296_TOFFSET_NS	14260u		/* t_exp = t_pulse + 14.26 us     */
#define IMX296_MIN_LOW_NS	50u		/* tTGSE                          */

#define READOUT_MARGIN_NS	1000000u	/* 1 ms of daylight before next edge */
#define MAX_PERIOD_NS		4000000000u	/* u32 ceiling; 0.25 fps             */

#define NCH			4
#define DEFAULT_FPS_MILLI	30000u
#define DEFAULT_EXP_US		5000u

#define BAD_U32			0xFFFFFFFFu

/* ------------------------------------------------------------------ *
 * State
 * ------------------------------------------------------------------ */
static uint32_t g_timer_hz;
static const char *g_clock_name = "hsi";
static uint32_t g_period_ns  = (uint32_t)(1000000000000ull / DEFAULT_FPS_MILLI);
static uint32_t g_exp_us[NCH];
static uint32_t g_pulse_ns[NCH];
static uint32_t g_opto_skew_ns;
static int      g_active_high = 1;
static int      g_running;
static volatile uint32_t g_pulses;
static volatile uint32_t g_burst_left;
static int      g_pwm_started;
static volatile uint32_t g_led_count;
static uint32_t g_led_div = 15;		/* recomputed per rate; see apply() */

/* ------------------------------------------------------------------ *
 * Clock.  HSE 25 MHz -> PLL -> SYSCLK 84 MHz (the F401 ceiling).
 *
 *   VCO_in  = HSE / PLLM = 25 / 25 = 1 MHz
 *   VCO_out = VCO_in * PLLN = 1 * 336 = 336 MHz
 *   SYSCLK  = VCO_out / PLLP = 336 / 4 = 84 MHz
 *
 * The HSI fallback is not decoration: this is a salvage board of unknown
 * provenance, and a missing or cracked crystal would otherwise hang the boot
 * in the HSERDY spin with no console to say why.  HSI on the F4 is 16 MHz, so
 * PLLM = 16 gives the same 1 MHz reference and therefore the *same* 84 MHz --
 * the fallback costs no speed at all, only crystal accuracy (HSI is ~1%, which
 * matters here because it lands directly on the frame rate).  `status` reports
 * which one won.
 * ------------------------------------------------------------------ */
static void clock_init(void)
{
	uint32_t spin, pllm, pllsrc;

	/* 84 MHz at 3.3 V needs 2 wait states (RM0368 table 6). Prefetch plus
	 * the instruction and data caches are free performance on the F4. */
	FLASH_ACR = (1u << 10) | (1u << 9) | (1u << 8) | 2u;

	RCC_CR |= (1u << 16);			/* HSEON */
	for (spin = 0; spin < 0x40000u; spin++)
		if (RCC_CR & (1u << 17))	/* HSERDY */
			break;

	if (RCC_CR & (1u << 17)) {
		pllm = HSE_HZ / 1000000u;	/* -> 1 MHz PLL input */
		pllsrc = (1u << 22);
		g_clock_name = "hse";
	} else {
		pllm = 16u;			/* HSI is 16 MHz -> 1 MHz */
		pllsrc = 0u;
		g_clock_name = "hsi";
	}
	g_timer_hz = SYSCLK_HZ;

	/* PLLP field: 00 = /2, 01 = /4, 10 = /6, 11 = /8.  PLLQ = 7 puts the
	 * USB clock at 336/7 = 48 MHz; this firmware does not use USB, but the
	 * ROM bootloader we may jump back into does. */
	/* Bits 31:28 are reserved and reset to a non-zero pattern (0x2 in
	 * bit 29); RM0368 says keep them at their reset value, so this is a
	 * read-modify-write rather than a wholesale store. */
	RCC_PLLCFGR = (RCC_PLLCFGR & 0xF0000000u)
		    | pllm
		    | (336u << 6)
		    | (1u << 16)			/* PLLP = /4     */
		    | pllsrc				/* HSE or HSI/PLL */
		    | (7u << 24);			/* PLLQ = /7     */

	RCC_CFGR = (0u << 4)			/* AHB  /1 -> 84 MHz */
		 | (4u << 10)			/* APB1 /2 -> 42 MHz */
		 | (0u << 13);			/* APB2 /1 -> 84 MHz */

	RCC_CR |= (1u << 24);			/* PLLON */
	while (!(RCC_CR & (1u << 25)))		/* PLLRDY */
		;

	RCC_CFGR = (RCC_CFGR & ~3u) | 2u;	/* SW = PLL */
	while (((RCC_CFGR >> 2) & 3u) != 2u)	/* SWS = PLL */
		;

	/* TIM2 sits on APB1.  With APB1 prescaled by 2 the timer clock is
	 * doubled back to SYSCLK (RM0368 6.2 "Clock tree" note), which is why
	 * g_timer_hz is the core clock and not PCLK1. */
}

/* Reboot into the ROM bootloader so the board can be reflashed over USB with
 * no BOOT0 press -- the same convenience the H7 build has.
 *
 * The magic goes in .noinit (survives reset, never zeroed by startup) and the
 * actual jump happens at the very top of Reset_Handler, before any peripheral
 * is touched.  Jumping from here instead would mean unwinding a running timer,
 * an enabled interrupt and a configured UART by hand, and getting that subtly
 * wrong shows up as a bootloader that enumerates once and then wedges. */
extern uint32_t g_boot_magic;

void request_bootloader(void)
{
	g_boot_magic = 0xB00710ADu;
	__asm volatile ("dsb");
	SCB_AIRCR = (0x5FAu << 16) | (1u << 2);		/* SYSRESETREQ */
	for (;;)
		;
}

/* ------------------------------------------------------------------ *
 * USART1 on PA9 / PA10.  Same pins and same 115200 8N1 as the H7 build, so
 * the dongle and the host tool do not know the MCU changed.
 * ------------------------------------------------------------------ */
static void uart_init(void)
{
	RCC_AHB1ENR |= (1u << 0);		/* GPIOA   */
	RCC_APB2ENR |= (1u << 4);		/* USART1  */

	/* PA9 (TX) and PA10 (RX) -> alternate function, AF7 = USART1. */
	GPIOA_MODER   = (GPIOA_MODER   & ~0x003C0000u) | 0x00280000u;
	GPIOA_OSPEEDR = (GPIOA_OSPEEDR & ~0x003C0000u) | 0x00280000u;
	GPIOA_PUPDR   = (GPIOA_PUPDR   & ~0x003C0000u) | 0x00100000u;  /* RX pull-up */
	GPIOA_AFRH    = (GPIOA_AFRH    & ~0x00000FF0u) | 0x00000770u;

	USART1_BRR = (g_timer_hz + 57600u) / 115200u;	/* rounded */
	USART1_CR1 = (1u << 13) | (1u << 3) | (1u << 2);/* UE | TE | RE */
}

static void uart_putc(char c)
{
	while (!(USART1_SR & (1u << 7)))	/* TXE */
		;
	USART1_DR = (uint32_t)(unsigned char)c;
}

/* Where output goes.  The H7 build threads a sink_t through every out_*
 * call; here the command loop is single-threaded with no RTOS and no
 * interrupt that prints, so a module-level current-sink is equivalent and
 * keeps the call sites identical to the H7's.  Set once per command, before
 * handle() runs.
 *
 * The rule is the H7's (design D11): a reply goes back to the transport the
 * command arrived on, and unsolicited output goes to both. */
#define SINK_UART	0
#define SINK_USB	1
#define SINK_ALL	2

static int g_sink = SINK_ALL;

static void out_putc(char c)
{
	if (g_sink != SINK_USB)
		uart_putc(c);
	if (g_sink != SINK_UART)
		usb_cdc_putc(c);
}

static void out_puts(const char *s)
{
	while (*s)
		out_putc(*s++);
}

static void out_putu(uint32_t v)
{
	char buf[11];
	int n = 0;

	if (!v) {
		out_putc('0');
		return;
	}
	while (v) {
		buf[n++] = (char)('0' + v % 10u);
		v /= 10u;
	}
	while (n)
		out_putc(buf[--n]);
}

static void out_hex(uint32_t v)
{
	const char *d = "0123456789abcdef";
	int i;

	out_puts("0x");
	for (i = 28; i >= 0; i -= 4)
		out_putc(d[(v >> i) & 0xFu]);
}

static void out_kv(const char *k, uint32_t v)
{
	out_puts(k);
	out_putc('=');
	out_putu(v);
	out_putc('\n');
}

/* ------------------------------------------------------------------ *
 * Status LED — PC13, the board's blue LED.
 *
 * Schematic MiniF4x1Cx_V31: 3V3 -> R5 1.5K -> LED -> PC13, so the LED is
 * ACTIVE LOW: driving the pin low lights it.
 *
 * The three states are meant to be readable across a room on a headless rig:
 *
 *   dark      no power, or the firmware is not running
 *   solid     alive, but not triggering (`stop`, or a burst that finished)
 *   blinking  triggering -- and the blink is driven from the TIM2 update
 *             interrupt, so it is proof the timer is actually running at the
 *             rate you asked for, not just that the CPU is alive.
 * ------------------------------------------------------------------ */
#define LED_PIN		13u

static void led_init(void)
{
	RCC_AHB1ENR |= (1u << 2);		/* GPIOC */
	GPIOC_MODER = (GPIOC_MODER & ~(3u << (LED_PIN * 2)))
		    | (1u << (LED_PIN * 2));	/* output push-pull */
}

#define GPIOC_ODR	REG(GPIOC_BASE + 0x14)
#define GPIOC_ODR_BIT()	((GPIOC_ODR >> LED_PIN) & 1u)

static void led_on(void)   { GPIOC_BSRR = 1u << (LED_PIN + 16); }

/* ------------------------------------------------------------------ *
 * TIM2 -> PA0-PA3, PWM mode 1, all four channels on one counter.
 *
 * One counter is the whole point: the four rising edges are produced by the
 * same overflow event, so channel-to-channel skew is a hardware property, not
 * a software one.  Per-channel exposure differences live in CCR1..4 and only
 * move the falling edges.
 * ------------------------------------------------------------------ */
static void tim2_init(void)
{
	RCC_AHB1ENR |= (1u << 0);		/* GPIOA */
	RCC_APB1ENR |= (1u << 0);		/* TIM2  */

	/* PA0-PA3 -> alternate function, AF1 = TIM2_CH1..4, very high speed.
	 * Speed matters: these drive optocoupler LEDs and the pulse edge is
	 * what the sensor times against. */
	GPIOA_MODER   = (GPIOA_MODER   & ~0x000000FFu) | 0x000000AAu;
	GPIOA_OSPEEDR = (GPIOA_OSPEEDR & ~0x000000FFu) | 0x000000FFu;
	GPIOA_AFRL    = (GPIOA_AFRL    & ~0x0000FFFFu) | 0x00001111u;

	/* PWM mode 1 (0b110) with preload on every channel */
	TIM2_CCMR1 = (6u << 4) | (1u << 3) | (6u << 12) | (1u << 11);
	TIM2_CCMR2 = (6u << 4) | (1u << 3) | (6u << 12) | (1u << 11);
	TIM2_CR1  |= (1u << 7);			/* ARPE */
	TIM2_DIER |= (1u << 0);			/* UIE -- drives the pulse count */
	NVIC_ISER0 = (1u << TIM2_IRQn);
}

/* Idle state = LED off.  The optocoupler makes the sense of the pulse a board
 * fact we cannot read from here, so polarity is runtime-settable and the
 * default is the safe one: an idle or unpowered board draws no LED current and
 * cannot hold a sensor inside an exposure. */
static void set_polarity(int active_high)
{
	unsigned i;

	g_active_high = active_high;
	for (i = 0; i < NCH; i++) {
		uint32_t mask = (1u << 1) << (i * 4);	/* CCxP */

		if (active_high)
			TIM2_CCER &= ~mask;
		else
			TIM2_CCER |= mask;
	}
}

/* Pick the smallest prescaler that fits the period.
 *
 * TIM2 on the F401 is a 32-bit counter, so div is 1 for every rate this
 * firmware accepts (4 s at 84 MHz is 336e6 ticks, well inside 2^32) and one
 * tick is 11.9 ns.  The division is kept rather than assumed away so the HSI
 * fallback and any future timer change stay correct -- the same reason the H7
 * build keeps it. */
static int tim2_program(void)
{
	uint64_t pt = ((uint64_t)g_period_ns * g_timer_hz) / 1000000000ull;
	uint32_t div, arr;
	unsigned i;

	if (pt < 2)
		return 0;

	div = (uint32_t)((pt + 0xFFFFFFFFull) / 0x100000000ull);
	if (div == 0)
		div = 1;
	if (div > 65536u)
		return 0;

	arr = (uint32_t)(pt / div);
	if (arr == 0)
		return 0;
	arr -= 1;

	TIM2_PSC = div - 1u;
	TIM2_ARR = arr;

	for (i = 0; i < NCH; i++) {
		uint64_t t = ((uint64_t)g_pulse_ns[i] * g_timer_hz)
			     / 1000000000ull / div;
		uint32_t ccr = (uint32_t)t;

		if (ccr == 0)
			ccr = 1;		/* never collapse to no pulse */
		if (ccr > arr)
			return 0;
		TIM2_CCR(i) = g_running ? ccr : 0;
	}

	TIM2_EGR = 1u;				/* UG: latch PSC/ARR/CCRx now */
	TIM2_SR  = 0;
	return 1;
}

static void trig_start(void)
{
	unsigned i;

	g_running = 1;
	if (!tim2_program()) {
		g_running = 0;
		return;
	}
	if (g_pwm_started)
		return;				/* already emitting; CCRs updated */

	TIM2_CNT = 0;
	TIM2_SR  = 0;
	for (i = 0; i < NCH; i++)
		TIM2_CCER |= (1u << (i * 4));	/* CCxE */
	TIM2_CR1 |= (1u << 0);			/* CEN */
	g_pwm_started = 1;
}

static void trig_stop(void)
{
	unsigned i;

	for (i = 0; i < NCH; i++)
		TIM2_CCR(i) = 0;
	TIM2_EGR = 1u;				/* park idle before halting */
	if (g_pwm_started) {
		for (i = 0; i < NCH; i++)
			TIM2_CCER &= ~(1u << (i * 4));
		TIM2_CR1 &= ~(1u << 0);
		g_pwm_started = 0;
	}
	g_running = 0;
	g_burst_left = 0;
	led_on();			/* solid: alive but not triggering */
}

void TIM2_IRQHandler(void)
{
	if (!(TIM2_SR & 1u))
		return;
	TIM2_SR = ~1u;

	g_pulses++;

	if (++g_led_count >= g_led_div) {
		g_led_count = 0;
		GPIOC_BSRR = (GPIOC_ODR_BIT() ? (1u << (LED_PIN + 16))
					      : (1u << LED_PIN));
	}

	if (g_burst_left) {
		if (--g_burst_left == 0) {
			unsigned i;

			for (i = 0; i < NCH; i++)
				TIM2_CCR(i) = 0;
			TIM2_EGR = 1u;
			for (i = 0; i < NCH; i++)
				TIM2_CCER &= ~(1u << (i * 4));
			TIM2_CR1 &= ~(1u << 0);
			g_pwm_started = 0;
			g_running = 0;
			GPIOC_BSRR = 1u << (LED_PIN + 16);   /* burst done: solid */
		}
	}
}

/* ------------------------------------------------------------------ *
 * Limits -- refuse, do not silently clamp.  Same two checks as the H7.
 * ------------------------------------------------------------------ */
static const char *pulse_for(uint32_t exp_us, uint32_t period_ns, uint32_t *out)
{
	uint64_t p = (uint64_t)exp_us * 1000ull;

	/* The sensor adds a fixed offset to whatever pulse it sees, and the
	 * optocoupler adds its own turn-on/turn-off asymmetry on top -- the
	 * latter is a property of the module, measured once and set with
	 * `skew`.  Both come off the requested exposure. */
	if (p <= (uint64_t)IMX296_TOFFSET_NS + g_opto_skew_ns + IMX296_MIN_LOW_NS)
		return "exposure below what the sensor and optocoupler can resolve";
	p -= IMX296_TOFFSET_NS;
	p -= g_opto_skew_ns;

	if (p + READOUT_MARGIN_NS > (uint64_t)period_ns)
		return "exposure leaves no readout margin before the next trigger";

	*out = (uint32_t)p;
	return 0;
}

static const char *check_period(uint32_t period_ns)
{
	if (period_ns < IMX296_MIN_PERIOD_NS)
		return "period below tTGPD (16.681 ms / 59.95 fps max)";
	if (period_ns > MAX_PERIOD_NS)
		return "period above 4 s (0.25 fps min)";
	return 0;
}

#define APPLY_ALL	(-1)
#define APPLY_KEEP	(-2)

static void apply(int which, uint32_t period_ns, uint32_t exp_us)
{
	uint32_t new_pulse[NCH];
	uint32_t new_exp[NCH];
	const char *err;
	unsigned i;

	err = check_period(period_ns);
	if (err) {
		out_puts("err ");
		out_puts(err);
		out_putc('\n');
		return;
	}

	for (i = 0; i < NCH; i++) {
		new_exp[i] = g_exp_us[i];
		if (which == APPLY_ALL || (int)i == which)
			new_exp[i] = exp_us;

		err = pulse_for(new_exp[i], period_ns, &new_pulse[i]);
		if (err) {
			out_puts("err ch");
			out_putu(i + 1);
			out_putc(' ');
			out_puts(err);
			out_putc('\n');
			return;
		}
	}

	/* Nothing above touched live state, so a rejected request leaves the
	 * timer exactly as it was -- the H7 has the same property and the rig
	 * depends on it: a bad command must not stop the cameras. */
	g_period_ns = period_ns;
	/* Half a second's worth of frames, so the blink stays near 1 Hz from
	 * 60 fps down to 0.25 fps instead of looking dead at low rates. */
	g_led_div = (uint32_t)(500000000ull / period_ns);
	if (!g_led_div)
		g_led_div = 1;
	for (i = 0; i < NCH; i++) {
		g_exp_us[i]   = new_exp[i];
		g_pulse_ns[i] = new_pulse[i];
	}

	if (!tim2_program()) {
		out_puts("err timing does not fit the timer\n");
		return;
	}
	out_puts("ok\n");
}

/* ------------------------------------------------------------------ *
 * Command surface.  Grammar identical to the H7 build minus `dfu` and `bl`,
 * so tools/j106-trigctl.py needs no change.
 * ------------------------------------------------------------------ */
static int str_eq(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return *a == 0 && *b == 0;
}

/* Split at the first space: `line` keeps the verb, the return value is the
 * rest.  Returns a pointer to the trailing NUL when there is no argument, so
 * callers can always dereference it. */
static char *split(char *line)
{
	while (*line && *line != ' ')
		line++;
	if (!*line)
		return line;
	*line++ = 0;
	while (*line == ' ')
		line++;
	return line;
}

static uint32_t parse_u32(const char *s)
{
	uint64_t v = 0;
	int any = 0;

	if (!s || !*s)
		return BAD_U32;
	while (*s) {
		if (*s < '0' || *s > '9')
			return BAD_U32;
		v = v * 10u + (uint32_t)(*s - '0');
		if (v > 0xFFFFFFFEull)
			return BAD_U32;
		any = 1;
		s++;
	}
	return any ? (uint32_t)v : BAD_U32;
}

/* "30" -> 30000, "59.94" -> 59940.  Three decimals, matching the H7's
 * fps_milli so `fps 59.94` means the same thing on both. */
static uint32_t parse_milli(const char *s)
{
	uint64_t whole = 0, frac = 0;
	int scale = 1000, seen = 0;

	if (!s || !*s)
		return 0;
	while (*s >= '0' && *s <= '9') {
		whole = whole * 10u + (uint32_t)(*s++ - '0');
		if (whole > 100000ull)
			return 0;
		seen = 1;
	}
	if (*s == '.') {
		s++;
		while (*s >= '0' && *s <= '9') {
			if (scale > 1) {
				scale /= 10;
				frac = frac * 10u + (uint32_t)(*s - '0');
			}
			s++;
			seen = 1;
		}
	}
	if (*s || !seen)
		return 0;
	while (scale > 1) {
		frac *= 10u;
		scale /= 10;
	}
	return (uint32_t)(whole * 1000ull + frac);
}

static void cmd_status(void)
{
	unsigned i;

	out_puts("clock=");
	out_puts(g_clock_name);
	out_putc('\n');
	out_kv("timer_hz", g_timer_hz);
	out_kv("running", (uint32_t)g_running);
	out_kv("period_us", g_period_ns / 1000u);
	out_kv("fps_milli", (uint32_t)(1000000000000ull / g_period_ns));
	out_puts("polarity=");
	out_puts(g_active_high ? "active_high (idle LED off)\n"
			       : "active_low (idle LED ON)\n");
	out_kv("opto_skew_ns", g_opto_skew_ns);
	for (i = 0; i < NCH; i++) {
		out_puts("ch");
		out_putu(i + 1);
		out_puts("_exposure_us=");
		out_putu(g_exp_us[i]);
		out_puts(" pulse_ns=");
		out_putu(g_pulse_ns[i]);
		out_puts(" ccr=");
		out_putu(TIM2_CCR(i));
		out_putc('\n');
	}
	out_kv("psc", TIM2_PSC);
	out_kv("arr", TIM2_ARR);
	/* One timer tick in ns -- the H7 does not print this because there it
	 * is always 8.33 ns.  Here it moves with the period and it is the
	 * number that tells you your pulse-width granularity. */
	out_kv("tick_ns", (uint32_t)((1000000000ull * (TIM2_PSC + 1u))
				     / g_timer_hz));
	out_kv("pulses", g_pulses);
	out_kv("burst_left", g_burst_left);
	out_puts("ok\n");
}

static void cmd_help(void)
{
	out_puts("IMX296 trigger generator (J106/TX2) -- STM32F401 stopgap\n"
		 "TIM2_CH1..4 on PA0-PA3 -> optocoupler LEDs\n"
		 "  fps <v>        frame rate, e.g. 30 or 59.94 (max 59.95)\n"
		 "  period <us>    frame period directly\n"
		 "  exp <us>       exposure, all four cameras\n"
		 "  exp <ch> <us>  exposure for one camera, ch = 1..4\n"
		 "  pol <0|1>      1 = pulse drives the LED on (default)\n"
		 "                 0 = inverted, if the module asserts on LED off\n"
		 "  skew <ns>      optocoupler on/off delay asymmetry, removed\n"
		 "                 from the pulse; measure once, then set\n"
		 "  start | stop\n"
		 "  burst <n>      emit n pulses then stop\n"
		 "  dfu            reboot into the ROM bootloader to reflash\n"
		 "  status | help\n"
		 "no bl command here: this board has no LCD\n"
		 "ok\n");
}

static void handle(char *line)
{
	char *arg = split(line);

	if (!*line)
		return;

	if (str_eq(line, "help")) {
		cmd_help();
	} else if (str_eq(line, "status")) {
		cmd_status();
	} else if (str_eq(line, "start")) {
		trig_start();
		out_puts(g_running ? "ok\n"
				   : "err cannot start with current settings\n");
	} else if (str_eq(line, "stop")) {
		trig_stop();
		out_puts("ok\n");
	} else if (str_eq(line, "fps")) {
		uint32_t m = parse_milli(arg);

		if (!m)
			out_puts("err bad fps\n");
		else
			apply(APPLY_KEEP, (uint32_t)(1000000000000ull / m), 0);
	} else if (str_eq(line, "period")) {
		uint32_t v = parse_u32(arg);

		if (v == BAD_U32 || v == 0)
			out_puts("err bad period\n");
		else
			apply(APPLY_KEEP, v * 1000u, 0);
	} else if (str_eq(line, "exp")) {
		char *second = split(arg);
		uint32_t a = parse_u32(arg);

		if (*second) {				/* "exp <ch> <us>" */
			uint32_t v = parse_u32(second);

			if (a < 1 || a > NCH || v == BAD_U32)
				out_puts("err usage: exp <1-4> <us>\n");
			else
				apply((int)a - 1, g_period_ns, v);
		} else if (a == BAD_U32) {
			out_puts("err bad exposure\n");
		} else {
			apply(APPLY_ALL, g_period_ns, a);
		}
	} else if (str_eq(line, "pol")) {
		uint32_t v = parse_u32(arg);

		if (v > 1) {
			out_puts("err usage: pol <0|1>\n");
		} else {
			set_polarity((int)v);
			out_puts("ok\n");
		}
	} else if (str_eq(line, "skew")) {
		uint32_t v = parse_u32(arg);

		if (v == BAD_U32 || v > 1000000u) {
			out_puts("err usage: skew <ns, 0..1000000>\n");
		} else {
			g_opto_skew_ns = v;
			apply(APPLY_KEEP, g_period_ns, 0);
		}
	} else if (str_eq(line, "dfu")) {
		/* Reply and drain before resetting: the host must see the "ok"
		 * before the device drops off the bus, or `make flash` cannot
		 * tell "entering bootloader" from "died". */
		out_puts("ok entering bootloader\n");
		/* The host must see the "ok" before the device drops off the
		 * bus, or `make flash` cannot tell "entering bootloader" from
		 * "died".  Drain the UART shift register and give USB enough
		 * poll cycles to push the reply out. */
		usb_cdc_flush();
		while (!(USART1_SR & (1u << 6)))	/* TC: last bit clear */
			;
		request_bootloader();
		return;					/* not reached */
	} else if (str_eq(line, "burst")) {
		uint32_t n = parse_u32(arg);

		if (n == BAD_U32 || n == 0) {
			out_puts("err bad count\n");
		} else {
			g_pulses = 0;
			g_burst_left = n;
			trig_start();
			out_puts(g_running ? "ok\n" : "err cannot start\n");
		}
	} else {
		out_puts("err unknown command (try help)\n");
	}
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */
/* One line assembler per transport: a half-typed command on the UART must not
 * be corrupted by a command arriving over USB at the same time.  The H7 gets
 * this from two FreeRTOS tasks; here it is two buffers and one loop. */
struct rxline {
	char     buf[64];
	unsigned n;
	int      sink;
};

static void feed(struct rxline *l, char c)
{
	if (c == '\r' || c == '\n') {
		l->buf[l->n] = 0;
		if (l->n) {
			g_sink = l->sink;
			handle(l->buf);
			g_sink = SINK_ALL;
		}
		l->n = 0;
	} else if (c == 8 || c == 127) {
		if (l->n)
			l->n--;
	} else if (l->n < sizeof l->buf - 1) {
		l->buf[l->n++] = c;
	}
}

int main(void)
{
	static struct rxline uart_line, usb_line;
	uint32_t usb_spin = 0, usb_dump_at = 400000u, usb_dumps_left = 14;
	unsigned i;

	clock_init();
	uart_init();
	led_init();
	led_on();			/* lit from the first instant of life */
	tim2_init();
	set_polarity(1);

	uart_line.sink = SINK_UART;
	usb_line.sink  = SINK_USB;

	/* Start triggering on power-up with the compiled-in defaults, exactly
	 * as the H7 build does: the rig must work with no host attached. */
	for (i = 0; i < NCH; i++)
		g_exp_us[i] = DEFAULT_EXP_US;
	for (i = 0; i < NCH; i++)
		if (pulse_for(g_exp_us[i], g_period_ns, &g_pulse_ns[i]))
			g_pulse_ns[i] = 0;
	trig_start();

	/* USB comes up after the trigger deliberately: enumeration takes the
	 * host a moment, and the cameras should already be running by then. */
	usb_cdc_init();

	/* The full report goes out unprompted at boot, not just a banner.
	 * With RX unwired -- or any half-working link -- a transmit-only board
	 * still tells you the one thing you cannot infer from the LED: whether
	 * the crystal started.  `clock=hsi` means the frame rate is only as
	 * good as a ~1% internal oscillator. */
	g_sink = SINK_UART;
	out_puts("\ncamtrig-f401 ready (30.000 fps, 5000 us, pol 1)\n");
	cmd_status();
	out_puts("type 'help' for commands\n");
	g_sink = SINK_ALL;

	for (;;) {
		char c;

		usb_cdc_poll();

		/* One-shot USB core dump once the host has had time to try
		 * enumerating.  Costs nothing after it has fired, and turns a
		 * silent "device descriptor read error" on the host side into
		 * something with actual register values behind it. */
		if (usb_dumps_left && ++usb_spin >= usb_dump_at) {
			uint32_t d[6], k[6];

			usb_spin = 0;
			usb_dumps_left--;
			usb_cdc_debug(d);
			g_sink = SINK_UART;
			out_puts("usb gintsts=");   out_hex(d[0]);
			out_puts(" dsts=");         out_hex(d[1]);
			out_puts(" gccfg=");        out_hex(d[2]);
			out_puts("\nusb diepctl0="); out_hex(d[3]);
			out_puts(" doepint0=");     out_hex(d[4]);
			out_puts(" diepint0=");     out_hex(d[5]);
			usb_cdc_counters(k);
			out_puts("\nusb rst=");  out_putu(k[0]);
			out_puts(" enum=");       out_putu(k[1]);
			out_puts(" setup=");      out_putu(k[2]);
			out_puts(" rxpkt=");      out_putu(k[3]);
			out_puts(" inxfrc=");     out_putu(k[4]);
			out_puts(" lastpkt=");    out_putu(k[5]);
			usb_cdc_setup_snapshot(k);
			out_puts("\nusb setup0=");  out_hex(k[0]);
			out_putc(' ');              out_hex(k[1]);
			out_puts(" cnt=");          out_putu(k[2]);
			out_puts(" replylen=");     out_putu(k[3]);
			out_puts("\n\n");
			g_sink = SINK_ALL;
		}

		if (USART1_SR & (1u << 5))	/* RXNE */
			feed(&uart_line, (char)(USART1_DR & 0xFFu));

		if (usb_cdc_getc(&c))
			feed(&usb_line, c);
	}
}
