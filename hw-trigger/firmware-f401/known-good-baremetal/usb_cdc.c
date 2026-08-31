/* usb_cdc.c — USB CDC-ACM device for the STM32F401 OTG_FS core.
 *
 * WHY THIS IS HAND-WRITTEN
 * ------------------------
 * The H7 build gets CDC from ST's STM32_USB_Device_Library sitting on top of
 * HAL_PCD.  That library's Core and CDC class are chip-independent and are
 * already vendored under ../firmware/Middlewares/, but they need a PCD layer,
 * and this repo has no F4 HAL -- only STM32H7xx_HAL_Driver.  Pulling CubeF4 in
 * would drag a multi-megabyte vendor tree into a build whose whole point is
 * that `make` needs nothing but a compiler.  A single-purpose CDC written
 * straight against OTG_FS is smaller than the PCD layer alone would be, and
 * every register write is visible.
 *
 * WHY POLLED, NOT INTERRUPT-DRIVEN
 * --------------------------------
 * The trigger waveform is emitted by TIM2 in hardware and is completely
 * independent of the CPU.  The only interrupt in this firmware is TIM2's
 * update, which counts pulses and ends a burst.  Adding a USB interrupt that
 * can preempt it buys nothing -- there is no latency requirement on the
 * console -- and costs the one property worth protecting: that nothing the
 * host does over USB can perturb frame timing.  usb_cdc_poll() is called from
 * the main loop and everything happens there.
 *
 * Reference: RM0368 rev 5, chapter 22 (USB OTG FS).
 */

#include "usb_cdc.h"

#define REG(a)		(*(volatile uint32_t *)(a))

#define OTG_BASE	0x50000000u
#define OTG_DEV		(OTG_BASE + 0x800u)
#define OTG_FIFO(i)	(OTG_BASE + 0x1000u + 0x1000u * (i))

/* Global */
#define GAHBCFG		REG(OTG_BASE + 0x008)
#define GUSBCFG		REG(OTG_BASE + 0x00C)
#define GRSTCTL		REG(OTG_BASE + 0x010)
#define GINTSTS		REG(OTG_BASE + 0x014)
#define GINTMSK		REG(OTG_BASE + 0x018)
#define GRXSTSP		REG(OTG_BASE + 0x020)
#define GRXFSIZ		REG(OTG_BASE + 0x024)
#define DIEPTXF0	REG(OTG_BASE + 0x028)
#define GCCFG		REG(OTG_BASE + 0x038)
#define DIEPTXF(i)	REG(OTG_BASE + 0x104 + 4u * ((i) - 1u))

/* Device */
#define DSTS		REG(OTG_DEV + 0x08)
#define DCFG		REG(OTG_DEV + 0x00)
#define DCTL		REG(OTG_DEV + 0x04)
#define DIEPMSK		REG(OTG_DEV + 0x10)
#define DOEPMSK		REG(OTG_DEV + 0x14)
#define DAINTMSK	REG(OTG_DEV + 0x1C)
#define DIEPCTL(i)	REG(OTG_DEV + 0x100 + 0x20u * (i))
#define DIEPINT(i)	REG(OTG_DEV + 0x108 + 0x20u * (i))
#define DIEPTSIZ(i)	REG(OTG_DEV + 0x110 + 0x20u * (i))
#define DTXFSTS(i)	REG(OTG_DEV + 0x118 + 0x20u * (i))
#define DOEPCTL(i)	REG(OTG_DEV + 0x300 + 0x20u * (i))
#define DOEPINT(i)	REG(OTG_DEV + 0x308 + 0x20u * (i))
#define DOEPTSIZ(i)	REG(OTG_DEV + 0x310 + 0x20u * (i))

#define PCGCCTL		REG(OTG_BASE + 0xE00)

#define RCC_AHB1ENR	REG(0x40023830u)
#define RCC_AHB2ENR	REG(0x40023834u)
#define GPIOA_MODER	REG(0x40020000u + 0x00)
#define GPIOA_OSPEEDR	REG(0x40020000u + 0x08)
#define GPIOA_AFRH	REG(0x40020000u + 0x24)

/* Endpoints: EP0 control, EP1 bulk in/out (data), EP2 interrupt in (notify) */
#define EP_CTL		0u
#define EP_DATA		1u
#define EP_NOTIFY	2u
#define MPS		64u

/* ------------------------------------------------------------------ *
 * Descriptors.
 *
 * VID:PID is ST's 0483:5740 -- deliberately the same pair the H7 build
 * enumerated as, so any udev rule, script or muscle memory that found the old
 * board finds this one unchanged.
 * ------------------------------------------------------------------ */
static const uint8_t dev_desc[18] = {
	18, 1,				/* bLength, DEVICE            */
	0x00, 0x02,			/* bcdUSB 2.00                */
	0x02, 0x00, 0x00,		/* class CDC, subclass, proto */
	MPS,				/* bMaxPacketSize0            */
	0x83, 0x04,			/* idVendor  0x0483           */
	0x40, 0x57,			/* idProduct 0x5740           */
	0x00, 0x02,			/* bcdDevice 2.00             */
	1, 2, 3,			/* iManufacturer/Product/Serial */
	1				/* bNumConfigurations         */
};

#define CONF_LEN 67
static const uint8_t conf_desc[CONF_LEN] = {
	9, 2, CONF_LEN, 0, 2, 1, 0, 0xC0, 50,	/* configuration, 2 interfaces */

	9, 4, 0, 0, 1, 0x02, 0x02, 0x01, 0,	/* interface 0: CDC comm     */
	5, 0x24, 0x00, 0x10, 0x01,		/* CDC header  1.10          */
	5, 0x24, 0x01, 0x00, 1,			/* call management           */
	4, 0x24, 0x02, 0x02,			/* ACM: supports line coding */
	5, 0x24, 0x06, 0, 1,			/* union: master 0, slave 1  */
	7, 5, 0x80 | EP_NOTIFY, 0x03, 8, 0, 16,	/* EP2 IN interrupt, 8 B     */

	9, 4, 1, 0, 2, 0x0A, 0x00, 0x00, 0,	/* interface 1: CDC data     */
	7, 5, EP_DATA, 0x02, MPS, 0, 0,		/* EP1 OUT bulk              */
	7, 5, 0x80 | EP_DATA, 0x02, MPS, 0, 0	/* EP1 IN  bulk              */
};

static const uint8_t str_lang[4]  = { 4, 3, 0x09, 0x04 };
static const uint8_t str_manuf[]  = { 18, 3, 'W',0,'e',0,'A',0,'c',0,'t',0,'-',0,'J',0,'1',0 };
static const uint8_t str_prod[]   = { 26, 3, 'c',0,'a',0,'m',0,'t',0,'r',0,'i',0,'g',0,'-',0,'f',0,'4',0,'0',0,'1',0 };
static const uint8_t str_serial[] = { 10, 3, 'J',0,'1',0,'0',0,'6',0 };

/* ------------------------------------------------------------------ *
 * State
 * ------------------------------------------------------------------ */
static uint8_t  rx_buf[256], tx_buf[256];
static volatile uint16_t rx_head, rx_tail, tx_head, tx_tail;
static uint8_t  setup_pkt[24];
static const uint8_t *ep0_src;
static uint16_t ep0_left;
static uint8_t  pending_addr;
static int      configured, dtr_open;
static uint8_t  line_coding[7] = { 0x00, 0xC2, 0x01, 0x00, 0, 0, 8 };  /* 115200 8N1 */
static int      tx_busy;
/* Set when a control transfer has a host-to-device data stage still to come.
 * The status IN must not be sent until that data has actually arrived --
 * ACKing early is a protocol error that Linux reports as a descriptor read
 * failure, intermittently and long after the fact. */
static int      ep0_out_pending;

/* Event counters.  With no debugger on this board, the only way to tell
 * "SETUP never arrived" from "SETUP arrived and we answered it wrongly" is to
 * count what actually happened and print it over the UART. */
static volatile uint32_t n_reset, n_enum, n_setup, n_rxpkt, n_in_xfrc, last_pktsts;
/* The first SETUP exactly as parsed, plus what we decided to reply with.
 * Captured here and printed later: writing to the UART inside the control
 * path would block for milliseconds and break the very transfer we are
 * trying to observe. */
static volatile uint8_t  first_setup[8];
static volatile uint16_t first_reply_len, first_setup_cnt;

static void fifo_write(uint8_t ep, const uint8_t *p, uint16_t n)
{
	volatile uint32_t *f = (volatile uint32_t *)OTG_FIFO(ep);
	uint16_t i;

	for (i = 0; i < n; i += 4) {
		uint32_t w = 0;
		uint8_t k;

		for (k = 0; k < 4 && i + k < n; k++)
			w |= (uint32_t)p[i + k] << (8 * k);
		*f = w;
	}
}

static void fifo_read(uint8_t *p, uint16_t n)
{
	volatile uint32_t *f = (volatile uint32_t *)OTG_FIFO(0);
	uint16_t i;

	for (i = 0; i < n; i += 4) {
		uint32_t w = *f;
		uint8_t k;

		for (k = 0; k < 4 && i + k < n; k++)
			p[i + k] = (uint8_t)(w >> (8 * k));
	}
}

/* Discard n bytes from the RX FIFO.  Needed because the FIFO is a single
 * shared queue: leaving an unread packet in it wedges every endpoint, not
 * just the one that received it. */
static void fifo_drop(uint16_t n)
{
	volatile uint32_t *f = (volatile uint32_t *)OTG_FIFO(0);
	uint16_t i;

	for (i = 0; i < n; i += 4)
		(void)*f;
}

static void ep_in_send(uint8_t ep, const uint8_t *p, uint16_t n)
{
	DIEPTSIZ(ep) = (1u << 19) | n;		/* one packet, n bytes */
	DIEPCTL(ep) |= (1u << 31) | (1u << 26);	/* EPENA | CNAK        */
	if (n)
		fifo_write(ep, p, n);
}

/* Control-IN transfers longer than one packet are fed a packet at a time from
 * ep0_src; the host asks for the next by ACKing the last. */
static void ep0_continue(void)
{
	uint16_t n = ep0_left > MPS ? MPS : ep0_left;

	ep_in_send(EP_CTL, ep0_src, n);
	ep0_src  += n;
	ep0_left -= n;
}

static void ep0_send(const uint8_t *p, uint16_t n, uint16_t requested)
{
	if (n > requested)
		n = requested;
	if (!first_reply_len)
		first_reply_len = n ? n : 0xFFFFu;	/* 0xFFFF = a real ZLP */
	ep0_src  = p;
	ep0_left = n;
	ep0_continue();
}

static void ep0_stall(void)
{
	DIEPCTL(EP_CTL) |= (1u << 21);		/* STALL */
	DOEPCTL(EP_CTL) |= (1u << 21);
}

static void ep0_prepare_setup(void)
{
	DOEPTSIZ(EP_CTL) = (3u << 29) | (1u << 19) | (3u * 8u);
	DOEPCTL(EP_CTL) |= (1u << 31) | (1u << 26);
}

static void ep_out_prepare(uint8_t ep)
{
	DOEPTSIZ(ep) = (1u << 19) | MPS;
	DOEPCTL(ep) |= (1u << 31) | (1u << 26);
}

/* ------------------------------------------------------------------ *
 * Standard and CDC control requests
 * ------------------------------------------------------------------ */
static void handle_setup(void)
{
	uint8_t  type  = setup_pkt[0];
	uint8_t  req   = setup_pkt[1];
	uint16_t value = (uint16_t)(setup_pkt[2] | (setup_pkt[3] << 8));
	uint16_t len   = (uint16_t)(setup_pkt[6] | (setup_pkt[7] << 8));

	if ((type & 0x60) == 0x00) {			/* standard */
		switch (req) {
		case 0x05:				/* SET_ADDRESS */
			/* The address may only take effect after the status
			 * stage completes, so it is stashed and applied when
			 * the zero-length IN is acknowledged. */
			pending_addr = (uint8_t)(value & 0x7F);
			ep_in_send(EP_CTL, 0, 0);
			return;
		case 0x06:				/* GET_DESCRIPTOR */
			switch (value >> 8) {
			case 1:
				ep0_send(dev_desc, sizeof dev_desc, len);
				return;
			case 2:
				ep0_send(conf_desc, CONF_LEN, len);
				return;
			case 3:
				switch (value & 0xFF) {
				case 0: ep0_send(str_lang,   sizeof str_lang,   len); return;
				case 1: ep0_send(str_manuf,  sizeof str_manuf,  len); return;
				case 2: ep0_send(str_prod,   sizeof str_prod,   len); return;
				case 3: ep0_send(str_serial, sizeof str_serial, len); return;
				}
				break;
			}
			break;
		case 0x09:				/* SET_CONFIGURATION */
			configured = (value != 0);
			ep_in_send(EP_CTL, 0, 0);
			return;
		case 0x08: {				/* GET_CONFIGURATION */
			static uint8_t cfg;

			cfg = (uint8_t)configured;
			ep0_send(&cfg, 1, len);
			return;
		}
		case 0x00: {				/* GET_STATUS */
			static const uint8_t zero[2] = { 0, 0 };

			ep0_send(zero, 2, len);
			return;
		}
		case 0x01: case 0x03:			/* CLEAR/SET_FEATURE */
			ep_in_send(EP_CTL, 0, 0);
			return;
		}
	} else if ((type & 0x60) == 0x20) {		/* class: CDC */
		switch (req) {
		case 0x20:				/* SET_LINE_CODING */
			/* The 7 data bytes arrive as a separate OUT packet.
			 * Their content is meaningless for a device with no
			 * real UART behind it, but the status stage still has
			 * to wait for them. */
			ep0_out_pending = 1;
			return;
		case 0x21:				/* GET_LINE_CODING */
			ep0_send(line_coding, 7, len);
			return;
		case 0x22:				/* SET_CONTROL_LINE_STATE */
			dtr_open = (value & 1) != 0;
			ep_in_send(EP_CTL, 0, 0);
			return;
		}
	}
	ep0_stall();
}

/* ------------------------------------------------------------------ *
 * Bring-up
 * ------------------------------------------------------------------ */
/* Flush one TX FIFO (or all of them with num = 0x10) and the RX FIFO.
 * Required after changing FIFO layout: stale words left over from the
 * previous configuration are read back as if they were packet data. */
static void fifo_flush(uint32_t num)
{
	unsigned spin;

	GRSTCTL = (num << 6) | (1u << 5);		/* TXFNUM | TXFFLSH */
	for (spin = 0; spin < 0x10000u; spin++)
		if (!(GRSTCTL & (1u << 5)))
			break;

	GRSTCTL = (1u << 4);				/* RXFFLSH */
	for (spin = 0; spin < 0x10000u; spin++)
		if (!(GRSTCTL & (1u << 4)))
			break;
}

static void usb_reset(void)
{
	unsigned i;

	for (i = 0; i < 4; i++) {
		DIEPCTL(i) = 0;
		DOEPCTL(i) = 0;
		DIEPINT(i) = 0xFFFF;
		DOEPINT(i) = 0xFFFF;
	}
	DCFG &= ~(0x7Fu << 4);			/* device address 0 */

	/* FIFO budget is 320 words of dedicated RAM on this core.
	 * 128 RX + 64 + 64 + 64 TX = 320, exactly. */
	GRXFSIZ    = 128;
	DIEPTXF0   = (64u << 16) | 128u;
	DIEPTXF(1) = (64u << 16) | 192u;
	DIEPTXF(2) = (64u << 16) | 256u;
	fifo_flush(0x10u);			/* all TX FIFOs, then RX */

	DAINTMSK = (1u << 0) | (1u << 16);	/* EP0 in + EP0 out */
	DOEPMSK  = (1u << 0) | (1u << 3);	/* XFRC | STUP      */
	DIEPMSK  = (1u << 0);			/* XFRC             */

	DIEPCTL(EP_CTL) = 0;			/* MPSIZ 00 = 64 bytes */
	ep0_prepare_setup();

	configured      = 0;
	dtr_open        = 0;
	tx_busy         = 0;
	ep0_out_pending = 0;
	tx_head = tx_tail = rx_head = rx_tail = 0;
}

static void usb_enum_done(void)
{
	/* Data and notification endpoints only exist once the bus speed is
	 * known; opening them earlier is what makes a device enumerate and
	 * then immediately fall off. */
	DIEPCTL(EP_DATA) = (1u << 31) | (1u << 28) | (EP_DATA << 22)
			 | (2u << 18) | (1u << 15) | MPS;
	DOEPCTL(EP_DATA) = (1u << 31) | (1u << 28) | (2u << 18)
			 | (1u << 15) | MPS;
	DIEPCTL(EP_NOTIFY) = (1u << 31) | (1u << 28) | (EP_NOTIFY << 22)
			   | (3u << 18) | (1u << 15) | 8u;

	DAINTMSK |= (1u << EP_DATA) | (1u << (16 + EP_DATA));
	ep_out_prepare(EP_DATA);
}

void usb_cdc_init(void)
{
	uint32_t spin;

	RCC_AHB1ENR |= (1u << 0);		/* GPIOA */

	/* PA11 = DM, PA12 = DP, AF10, very high speed. */
	GPIOA_MODER   = (GPIOA_MODER   & ~0x03C00000u) | 0x02800000u;
	GPIOA_OSPEEDR = (GPIOA_OSPEEDR & ~0x03C00000u) | 0x03C00000u;
	GPIOA_AFRH    = (GPIOA_AFRH    & ~0x000FF000u) | 0x000AA000u;

	RCC_AHB2ENR |= (1u << 7);		/* OTGFSEN */

	PCGCCTL = 0;

	/* PHYSEL = 1 selects the full-speed serial transceiver.
	 *
	 * TRDT is the turnaround time and it is NOT a don't-care: RM0368
	 * table 132 wants 6 for an AHB clock of 32 MHz and above (we run 84).
	 * An out-of-range value here does not fail loudly -- it fails as
	 * "device descriptor read/8, error -32", a stall on the very first
	 * control transfer, which looks like a descriptor bug and is not one. */
	GUSBCFG = (GUSBCFG & ~(0xFu << 10)) | (1u << 6) | (6u << 10);

	/* Core soft reset: wait for the AHB to go idle first, then for the
	 * reset bit to self-clear.  Skipping either wait leaves the FIFOs in
	 * an undefined state that only shows up as random enumeration
	 * failures much later. */
	for (spin = 0; spin < 0x30000u; spin++)
		if (GRSTCTL & (1u << 31))	/* AHBIDL */
			break;
	GRSTCTL |= (1u << 0);			/* CSRST */
	for (spin = 0; spin < 0x30000u; spin++)
		if (!(GRSTCTL & (1u << 0)))
			break;

	/* NOVBUSSENS: the Black Pill does not wire VBUS to the sense pin, so
	 * without this the core waits forever for a session it cannot see. */
	GCCFG = (1u << 16) | (1u << 21);	/* PWRDWN | NOVBUSSENS */

	GUSBCFG |= (1u << 30);			/* FDMOD: force device mode */
	/* The mode change needs at least 25 ms to settle (RM0368 22.17.1).
	 * At 84 MHz this loop is comfortably past that; being generous costs
	 * nothing at boot and an under-wait produces intermittent failures
	 * that look like anything but a missing delay. */
	for (spin = 0; spin < 0x200000u; spin++)
		__asm volatile ("nop");

	DCFG |= 3u;				/* DSPD = full speed */
	DCTL |= (1u << 1);			/* soft disconnect while we set up */

	usb_reset();

	GINTMSK = 0;				/* polled; see file header */
	GAHBCFG &= ~1u;				/* global interrupt off     */
	DCTL &= ~(1u << 1);			/* clear soft disconnect    */
}

/* ------------------------------------------------------------------ *
 * The pump.  Called from the main loop.
 * ------------------------------------------------------------------ */
static void drain_rx_fifo(void)
{
	uint32_t sts = GRXSTSP;
	uint8_t  ep  = (uint8_t)(sts & 0x0F);
	uint16_t cnt = (uint16_t)((sts >> 4) & 0x7FF);
	uint8_t  pkt = (uint8_t)((sts >> 17) & 0x0F);

	n_rxpkt++;
	last_pktsts = pkt;

	switch (pkt) {
	case 6:					/* SETUP data */
		if (cnt >= 8) {
			fifo_read(setup_pkt, 8);
			if (!first_setup_cnt) {
				unsigned q;

				first_setup_cnt = cnt;
				for (q = 0; q < 8; q++)
					first_setup[q] = setup_pkt[q];
			}
		} else {
			fifo_drop(cnt);
		}
		break;
	case 2:					/* OUT data */
		if (ep == EP_DATA) {
			uint16_t i;
			uint8_t  tmp[MPS];

			if (cnt > MPS)
				cnt = MPS;
			fifo_read(tmp, cnt);
			for (i = 0; i < cnt; i++) {
				uint16_t nxt = (uint16_t)((rx_head + 1u) % sizeof rx_buf);

				if (nxt == rx_tail)
					break;	/* full: drop, never block */
				rx_buf[rx_head] = tmp[i];
				rx_head = nxt;
			}
		} else {
			fifo_drop(cnt);		/* EP0 OUT data stage */
		}
		break;
	default:
		if (cnt)
			fifo_drop(cnt);
		break;
	}
}

static void pump_tx(void)
{
	uint8_t  pkt[MPS];
	uint16_t n = 0;

	if (!configured || tx_busy || tx_tail == tx_head)
		return;
	if (DIEPCTL(EP_DATA) & (1u << 31))
		return;				/* still enabled from last time */

	while (n < MPS && tx_tail != tx_head) {
		pkt[n++] = tx_buf[tx_tail];
		tx_tail = (uint16_t)((tx_tail + 1u) % sizeof tx_buf);
	}
	if (!n)
		return;

	if (((DTXFSTS(EP_DATA) & 0xFFFFu) * 4u) < n)
		return;				/* FIFO not drained yet */

	tx_busy = 1;
	ep_in_send(EP_DATA, pkt, n);
}

void usb_cdc_poll(void)
{
	uint32_t sts = GINTSTS;

	if (sts & (1u << 12)) {			/* USBRST */
		GINTSTS = (1u << 12);
		n_reset++;
		usb_reset();
	}
	if (sts & (1u << 13)) {			/* ENUMDNE */
		GINTSTS = (1u << 13);
		n_enum++;
		usb_enum_done();
	}
	while (GINTSTS & (1u << 4))		/* RXFLVL */
		drain_rx_fifo();

	/* OUT endpoints */
	if (DOEPINT(EP_CTL) & (1u << 3)) {	/* STUP */
		DOEPINT(EP_CTL) = (1u << 3);
		n_setup++;
		handle_setup();
		ep0_prepare_setup();
	}
	if (DOEPINT(EP_CTL) & (1u << 0)) {	/* XFRC */
		DOEPINT(EP_CTL) = (1u << 0);
		if (ep0_out_pending) {
			ep0_out_pending = 0;
			ep_in_send(EP_CTL, 0, 0);	/* status stage, now */
		}
		ep0_prepare_setup();
	}
	if (DOEPINT(EP_DATA) & (1u << 0)) {	/* XFRC */
		DOEPINT(EP_DATA) = (1u << 0);
		ep_out_prepare(EP_DATA);
	}

	/* IN endpoints */
	if (DIEPINT(EP_CTL) & (1u << 0)) {	/* XFRC */
		DIEPINT(EP_CTL) = (1u << 0);
		n_in_xfrc++;
		if (pending_addr) {
			DCFG = (DCFG & ~(0x7Fu << 4)) | ((uint32_t)pending_addr << 4);
			pending_addr = 0;
		}
		if (ep0_left)
			ep0_continue();
	}
	if (DIEPINT(EP_DATA) & (1u << 0)) {
		DIEPINT(EP_DATA) = (1u << 0);
		tx_busy = 0;
	}

	pump_tx();
}

void usb_cdc_putc(char c)
{
	uint16_t nxt = (uint16_t)((tx_head + 1u) % sizeof tx_buf);

	if (!configured)
		return;				/* no host: drop, never block */
	if (nxt == tx_tail) {
		/* Ring full.  Pumping here rather than spinning keeps a slow or
		 * stalled host from ever blocking the command path. */
		usb_cdc_poll();
		if (nxt == tx_tail)
			return;
	}
	tx_buf[tx_head] = (uint8_t)c;
	tx_head = nxt;
}

int usb_cdc_getc(char *c)
{
	if (rx_tail == rx_head)
		return 0;
	*c = (char)rx_buf[rx_tail];
	rx_tail = (uint16_t)((rx_tail + 1u) % sizeof rx_buf);
	return 1;
}

int usb_cdc_ready(void)
{
	return configured && dtr_open;
}

/* Push whatever is queued, then return.  Bounded: a host that has stopped
 * reading must not be able to hold the firmware here -- the trigger keeps
 * running either way, but a command that never returns would wedge the
 * console. */
void usb_cdc_flush(void)
{
	unsigned spin;

	for (spin = 0; spin < 20000u; spin++) {
		usb_cdc_poll();
		if (tx_tail == tx_head && !tx_busy)
			return;
	}
}

void usb_cdc_debug(uint32_t out[6])
{
	out[0] = GINTSTS;
	out[1] = DSTS;
	out[2] = GCCFG;
	out[3] = DIEPCTL(EP_CTL);
	out[4] = DOEPINT(EP_CTL);
	out[5] = DIEPINT(EP_CTL);
}

void usb_cdc_setup_snapshot(uint32_t out[4])
{
	out[0] = (uint32_t)first_setup[0] | ((uint32_t)first_setup[1] << 8)
	       | ((uint32_t)first_setup[2] << 16) | ((uint32_t)first_setup[3] << 24);
	out[1] = (uint32_t)first_setup[4] | ((uint32_t)first_setup[5] << 8)
	       | ((uint32_t)first_setup[6] << 16) | ((uint32_t)first_setup[7] << 24);
	out[2] = first_setup_cnt;
	out[3] = first_reply_len;
}

void usb_cdc_counters(uint32_t out[6])
{
	out[0] = n_reset;
	out[1] = n_enum;
	out[2] = n_setup;
	out[3] = n_rxpkt;
	out[4] = n_in_xfrc;
	out[5] = last_pktsts;
}
