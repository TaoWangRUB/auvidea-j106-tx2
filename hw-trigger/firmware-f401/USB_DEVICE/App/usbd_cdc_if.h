/* usbd_cdc_if.h — CDC class glue for the camtrig command protocol. */
#ifndef USBD_CDC_IF_H
#define USBD_CDC_IF_H

#include "usbd_cdc.h"

extern USBD_CDC_ItfTypeDef USBD_Interface_fops_FS;

/* Queue one byte for transmission to the host.  Never blocks and never stalls
 * the caller: if the host is not draining, bytes are dropped and counted. */
void     cdc_putc(char c);
uint32_t cdc_dropped(void);
int      cdc_ready(void);
void     cdc_pump(void);   /* drain the TX ring; safe from task or ISR */

#endif /* USBD_CDC_IF_H */
