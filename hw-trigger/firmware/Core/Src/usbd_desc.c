/* usbd_desc.c — USB descriptors for the camtrig CDC-ACM control link.
 *
 * VID/PID are ST's own 0483:5740, the standard STM32 virtual COM port pair.
 * That is deliberate rather than lazy: this *is* an ST part, Linux binds
 * cdc_acm to it with no vendor driver or udev rule, and a genuinely unique PID
 * would require vendor registration for a one-off lab instrument.  What makes
 * a particular board identifiable is the serial number, which is derived from
 * the MCU's 96-bit unique ID below — so two of these on one host still get
 * stable, distinguishable /dev/serial/by-id/ paths.
 */
#include "usbd_core.h"
#include "usbd_desc.h"
#include "usbd_conf.h"

#define USBD_VID          0x0483
#define USBD_PID          0x5740
#define USBD_LANGID       0x0409		/* en-US */
#define USBD_MANUFACTURER "J106/TX2 rig"
#define USBD_PRODUCT      "camtrig IMX296 trigger"
#define USBD_CONFIG_STR   "CDC Config"
#define USBD_INTERFACE    "CDC Interface"

static void     *Get_SerialNum(void);
static uint8_t  *USBD_DeviceDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t  *USBD_LangIDStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t  *USBD_ManufacturerStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t  *USBD_ProductStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t  *USBD_SerialStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t  *USBD_ConfigStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t  *USBD_InterfaceStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length);

USBD_DescriptorsTypeDef CDC_Desc = {
	USBD_DeviceDescriptor,
	USBD_LangIDStrDescriptor,
	USBD_ManufacturerStrDescriptor,
	USBD_ProductStrDescriptor,
	USBD_SerialStrDescriptor,
	USBD_ConfigStrDescriptor,
	USBD_InterfaceStrDescriptor,
};

__ALIGN_BEGIN static uint8_t hUSBDDeviceDesc[USB_LEN_DEV_DESC] __ALIGN_END = {
	0x12,				/* bLength                */
	USB_DESC_TYPE_DEVICE,		/* bDescriptorType        */
	0x00, 0x02,			/* bcdUSB = 2.00          */
	0x02,				/* bDeviceClass = CDC     */
	0x02,				/* bDeviceSubClass        */
	0x00,				/* bDeviceProtocol        */
	USB_MAX_EP0_SIZE,		/* bMaxPacketSize0        */
	LOBYTE(USBD_VID), HIBYTE(USBD_VID),
	LOBYTE(USBD_PID), HIBYTE(USBD_PID),
	0x00, 0x02,			/* bcdDevice = 2.00       */
	USBD_IDX_MFC_STR,		/* iManufacturer          */
	USBD_IDX_PRODUCT_STR,		/* iProduct               */
	USBD_IDX_SERIAL_STR,		/* iSerialNumber          */
	USBD_MAX_NUM_CONFIGURATION	/* bNumConfigurations     */
};

__ALIGN_BEGIN static uint8_t USBD_LangIDDesc[USB_LEN_LANGID_STR_DESC] __ALIGN_END = {
	USB_LEN_LANGID_STR_DESC,
	USB_DESC_TYPE_STRING,
	LOBYTE(USBD_LANGID), HIBYTE(USBD_LANGID)
};

__ALIGN_BEGIN static uint8_t USBD_StrDesc[USBD_MAX_STR_DESC_SIZ] __ALIGN_END;
__ALIGN_BEGIN static uint8_t USBD_StringSerial[26] __ALIGN_END = {
	26, USB_DESC_TYPE_STRING,
};

static uint8_t *USBD_DeviceDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
	(void)speed;
	*length = sizeof(hUSBDDeviceDesc);
	return hUSBDDeviceDesc;
}

static uint8_t *USBD_LangIDStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
	(void)speed;
	*length = sizeof(USBD_LangIDDesc);
	return USBD_LangIDDesc;
}

static uint8_t *USBD_ProductStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
	(void)speed;
	USBD_GetString((uint8_t *)USBD_PRODUCT, USBD_StrDesc, length);
	return USBD_StrDesc;
}

static uint8_t *USBD_ManufacturerStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
	(void)speed;
	USBD_GetString((uint8_t *)USBD_MANUFACTURER, USBD_StrDesc, length);
	return USBD_StrDesc;
}

static uint8_t *USBD_SerialStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
	(void)speed;
	*length = USBD_StringSerial[0];
	Get_SerialNum();
	return (uint8_t *)USBD_StringSerial;
}

static uint8_t *USBD_ConfigStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
	(void)speed;
	USBD_GetString((uint8_t *)USBD_CONFIG_STR, USBD_StrDesc, length);
	return USBD_StrDesc;
}

static uint8_t *USBD_InterfaceStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
	(void)speed;
	USBD_GetString((uint8_t *)USBD_INTERFACE, USBD_StrDesc, length);
	return USBD_StrDesc;
}

/* Render `value` as `len` hex digits, UTF-16LE, into the serial descriptor. */
static void IntToUnicode(uint32_t value, uint8_t *pbuf, uint8_t len)
{
	uint8_t idx;

	for (idx = 0; idx < len; idx++) {
		uint8_t nib = (value >> 28) & 0x0F;

		pbuf[2 * idx]     = (uint8_t)(nib < 10 ? '0' + nib : 'A' + nib - 10);
		pbuf[2 * idx + 1] = 0;
		value <<= 4;
	}
}

/* 96-bit device unique ID -> 24 hex digits, folded to 12 so it fits the
 * descriptor.  Distinct per board, stable across reflashes, which is what
 * /dev/serial/by-id/ needs to stay meaningful when two rigs share a host. */
static void *Get_SerialNum(void)
{
	uint32_t d0 = *(uint32_t *)(UID_BASE);
	uint32_t d1 = *(uint32_t *)(UID_BASE + 4U);
	uint32_t d2 = *(uint32_t *)(UID_BASE + 8U);

	d0 += d2;
	if (d0 != 0) {
		IntToUnicode(d0, &USBD_StringSerial[2], 8);
		IntToUnicode(d1, &USBD_StringSerial[18], 4);
	}
	return NULL;
}
