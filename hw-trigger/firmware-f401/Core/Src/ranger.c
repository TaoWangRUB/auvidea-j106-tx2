/* ranger.c — Garmin LIDAR-Lite ranging on I2C1, PB6 (SCL) / PB7 (SDA).
 *
 * Port B was entirely free before this: camtrig uses PA0-PA3 (TIM2 trigger),
 * PA5 (the open-drain delta echo), PA9/PA10 (USART1), PA11/PA12 (USB) and
 * PC13 (LED).  So I2C1 on its default AF4 pins costs nothing already spoken for.
 *
 * ⚠ Nothing in this file may stall the waveform.  TIM2 emits the trigger in
 * hardware and no code here touches it, but the *timing* still matters one step
 * removed: a measurement blocks for as long as the sensor takes and runs on the
 * `cli` task under camtrig's mutex — the same mutex the `trig` task wants for
 * its pulse bookkeeping.  One acquisition is the same order as the ~26 ms a
 * full `status` already spends transmitting while holding it, which is why
 * `range <n>` is capped rather than unbounded.
 *
 * ## Two transports, and the two separate faults that made this look hard
 *
 * Bring-up on 2026-09-04 hit two independent problems that presented as one
 * ("the hardware peripheral cannot talk to the sensor, a bit-banged master on
 * the same two pins can").  Both are fixed; both are worth recording, because
 * each one on its own is easy to misdiagnose as the other.
 *
 * **Fault 1 — SDA and SCL twisted together as a pair.**  The hardware master
 * NAKed every address (HAL_I2C_ERROR_AF) while the bit-bang found 0x62 every
 * time.  Twisting the two signal lines together maximises their mutual
 * capacitance, so each SCL edge couples a spike onto SDA.  The F4 samples SDA a
 * short, *fixed* delay after the SCL rising edge — which lands in that spike.
 * The bit-bang waits for SCL to actually reach a high level and then waits
 * another half-period (~8 us) before sampling, by which time the spike has
 * decayed.
 *
 * This also explains the single most misleading measurement of the session:
 * sweeping the bus 100 -> 50 -> 20 -> 10 -> 5 kHz changed nothing.  Slowing the
 * bus lengthens the period but *not* the edge-to-sample delay, so the spike
 * stays exactly where it was relative to the sample point.  A speed sweep that
 * does nothing is evidence for edge-coupled noise, not against it.
 * ⚠ Do not twist SDA and SCL together.  Twist each with a ground return, or
 * run them as plain parallel conductors — see WIRING.md section 4.5.
 *
 * **Fault 2 — repeated-START reads.**  With the wiring corrected the hardware
 * master addressed the sensor fine and then read all-zero data from every
 * register, with no error reported.  HAL_I2C_Mem_Read() issues a repeated START
 * between the register pointer and the data phase; Garmin's own Arduino library
 * ends the pointer write with a STOP (endTransmission(), then requestFrom()),
 * and the bit-banged path here did the same and read correctly on identical
 * wiring.  reg_read() therefore uses Master_Transmit + Master_Receive.
 *
 * With both fixed the two transports agree — 182-185 cm against 184 cm, 40
 * hardware acquisitions with zero errors — so the hardware peripheral is the
 * default.  The bit-bang stays because it costs little, it is independent of
 * every I2C erratum the F4 has, and it is what made the first fault visible at
 * all: `lidar io bb` switches, `lidar bb [swap]` scans in software with the
 * SCL/SDA roles optionally exchanged.
 *
 * ## Two register maps, one address
 *
 * v3/v3HP and v4 LED both answer at 0x62 and both start an acquisition the same
 * way, but they report the result differently:
 *
 *   v3/v3HP  distance at 0x0f (high) / 0x10 (low) -> read 2 bytes at 0x8f,
 *            where bit 7 of the register address is the auto-increment flag.
 *            BIG-endian.
 *   v4 LED   distance at 0x10 (low) / 0x11 (high) -> read 2 bytes at 0x10.
 *            LITTLE-endian.
 *
 * Read a v4 with the v3 map and you get a plausible-looking but wrong number,
 * which is the worst possible failure mode — so the map is explicit (`lidar 3`
 * / `lidar 4`), never guessed.  Default is 3.
 *
 * Wiring, pull-ups and the bulk cap: ../../WIRING.md section 4.5.
 */
#include "main.h"
#include "camtrig.h"
#include "ranger.h"
#include "cmsis_os2.h"

I2C_HandleTypeDef hi2c1;

/* Per-transfer ceiling for the hardware path.  Long enough that a slave
 * clock-stretching through an acquisition is not mistaken for a dead bus, short
 * enough that a bus with no pull-ups fails fast instead of hanging the console. */
#define I2C_TMO_MS		20u

/* Acquisition ceiling.  Garmin quotes 5-20 ms typical for a v3; past this it is
 * a stuck STATUS register, not a slow measurement. */
#define ACQ_TMO_MS		100u

#define REG_ACQ_COMMAND		0x00u
#define REG_STATUS		0x01u
#define ACQ_WITH_BIAS_CORR	0x04u
#define STATUS_BUSY		0x01u

#define REG_DIST_V3		0x8fu	/* 0x0f | auto-increment, big-endian */
#define REG_DIST_V4		0x10u	/* 0x10 then 0x11, little-endian     */

static unsigned g_addr7 = RANGER_ADDR_DEFAULT;
static int      g_model = 3;
static int      g_present;
static uint32_t g_errors;
static uint16_t g_last_cm;
static int      g_inited;
static uint32_t g_speed_hz = 100000;
static int      g_use_bb;		/* 0 = hardware; see the transport note above */

/* Sticky copy of the HAL error from the last failed hardware transfer.
 * hi2c1.ErrorCode is cleared on the next call, so without this the one number
 * that says *how* a transfer failed — NAK versus never-started — is gone by the
 * time anyone asks. */
static uint32_t g_last_hal_err;
static uint32_t g_last_hal_ret;

static uint16_t addr8(void) { return (uint16_t)(g_addr7 << 1); }

/* osDelay() is illegal before the scheduler runs, and ranger_init() runs from
 * main() alongside MX_TIM2_Init().  Everything else here runs on the `cli`
 * task, where yielding is what we want. */
static void rng_delay(uint32_t ms)
{
	if (osKernelGetState() == osKernelRunning)
		osDelay(ms);
	else
		HAL_Delay(ms);
}

/* ------------------------------------------------------------------ *
 * Hardware peripheral
 * ------------------------------------------------------------------ */
static int i2c_setup(void)
{
	/* DeInit first, so State goes back to RESET.
	 *
	 * ⚠ HAL_I2C_Init() calls HAL_I2C_MspInit() *only* out of the RESET
	 * state.  Re-initialising a peripheral that is already READY therefore
	 * silently skips the pin setup — so after the bit-bang path has taken
	 * PB6/PB7 over as plain GPIO, a bare HAL_I2C_Init() would configure the
	 * block perfectly and leave it wired to nothing.  Every register would
	 * read back correct and every transfer would fail. */
	HAL_I2C_DeInit(&hi2c1);

	hi2c1.Instance             = I2C1;
	hi2c1.Init.ClockSpeed      = g_speed_hz;
	hi2c1.Init.DutyCycle       = I2C_DUTYCYCLE_2;
	hi2c1.Init.OwnAddress1     = 0;
	hi2c1.Init.AddressingMode  = I2C_ADDRESSINGMODE_7BIT;
	hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
	hi2c1.Init.OwnAddress2     = 0;
	hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
	/* Clock stretching must stay ENABLED (NoStretch DISABLE): the sensor
	 * holds SCL during an acquisition. */
	hi2c1.Init.NoStretchMode   = I2C_NOSTRETCH_DISABLE;

	return HAL_I2C_Init(&hi2c1) == HAL_OK;
}

/* ~8 us at 84 MHz, giving the bit-bang master roughly 58 kHz. */
static void bit_delay(void)
{
	volatile uint32_t d;

	for (d = 0; d < 120u; d++)
		;
}

/* ------------------------------------------------------------------ *
 * Bit-banged master
 *
 * Open-drain throughout: writing 1 releases the pin and the external pull-up
 * provides the high, exactly as I2C requires.  Nothing here ever drives a line
 * high, so no configuration mistake can produce a bus fight.
 * ------------------------------------------------------------------ */
struct bb {
	uint16_t scl;
	uint16_t sda;
};

static const struct bb bb_normal = { GPIO_PIN_6, GPIO_PIN_7 };

static void bb_set(uint16_t pin, int high)
{
	HAL_GPIO_WritePin(GPIOB, pin, high ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static int bb_get(uint16_t pin)
{
	return HAL_GPIO_ReadPin(GPIOB, pin) == GPIO_PIN_SET;
}

/* Take PB6/PB7 as open-drain GPIO, idle high.  Idempotent, and cheap next to a
 * transaction, so every bit-banged operation starts with it rather than relying
 * on whatever the last caller left behind. */
static void bb_begin(void)
{
	GPIO_InitTypeDef g = {0};

	HAL_I2C_DeInit(&hi2c1);
	__HAL_RCC_GPIOB_CLK_ENABLE();

	g.Pin   = GPIO_PIN_6 | GPIO_PIN_7;
	g.Mode  = GPIO_MODE_OUTPUT_OD;
	g.Pull  = GPIO_NOPULL;		/* the external pull-ups do this */
	g.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(GPIOB, &g);

	bb_set(GPIO_PIN_6, 1);
	bb_set(GPIO_PIN_7, 1);
	bit_delay();
}

/* Release SCL and wait for it to actually rise.  A slave may hold the clock
 * down to buy time, and ignoring that turns a working slow device into one that
 * never answers. */
static void bb_scl_high(const struct bb *b)
{
	int i;

	bb_set(b->scl, 1);
	for (i = 0; i < 1000 && !bb_get(b->scl); i++)
		bit_delay();
	bit_delay();
}

static void bb_start(const struct bb *b)
{
	bb_set(b->sda, 1);
	bb_scl_high(b);
	bb_set(b->sda, 0);		/* SDA falls while SCL is high */
	bit_delay();
	bb_set(b->scl, 0);
	bit_delay();
}

static void bb_stop(const struct bb *b)
{
	bb_set(b->sda, 0);
	bit_delay();
	bb_scl_high(b);
	bb_set(b->sda, 1);		/* SDA rises while SCL is high */
	bit_delay();
}

/* Returns the ACK bit: 0 = acknowledged, 1 = not. */
static int bb_write_byte(const struct bb *b, uint8_t v)
{
	int i, nak;

	for (i = 7; i >= 0; i--) {
		bb_set(b->sda, (v >> i) & 1);
		bit_delay();
		bb_scl_high(b);
		bb_set(b->scl, 0);
		bit_delay();
	}

	bb_set(b->sda, 1);		/* release for the slave to pull down */
	bit_delay();
	bb_scl_high(b);
	nak = bb_get(b->sda);
	bb_set(b->scl, 0);
	bit_delay();

	return nak;
}

static uint8_t bb_read_byte(const struct bb *b, int ack)
{
	uint8_t v = 0;
	int i;

	bb_set(b->sda, 1);		/* release; the slave drives */
	for (i = 7; i >= 0; i--) {
		bit_delay();
		bb_scl_high(b);
		if (bb_get(b->sda))
			v |= (uint8_t)(1u << i);
		bb_set(b->scl, 0);
	}

	bb_set(b->sda, ack ? 0 : 1);
	bit_delay();
	bb_scl_high(b);
	bb_set(b->scl, 0);
	bit_delay();
	bb_set(b->sda, 1);

	return v;
}

static int bb_ready(void)
{
	int nak;

	bb_begin();
	bb_start(&bb_normal);
	nak = bb_write_byte(&bb_normal, (uint8_t)(g_addr7 << 1));
	bb_stop(&bb_normal);

	return !nak;
}

static rng_status_t bb_reg_write(uint8_t reg, uint8_t val)
{
	int nak;

	bb_begin();
	bb_start(&bb_normal);
	nak = bb_write_byte(&bb_normal, (uint8_t)(g_addr7 << 1));
	if (!nak)
		nak = bb_write_byte(&bb_normal, reg);
	if (!nak)
		nak = bb_write_byte(&bb_normal, val);
	bb_stop(&bb_normal);

	if (nak) {
		g_errors++;
		return RNG_ERR_IO;
	}
	return RNG_OK;
}

/* STOP-separated rather than repeated-START, which is what Garmin's own
 * Arduino library does for these parts (endTransmission() then requestFrom()).
 * Both are legal I2C; this is the variant the sensor is known to be exercised
 * with, so it is the one to use when the goal is a first reading. */
static rng_status_t bb_reg_read(uint8_t reg, uint8_t *buf, uint16_t n)
{
	uint16_t i;
	int nak;

	bb_begin();
	bb_start(&bb_normal);
	nak = bb_write_byte(&bb_normal, (uint8_t)(g_addr7 << 1));
	if (!nak)
		nak = bb_write_byte(&bb_normal, reg);
	bb_stop(&bb_normal);
	if (nak) {
		g_errors++;
		return RNG_ERR_IO;
	}

	bb_start(&bb_normal);
	nak = bb_write_byte(&bb_normal, (uint8_t)((g_addr7 << 1) | 1u));
	if (nak) {
		bb_stop(&bb_normal);
		g_errors++;
		return RNG_ERR_IO;
	}
	for (i = 0; i < n; i++)
		buf[i] = bb_read_byte(&bb_normal, i + 1u < n);
	bb_stop(&bb_normal);

	return RNG_OK;
}

/* ------------------------------------------------------------------ *
 * Transport-independent register access
 * ------------------------------------------------------------------ */
static rng_status_t reg_write(uint8_t reg, uint8_t val)
{
	HAL_StatusTypeDef r;

	if (g_use_bb)
		return bb_reg_write(reg, val);

	if (!g_inited)
		return RNG_ERR_IO;
	r = HAL_I2C_Mem_Write(&hi2c1, addr8(), reg, I2C_MEMADD_SIZE_8BIT,
			      &val, 1, I2C_TMO_MS);
	if (r != HAL_OK) {
		g_last_hal_ret = (uint32_t)r;
		g_last_hal_err = HAL_I2C_GetError(&hi2c1);
		g_errors++;
		return RNG_ERR_IO;
	}
	return RNG_OK;
}

static rng_status_t reg_read(uint8_t reg, uint8_t *buf, uint16_t n)
{
	HAL_StatusTypeDef r;

	if (g_use_bb)
		return bb_reg_read(reg, buf, n);

	if (!g_inited)
		return RNG_ERR_IO;

	/* STOP-separated, NOT HAL_I2C_Mem_Read().
	 *
	 * Mem_Read() issues a repeated START between the register pointer and
	 * the data phase.  Against this sensor that addresses fine and then
	 * returns all-zero data with no error reported — every register, every
	 * bus speed.  Garmin's own Arduino library ends the pointer write with a
	 * STOP (endTransmission(), then requestFrom()), and the bit-banged path
	 * here does the same and reads correctly on the identical wiring.  So
	 * the transaction shape is the variable, not the electrical layer. */
	r = HAL_I2C_Master_Transmit(&hi2c1, addr8(), &reg, 1, I2C_TMO_MS);
	if (r == HAL_OK)
		r = HAL_I2C_Master_Receive(&hi2c1, addr8(), buf, n, I2C_TMO_MS);
	if (r != HAL_OK) {
		g_last_hal_ret = (uint32_t)r;
		g_last_hal_err = HAL_I2C_GetError(&hi2c1);
		g_errors++;
		return RNG_ERR_IO;
	}
	return RNG_OK;
}

static int dev_ready(void)
{
	if (g_use_bb)
		return bb_ready();
	if (!g_inited)
		return 0;
	return HAL_I2C_IsDeviceReady(&hi2c1, addr8(), 2, I2C_TMO_MS) == HAL_OK;
}

rng_status_t ranger_read_reg(uint8_t reg, uint8_t *val)
{
	return reg_read(reg, val, 1);
}

/* ------------------------------------------------------------------ *
 * Bring-up
 * ------------------------------------------------------------------ */
/* Nine clocks with SDA released, then a STOP.  The standard escape from the one
 * state a master cannot fix by re-initialising itself: a slave reset mid-byte,
 * holding SDA low waiting to finish sending a bit. */
void ranger_bus_reset(void)
{
	int i;

	bb_begin();

	bb_set(GPIO_PIN_7, 1);		/* release SDA */
	for (i = 0; i < 9; i++) {
		bb_set(GPIO_PIN_6, 0);
		bit_delay();
		bb_set(GPIO_PIN_6, 1);
		bit_delay();
	}
	bb_stop(&bb_normal);

	if (!g_use_bb)
		g_inited = i2c_setup();
}

void ranger_init(void)
{
	g_inited = i2c_setup();

	/* One probe so `status` can say whether anything is on the bus without
	 * the operator having to run a measurement first.  A failure here is
	 * recorded, never fatal — the trigger must come up regardless. */
	g_present = dev_ready();
}

/* ------------------------------------------------------------------ *
 * Measurement
 * ------------------------------------------------------------------ */
rng_status_t ranger_measure(uint16_t *cm)
{
	uint8_t st, d[2];
	uint32_t waited = 0;
	rng_status_t r;

	if (!dev_ready()) {
		g_present = 0;
		return RNG_ERR_NODEV;
	}
	g_present = 1;

	r = reg_write(REG_ACQ_COMMAND, ACQ_WITH_BIAS_CORR);
	if (r != RNG_OK)
		return r;

	/* Poll rather than assume a fixed conversion time: the acquisition is
	 * longer in low light and at long range, and a fixed wait would read a
	 * stale register exactly when the measurement is hardest. */
	for (;;) {
		r = reg_read(REG_STATUS, &st, 1);
		if (r != RNG_OK)
			return r;
		if (!(st & STATUS_BUSY))
			break;
		if (waited >= ACQ_TMO_MS)
			return RNG_ERR_BUSY;
		rng_delay(1);
		waited++;
	}

	r = reg_read(g_model == 4 ? REG_DIST_V4 : REG_DIST_V3, d, 2);
	if (r != RNG_OK)
		return r;

	*cm = (g_model == 4) ? (uint16_t)(d[0] | (d[1] << 8))
			     : (uint16_t)((d[0] << 8) | d[1]);
	g_last_cm = *cm;
	return RNG_OK;
}

/* ------------------------------------------------------------------ *
 * Streaming task
 *
 * ## A missing sensor is a normal state, not a failure
 *
 * The node on the other end of this UART also carries trigger control, so
 * anything that lets an absent rangefinder escalate would take camera
 * triggering down with it.  Three rules follow, and they are the reason this
 * task looks the way it does:
 *
 *   1. Never stop.  A failed acquisition is skipped, not fatal; the task keeps
 *      running so the sensor can be plugged in later and just start working.
 *   2. Never spam.  One line per failed attempt would flood the very link the
 *      trigger commands share.  The absent/present TRANSITION is announced
 *      once; the steady state says nothing.
 *   3. Never busy-wait on it.  A probe against an empty bus costs a full
 *      timeout, so once absent the retry rate drops to ABSENT_RETRY_MS instead
 *      of running at the streaming rate.
 *
 * Silence is not the same as hiding it: `status` always reports
 * `lidar_present`, `lidar_errors` and `lidar_last_cm`, so the state is
 * observable at any time — this rig does not quietly substitute values it does
 * not have (compare `j106-record-sync.py`'s "UNMEASURED" provenance marker).
 * ------------------------------------------------------------------ */
#define ABSENT_RETRY_MS		2000u
#define ABSENT_AFTER_FAILS	3u

static uint32_t g_divisor;		/* 0 = streaming off */
static uint32_t g_absent_fails;
static int      g_absent_announced;

void ranger_set_stream(uint32_t divisor)
{
	g_divisor = divisor;
	/* Re-arm the announcement so enabling the stream reports the state once,
	 * rather than staying quiet because it was already absent. */
	g_absent_announced = 0;
	g_absent_fails = 0;
}

uint32_t ranger_stream_divisor(void) { return g_divisor; }

static void announce(const char *what, uint32_t v, int have_v)
{
	camtrig_out_lock();
	out_puts(SINK_ALL, ASYNC_PREFIX);
	out_puts(SINK_ALL, what);
	if (have_v)
		out_putu(SINK_ALL, v);
	out_putc(SINK_ALL, '\n');
	camtrig_out_unlock();
}

void ranger_task(void *arg)
{
	uint32_t last_stamp = 0;

	(void)arg;

	for (;;) {
		uint32_t stamp;
		uint16_t cm = 0;
		rng_status_t st;

		if (g_divisor == 0u) {
			rng_delay(50);
			continue;
		}

		/* Phase-lock to the trigger: wait until the free-running pulse
		 * counter has advanced a whole divisor since the last reading.
		 * g_pulses_isr is maintained in the TIM2 ISR and needs no mutex,
		 * which is what lets this task stamp a sample with the edge it
		 * actually belongs to. */
		stamp = g_pulses_isr;
		if ((uint32_t)(stamp - last_stamp) < g_divisor) {
			rng_delay(1);
			continue;
		}
		last_stamp = stamp;

		/* No lock held here — the acquisition is the slow part. */
		st = ranger_measure(&cm);

		if (st != RNG_OK) {
			if (g_absent_fails < ABSENT_AFTER_FAILS)
				g_absent_fails++;
			if (g_absent_fails >= ABSENT_AFTER_FAILS
			    && !g_absent_announced) {
				g_absent_announced = 1;
				announce("range absent - continuing without it",
					 0, 0);
			}
			/* Back off, and re-sync the phase reference so the
			 * first reading after a reconnect is not a burst of
			 * catch-up attempts. */
			rng_delay(ABSENT_RETRY_MS);
			last_stamp = g_pulses_isr;
			continue;
		}

		if (g_absent_announced) {
			g_absent_announced = 0;
			announce("range present again", 0, 0);
		}
		g_absent_fails = 0;

		camtrig_out_lock();
		out_puts(SINK_ALL, ASYNC_PREFIX "range_cm=");
		out_putu(SINK_ALL, cm);
		out_puts(SINK_ALL, " pulses=");
		out_putu(SINK_ALL, stamp);
		out_putc(SINK_ALL, '\n');
		camtrig_out_unlock();
	}
}

const char *ranger_strerror(rng_status_t st)
{
	switch (st) {
	case RNG_OK:		return "ok";
	case RNG_ERR_NODEV:	return "no device at address (check wiring, 5V, pull-ups)";
	case RNG_ERR_IO:	return "i2c transfer failed";
	case RNG_ERR_BUSY:	return "acquisition did not complete";
	case RNG_ERR_RANGE:	return "value out of range";
	}
	return "unknown";
}

/* ------------------------------------------------------------------ *
 * Bring-up aids
 * ------------------------------------------------------------------ */
void ranger_scan(sink_t s)
{
	unsigned a, found = 0;

	/* 0x08..0x77 — the reserved ranges at either end are skipped because
	 * probing them can put a compliant device into a mode it will not
	 * leave (0x00 is the general call). */
	if (g_use_bb) {
		bb_begin();
		for (a = 0x08u; a <= 0x77u; a++) {
			bb_start(&bb_normal);
			if (bb_write_byte(&bb_normal,
					  (uint8_t)(a << 1)) == 0) {
				out_kvx(s, "found", a);
				found++;
			}
			bb_stop(&bb_normal);
		}
	} else {
		if (!g_inited) {
			out_puts(s, "i2c1 not initialised\n");
			return;
		}
		for (a = 0x08u; a <= 0x77u; a++) {
			if (HAL_I2C_IsDeviceReady(&hi2c1,
						  (uint16_t)(a << 1), 1, 5)
			    == HAL_OK) {
				out_kvx(s, "found", a);
				found++;
			}
		}
	}
	out_kv(s, "devices", found);
}

/* Read one pin with a chosen internal pull, after letting it settle.
 *
 * The settle time is the point of the loop: against an internal pull of tens of
 * kOhm and a metre of cable at ~100 pF the time constant is a few microseconds,
 * so reading immediately after HAL_GPIO_Init() samples the *previous* state and
 * every answer comes back high. */
static int probe_level(uint16_t pin, uint32_t pull)
{
	GPIO_InitTypeDef g = {0};
	int i;

	g.Pin   = pin;
	g.Mode  = GPIO_MODE_INPUT;
	g.Pull  = pull;
	g.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(GPIOB, &g);

	for (i = 0; i < 10; i++)
		bit_delay();

	return HAL_GPIO_ReadPin(GPIOB, pin) == GPIO_PIN_SET;
}

/* An empty scan has three very different causes and they are indistinguishable
 * from the I2C API.  Fighting each pin against a known internal pull separates
 * them:
 *
 *   internal PULL-DOWN still reads high -> a strong external pull-up is fitted,
 *                                          so the bus is really wired
 *   PULL-UP high, PULL-DOWN low         -> the pin is free; no external
 *                                          pull-ups, probably nothing attached
 *   internal PULL-UP reads low          -> something is pulling the line to
 *                                          ground
 */
void ranger_probe_pins(sink_t s)
{
	int scl_up, sda_up, scl_dn, sda_dn;

	HAL_I2C_DeInit(&hi2c1);
	__HAL_RCC_GPIOB_CLK_ENABLE();

	scl_up = probe_level(GPIO_PIN_6, GPIO_PULLUP);
	sda_up = probe_level(GPIO_PIN_7, GPIO_PULLUP);
	scl_dn = probe_level(GPIO_PIN_6, GPIO_PULLDOWN);
	sda_dn = probe_level(GPIO_PIN_7, GPIO_PULLDOWN);

	out_kv(s, "scl_pb6_int_pullup",   (uint32_t)scl_up);
	out_kv(s, "sda_pb7_int_pullup",   (uint32_t)sda_up);
	out_kv(s, "scl_pb6_int_pulldown", (uint32_t)scl_dn);
	out_kv(s, "sda_pb7_int_pulldown", (uint32_t)sda_dn);

	out_puts(s, "verdict=");
	if (!scl_up || !sda_up)
		out_puts(s, "LINE HELD LOW - short to GND, or a slave holding "
			    "the bus\n");
	else if (scl_dn && sda_dn)
		out_puts(s, "external pull-ups on both lines - bus is wired\n");
	else if (scl_dn || sda_dn)
		out_puts(s, "external pull-up on ONE line only - check the "
			    "other\n");
	else
		out_puts(s, "both pins FLOAT - no external pull-ups, nothing "
			    "connected to PB6/PB7\n");

	if (g_use_bb)
		bb_begin();
	else
		g_inited = i2c_setup();
}

void ranger_bitbang_scan(sink_t s, int swap)
{
	struct bb b;
	unsigned a, found = 0;

	b.scl = swap ? GPIO_PIN_7 : GPIO_PIN_6;
	b.sda = swap ? GPIO_PIN_6 : GPIO_PIN_7;

	bb_begin();

	out_puts(s, swap ? "roles=SWAPPED (scl=PB7 sda=PB6)\n"
			 : "roles=normal (scl=PB6 sda=PB7)\n");

	for (a = 0x08u; a <= 0x77u; a++) {
		bb_start(&b);
		if (bb_write_byte(&b, (uint8_t)(a << 1)) == 0) {
			out_kvx(s, "found", a);
			found++;
		}
		bb_stop(&b);
	}
	out_kv(s, "devices", found);

	if (!g_use_bb)
		g_inited = i2c_setup();
}

/* Raw peripheral state.  Worth having permanently: an I2C block that has
 * latched BUSY looks identical from the API — every call just times out — and
 * SR2 is the only place that distinguishes it from a bus with nothing on it. */
void ranger_dump_hw(sink_t s)
{
	uint32_t sr1 = I2C1->SR1;
	uint32_t sr2 = I2C1->SR2;

	out_kvx(s, "i2c_cr1", I2C1->CR1);
	out_kvx(s, "i2c_cr2", I2C1->CR2);
	out_kvx(s, "i2c_sr1", sr1);
	out_kvx(s, "i2c_sr2", sr2);
	out_kvx(s, "i2c_ccr", I2C1->CCR);
	out_kvx(s, "i2c_trise", I2C1->TRISE);
	out_kv(s, "busy", (sr2 & I2C_SR2_BUSY) ? 1u : 0u);
	out_kv(s, "pe", (I2C1->CR1 & I2C_CR1_PE) ? 1u : 0u);
	out_kv(s, "pclk1_hz", HAL_RCC_GetPCLK1Freq());
	out_kvx(s, "hal_state", (uint32_t)HAL_I2C_GetState(&hi2c1));
	out_kvx(s, "hal_error", HAL_I2C_GetError(&hi2c1));
	out_kv(s, "inited", (uint32_t)g_inited);
	out_kv(s, "speed_hz", g_speed_hz);
	out_puts(s, g_use_bb ? "transport=bitbang\n" : "transport=hardware\n");

	/* HAL_StatusTypeDef: 0 OK, 1 ERROR, 2 BUSY, 3 TIMEOUT.
	 * ErrorCode bits: 01 BERR, 02 ARLO, 04 AF (address/data NAK),
	 * 08 OVR, 20 TIMEOUT, 40 SIZE.  AF means the peripheral drove a real
	 * transfer and the slave did not answer; TIMEOUT with no AF means the
	 * transfer never got off the ground. */
	out_kvx(s, "last_hal_ret", g_last_hal_ret);
	out_kvx(s, "last_hal_err", g_last_hal_err);

	/* PB6/PB7 must read MODER = 10 (alternate function), OTYPER = 1 (open
	 * drain) and AFR[0] nibbles 6 and 7 = 4 (AF4 = I2C1) for the hardware
	 * path.  The bit-bang path leaves MODER = 01 instead. */
	out_kvx(s, "gpiob_moder", GPIOB->MODER);
	out_kvx(s, "gpiob_otyper", GPIOB->OTYPER);
	out_kvx(s, "gpiob_afrl", GPIOB->AFR[0]);
	out_kv(s, "pb6_mode", (GPIOB->MODER >> 12) & 3u);
	out_kv(s, "pb7_mode", (GPIOB->MODER >> 14) & 3u);
	out_kv(s, "pb6_af", (GPIOB->AFR[0] >> 24) & 0xfu);
	out_kv(s, "pb7_af", (GPIOB->AFR[0] >> 28) & 0xfu);
}

/* Re-initialise the hardware path at a different bus speed.
 *
 * Kept mainly as a warning label.  A slow bus is the classic reason a hardware
 * master fails where a bit-banged one succeeds, so sweeping this is the obvious
 * first move — and here it proved nothing, because the fault was edge-coupled
 * crosstalk from a twisted SDA/SCL pair and the F4 samples SDA at a fixed delay
 * after the SCL edge no matter how slow the bus runs.  100 kHz is the v3's
 * ceiling; the part also runs clean at 400 kHz on this rig, out of spec. */
void ranger_set_speed(uint32_t hz)
{
	if (hz < 1000u || hz > 400000u)
		return;
	g_speed_hz = hz;
	if (!g_use_bb)
		g_inited = i2c_setup();
}

void ranger_set_io(int use_bitbang)
{
	g_use_bb = use_bitbang ? 1 : 0;
	if (g_use_bb)
		bb_begin();
	else
		g_inited = i2c_setup();
}

int      ranger_io_bitbang(void) { return g_use_bb; }
uint32_t ranger_speed(void)   { return g_speed_hz; }
int      ranger_model(void)   { return g_model; }
unsigned ranger_addr(void)    { return g_addr7; }
int      ranger_present(void) { return g_present; }
uint32_t ranger_errors(void)  { return g_errors; }
uint16_t ranger_last_cm(void) { return g_last_cm; }

void ranger_set_model(int m)
{
	if (m == 3 || m == 4)
		g_model = m;
}

void ranger_set_addr(unsigned a7)
{
	if (a7 >= 0x08u && a7 <= 0x77u) {
		g_addr7 = a7;
		g_present = 0;		/* re-probed on the next measurement */
	}
}
