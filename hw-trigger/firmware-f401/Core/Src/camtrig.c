/* camtrig.c — IMX296 external-trigger generator for the J106/TX2 4-camera rig.
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
 * software loop.  Nothing in this file — and nothing in the scheduler or the
 * USB stack added alongside it — can move an edge once the registers are
 * written.  That is the invariant the whole restructure rests on.
 *
 * Wiring, resistor values and bring-up: ../../WIRING.md
 */
#include "main.h"
#include "camtrig.h"
#include "ranger.h"
#include "cmsis_os2.h"
#include "usbd_cdc_if.h"

extern TIM_HandleTypeDef htim2;
extern uint32_t g_boot_seen, g_sysmem_sp, g_sysmem_pc, g_sysmem_alt;

/* ------------------------------------------------------------------ *
 * State
 * ------------------------------------------------------------------ */
static uint32_t g_timer_hz;
static uint32_t g_period_ns;
static uint32_t g_exp_us[NCH];
static uint32_t g_pulse_ns[NCH];	/* what TIM1 is actually asked for */
static int      g_active_high = 1;	/* see set_polarity() */
static uint32_t g_opto_skew_ns;		/* see pulse_for() */
static int      g_running;
static uint32_t g_pulses;
static uint32_t g_burst_left;

/* One mutex guards trigger state *and* output.  Sharing it is deliberate: a
 * reply and an unsolicited "burst done" must not interleave on the wire, and a
 * second mutex acquired in the other order would be a deadlock waiting to
 * happen.  Priority inheritance keeps `trig` from being blocked for long by
 * `cli` holding it through a slow transmit. */
static osMutexId_t   g_lock;
static TaskHandle_t  g_trig_handle;

static void lock(void)   { if (g_lock) osMutexAcquire(g_lock, osWaitForever); }
static void unlock(void) { if (g_lock) osMutexRelease(g_lock); }

/* The same mutex, exposed for the ranger task.  It guards output as well as
 * trigger state (see the note above), which is exactly what a second producer
 * of unsolicited lines needs — but only around the print.  Holding it across an
 * I2C acquisition would stall the `trig` task for the whole conversion. */
void camtrig_out_lock(void)   { lock(); }
void camtrig_out_unlock(void) { unlock(); }

static const uint32_t ch_of[NCH] = {
	TIM_CHANNEL_1, TIM_CHANNEL_2, TIM_CHANNEL_3, TIM_CHANNEL_4
};

uint32_t camtrig_timer_hz(void) { return g_timer_hz; }

/* Read-only accessors for the display.  Each takes the same mutex the command
 * path uses, so the panel can never show a half-applied change. */
uint32_t camtrig_fps_milli(void)
{
	uint32_t v;
	lock(); v = (uint32_t)(1000000000000ull / g_period_ns); unlock();
	return v;
}
uint32_t camtrig_pulses(void)   { uint32_t v; lock(); v = g_pulses;        unlock(); return v; }
uint32_t camtrig_skew_ns(void)  { uint32_t v; lock(); v = g_opto_skew_ns;  unlock(); return v; }
int      camtrig_running(void)  { int v;      lock(); v = g_running;       unlock(); return v; }
int      camtrig_polarity(void) { int v;      lock(); v = g_active_high;   unlock(); return v; }
uint32_t camtrig_exp_us(unsigned ch)
{
	uint32_t v;
	if (ch >= NCH) return 0;
	lock(); v = g_exp_us[ch]; unlock();
	return v;
}
int camtrig_exp_uniform(void)
{
	unsigned i; int u = 1;
	lock();
	for (i = 1; i < NCH; i++)
		if (g_exp_us[i] != g_exp_us[0]) { u = 0; break; }
	unlock();
	return u;
}

/* ------------------------------------------------------------------ *
 * Output sinks
 * ------------------------------------------------------------------ */
void out_puts(sink_t s, const char *str)
{
	while (*str) {
		if (*str == '\n')
			out_putc(s, '\r');
		out_putc(s, *str++);
	}
}

void out_putu(sink_t s, uint32_t v)
{
	char buf[11];
	int i = 0;

	if (!v) {
		out_putc(s, '0');
		return;
	}
	while (v) {
		buf[i++] = (char)('0' + v % 10);
		v /= 10;
	}
	while (i)
		out_putc(s, buf[--i]);
}

void out_kvx(sink_t s, const char *k, uint32_t v)
{
	static const char hex[] = "0123456789abcdef";
	int i;

	out_puts(s, k);
	out_puts(s, "=0x");
	for (i = 28; i >= 0; i -= 4)
		out_putc(s, hex[(v >> i) & 0xF]);
	out_putc(s, '\n');
}

void out_kv(sink_t s, const char *k, uint32_t v)
{
	out_puts(s, k);
	out_putc(s, '=');
	out_putu(s, v);
	out_putc(s, '\n');
}

/* ------------------------------------------------------------------ *
 * TIM1 — four channels, one counter
 *
 * PWM mode 1 drives a channel "active" while CNT < CCRx.  Whether active means
 * the pin is high or low is CCxP, which set_polarity() owns.  CCRx = 0 means
 * never active, so it parks the pin in its idle state - that is what stop()
 * uses, rather than freezing the timer mid-pulse.
 * ------------------------------------------------------------------ */

/* Idle state = LED off.
 *
 * The optocoupler makes the sense of the pulse a board fact we cannot read
 * from here: driving the LED might pull the sensor's XTRIG low (assert) or
 * release it, depending on the module's internal wiring.  So polarity is
 * runtime-settable, and the default is the safe one - idle LOW, so an idle or
 * unpowered board draws no LED current and cannot hold the sensors inside an
 * exposure.
 *
 * HAL has no runtime polarity setter, so CCER is written directly.  The bit is
 * CCxP at bit 1 of each channel's 4-bit field.
 */
static void set_polarity(int active_high)
{
	unsigned i;

	g_active_high = active_high;
	for (i = 0; i < NCH; i++) {
		uint32_t mask = TIM_CCER_CC1P << (i * 4);

		if (active_high)
			htim2.Instance->CCER &= ~mask;
		else
			htim2.Instance->CCER |= mask;
	}
}

/* Pick the smallest prescaler that fits the period.
 *
 * On TIM1 this mattered: a 16-bit ARR forced div = ceil(pt / 65536), which made
 * the effective step ~= period / 65536 (~517 ns at 30 fps) no matter how fast
 * the timer clock ran.  TIM2's 32-bit counter removes that entirely - div is 1
 * for every rate in range, so the step is one tick, 8.33 ns at 120 MHz. */
static int tim1_program(void)
{
	uint64_t pt = ((uint64_t)g_period_ns * g_timer_hz) / 1000000000ull;
	uint32_t div, arr;
	unsigned i;

	if (pt < 2)
		return 0;

	/* TIM2's counter is 32-bit, so the prescaler is only needed for periods
	 * beyond 2^32 ticks (~35 s at 120 MHz) — i.e. never, within the rates
	 * this firmware accepts.  The division is kept rather than assumed away
	 * so the HSI-fallback clock and any future timer change stay correct. */
	div = (uint32_t)((pt + 0xFFFFFFFFull) / 0x100000000ull);
	if (div == 0)
		div = 1;

	arr = (uint32_t)(pt / div);
	if (arr == 0)
		return 0;
	arr -= 1;

	__HAL_TIM_SET_PRESCALER(&htim2, div - 1);
	__HAL_TIM_SET_AUTORELOAD(&htim2, arr);

	for (i = 0; i < NCH; i++) {
		uint64_t t = ((uint64_t)g_pulse_ns[i] * g_timer_hz)
			     / 1000000000ull / div;
		uint32_t ccr = (uint32_t)t;

		if (ccr == 0)
			ccr = 1;		/* never collapse to no pulse */
		if (ccr > arr)
			return 0;
		__HAL_TIM_SET_COMPARE(&htim2, ch_of[i], g_running ? ccr : 0);
	}

	htim2.Instance->EGR = TIM_EGR_UG;	/* latch PSC/ARR/CCRx immediately */
	htim2.Instance->SR  = 0;
	return 1;
}

/* HAL's channel state machine rejects a second Start on an already-started
 * channel, so the HAL calls are guarded separately from g_running: `start`
 * while running, or `burst` while running, must reprogram without glitching
 * the output by stopping and restarting it. */
static int g_pwm_started;

static void trig_start(void)
{
	unsigned i;

	g_running = 1;
	if (!tim1_program()) {
		g_running = 0;
		return;
	}
	if (g_pwm_started)
		return;			/* already emitting; CCRs just updated */

	__HAL_TIM_SET_COUNTER(&htim2, 0);
	htim2.Instance->SR = 0;
	for (i = 0; i < NCH; i++)
		HAL_TIM_PWM_Start(&htim2, ch_of[i]);
	g_pwm_started = 1;
}

static void trig_stop(void)
{
	unsigned i;

	for (i = 0; i < NCH; i++)
		__HAL_TIM_SET_COMPARE(&htim2, ch_of[i], 0);
	htim2.Instance->EGR = TIM_EGR_UG;	/* park idle before the timer halts */
	if (g_pwm_started) {
		for (i = 0; i < NCH; i++)
			HAL_TIM_PWM_Stop(&htim2, ch_of[i]);
		g_pwm_started = 0;
	}
	g_running = 0;
	g_burst_left = 0;
	HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET);	/* LED off */
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

/* ch >= 0        -> set that channel's exposure to exp_us
 * ch == APPLY_ALL -> set every channel's exposure to exp_us
 * ch == APPLY_KEEP-> leave every channel's exposure alone, exp_us ignored
 *
 * APPLY_KEEP exists because `fps`, `period` and `skew` have to re-derive every
 * channel's pulse without changing what any of them is set to.  They used to
 * pass g_exp_us[0] with APPLY_ALL, which silently reset all four cameras to
 * camera 1's exposure — so tuning `skew`, the one command the optocoupler
 * bring-up procedure depends on, destroyed per-camera exposure as a side
 * effect.  Pre-existing; found by reading back `status` over the new link. */
#define APPLY_ALL	(-1)
#define APPLY_KEEP	(-2)

static void apply(int ch, uint32_t period_ns, uint32_t exp_us, sink_t s)
{
	uint32_t stage[NCH];
	unsigned i;
	const char *err = check_period(period_ns);

	if (err)
		goto fail;

	for (i = 0; i < NCH; i++) {
		uint32_t want = (ch == APPLY_KEEP) ? g_exp_us[i]
			      : (ch == APPLY_ALL || (unsigned)ch == i) ? exp_us
			      : g_exp_us[i];

		err = pulse_for(want, period_ns, &stage[i]);
		if (err)
			goto fail;
	}

	g_period_ns = period_ns;
	for (i = 0; i < NCH; i++) {
		g_pulse_ns[i] = stage[i];
		if (ch != APPLY_KEEP && (ch == APPLY_ALL || (unsigned)ch == i))
			g_exp_us[i] = exp_us;
	}

	if (!tim1_program()) {
		out_puts(s, "err timer cannot represent that period\n");
		return;
	}
	out_puts(s, "ok\n");
	return;

fail:
	out_puts(s, "err ");
	out_puts(s, err);
	out_putc(s, '\n');
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

/* Hex, with or without an 0x prefix.  I2C addresses and register numbers are
 * quoted in hex everywhere in Garmin's manuals and in `i2cdetect` output, so
 * accepting decimal here would just be an invitation to typo 0x62 as 62. */
static uint32_t parse_hex(const char *s)
{
	uint32_t v = 0;
	int seen = 0;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	while (*s) {
		uint32_t d;

		if (*s >= '0' && *s <= '9')
			d = (uint32_t)(*s - '0');
		else if (*s >= 'a' && *s <= 'f')
			d = (uint32_t)(*s - 'a') + 10u;
		else if (*s >= 'A' && *s <= 'F')
			d = (uint32_t)(*s - 'A') + 10u;
		else
			return BAD_U32;
		v = v * 16u + d;
		seen = 1;
		s++;
	}
	if (!seen)
		return BAD_U32;
	return v;
}

static void cmd_status(sink_t s)
{
	unsigned i;

	out_puts(s, "clock=");
	out_puts(s, clock_source_name());
	out_putc(s, '\n');
	out_kv(s, "timer_hz", g_timer_hz);
	out_kv(s, "running", (uint32_t)g_running);
	out_kv(s, "period_us", g_period_ns / 1000u);
	out_kv(s, "fps_milli", (uint32_t)(1000000000000ull / g_period_ns));
	out_puts(s, "polarity=");
	out_puts(s, g_active_high ? "active_high (idle LED off)\n"
				  : "active_low (idle LED ON)\n");
	out_kv(s, "opto_skew_ns", g_opto_skew_ns);
	for (i = 0; i < NCH; i++) {
		out_puts(s, "ch");
		out_putu(s, i + 1);
		out_puts(s, "_exposure_us=");
		out_putu(s, g_exp_us[i]);
		out_puts(s, " pulse_ns=");
		out_putu(s, g_pulse_ns[i]);
		out_puts(s, " ccr=");
		out_putu(s, __HAL_TIM_GET_COMPARE(&htim2, ch_of[i]));
		out_putc(s, '\n');
	}
	out_kv(s, "psc", htim2.Instance->PSC);
	out_kv(s, "arr", htim2.Instance->ARR);
	out_kv(s, "pulses", g_pulses);
	out_kv(s, "burst_left", g_burst_left);
	out_kv(s, "stack_free_trig", task_stack_free(0));
	out_kv(s, "stack_free_cli", task_stack_free(1));
	out_kv(s, "cmds_dropped", cli_dropped());
	out_kv(s, "usb_dropped", cdc_dropped());
	out_kv(s, "usb_ready", (uint32_t)cdc_ready());
	out_kvx(s, "boot_req_seen", g_boot_seen);
	out_kvx(s, "sysmem_sp", g_sysmem_sp);
	out_kvx(s, "sysmem_pc", g_sysmem_pc);
	out_kvx(s, "sysmem_alt", g_sysmem_alt);
	out_kv(s, "lidar_model", (uint32_t)ranger_model());
	out_kvx(s, "lidar_addr", ranger_addr());
	out_kv(s, "lidar_present", (uint32_t)ranger_present());
	out_kv(s, "lidar_errors", ranger_errors());
	out_kv(s, "lidar_last_cm", ranger_last_cm());
	out_kv(s, "lidar_stream_div", ranger_stream_divisor());
	out_puts(s, "ok\n");
}

/* Each sample costs an acquisition (5-20 ms on a v3) and the whole burst runs
 * under the trigger mutex, so the ceiling is what bounds how long the `trig`
 * task can be held off its pulse bookkeeping.  20 samples is ~0.5 s worst case
 * — the LED blink pauses and a concurrent `burst` terminates late, which is why
 * this is not unbounded.  Continuous logging belongs on the TX2 side, polling
 * at whatever rate it actually needs. */
#define RANGE_MAX_BURST  20u

static void cmd_range(uint32_t n, sink_t s)
{
	uint32_t i;

	for (i = 0; i < n; i++) {
		uint16_t cm = 0;
		rng_status_t st = ranger_measure(&cm);

		if (st != RNG_OK) {
			out_puts(s, "err ");
			out_puts(s, ranger_strerror(st));
			out_putc(s, '\n');
			return;
		}

		/* Stamped with the trigger's own pulse counter.  That is the
		 * entire reason the ranger lives on this board rather than on
		 * the TX2's i2c: `seq` on the capture side advances exactly one
		 * per trigger edge, so `pulses` is already the cameras' clock
		 * and joining a range to a frame involves no timebase offset —
		 * none of the Delta problem that WIRING.md section 4.4 exists
		 * to solve for the IMU.
		 *
		 * g_pulses is read directly, NOT via camtrig_pulses(): the
		 * mutex is held for the whole of camtrig_handle() and it is not
		 * recursive, so the accessor would deadlock here. */
		out_puts(s, "range_cm=");
		out_putu(s, cm);
		out_puts(s, " pulses=");
		out_putu(s, g_pulses);
		out_putc(s, '\n');
	}
	out_puts(s, "ok\n");
}

static void cmd_lidar(char *arg, sink_t s)
{
	char *second = split(arg);

	if (!*arg) {
		out_kv(s, "lidar_model", (uint32_t)ranger_model());
		out_kvx(s, "lidar_addr", ranger_addr());
		out_kv(s, "lidar_present", (uint32_t)ranger_present());
		out_kv(s, "lidar_errors", ranger_errors());
		out_puts(s, "ok\n");
	} else if (str_eq(arg, "3") || str_eq(arg, "4")) {
		ranger_set_model(arg[0] - '0');
		out_puts(s, "ok\n");
	} else if (str_eq(arg, "addr")) {
		uint32_t v = parse_hex(second);

		if (v == BAD_U32 || v < 0x08u || v > 0x77u)
			out_puts(s, "err usage: lidar addr <hex 08..77>\n");
		else {
			ranger_set_addr((unsigned)v);
			out_puts(s, "ok\n");
		}
	} else if (str_eq(arg, "scan")) {
		ranger_scan(s);
		out_puts(s, "ok\n");
	} else if (str_eq(arg, "reg")) {
		uint32_t v = parse_hex(second);
		uint8_t val = 0;

		if (v == BAD_U32 || v > 0xffu) {
			out_puts(s, "err usage: lidar reg <hex 00..ff>\n");
		} else {
			rng_status_t st = ranger_read_reg((uint8_t)v, &val);

			if (st != RNG_OK) {
				out_puts(s, "err ");
				out_puts(s, ranger_strerror(st));
				out_putc(s, '\n');
			} else {
				out_kvx(s, "value", val);
				out_puts(s, "ok\n");
			}
		}
	} else if (str_eq(arg, "reset")) {
		ranger_bus_reset();
		out_puts(s, "ok\n");
	} else if (str_eq(arg, "pins")) {
		ranger_probe_pins(s);
		out_puts(s, "ok\n");
	} else if (str_eq(arg, "bb")) {
		ranger_bitbang_scan(s, str_eq(second, "swap"));
		out_puts(s, "ok\n");
	} else if (str_eq(arg, "hw")) {
		ranger_dump_hw(s);
		out_puts(s, "ok\n");
	} else if (str_eq(arg, "io")) {
		if (str_eq(second, "bb") || str_eq(second, "hw")) {
			ranger_set_io(str_eq(second, "bb"));
			out_puts(s, "ok\n");
		} else {
			out_puts(s, "err usage: lidar io <bb|hw>\n");
		}
	} else if (str_eq(arg, "khz")) {
		uint32_t v = parse_u32(second);

		if (v == BAD_U32 || v < 1u || v > 400u) {
			out_puts(s, "err usage: lidar khz <1-400>\n");
		} else {
			ranger_set_speed(v * 1000u);
			out_puts(s, "ok\n");
		}
	} else {
		out_puts(s, "err usage: lidar "
			    "[3|4|addr|scan|reg|reset|pins|bb|hw|khz|io]\n");
	}
}

static void cmd_help(sink_t s)
{
	out_puts(s,
		 "IMX296 trigger generator (J106/TX2)\n"
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
		 "  status | help\n"
		 "Garmin LIDAR-Lite on I2C1, PB6=SCL PB7=SDA\n"
		 "  range [n]      n acquisitions (1..20), cm, stamped with\n"
		 "                 the trigger pulse counter\n"
		 "  range auto <n>   stream one reading every n trigger\n"
		 "                 pulses; 0 = off.  Lines start with `!`,\n"
		 "                 as all unsolicited output does.  A missing\n"
		 "                 sensor is skipped, never fatal\n"
		 "  lidar          report model, address, probe result\n"
		 "  lidar 3|4      register map: 3 = v3/v3HP, 4 = v4 LED\n"
		 "                 WRONG MAP READS A PLAUSIBLE WRONG NUMBER\n"
		 "  lidar addr <h> i2c address in hex, default 62\n"
		 "  lidar scan     list every address that ACKs\n"
		 "  lidar reg <h>  read one register, hex\n"
		 "  lidar reset    9-clock bus recovery, then re-init\n"
		 "  lidar pins     electrical state of SCL/SDA - tells an\n"
		 "                 unwired bus from one held low\n"
		 "  lidar bb [swap]  bit-banged scan; `swap` tries the\n"
		 "                 SCL/SDA roles the other way round\n"
		 "  lidar io bb|hw   transport: bit-bang (default, works) or\n"
		 "                 the I2C peripheral (NAKs on this rig)\n"
		 "ok\n");
}

void camtrig_handle(char *line, sink_t s)
{
	char *arg = split(line);

	if (!*line)
		return;

	/* Serialised so two hosts on two transports cannot interleave into a
	 * state neither of them requested (spec: "Concurrent hosts"). */
	lock();

	if (str_eq(line, "help")) {
		cmd_help(s);
	} else if (str_eq(line, "status")) {
		cmd_status(s);
	} else if (str_eq(line, "start")) {
		trig_start();
		out_puts(s, g_running ? "ok\n"
				      : "err cannot start with current settings\n");
	} else if (str_eq(line, "stop")) {
		trig_stop();
		out_puts(s, "ok\n");
	} else if (str_eq(line, "fps")) {
		uint32_t m = parse_milli(arg);

		if (!m)
			out_puts(s, "err bad fps\n");
		else
			apply(APPLY_KEEP, (uint32_t)(1000000000000ull / m), 0, s);
	} else if (str_eq(line, "period")) {
		uint32_t v = parse_u32(arg);

		if (v == BAD_U32 || v == 0)
			out_puts(s, "err bad period\n");
		else
			apply(APPLY_KEEP, v * 1000u, 0, s);
	} else if (str_eq(line, "exp")) {
		char *second = split(arg);
		uint32_t a = parse_u32(arg);

		if (*second) {			/* "exp <ch> <us>" */
			uint32_t v = parse_u32(second);

			if (a < 1 || a > NCH || v == BAD_U32)
				out_puts(s, "err usage: exp <1-4> <us>\n");
			else
				apply((int)a - 1, g_period_ns, v, s);
		} else if (a == BAD_U32) {
			out_puts(s, "err bad exposure\n");
		} else {
			apply(APPLY_ALL, g_period_ns, a, s);
		}
	} else if (str_eq(line, "pol")) {
		uint32_t v = parse_u32(arg);

		if (v > 1) {
			out_puts(s, "err usage: pol <0|1>\n");
		} else {
			set_polarity((int)v);
			out_puts(s, "ok\n");
		}
	} else if (str_eq(line, "skew")) {
		uint32_t v = parse_u32(arg);

		if (v == BAD_U32 || v > 1000000u) {
			out_puts(s, "err usage: skew <ns, 0..1000000>\n");
		} else {
			g_opto_skew_ns = v;
			apply(APPLY_KEEP, g_period_ns, 0, s);
		}
	} else if (str_eq(line, "range")) {
		char *second = split(arg);

		if (str_eq(arg, "auto")) {		/* "range auto <n|0>" */
			uint32_t d = parse_u32(second);

			if (d == BAD_U32) {
				out_puts(s, "err usage: range auto <pulses, 0=off>\n");
			} else {
				ranger_set_stream(d);
				out_puts(s, "ok\n");
			}
		} else {
			uint32_t n = *arg ? parse_u32(arg) : 1u;

			if (n == BAD_U32 || n == 0 || n > RANGE_MAX_BURST)
				out_puts(s, "err usage: range [1-20] | range auto <n>\n");
			else
				cmd_range(n, s);
		}
	} else if (str_eq(line, "lidar")) {
		cmd_lidar(arg, s);
	} else if (str_eq(line, "dfu")) {
		/* Reply and drain before resetting: the host must see the "ok"
		 * before the device drops off the bus, or `make flash` cannot
		 * tell "entering bootloader" from "died". */
		out_puts(s, "ok entering bootloader\n");
		unlock();
		HAL_Delay(150);
		request_bootloader();
		return;			/* not reached */
	} else if (str_eq(line, "burst")) {
		uint32_t n = parse_u32(arg);

		if (n == BAD_U32 || n == 0) {
			out_puts(s, "err bad count\n");
		} else {
			g_pulses = 0;
			g_burst_left = n;
			trig_start();
			out_puts(s, g_running ? "ok\n" : "err cannot start\n");
		}
	} else {
		out_puts(s, "err unknown command (try help)\n");
	}

	unlock();
}

/* ------------------------------------------------------------------ *
 * Bring-up and servicing
 * ------------------------------------------------------------------ */
void camtrig_init(void)
{
	unsigned i;

	/* Attributes stated explicitly.  FreeRTOS mutexes inherit priority
	 * unconditionally, so osMutexPrioInherit changes nothing here — but the
	 * design depends on that inheritance (D8), and a reader should not have
	 * to know the implementation to see that it was intended. */
	static const osMutexAttr_t lock_attr = {
		.name      = "camtrig",
		.attr_bits = osMutexPrioInherit,
	};

	g_lock = osMutexNew(&lock_attr);
	configASSERT(g_lock != NULL);

	g_timer_hz = timer_clock_hz();

	/* Boot defaults, so the rig triggers with no host attached. */
	g_period_ns = (uint32_t)(1000000000000ull / DEFAULT_FPS_MILLI);
	for (i = 0; i < NCH; i++) {
		g_exp_us[i] = DEFAULT_EXP_US;
		pulse_for(DEFAULT_EXP_US, g_period_ns, &g_pulse_ns[i]);
	}
	set_polarity(1);
	trig_start();
}

void camtrig_banner(sink_t s)
{
	lock();
	out_puts(s, "\ncamtrig ready - type help\n");
	cmd_status(s);
	unlock();
}

/* Free-running copy of the pulse count, maintained in the ISR.
 *
 * g_pulses is owned by the `trig` task and guarded by the mutex, which makes it
 * useless as a timestamp for anything that runs while that mutex is held — the
 * count simply stops advancing, so every reading in a burst got the same stamp.
 * This one advances on the hardware event itself and is readable lock-free, so
 * a range sample can be stamped with the edge it actually belongs to.
 *
 * Deliberately additive: g_pulses keeps its exact previous meaning and the
 * burst logic is untouched.  A u32 at 60 fps wraps in 2.3 years. */
volatile uint32_t g_pulses_isr;

void camtrig_notify_pulse_from_isr(BaseType_t *woken)
{
	g_pulses_isr++;
	if (g_trig_handle)
		vTaskNotifyGiveFromISR(g_trig_handle, woken);
}

void camtrig_task(void *arg)
{
	(void)arg;
	g_trig_handle = xTaskGetCurrentTaskHandle();

	for (;;) {
		/* Counting take: returns how many pulses fired since the last
		 * wake, so a burst stays exact even if this task was held off
		 * (by `cli` transmitting a long `status`, say). */
		uint32_t n = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		uint32_t done_at = 0;
		int done = 0;

		if (!n)
			continue;

		lock();
		g_pulses += n;

		/* Toggle every 15 pulses = 0.5 s at 30 fps, so the blink rate
		 * is derived from the real timer clock and a wrong clock tree
		 * is visible by eye with no instrument attached. */
		HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin,
				  ((g_pulses / 15u) & 1u) ? GPIO_PIN_SET
							  : GPIO_PIN_RESET);

		if (g_burst_left) {
			if (g_burst_left <= n) {
				g_burst_left = 0;
				trig_stop();
				done_at = g_pulses;
				done = 1;
			} else {
				g_burst_left -= n;
			}
		}

		if (done) {
			out_puts(SINK_ALL, ASYNC_PREFIX "burst done pulses=");
			out_putu(SINK_ALL, done_at);
			out_putc(SINK_ALL, '\n');
		}
		unlock();
	}
}
