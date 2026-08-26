/* lcd_ui.c — what the on-board 0.96" ST7735 actually shows.
 *
 * The panel is reachable at all because the trigger moved from TIM1 on
 * PE9/PE11/PE13/PE14 to TIM5 on PA0-PA3; those port E pins are the display's
 * CS / WR_RS / MOSI / SCK / backlight.  See camtrig.h.
 *
 * This runs at the LOWEST task priority and repaints twice a second.  The
 * panel is a slow serial device, and nothing about the rig may wait on it:
 * the waveform is emitted by TIM5 in hardware, and the command path sits above
 * this task.  If the display were unplugged, faulty, or simply slow, the only
 * consequence would be a stale screen.
 */
#include "main.h"
#include "camtrig.h"
#include "cmsis_os2.h"
#include "lcd.h"
#include "usbd_cdc_if.h"

/* The driver reports 160x80 (confirmed at runtime: lcd_w=160, lcd_h=80), which
 * would be 20 columns x 5 rows at 8x16 px.  The glass shows LESS than that:
 * measured on the bench, column 19 and row 4 are both invisible even when drawn
 * from x=0,y=0 with every clip in the driver satisfied.  The panel's visible
 * window is evidently inset from the controller's addressable area, and the
 * HannStar/BOE offset pair the driver knows about differ by only (1,2) px --
 * far too little to account for a whole glyph or row.
 *
 * So the layout targets what actually renders, 19 x 4, rather than what the
 * driver claims. Everything the 5-row version showed still fits. */
#define NROWS	4
#define ROW(n)	((n) * 16)
#define COLS	19

uint32_t g_lcd_w, g_lcd_h;
uint32_t g_backlight = 100;	/* percent; `bl <0-100>` */

void lcd_set_backlight(uint32_t pct)
{
	if (pct > 100u)
		pct = 100u;
	g_backlight = pct;
	LCD_SetBrightness(pct * 999u / 100u);
}	/* actual panel geometry, reported in `status` */

static char g_row[NROWS][COLS + 1];
static char g_shown[NROWS][COLS + 1];

/* Minimal formatting, deliberately: no printf.  The reply path already
 * formats integers without it (design D10), and pulling a variadic formatter
 * in for four lines of text would grow the image for nothing. */
/* pad < 0 => zero-pad to -pad digits; pad > 0 => space-pad to pad width.
 * The fractional part of a frame rate must zero-pad: 30.0 fps has a remainder
 * of 0, which space-padded rendered as "30. 0fps". */
static int putu(char *b, int i, uint32_t v, int pad)
{
	char t[11];
	int n = 0;
	char fill = (pad < 0) ? '0' : ' ';
	int want = (pad < 0) ? -pad : pad;

	if (!v)
		t[n++] = '0';
	while (v) {
		t[n++] = (char)('0' + v % 10);
		v /= 10;
	}
	while (want-- > n)
		if (i < COLS) b[i++] = fill;
	while (n)
		if (i < COLS) b[i++] = t[--n]; else n--;
	return i;
}

static int puts_(char *b, int i, const char *s)
{
	while (*s && i < COLS)
		b[i++] = *s++;
	return i;
}

static void build(void)
{
	uint32_t fps_m = camtrig_fps_milli();
	unsigned r;
	int i;

	for (r = 0; r < NROWS; r++) {
		for (i = 0; i < COLS; i++)
			g_row[r][i] = ' ';
		g_row[r][COLS] = 0;
	}

	/* 0: run state, rate, and whether a host is really attached */
	i = puts_(g_row[0], 0, camtrig_running() ? "RUN " : "STOP");
	i = putu(g_row[0], i, fps_m / 1000u, 0);
	i = puts_(g_row[0], i, ".");
	i = putu(g_row[0], i, (fps_m % 1000u) / 10u, -2);
	i = puts_(g_row[0], i, "fps");
	if (cdc_ready())
		puts_(g_row[0], 16, "USB");

	/* 1: exposure of ch1; '*' when the four channels differ */
	i = puts_(g_row[1], 0, "exp ");
	i = putu(g_row[1], i, camtrig_exp_us(0), 0);
	i = puts_(g_row[1], i, "us");
	if (!camtrig_exp_uniform())
		puts_(g_row[1], i + 1, "*");

	/* 2: emitted pulse count — advances iff the trigger really runs */
	i = puts_(g_row[2], 0, "pulses ");
	i = putu(g_row[2], i, camtrig_pulses(), 0);

	/* 3: the two opto knobs, and which clock came up.  "HSI!" means the
	 * crystal did not start: 1% instead of 20 ppm, and no USB. */
	i = puts_(g_row[3], 0, "pol ");
	i = putu(g_row[3], i, (uint32_t)camtrig_polarity(), 0);
	i = puts_(g_row[3], i, " sk");
	i = putu(g_row[3], i, camtrig_skew_ns(), 0);
	puts_(g_row[3], 14, camtrig_on_hse() ? "hse25" : "HSI!");
}

void lcd_task(void *arg)
{
	unsigned r;

	(void)arg;

	LCD_Init();
	lcd_set_backlight(g_backlight);
	ST7735_GetXSize(&st7735_pObj, &g_lcd_w);
	ST7735_GetYSize(&st7735_pObj, &g_lcd_h);
	POINT_COLOR = WHITE;
	BACK_COLOR  = BLACK;

	for (r = 0; r < NROWS; r++)
		g_shown[r][0] = 0;

	for (;;) {
		build();

		/* Repaint only rows that changed.  A full 5-row redraw is a few
		 * thousand SPI bytes; at 2 Hz that is harmless, but most ticks
		 * change one row and there is no reason to send the rest. */
		for (r = 0; r < NROWS; r++) {
			int same = 1, i;

			for (i = 0; i <= COLS; i++) {
				if (g_row[r][i] != g_shown[r][i]) {
					same = 0;
					break;
				}
			}
			if (same)
				continue;

			for (i = 0; i <= COLS; i++)
				g_shown[r][i] = g_row[r][i];

			LCD_ShowString(0, ROW(r), 160, 16, 16,
				       (uint8_t *)g_row[r]);
		}

		/* Safety drain: if a transmit was refused while nothing further
		 * was being written, nobody else would re-pump. Costs nothing
		 * when the ring is empty. */
		cdc_pump();

		osDelay(500);
	}
}
