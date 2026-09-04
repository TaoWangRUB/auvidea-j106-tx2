/* ranger.h — Garmin LIDAR-Lite ranging on I2C1, PB6/PB7.
 *
 * Deliberately independent of camtrig.c's state: the ranger never reads or
 * writes trigger state, and nothing here can move a TIM2 edge.  The caller
 * prints, so the pulse counter that stamps a reading stays camtrig's business.
 *
 * Wiring: ../../WIRING.md section 4.5.
 */
#ifndef RANGER_H
#define RANGER_H

#include <stdint.h>
#include "camtrig.h"		/* sink_t, for ranger_scan() */

/* 7-bit.  Same default on v3, v3HP and v4 LED. */
#define RANGER_ADDR_DEFAULT	0x62u

typedef enum {
	RNG_OK = 0,
	RNG_ERR_NODEV,		/* nothing acknowledged the address     */
	RNG_ERR_IO,		/* a transfer failed or timed out       */
	RNG_ERR_BUSY,		/* acquisition never cleared its status */
	RNG_ERR_RANGE,		/* answered, but the value is not usable */
} rng_status_t;

/* Bring up I2C1 and probe once.  Never calls Error_Handler(): a missing or
 * miswired ranger must not stop the trigger, which is the one thing this
 * firmware is not allowed to break. */
void ranger_init(void);

/* One acquisition.  Blocks for as long as the sensor takes (typically 5-20 ms
 * on a v3), so the caller must not hold anything the `trig` task needs for
 * longer than it would hold it across a `status` transmit. */
rng_status_t ranger_measure(uint16_t *cm);

const char *ranger_strerror(rng_status_t st);

/* Bring-up aids — none of these are needed in normal operation, and all of
 * them exist because the wiring and the module variant were both unverified
 * when this was written. */
void         ranger_scan(sink_t s);		/* every address that ACKs   */
rng_status_t ranger_read_reg(uint8_t reg, uint8_t *val);
void         ranger_bus_reset(void);		/* 9 clocks, STOP, re-init   */

/* Electrical state of SCL/SDA, which an empty scan cannot distinguish: a bus
 * with nothing on it, a bus with no pull-ups, and a bus with a line shorted low
 * all report zero devices.  Leaves I2C1 re-initialised. */
void         ranger_probe_pins(sink_t s);

/* Address scan done entirely in software on the same two pins, with the SCL/SDA
 * roles selectable.  Two things it settles that the hardware peripheral cannot:
 * whether the two signal wires are swapped (`swap = 1` finds a device the normal
 * orientation misses), and whether an empty scan is the F4's I2C block sulking
 * rather than a silent bus. */
void         ranger_bitbang_scan(sink_t s, int swap);

/* I2C1's raw registers.  The only way to see a latched BUSY, which is
 * indistinguishable from an empty bus through the HAL API. */
void         ranger_dump_hw(sink_t s);

void         ranger_set_speed(uint32_t hz);	/* re-inits I2C1 */
uint32_t     ranger_speed(void);

/* Transport.  Hardware is the default; see the transport note at the top of
 * ranger.c before changing it. */
void         ranger_set_io(int use_bitbang);
int          ranger_io_bitbang(void);

/* ------------------------------------------------------------------ *
 * Streaming
 *
 * `divisor` = emit one reading every N trigger pulses; 0 = off.  Phase-locking
 * to the trigger rather than to a timer is the whole point: the reading then
 * belongs to a specific camera frame by construction, with no clock comparison
 * anywhere.
 *
 * ⚠ A MISSING SENSOR IS NOT AN ERROR HERE.  Streaming with nothing attached
 * must not throw, must not stop the task, and must not emit a line per failed
 * attempt — it backs off and keeps the rest of the firmware working.  The
 * absence is still visible: `status` reports `lidar_present`, and the
 * transition to and from absent is announced exactly once.
 * ------------------------------------------------------------------ */
void         ranger_set_stream(uint32_t divisor);
uint32_t     ranger_stream_divisor(void);

/* The `rng` task.  Owns every autonomous acquisition; the `range` command still
 * measures inline on the `cli` task. */
void         ranger_task(void *arg);

int      ranger_model(void);			/* 3 = v3/v3HP, 4 = v4 LED   */
void     ranger_set_model(int m);
unsigned ranger_addr(void);			/* 7-bit                     */
void     ranger_set_addr(unsigned a7);
int      ranger_present(void);			/* result of the last probe  */
uint32_t ranger_errors(void);
uint16_t ranger_last_cm(void);

#endif /* RANGER_H */
