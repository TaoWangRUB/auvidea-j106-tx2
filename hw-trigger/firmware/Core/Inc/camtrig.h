/* camtrig.h — the IMX296 trigger generator's own interface.
 *
 * Everything here is board- and sensor-specific.  The HAL underneath is
 * incidental; the numbers are from IMX296LQR-C_Fulldatasheet_Awin.pdf and are
 * unchanged from the pre-restructure bare-metal build (design D12).
 */
#ifndef CAMTRIG_H
#define CAMTRIG_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"

/* ------------------------------------------------------------------ *
 * Sensor limits.  Datasheet constants in physical units, so they can be
 * checked against the datasheet directly rather than being pre-baked into
 * tick counts.
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

/* TIM5_CH1..CH4 on PA0 / PA1 / PA2 / PA3, all AF2.
 *
 * Moved here from TIM1 on PE9/PE11/PE13/PE14.  Those pins are the on-board
 * TFT-LCD connector (PE11 = CS, PE13 = WR_RS, PE14 = MOSI, PE12 = SCK,
 * PE10 = backlight), so using them for the trigger made the integrated display
 * unusable.  TIM5's channels land on PA0-PA3, which this board leaves entirely
 * free, and the swap costs nothing: the optocouplers are not wired yet.
 *
 * TIM5 is also a 32-bit counter, unlike TIM1.  The auto-prescaler therefore
 * settles on div = 1 for every rate in range, so pulse-width resolution is one
 * timer tick — 8.33 ns at 120 MHz — instead of the ~517 ns that a 16-bit ARR
 * forced.  See WIRING.md for the header pin numbers. */
#define LED_PIN			3		/* PE3, blue, active low on this board */

/* ------------------------------------------------------------------ *
 * Output sinks — a command's reply goes back to the transport it arrived
 * on; unsolicited output goes to all of them (design D11).
 * ------------------------------------------------------------------ */
typedef enum {
	SINK_UART = 0,		/* USART1 on PA9/PA10                 */
	SINK_USB,		/* USB CDC-ACM (added in task group 6) */
	SINK_ALL		/* banner, burst-done: every transport */
} sink_t;

void out_putc(sink_t s, char c);
void out_puts(sink_t s, const char *str);
void out_putu(sink_t s, uint32_t v);
void out_kv(sink_t s, const char *k, uint32_t v);
void out_kvx(sink_t s, const char *k, uint32_t v);	/* hex, for addresses */

/* ------------------------------------------------------------------ *
 * Trigger core
 * ------------------------------------------------------------------ */
void     camtrig_init(void);		/* TIM1 + boot defaults + start */
uint32_t camtrig_timer_hz(void);

/* Read-only state, for the LCD.  All take the trigger mutex internally. */
uint32_t camtrig_fps_milli(void);
uint32_t camtrig_pulses(void);
uint32_t camtrig_skew_ns(void);
uint32_t camtrig_exp_us(unsigned ch);
int      camtrig_running(void);
int      camtrig_polarity(void);
int      camtrig_exp_uniform(void);
int      camtrig_on_hse(void);
void     lcd_set_backlight(uint32_t pct);	/* 0..100 */
extern uint32_t g_backlight;	/* false => the HSI fallback path is live */

/* The `trig` task: counts emitted pulses, terminates bursts, drives the LED.
 * It does NOT generate the waveform — TIM1 does that in hardware, so this task
 * being delayed, starved or killed cannot move an edge. */
void camtrig_task(void *arg);

/* Called from TIM1_UP_IRQHandler.  Uses a *counting* notification rather than
 * a flag, so pulses that arrive while the task is blocked are not lost. */
void camtrig_notify_pulse_from_isr(BaseType_t *woken);

/* Command interface.  `line` is modified in place. */
void camtrig_handle(char *line, sink_t reply);
void camtrig_banner(sink_t s);

/* cli.c: a line has been assembled from `from`.  Called from ISR context. */
void cli_rx_from_isr(sink_t from, char c, BaseType_t *woken);
void cli_task(void *arg);

#endif /* CAMTRIG_H */
