/* usb_cdc.h — minimal USB CDC-ACM device for the STM32F401's OTG_FS core. */
#ifndef USB_CDC_H
#define USB_CDC_H

#include <stdint.h>

void usb_cdc_init(void);
/* Poll from the main loop; there is no interrupt handler by design (see .c). */
void usb_cdc_poll(void);
/* Queue one byte for the host.  Silently drops when no host is listening, so
 * an unplugged USB port can never stall the trigger loop. */
void usb_cdc_putc(char c);
/* Non-blocking receive: returns 0 when nothing is pending. */
int  usb_cdc_getc(char *c);
/* True once the host has selected the configuration and opened the port. */
int  usb_cdc_ready(void);
/* Push queued output out; bounded, so a stalled host cannot block the caller. */
void usb_cdc_flush(void);
/* Raw core state, for diagnosing enumeration failures over the UART.  Fills
 * six values: GINTSTS, DSTS, GCCFG, DIEPCTL0, DOEPINT0, DIEPINT0. */
void usb_cdc_debug(uint32_t out[6]);
/* Event counts: reset, enumdone, setup, rx-packets, ep0-in-complete, last PKTSTS. */
void usb_cdc_counters(uint32_t out[6]);
/* First SETUP packet as parsed (2 words), its byte count, and the length we
 * queued in reply.  Answers "did we read the host correctly?". */
void usb_cdc_setup_snapshot(uint32_t out[4]);

#endif
