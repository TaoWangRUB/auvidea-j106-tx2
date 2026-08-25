/* main.c — IMX296 external-trigger generator for the J106/TX2 4-camera rig.
 *
 * The camera modules' XTR+/XTR- pads are an OPTOCOUPLER LED, galvanically
 * isolated from the module's ground (measured: 1.2 V forward one way, open the
 * other, and both legs open to a verified module ground).  So this drives
 * current through four LEDs, not voltage into four CMOS inputs:
 *
 *     PEx --[ R ]--> LED anode ... LED cathode --> this board's GND
 *
 * Nothing connects to the cameras' ground.  One TIM1 channel per camera, all
 * four sharing one counter, so the frame start is identical across cameras by
 * construction while each can carry its own exposure.
 *
 * In the IMX296's Fast Trigger mode the asserted pulse width IS the exposure
 * time, so the pulse is produced entirely by TIM1 hardware and never by a
 * software loop.
 *
 * Wiring, resistor values and bring-up: ../WIRING.md
 */
#include <stdint.h>
#include "hw.h"

/* ------------------------------------------------------------------ *
 * Sensor limits.  Datasheet constants in physical units, so they can be
 * checked against IMX296LQR-C_Fulldatasheet_Awin.pdf directly rather than
 * being pre-baked into tick counts.
 * ------------------------------------------------------------------ */
#define IMX296_1H_NS		14815u		/* HMAX 1100 / 74.25 MHz = 14.8148 us */
#define IMX296_TTGPD_H		1126u		/* fast trigger, all-pixel readout    */
#define IMX296_MIN_PERIOD_NS	(IMX296_1H_NS * IMX296_TTGPD_H)	/* 16.68 ms => 59.95 fps */
#define IMX296_TOFFSET_NS	14260u		/* t_exp = t_pulse + 14.26 us         */
#define IMX296_MIN_LOW_NS	50u		/* tTGSE                              */

#define READOUT_MARGIN_NS	1000000u	/* 1 ms of daylight before the next edge */
#define MAX_PERIOD_NS		4000000000u	/* u32 ceiling; 0.25 fps                 */

#define NCH			4		/* one channel per camera */

#define DEFAULT_FPS_MILLI	30000u		/* 30.000 fps */
#define DEFAULT_EXP_US		5000u		/* 5 ms       */

#define HSE_HZ			25000000u	/* WeAct MiniSTM32H7xx crystal */
#define HSI_HZ			64000000u	/* reset clock, fallback       */
#define BAUD			115200u

#define LED_PIN			3		/* PE3, blue, active low on this board */

/* TIM1_CH1..CH4 on PE9 / PE11 / PE13 / PE14, all AF1. Chosen because port E
 * is where this board leaves pins free: PE0/1/4/5/6 are the DCMI connector,
 * PE10/PE12/PE15 the TFT-LCD, PE3 the LED, PA11/PA12 USB. */
static const uint8_t ch_pin[NCH] = { 9, 11, 13, 14 };

/* ------------------------------------------------------------------ *
 * State
 * ------------------------------------------------------------------ */
static uint32_t g_timer_hz;
static int      g_on_hse;
static uint32_t g_period_ns;
static uint32_t g_exp_us[NCH];
static uint32_t g_pulse_ns[NCH];	/* what TIM1 is actually asked for */
static int      g_active_high = 1;	/* see set_polarity() */
static uint32_t g_opto_skew_ns;		/* see apply_one() */
static int      g_running;
static uint32_t g_pulses;
static uint32_t g_burst_left;

/* ------------------------------------------------------------------ *
 * Clock
 *
 * HSE 25 MHz straight to SYSCLK, no PLL.  Both this and the HSI fallback are
 * at or below the reset clock (HSI 64 MHz), so the reset values of FLASH_ACR
 * and PWR VOS stay valid and are deliberately never written - under-provisioned
 * flash wait states are the classic way to brick a first bring-up.
 * ------------------------------------------------------------------ */
static void clock_init(void)
{
	volatile uint32_t timeout;

	RCC_D1CFGR = 0;		/* all domain prescalers /1: SYSCLK = HCLK = PCLK2 */
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
		/* Crystal did not start.  1% instead of 20 ppm: costs absolute
		 * rate accuracy, not synchronisation, since every camera is
		 * driven from this one timer.  Reported by `status`. */
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
	unsigned i;

	RCC_AHB4ENR |= RCC_AHB4ENR_GPIOAEN | RCC_AHB4ENR_GPIOEEN;

	for (i = 0; i < NCH; i++) {
		uint8_t p = ch_pin[i];

		GPIO_MODER(GPIOE_BASE) &= ~(3UL << (p * 2));
		GPIO_MODER(GPIOE_BASE) |= GPIO_MODE_AF << (p * 2);
		GPIO_OTYPER(GPIOE_BASE) &= ~(1UL << p);
		/* High speed: these pins now source ~10 mA into an LED, and a
		 * crisp edge matters because the pulse width is the exposure. */
		GPIO_OSPEEDR(GPIOE_BASE) |= 3UL << (p * 2);
		/* Pull DOWN, not up: idle must be LED-off. If this board is in
		 * reset or unplugged the pins float, and a pull-down keeps the
		 * optos dark rather than holding four sensors inside an
		 * exposure. */
		GPIO_PUPDR(GPIOE_BASE) &= ~(3UL << (p * 2));
		GPIO_PUPDR(GPIOE_BASE) |= 2UL << (p * 2);
		GPIO_AFRH(GPIOE_BASE) &= ~(0xFUL << ((p - 8) * 4));
		GPIO_AFRH(GPIOE_BASE) |= 1UL << ((p - 8) * 4);	/* AF1 */
	}

	GPIO_MODER(GPIOE_BASE) &= ~(3UL << (LED_PIN * 2));
	GPIO_MODER(GPIOE_BASE) |= GPIO_MODE_OUT << (LED_PIN * 2);
	GPIO_BSRR(GPIOE_BASE) = 1UL << LED_PIN;			/* off */

	/* PA9 / PA10 -> AF7 (USART1) */
	GPIO_MODER(GPIOA_BASE) &= ~((3UL << (9 * 2)) | (3UL << (10 * 2)));
	GPIO_MODER(GPIOA_BASE) |= (GPIO_MODE_AF << (9 * 2)) |
				  (GPIO_MODE_AF << (10 * 2));
	GPIO_AFRH(GPIOA_BASE) &= ~((0xFUL << 4) | (0xFUL << 8));
	GPIO_AFRH(GPIOA_BASE) |= (7UL << 4) | (7UL << 8);
}

/* ------------------------------------------------------------------ *
 * USART1 — polled, no interrupts, no buffering
 * ------------------------------------------------------------------ */
static void uart_init(void)
{
	RCC_APB2ENR |= RCC_APB2ENR_USART1EN;
	USART1_CR1 = 0;
	USART1_BRR = (g_timer_hz + BAUD / 2) / BAUD;
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

static void uart_kv(const char *k, uint32_t v)
{
	uart_puts(k);
	uart_putc('=');
	uart_putu(v);
	uart_putc('\n');
}

/* ------------------------------------------------------------------ *
 * TIM1 — four channels, one counter
 *
 * PWM mode 1 drives a channel "active" while CNT < CCRx.  Whether active means
 * the pin is high or low is CCxP, which set_polarity() owns.  CCRx = 0 means
 * never active, so it parks the pin in its idle state - that is what stop()
 * uses, rather than freezing the timer mid-pulse.
 * ------------------------------------------------------------------ */
static volatile uint32_t *ccr_of(unsigned ch)
{
	switch (ch) {
	case 0:  return &TIM1_CCR1;
	case 1:  return &TIM1_CCR2;
	case 2:  return &TIM1_CCR3;
	default: return &TIM1_CCR4;
	}
}

/* Idle state = LED off.
 *
 * The optocoupler makes the sense of the pulse a board fact we cannot read
 * from here: driving the LED might pull the sensor's XTRIG low (assert) or
 * release it, depending on the module's internal wiring.  So polarity is
 * runtime-settable, and the default is the safe one - idle LOW, so an idle or
 * unpowered board draws no LED current and cannot hold the sensors inside an
 * exposure.
 */
static void set_polarity(int active_high)
{
	unsigned i;

	g_active_high = active_high;
	for (i = 0; i < NCH; i++) {
		if (active_high)
			TIM1_CCER &= ~TIM_CCER_CCxP(i);
		else
			TIM1_CCER |= TIM_CCER_CCxP(i);
	}
}

static void tim1_init(void)
{
	unsigned i;

	RCC_APB2ENR |= RCC_APB2ENR_TIM1EN;

	TIM1_CR1 = TIM_CR1_ARPE;
	TIM1_CCMR1 = TIM_CCMR_LO_PWM1 | TIM_CCMR_HI_PWM1;	/* CH1, CH2 */
	TIM1_CCMR2 = TIM_CCMR_LO_PWM1 | TIM_CCMR_HI_PWM1;	/* CH3, CH4 */

	TIM1_CCER = 0;
	for (i = 0; i < NCH; i++) {
		*ccr_of(i) = 0;
		TIM1_CCER |= TIM_CCER_CCxE(i);
	}
	set_polarity(1);

	TIM1_BDTR = TIM_BDTR_MOE;	/* advanced timer: outputs stay off without this */
}

/* ARR is 16-bit on TIM1, so pick the smallest prescaler that fits the period -
 * keeping the finest resolution the requested rate allows (~280 ns at 60 fps)
 * while still reaching below 1 fps. */
static int tim1_program(void)
{
	uint64_t pt = ((uint64_t)g_period_ns * g_timer_hz) / 1000000000ull;
	uint32_t div, arr;
	unsigned i;

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

	TIM1_PSC = div - 1;
	TIM1_ARR = arr;

	for (i = 0; i < NCH; i++) {
		uint64_t t = ((uint64_t)g_pulse_ns[i] * g_timer_hz)
			     / 1000000000ull / div;
		uint32_t ccr = (uint32_t)t;

		if (ccr == 0)
			ccr = 1;		/* never collapse to no pulse */
		if (ccr > arr)
			return 0;
		*ccr_of(i) = g_running ? ccr : 0;
	}

	TIM1_EGR = TIM_EGR_UG;		/* latch PSC/ARR/CCRx immediately */
	TIM1_SR = 0;
	return 1;
}

static void trig_start(void)
{
	g_running = 1;
	if (!tim1_program()) {
		g_running = 0;
		return;
	}
	TIM1_CNT = 0;
	TIM1_SR = 0;
	TIM1_CR1 |= TIM_CR1_CEN;
}

static void trig_stop(void)
{
	unsigned i;

	for (i = 0; i < NCH; i++)
		*ccr_of(i) = 0;		/* park idle before the timer halts */
	TIM1_EGR = TIM_EGR_UG;
	TIM1_CR1 &= ~TIM_CR1_CEN;
	g_running = 0;
	g_burst_left = 0;
	GPIO_BSRR(GPIOE_BASE) = 1UL << LED_PIN;
}

/* ------------------------------------------------------------------ *
 * Limits — refuse, do not silently clamp
 * ------------------------------------------------------------------ */
static const char *pulse_for(uint32_t exp_us, uint32_t period_ns, uint32_t *out)
{
	uint64_t p = (uint64_t)exp_us * 1000ull;

	/* The sensor adds a fixed offset to whatever pulse it sees, and the
	 * optocoupler adds its own turn-on/turn-off asymmetry on top - the
	 * latter is a property of the module, so it is measured once and set
	 * with `skew`.  Both come off the requested exposure. */
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
		return "period too long (max 4 s)";
	return 0;
}

/* ch < 0 -> all channels */
static void apply(int ch, uint32_t period_ns, uint32_t exp_us)
{
	uint32_t stage[NCH];
	unsigned i;
	const char *err = check_period(period_ns);

	if (err)
		goto fail;

	for (i = 0; i < NCH; i++) {
		uint32_t want = (ch < 0 || (unsigned)ch == i) ? exp_us
							      : g_exp_us[i];

		err = pulse_for(want, period_ns, &stage[i]);
		if (err)
			goto fail;
	}

	g_period_ns = period_ns;
	for (i = 0; i < NCH; i++) {
		g_pulse_ns[i] = stage[i];
		if (ch < 0 || (unsigned)ch == i)
			g_exp_us[i] = exp_us;
	}

	if (!tim1_program()) {
		uart_puts("err timer cannot represent that period\n");
		return;
	}
	uart_puts("ok\n");
	return;

fail:
	uart_puts("err ");
	uart_puts(err);
	uart_putc('\n');
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

static char *split(char *s)
{
	while (*s && *s != ' ')
		s++;
	if (*s == ' ')
		*s++ = 0;
	return s;
}

/* "30" or "59.94" -> thousandths.  0 on garbage. */
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

#define BAD_U32 0xFFFFFFFFu

static uint32_t parse_u32(const char *s)
{
	uint32_t v = 0;
	int seen = 0;

	while (*s >= '0' && *s <= '9') {
		v = v * 10 + (uint32_t)(*s++ - '0');
		seen = 1;
	}
	if (!seen || *s)
		return BAD_U32;
	return v;
}

static void cmd_status(void)
{
	unsigned i;

	uart_puts("clock=");
	uart_puts(g_on_hse ? "hse25\n" : "hsi64-FALLBACK\n");
	uart_kv("timer_hz", g_timer_hz);
	uart_kv("running", (uint32_t)g_running);
	uart_kv("period_us", g_period_ns / 1000u);
	uart_kv("fps_milli", (uint32_t)(1000000000000ull / g_period_ns));
	uart_puts("polarity=");
	uart_puts(g_active_high ? "active_high (idle LED off)\n"
				: "active_low (idle LED ON)\n");
	uart_kv("opto_skew_ns", g_opto_skew_ns);
	for (i = 0; i < NCH; i++) {
		uart_puts("ch");
		uart_putu(i + 1);
		uart_puts("_exposure_us=");
		uart_putu(g_exp_us[i]);
		uart_puts(" pulse_ns=");
		uart_putu(g_pulse_ns[i]);
		uart_puts(" ccr=");
		uart_putu(*ccr_of(i));
		uart_putc('\n');
	}
	uart_kv("psc", TIM1_PSC);
	uart_kv("arr", TIM1_ARR);
	uart_kv("pulses", g_pulses);
	uart_kv("burst_left", g_burst_left);
	uart_puts("ok\n");
}

static void cmd_help(void)
{
	uart_puts("IMX296 trigger generator (J106/TX2)\n"
		  "TIM1_CH1..4 on PE9/PE11/PE13/PE14 -> optocoupler LEDs\n"
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
		  "  status | help\n"
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
		uart_puts(g_running ? "ok\n"
				    : "err cannot start with current settings\n");
	} else if (str_eq(line, "stop")) {
		trig_stop();
		uart_puts("ok\n");
	} else if (str_eq(line, "fps")) {
		uint32_t m = parse_milli(arg);

		if (!m)
			uart_puts("err bad fps\n");
		else
			apply(-1, (uint32_t)(1000000000000ull / m), g_exp_us[0]);
	} else if (str_eq(line, "period")) {
		uint32_t v = parse_u32(arg);

		if (v == BAD_U32 || v == 0)
			uart_puts("err bad period\n");
		else
			apply(-1, v * 1000u, g_exp_us[0]);
	} else if (str_eq(line, "exp")) {
		char *second = split(arg);
		uint32_t a = parse_u32(arg);

		if (*second) {			/* "exp <ch> <us>" */
			uint32_t v = parse_u32(second);

			if (a < 1 || a > NCH || v == BAD_U32)
				uart_puts("err usage: exp <1-4> <us>\n");
			else
				apply((int)a - 1, g_period_ns, v);
		} else if (a == BAD_U32) {
			uart_puts("err bad exposure\n");
		} else {
			apply(-1, g_period_ns, a);
		}
	} else if (str_eq(line, "pol")) {
		uint32_t v = parse_u32(arg);

		if (v > 1)
			uart_puts("err usage: pol <0|1>\n");
		else {
			set_polarity((int)v);
			uart_puts("ok\n");
		}
	} else if (str_eq(line, "skew")) {
		uint32_t v = parse_u32(arg);

		if (v == BAD_U32 || v > 1000000u) {
			uart_puts("err usage: skew <ns, 0..1000000>\n");
		} else {
			g_opto_skew_ns = v;
			apply(-1, g_period_ns, g_exp_us[0]);
		}
	} else if (str_eq(line, "burst")) {
		uint32_t n = parse_u32(arg);

		if (n == BAD_U32 || n == 0) {
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
	unsigned i;

	clock_init();
	gpio_init();
	uart_init();
	tim1_init();

	/* Boot defaults, so the rig triggers with no host attached. */
	g_period_ns = (uint32_t)(1000000000000ull / DEFAULT_FPS_MILLI);
	for (i = 0; i < NCH; i++) {
		g_exp_us[i] = DEFAULT_EXP_US;
		pulse_for(DEFAULT_EXP_US, g_period_ns, &g_pulse_ns[i]);
	}
	trig_start();

	uart_puts("\ncamtrig ready - type help\n");
	cmd_status();

	for (;;) {
		if (TIM1_SR & TIM_SR_UIF) {
			TIM1_SR = ~TIM_SR_UIF;
			g_pulses++;

			if ((g_pulses % 15u) == 0)
				GPIO_BSRR(GPIOE_BASE) =
					(g_pulses % 30u) ? (1UL << LED_PIN)
							 : (1UL << (LED_PIN + 16));

			if (g_burst_left && --g_burst_left == 0) {
				trig_stop();
				uart_puts("burst done pulses=");
				uart_putu(g_pulses);
				uart_putc('\n');
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
