/* usb_device.h — bring-up of the USB CDC-ACM control transport. */
#ifndef USB_DEVICE_H
#define USB_DEVICE_H

#include "usbd_def.h"

extern USBD_HandleTypeDef hUsbDeviceFS;

void MX_USB_DEVICE_Init(void);

#endif /* USB_DEVICE_H */
