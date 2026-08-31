/* cli.c — command transports and line assembly.
 *
 * The command protocol is transport-agnostic by contract: identical syntax,
 * semantics and replies on every transport, with a reply going back to
 * whichever transport issued the command and unsolicited output going to all
 * of them (design D11).  Today that is USART1 alone; the USB CDC sink slots in
 * beside it without the protocol or camtrig.c changing.
 *
 * Bytes arrive in interrupt context and are assembled per transport; only a
 * complete line is handed to the `cli` task, tagged with where it came from.
 * Assembling in the ISR keeps the queue one-message-per-command rather than
 * one-per-byte, and keeps the origin attached to the line without a second
 * queue or a queue set.
 */
#include "main.h"
#include "camtrig.h"
#include "cmsis_os2.h"
#include "queue.h"
#include "usbd_cdc_if.h"
#include "usb_device.h"

extern UART_HandleTypeDef huart1;

#define LINE_MAX  64
#define CMD_DEPTH 4

typedef struct {
	sink_t from;
	char   line[LINE_MAX];
} cmd_msg_t;

struct line_asm {
	char     buf[LINE_MAX];
	unsigned len;
	int      overflow;	/* drop the rest of an over-long line rather than
				 * execute a truncated command */
};

static struct line_asm g_asm[2];		/* indexed by sink_t: UART, USB */
static QueueHandle_t   g_cmdq;
static uint32_t        g_dropped;		/* queue-full commands, in status */

/* ------------------------------------------------------------------ *
 * Sinks
 *
 * Polled and blocking, as the pre-restructure build was: at 115200 a full
 * `status` is ~26 ms of transmit.  That is on the command path only, never on
 * the waveform, which TIM1 emits without software involvement.
 * ------------------------------------------------------------------ */
static void uart_putc(char c)
{
	while (!__HAL_UART_GET_FLAG(&huart1, UART_FLAG_TXE))
		;
	huart1.Instance->DR = (uint8_t)c;
}

void out_putc(sink_t s, char c)
{
	if (s == SINK_UART || s == SINK_ALL)
		uart_putc(c);
	if (s == SINK_USB || s == SINK_ALL)
		cdc_putc(c);		/* never blocks; drops if no reader */
}

uint32_t cli_dropped(void)
{
	return g_dropped;
}

/* ------------------------------------------------------------------ *
 * Line assembly — ISR context
 * ------------------------------------------------------------------ */
void cli_rx_from_isr(sink_t from, char c, BaseType_t *woken)
{
	struct line_asm *la = &g_asm[from];

	if (c != '\r' && c != '\n') {
		if (la->len < LINE_MAX - 1)
			la->buf[la->len++] = c;
		else
			la->overflow = 1;
		return;
	}

	if (la->overflow || la->len) {
		cmd_msg_t msg;
		unsigned i;

		msg.from = from;
		if (la->overflow) {
			/* Signal it as a command the task will reject, rather
			 * than silently running a truncated one. */
			msg.line[0] = '\x01';
			msg.line[1] = 0;
		} else {
			for (i = 0; i < la->len; i++)
				msg.line[i] = la->buf[i];
			msg.line[la->len] = 0;
		}
		if (xQueueSendFromISR(g_cmdq, &msg, woken) != pdTRUE)
			g_dropped++;
	}
	la->len = 0;
	la->overflow = 0;
}

/* ------------------------------------------------------------------ *
 * Task
 * ------------------------------------------------------------------ */
void cli_task(void *arg)
{
	cmd_msg_t msg;

	(void)arg;

	/* Start USB from task context: the OTG ISR can fire as soon as the
	 * device connects, and it uses ...FromISR APIs that require a running
	 * scheduler. */
	if (clock_usb_available())
		MX_USB_DEVICE_Init();

	camtrig_banner(SINK_ALL);

	for (;;) {
		if (xQueueReceive(g_cmdq, &msg, portMAX_DELAY) != pdTRUE)
			continue;

		if (msg.line[0] == '\x01') {
			out_puts(msg.from, "err line too long\n");
			continue;
		}
		camtrig_handle(msg.line, msg.from);
	}
}

void cli_init(void)
{
	unsigned i;

	for (i = 0; i < 2; i++) {
		g_asm[i].len = 0;
		g_asm[i].overflow = 0;
	}

	g_cmdq = xQueueCreate(CMD_DEPTH, sizeof(cmd_msg_t));
	configASSERT(g_cmdq != NULL);

	/* Priority 5 == configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY: the
	 * highest priority from which xQueueSendFromISR may be called. */
	HAL_NVIC_SetPriority(USART1_IRQn, 5, 0);
	HAL_NVIC_EnableIRQ(USART1_IRQn);
	__HAL_UART_ENABLE_IT(&huart1, UART_IT_RXNE);
}
