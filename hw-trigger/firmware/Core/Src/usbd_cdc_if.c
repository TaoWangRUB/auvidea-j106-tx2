/* usbd_cdc_if.c — the CDC-ACM side of the camtrig command transport.
 *
 * Receive: bytes handed up by the class run straight into the same line
 * assembler USART1 feeds, tagged SINK_USB, so the protocol, the parser and
 * every reply string are shared rather than duplicated.
 *
 * Transmit: a ring buffer drained on the TX-complete callback.  CDC transmits
 * are asynchronous — USBD_CDC_TransmitPacket returns USBD_BUSY while a previous
 * IN transfer is still in flight — and ignoring that return is the standard way
 * CDC output silently truncates.  Equally, out_putc() must never block waiting
 * for a host, because a host that has enumerated the port but is not reading it
 * would otherwise stall whichever task is printing.  So writes are lossy by
 * construction and the loss is *counted*, surfacing in `status` as
 * usb_dropped rather than as mysteriously missing text.
 */
#include "usbd_cdc_if.h"
#include "usb_device.h"
#include "camtrig.h"
#include "FreeRTOS.h"
#include "task.h"

/* Line coding is accepted and echoed back but has no effect: there is no real
 * UART behind this endpoint, and the host's baud rate is meaningless. */
static USBD_CDC_LineCodingTypeDef g_lc = { 115200U, 0U, 0U, 8U };

static uint8_t  g_rx[CDC_DATA_FS_MAX_PACKET_SIZE];

#define TXRING 512U
static uint8_t   g_tx[TXRING];
static volatile uint32_t g_head, g_tail;
static uint8_t   g_txpkt[CDC_DATA_FS_MAX_PACKET_SIZE];
static volatile int      g_tx_busy;
static volatile uint32_t g_dropped;

/* Updated from the SOF callback.  A host emits a start-of-frame every 1 ms, so
 * silence means unplugged or suspended.
 *
 * This exists because `vbus_sensing_enable` must be DISABLE on this board --
 * VBUS is not routed to the MCU (design D4) -- which leaves the core unable to
 * detect cable removal.  dev_state therefore stays CONFIGURED after an unplug,
 * so it is not a usable presence indicator on its own. */
volatile uint32_t g_last_sof;

static int8_t CDC_Init_FS(void);
static int8_t CDC_DeInit_FS(void);
static int8_t CDC_Control_FS(uint8_t cmd, uint8_t *pbuf, uint16_t length);
static int8_t CDC_Receive_FS(uint8_t *pbuf, uint32_t *Len);
static int8_t CDC_TransmitCplt_FS(uint8_t *pbuf, uint32_t *Len, uint8_t epnum);

USBD_CDC_ItfTypeDef USBD_Interface_fops_FS = {
	CDC_Init_FS,
	CDC_DeInit_FS,
	CDC_Control_FS,
	CDC_Receive_FS,
	CDC_TransmitCplt_FS,
};

int cdc_ready(void)
{
	if (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED)
		return 0;
	/* 50 ms of no SOF = gone.  Generous against a momentarily busy host,
	 * still far quicker than a human notices. */
	return (HAL_GetTick() - g_last_sof) < 50u;
}

uint32_t cdc_dropped(void)
{
	return g_dropped;
}

/* Kick the ring into the endpoint if one is not already in flight.
 *
 * Reached from BOTH task context (cdc_putc) and ISR context (TransmitCplt), so
 * the masking must save and restore the previous BASEPRI.  portDISABLE/ENABLE_
 * INTERRUPTS would clear it to 0 outright, which inside an ISR unmasks
 * everything instead of restoring — a nested-interrupt bug that only shows up
 * under load.  The ...FROM_ISR pair is correct in either context.
 *
 * The g_tx_busy handshake is what makes the two callers safe: a TransmitCplt
 * can only fire while a transfer is in flight, which implies g_tx_busy was
 * already 1, so it cannot race a task that is between setting the flag and
 * calling TransmitPacket. */
static void tx_pump(void)
{
	uint32_t n = 0, tail;
	UBaseType_t mask;

	if (!cdc_ready())
		return;

	mask = taskENTER_CRITICAL_FROM_ISR();
	if (g_tx_busy || g_head == g_tail) {
		taskEXIT_CRITICAL_FROM_ISR(mask);
		return;
	}
	/* Copy WITHOUT advancing the tail: the bytes stay in the ring until the
	 * class actually accepts them.  Consuming first and dropping on a failed
	 * TransmitPacket loses a whole packet every time, which is what shredded
	 * long replies -- USBD_CDC_DataIn() defers TxState/TransmitCplt by one
	 * interrupt whenever a transfer is an exact multiple of the 64-byte max
	 * packet (it sends a ZLP first), so a private busy flag inevitably
	 * desyncs from the class's own state and TransmitPacket returns BUSY. */
	tail = g_tail;
	while (n < sizeof(g_txpkt) && tail != g_head) {
		g_txpkt[n++] = g_tx[tail];
		tail = (tail + 1U) % TXRING;
	}
	g_tx_busy = 1;
	taskEXIT_CRITICAL_FROM_ISR(mask);

	USBD_CDC_SetTxBuffer(&hUsbDeviceFS, g_txpkt, (uint16_t)n);
	mask = taskENTER_CRITICAL_FROM_ISR();
	if (USBD_CDC_TransmitPacket(&hUsbDeviceFS) == USBD_OK) {
		g_tail = tail;		/* commit only on success */
	} else {
		/* Still busy, or the host went away.  Keep the bytes; the next
		 * TransmitCplt or write re-pumps.  Nothing is lost. */
		g_tx_busy = 0;
	}
	taskEXIT_CRITICAL_FROM_ISR(mask);
}

void cdc_pump(void)
{
	tx_pump();
}

void cdc_putc(char c)
{
	uint32_t next;
	UBaseType_t mask;

	if (!cdc_ready()) {
		g_dropped++;		/* no host: discard, never block -- but
					 * count it, so "where did my output go"
					 * has an answer in `status` */
		return;
	}

	mask = taskENTER_CRITICAL_FROM_ISR();
	next = (g_head + 1U) % TXRING;
	if (next == g_tail) {
		g_dropped++;		/* ring full: drop, keep the trigger running */
		taskEXIT_CRITICAL_FROM_ISR(mask);
		return;
	}
	g_tx[g_head] = (uint8_t)c;
	g_head = next;
	taskEXIT_CRITICAL_FROM_ISR(mask);

	tx_pump();
}

static int8_t CDC_Init_FS(void)
{
	g_head = g_tail = 0;
	g_tx_busy = 0;
	USBD_CDC_SetTxBuffer(&hUsbDeviceFS, g_txpkt, 0);
	USBD_CDC_SetRxBuffer(&hUsbDeviceFS, g_rx);
	return USBD_OK;
}

static int8_t CDC_DeInit_FS(void)
{
	g_tx_busy = 0;
	return USBD_OK;
}

static int8_t CDC_Control_FS(uint8_t cmd, uint8_t *pbuf, uint16_t length)
{
	(void)length;

	switch (cmd) {
	case CDC_SET_LINE_CODING:
		g_lc.bitrate    = (uint32_t)(pbuf[0] | (pbuf[1] << 8) |
					     (pbuf[2] << 16) | (pbuf[3] << 24));
		g_lc.format     = pbuf[4];
		g_lc.paritytype = pbuf[5];
		g_lc.datatype   = pbuf[6];
		break;

	case CDC_GET_LINE_CODING:
		pbuf[0] = (uint8_t)(g_lc.bitrate);
		pbuf[1] = (uint8_t)(g_lc.bitrate >> 8);
		pbuf[2] = (uint8_t)(g_lc.bitrate >> 16);
		pbuf[3] = (uint8_t)(g_lc.bitrate >> 24);
		pbuf[4] = g_lc.format;
		pbuf[5] = g_lc.paritytype;
		pbuf[6] = g_lc.datatype;
		break;

	case CDC_SET_CONTROL_LINE_STATE:
		/* DTR/RTS deliberately ignored.  The spec requires the device
		 * to run and stay enumerated whether or not the host asserts
		 * any modem control signal, and screen/minicom/pyserial differ
		 * in what they assert.  Gating output on DTR is why some CDC
		 * devices appear mute to one terminal and fine to another. */
		break;

	default:
		break;
	}
	return USBD_OK;
}

/* Called from OTG_FS_IRQHandler at priority 5 (== configMAX_SYSCALL), so
 * ...FromISR is legal here. */
static int8_t CDC_Receive_FS(uint8_t *pbuf, uint32_t *Len)
{
	BaseType_t woken = pdFALSE;
	uint32_t i, n = *Len;

	for (i = 0; i < n; i++)
		cli_rx_from_isr(SINK_USB, (char)pbuf[i], &woken);

	USBD_CDC_SetRxBuffer(&hUsbDeviceFS, g_rx);
	USBD_CDC_ReceivePacket(&hUsbDeviceFS);

	portYIELD_FROM_ISR(woken);
	return USBD_OK;
}

static int8_t CDC_TransmitCplt_FS(uint8_t *pbuf, uint32_t *Len, uint8_t epnum)
{
	(void)pbuf;
	(void)Len;
	(void)epnum;

	g_tx_busy = 0;
	tx_pump();			/* drain whatever queued while in flight */
	return USBD_OK;
}
