/* main.c — IMX296 external-trigger generator for the J106/TX2 4-camera rig.
 *
 * Drives XTRIG on PE9 (TIM1_CH1, header P2-32) as an idle-high waveform with
 * one active-low pulse per frame.  In the IMX296's Fast Trigger mode the low
 * pulse width IS the exposure time, so the pulse is produced entirely by
 * TIM1 hardware and never by software timing.
 *
 * Wiring, level translation and bring-up: ../WIRING.md
 */
#include <stdint.h>
#include "hw.h"

/* ------------------------------------------------------------------ *
 * Sensor limits.  Every one of these is a datasheet constant, kept in
 * physical units so the arithmetic below stays auditable against
 * IMX296LQR-C_Fulldatasheet_Awin.pdf rather than being pre-baked ticks.
 * ------------------------------------------------------------------ */
#define IMX296_1H_NS		14815u		/* HMAX 1100 / 74.25 MHz  = 14.8148 us */
#define IMX296_TTGPD_H		1126u		/* fast trigger, all-pixel readout     */
#define IMX296_MIN_PERIOD_NS	(IMX296_1H_NS * IMX296_TTGPD_H)	/* 16.68 ms => 59.95 fps */
#define IMX296_TOFFSET_NS	14260u		/* t_exp = t_low + 14.26 us            */
#define IMX296_MIN_LOW_NS	50u		/* tTGSE                               */

#define READOUT_MARGIN_NS	1000000u	/* keep 1 ms of daylight before the
						 * next falling edge                   */
#define MAX_PERIOD_NS		4000000000u	/* u32 ceiling; 0.25 fps               */

/* Boot defaults — chosen so the rig triggers with no host attached. */
#define DEFAULT_FPS_MILLI	30000u		/* 30.000 fps */
#define DEFAULT_EXP_US		5000u		/* 5 ms       */

#define HSE_HZ			25000000u	/* WeAct MiniSTM32H7xx crystal */
#define HSI_HZ			64000000u	/* reset clock, fallback       */
#define BAUD			115200u

#define LED_PIN			3		/* PE3, blue, active low on this board */
#define TRIG_PIN		9		/* PE9 = TIM1_CH1, AF1                 */

/* ------------------------------------------------------------------ *
 * State
 * ------------------------------------------------------------------ */
static uint32_t g_timer_hz;		/* also the USART kernel clock */
static int      g_on_hse;
static uint32_t g_period_ns = 1000000000u / (DEFAULT_FPS_MILLI / 1000u);
static uint32_t g_low_ns;
static uint32_t g_exp_us = DEFAULT_EXP_US;
static int      g_running;
static uint32_t g_pulses;
static uint32_t g_burst_left;		/* 0 = free running */

/* ------------------------------------------------------------------ *
 * Clock
 *
 * HSE 25 MHz straight to SYSCLK, no PLL.  Both this and the HSI fallback
 * are at or below the reset clock (HSI 64 MHz), so the reset values of
 * FLASH_ACR and PWR VOS remain valid and are deliberately never written —
 * under-provisioned flash wait states are the classic way to brick a
 * first bring-up, and skipping the write removes that failure entirely.
 * ------------------------------------------------------------------ */
static void clock_init(void)
{
	volatile uint32_t timeout;

	/* All domain prescalers /1: SYSCLK = HCLK = PCLK2 = timer clock.
	 * These are the reset values; written explicitly so the timer clock
	 * does not depend on what a bootloader left behind. */
	RCC_D1CFGR = 0;
	RCC_D2CFGR = 0;

	RCC_CR |= RCC_CR_HSEON;
	for (timeout = 0; timeout < 2000000u; timeout++) {
		if (RCC_CR & RCC_CR_HSERDY)
			break;
	}

	if (RCC_CR & RCC_CR_HSERDY) {
		RCC_CFGR = (RCC_CFGR & ~RCC_CFGR_SW_MSK) | RCC_CFGR_SW_HSE;
		while (((RCC_CFGR >> RCC_CFGR_SWS_SH) & RCC_CFGR_SW_MSK) !=
		       RCC_CFGR_SW_HSE)
			;
		g_timer_hz = HSE_HZ;
		g_on_hse = 1;
	} else {
		/* Crystal did not start.  Stay on HSI: 1% instead of 20 ppm,
		 * which costs absolute rate accuracy but not synchronisation,
		 * since every camera shares this one net.  Reported by
		 * `status` so it cannot pass unnoticed. */
		RCC_CR &= ~RCC_CR_HSEON;
		g_timer_hz = HSI_HZ;
		g_on_hse = 0;
	}
}

/* ------------------------------------------------------------------ *
 * GPIO
 * ------------------------------------------------------------------ */
static void gpio_init(void)
{
	RCC_AHB4ENR |= RCC_AHB4ENR_GPIOAEN | RCC_AHB4ENR_GPIOEEN;

	/* PE9 -> AF1 (TIM1_CH1), push-pull, medium speed.
	 * Medium rather than high on purpose: the trigger fans out to four
	 * stubs on flying leads, and a slower edge rings less.  40 ns of
	 * timer resolution swamps the difference in exposure accuracy. */
	GPIO_MODER(GPIOE_BASE) &= ~(3UL << (TRIG_PIN * 2));
	GPIO_MODER(GPIOE_BASE) |= GPIO_MODE_AF << (TRIG_PIN * 2);
	GPIO_OTYPER(GPIOE_BASE) &= ~(1UL << TRIG_PIN);
	GPIO_OSPEEDR(GPIOE_BASE) &= ~(3UL << (TRIG_PIN * 2));
	GPIO_OSPEEDR(GPIOE_BASE) |= 1UL << (TRIG_PIN * 2);
	GPIO_PUPDR(GPIOE_BASE) &= ~(3UL << (TRIG_PIN * 2));
	GPIO_PUPDR(GPIOE_BASE) |= 1UL << (TRIG_PIN * 2);	/* pull-up: idle
								 * high even before
								 * TIM1 drives */
	GPIO_AFRH(GPIOE_BASE) &= ~(0xFUL << ((TRIG_PIN - 8) * 4));
	GPIO_AFRH(GPIOE_BASE) |= 1UL << ((TRIG_PIN - 8) * 4);	/* AF1 */

	/* PE3 -> output (LED) */
	GPIO_MODER(GPIOE_BASE) &= ~(3UL << (LED_PIN * 2));
	GPIO_MODER(GPIOE_BASE) |= GPIO_MODE_OUT << (LED_PIN * 2);
	GPIO_BSRR(GPIOE_BASE) = 1UL << LED_PIN;			/* off (active low) */

	/* PA9 / PA10 -> AF7 (USART1) */
	GPIO_MODER(GPIOA_BASE) &= ~((3UL << (9 * 2)) | (3UL << (10 * 2)));
	GPIO_MODER(GPIOA_BASE) |= (GPIO_MODE_AF << (9 * 2)) |
				  (GPIO_MODE_AF << (10 * 2));
	GPIO_AFRH(GPIOA_BASE) &= ~((0xFUL << ((9 - 8) * 4)) |
				   (0xFUL << ((10 - 8) * 4)));
	GPIO_AFRH(GPIOA_BASE) |= (7UL << ((9 - 8) * 4)) |
				 (7UL << ((10 - 8) * 4));
}

/* ------------------------------------------------------------------ *
 * USART1 — polled, no interrupts, no buffering
 * ------------------------------------------------------------------ */
static void uart_init(void)
{
	RCC_APB2ENR |= RCC_APB2ENR_USART1EN;
	USART1_CR1 = 0;
	USART1_BRR = (g_timer_hz + BAUD / 2) / BAUD;	/* OVER8 = 0 */
	USART1_CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

static void uart_putc(char c)
{
	while (!(USART1_ISR & USART_ISR_TXE))
		;
	USART1_TDR = (uint8_t)c;
}

static void uart_puts(const char *s)
{
	while (*s) {
		if (*s == '\n')
			uart_putc('\r');
		uart_putc(*s++);
	}
}

static void uart_putu(uint32_t v)
{
	char buf[11];
	int i = 0;

	if (!v) {
		uart_putc('0');
		return;
	}
	while (v) {
		buf[i++] = (char)('0' + v % 10);
		v /= 10;
	}
	while (i)
		uart_putc(buf[--i]);
}

/* key=value line, integer value */
static void uart_kv(const char *k, uint32_t v)
{
	uart_puts(k);
	uart_putc('=');
	uart_putu(v);
	uart_putc('\n');
}

/* ------------------------------------------------------------------ *
 * TIM1 — inverted PWM on CH1
 *
 * PWM mode 1 drives the channel "active" while CNT < CCR1.  With CC1P set
 * (active low) that means: LOW for CCR1 ticks at the start of every period,
 * HIGH for the rest.  CCR1 = 0 therefore parks the pin HIGH, which is what
 * stop() wants — the pin is never frozen mid-pulse.
 * ------------------------------------------------------------------ */
static void tim1_init(void)
{
	RCC_APB2ENR |= RCC_APB2ENR_TIM1EN;

	TIM1_CR1 = TIM_CR1_ARPE;
	TIM1_CCMR1 = TIM_CCMR1_OC1M_PWM1 | TIM_CCMR1_OC1PE;
	TIM1_CCER = TIM_CCER_CC1E | TIM_CCER_CC1P;
	TIM1_CCR1 = 0;
	TIM1_BDTR = TIM_BDTR_MOE;	/* advanced timer: outputs stay off without this */
}

/* Translate the requested period/pulse into PSC + ARR + CCR1.
 * ARR is 16-bit on TIM1, so the prescaler is chosen at runtime as the
 * smallest divider that makes the period fit — keeping the finest
 * resolution the requested rate allows. */
static int tim1_program(uint32_t period_ns, uint32_t low_ns)
{
	uint64_t pt = ((uint64_t)period_ns * g_timer_hz) / 1000000000ull;
	uint64_t lt;
	uint32_t div, arr, ccr;

	if (pt < 2)
		return 0;

	div = (uint32_t)((pt + 65535ull) / 65536ull);
	if (div == 0)
		div = 1;
	if (div > 65536u)
		return 0;

	arr = (uint32_t)(pt / div);
	if (arr == 0)
		return 0;
	arr -= 1;

	lt = ((uint64_t)low_ns * g_timer_hz) / 1000000000ull / div;
	ccr = (uint32_t)lt;
	if (ccr == 0)
		ccr = 1;		/* never collapse to "no pulse" */
	if (ccr > arr)
		return 0;

	TIM1_PSC = div - 1;
	TIM1_ARR = arr;
	TIM1_CCR1 = g_running ? ccr : 0;
	TIM1_EGR = TIM_EGR_UG;		/* latch PSC immediately */
	TIM1_SR = 0;
	return 1;
}

static void trig_start(void)
{
	g_running = 1;
	if (!tim1_program(g_period_ns, g_low_ns)) {
		g_running = 0;
		return;
	}
	TIM1_CNT = 0;
	TIM1_SR = 0;
	TIM1_CR1 |= TIM_CR1_CEN;
}

static void trig_stop(void)
{
	TIM1_CCR1 = 0;			/* park high before the timer halts */
	TIM1_EGR = TIM_EGR_UG;
	TIM1_CR1 &= ~TIM_CR1_CEN;
	g_running = 0;
	g_burst_left = 0;
	GPIO_BSRR(GPIOE_BASE) = 1UL << LED_PIN;		/* LED off */
}

/* ------------------------------------------------------------------ *
 * Limit checks — the spec's "refuse, do not silently clamp"
 * ------------------------------------------------------------------ */
static const char *check_and_stage(uint32_t period_ns, uint32_t exp_us,
				   uint32_t *out_low_ns)
{
	uint64_t low;

	if (period_ns < IMX296_MIN_PERIOD_NS)
		return "period below tTGPD (16.681 ms / 59.95 fps max)";
	if (period_ns > MAX_PERIOD_NS)
		return "period too long (max 4 s)";

	low = (uint64_t)exp_us * 1000ull;
	if (low <= IMX296_TOFFSET_NS + IMX296_MIN_LOW_NS)
		return "exposure below sensor minimum (~14.31 us)";
	low -= IMX296_TOFFSET_NS;

	if (low + READOUT_MARGIN_NS > (uint64_t)period_ns)
		return "exposure leaves no readout margin before next trigger";

	*out_low_ns = (uint32_t)low;
	return 0;
}

static void apply(uint32_t period_ns, uint32_t exp_us)
{
	uint32_t low_ns;
	const char *err = check_and_stage(period_ns, exp_us, &low_ns);

	if (err) {
		uart_puts("err ");
		uart_puts(err);
		uart_putc('\n');
		return;
	}
	g_period_ns = period_ns;
	g_exp_us = exp_us;
	g_low_ns = low_ns;
	if (!tim1_program(g_period_ns, g_low_ns)) {
		uart_puts("err timer cannot represent that period\n");
		return;
	}
	uart_puts("ok\n");
}

/* ------------------------------------------------------------------ *
 * Command parsing
 * ------------------------------------------------------------------ */
static int str_eq(const char *a, const char *b)
{
	while (*a && *b) {
		if (*a++ != *b++)
			return 0;
	}
	return *a == *b;
}

/* Parses "30" or "59.94" into thousandths.  Returns 0 on garbage. */
static uint32_t parse_milli(const char *s)
{
	uint32_t whole = 0, frac = 0, scale = 1000;
	int seen = 0;

	while (*s >= '0' && *s <= '9') {
		whole = whole * 10 + (uint32_t)(*s++ - '0');
		seen = 1;
	}
	if (*s == '.') {
		s++;
		while (*s >= '0' && *s <= '9') {
			if (scale > 1) {
				scale /= 10;
				frac += (uint32_t)(*s - '0') * scale;
			}
			s++;
			seen = 1;
		}
	}
	if (!seen || *s)
		return 0;
	return whole * 1000 + frac;
}

static uint32_t parse_u32(const char *s)
{
	uint32_t v = 0;
	int seen = 0;

	while (*s >= '0' && *s <= '9') {
		v = v * 10 + (uint32_t)(*s++ - '0');
		seen = 1;
	}
	if (!seen || *s)
		return 0xFFFFFFFFu;
	return v;
}

static void cmd_status(void)
{
	uart_puts("clock=");
	uart_puts(g_on_hse ? "hse25\n" : "hsi64-FALLBACK\n");
	uart_kv("timer_hz", g_timer_hz);
	uart_kv("running", (uint32_t)g_running);
	uart_kv("period_us", g_period_ns / 1000u);
	uart_kv("fps_milli", (uint32_t)(1000000000000ull / g_period_ns));
	uart_kv("exposure_us", g_exp_us);
	uart_kv("pulse_low_ns", g_low_ns);
	uart_kv("psc", TIM1_PSC);
	uart_kv("arr", TIM1_ARR);
	uart_kv("ccr1", TIM1_CCR1);
	uart_kv("pulses", g_pulses);
	uart_kv("burst_left", g_burst_left);
	uart_puts("ok\n");
}

static void cmd_help(void)
{
	uart_puts("IMX296 trigger generator (J106/TX2) - TIM1_CH1 on PE9\n"
		  "  fps <v>      frame rate, e.g. 30 or 59.94 (max 59.95)\n"
		  "  period <us>  frame period directly\n"
		  "  exp <us>     exposure; sensor adds 14.26 us to the pulse\n"
		  "  start | stop\n"
		  "  burst <n>    emit n pulses then stop\n"
		  "  status       key=value report\n"
		  "  help\n"
		  "ok\n");
}

static void handle(char *line)
{
	char *arg = line;

	while (*arg && *arg != ' ')
		arg++;
	if (*arg == ' ')
		*arg++ = 0;

	if (!*line)
		return;

	if (str_eq(line, "help")) {
		cmd_help();
	} else if (str_eq(line, "status")) {
		cmd_status();
	} else if (str_eq(line, "start")) {
		trig_start();
		uart_puts(g_running ? "ok\n" : "err cannot start with current settings\n");
	} else if (str_eq(line, "stop")) {
		trig_stop();
		uart_puts("ok\n");
	} else if (str_eq(line, "fps")) {
		uint32_t m = parse_milli(arg);

		if (!m)
			uart_puts("err bad fps\n");
		else
			apply((uint32_t)(1000000000000ull / m), g_exp_us);
	} else if (str_eq(line, "period")) {
		uint32_t us = parse_u32(arg);

		if (us == 0xFFFFFFFFu || us == 0)
			uart_puts("err bad period\n");
		else
			apply(us * 1000u, g_exp_us);
	} else if (str_eq(line, "exp")) {
		uint32_t us = parse_u32(arg);

		if (us == 0xFFFFFFFFu)
			uart_puts("err bad exposure\n");
		else
			apply(g_period_ns, us);
	} else if (str_eq(line, "burst")) {
		uint32_t n = parse_u32(arg);

		if (n == 0xFFFFFFFFu || n == 0) {
			uart_puts("err bad count\n");
		} else {
			g_pulses = 0;
			g_burst_left = n;
			trig_start();
			uart_puts(g_running ? "ok\n" : "err cannot start\n");
		}
	} else {
		uart_puts("err unknown command (try help)\n");
	}
}

/* ------------------------------------------------------------------ *
 * Main
 * ------------------------------------------------------------------ */
int main(void)
{
	static char line[64];
	unsigned len = 0;

	clock_init();
	gpio_init();
	uart_init();
	tim1_init();

	/* Stage the boot defaults, then run.  Triggering without a host
	 * attached is deliberate: the serial link is optional. */
	g_period_ns = (uint32_t)(1000000000000ull / DEFAULT_FPS_MILLI);
	check_and_stage(g_period_ns, DEFAULT_EXP_US, &g_low_ns);
	trig_start();

	uart_puts("\ncamtrig ready - type help\n");
	cmd_status();

	for (;;) {
		/* Frame accounting: TIM1 sets UIF once per period. */
		if (TIM1_SR & TIM_SR_UIF) {
			TIM1_SR = ~TIM_SR_UIF;
			g_pulses++;

			if ((g_pulses % 15u) == 0)
				GPIO_BSRR(GPIOE_BASE) =
					(g_pulses % 30u) ? (1UL << LED_PIN)
							 : (1UL << (LED_PIN + 16));

			if (g_burst_left) {
				if (--g_burst_left == 0) {
					trig_stop();
					uart_puts("burst done pulses=");
					uart_putu(g_pulses);
					uart_putc('\n');
				}
			}
		}

		if (USART1_ISR & USART_ISR_RXNE) {
			char c = (char)(USART1_RDR & 0xFF);

			if (c == '\r' || c == '\n') {
				line[len] = 0;
				if (len)
					handle(line);
				len = 0;
			} else if (len < sizeof(line) - 1) {
				line[len++] = c;
			}
		}
	}
}
