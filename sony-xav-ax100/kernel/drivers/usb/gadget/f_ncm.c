/*
 * f_ncm.c -- USB CDC Network (NCM) link function driver
 *
 * Copyright (C) 2010 Nokia Corporation
 * Contact: Yauheni Kaliuta <yauheni.kaliuta@nokia.com>
 *
 * The driver borrows from f_ecm.c which is:
 *
 * Copyright (C) 2003-2005,2008 David Brownell
 * Copyright (C) 2008 Nokia Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/etherdevice.h>
#include <linux/crc32.h>
#include <linux/interrupt.h>
#include <linux/time.h>

#include <linux/usb/cdc.h>

#include "u_ether.h"

extern struct tasklet_struct *t_task;

static int ncm_intf = 0;
module_param(ncm_intf, int, S_IRUGO);
MODULE_PARM_DESC(ncm_intf, "ncm ok");
struct usb_function *carplay_f;
struct usb_gadget *gadget_iap;

//#define INTF_BUG
//#define HR_TIMER

/*
 * This function is a "CDC Network Control Model" (CDC NCM) Ethernet link.
 * NCM is intended to be used with high-speed network attachments.
 *
 * Note that NCM requires the use of "alternate settings" for its data
 * interface.  This means that the set_alt() method has real work to do,
 * and also means that a get_alt() method is required.
 */

/* to trigger crc/non-crc ndp signature */

#define NCM_NDP_HDR_CRC_MASK	0x01000000
#define NCM_NDP_HDR_CRC		0x01000000
#define NCM_NDP_HDR_NOCRC	0x00000000

enum ncm_notify_state {
	NCM_NOTIFY_NONE,	/* don't notify */
	NCM_NOTIFY_CONNECT,	/* issue CONNECT next */
	NCM_NOTIFY_SPEED,	/* issue SPEED_CHANGE next */
};

struct f_ncm {
	struct gether port;
	u8 ctrl_id, data_id;

	char ethaddr[14];

	struct usb_ep *notify;
	struct usb_request *notify_req;
	u8 notify_state;
	bool is_open;

	struct ndp_parser_opts *parser_opts;
	bool is_crc;
	u32 ndp_sign;

	/*
	 * for notification, it is accessed from both
	 * callback and ethernet open/close
	 */
	spinlock_t lock;
	struct net_device *netdev;

	/* For multi-frame NDP TX */
	struct sk_buff *skb_tx_data;
	struct sk_buff *skb_tx_ndp;
	u16 ndp_dgram_count;
	bool timer_force_tx;
	struct tasklet_struct tx_tasklet;
#ifdef HR_TIMER
	struct hrtimer task_timer;
#else
	struct timer_list task_timer;
#endif

	bool timer_stopping;
};

static inline struct f_ncm *func_to_ncm(struct usb_function *f)
{
	return container_of(f, struct f_ncm, port.func);
}

/* peak (theoretical) bulk transfer rate in bits-per-second */
static inline unsigned ncm_bitrate(struct usb_gadget *g)
{
	if (gadget_is_dualspeed(g) && g->speed == USB_SPEED_HIGH)
		return 13 * 512 * 8 * 1000 * 8;
	else
		return 19 * 64 * 1 * 1000 * 8;
}

/*-------------------------------------------------------------------------*/

/*
 * We cannot group frames so use just the minimal size which ok to put
 * one max-size ethernet frame.
 * If the host can group frames, allow it to do that, 16K is selected,
 * because it's used by default by the current linux host driver
 */
//#define NTB_DEFAULT_IN_SIZE	16384
#define NTB_DEFAULT_IN_SIZE   4096

#define NTB_OUT_SIZE		16384
#define TX_MAX_NUM_DPE		32
#define TX_TIMEOUT_NSECS	300000

/*
 * skbs of size less than that will not be aligned
 * to NCM's dwNtbInMaxSize to save bus bandwidth
 */

#define	MAX_TX_NONFIXED		(512 * 3)

#define FORMATS_SUPPORTED	(USB_CDC_NCM_NTB16_SUPPORTED |	\
				 USB_CDC_NCM_NTB32_SUPPORTED)
static struct usb_cdc_ncm_ntb_parameters ntb_parameters = {
	.wLength = sizeof ntb_parameters,
	.bmNtbFormatsSupported = cpu_to_le16(1),
	.dwNtbInMaxSize = cpu_to_le32(NTB_DEFAULT_IN_SIZE),
	.wNdpInDivisor = cpu_to_le16(4),
	.wNdpInPayloadRemainder = cpu_to_le16(0),
	.wNdpInAlignment = cpu_to_le16(4),

	.dwNtbOutMaxSize = cpu_to_le32(NTB_OUT_SIZE),
	.wNdpOutDivisor = cpu_to_le16(4),
	.wNdpOutPayloadRemainder = cpu_to_le16(2),
	.wNdpOutAlignment = cpu_to_le16(4),
};

#if 0
static struct usb_cdc_ncm_ntb_parameters ntb_parameters = {
	.wLength = sizeof ntb_parameters,
	.bmNtbFormatsSupported = cpu_to_le16(1),
	.dwNtbInMaxSize = cpu_to_le32(NTB_DEFAULT_IN_SIZE),
	.wNdpInDivisor = cpu_to_le16(4),
	.wNdpInPayloadRemainder = cpu_to_le16(0),
	.wNdpInAlignment = cpu_to_le16(4),

	.dwNtbOutMaxSize = cpu_to_le32(2),
	.wNdpOutDivisor = cpu_to_le16(4),
	.wNdpOutPayloadRemainder = cpu_to_le16(2),
	.wNdpOutAlignment = cpu_to_le16(4),
};
#endif

/*
 * Use wMaxPacketSize big enough to fit CDC_NOTIFY_SPEED_CHANGE in one
 * packet, to simplify cancellation; and a big transfer interval, to
 * waste less bandwidth.
 */

#define LOG2_STATUS_INTERVAL_MSEC	5	/* 1 << 5 == 32 msec */
#define NCM_STATUS_BYTECOUNT		16	/* 8 byte header + data */

static struct usb_interface_assoc_descriptor ncm_iad_desc /*__initdata*/  = {
	.bLength = sizeof ncm_iad_desc,
	.bDescriptorType = USB_DT_INTERFACE_ASSOCIATION,

	/* .bFirstInterface =   DYNAMIC, */
	.bInterfaceCount = 2,	/*iap+ control + data */
	.bFunctionClass = 0x2,
	.bFunctionSubClass = 0xd,
	.bFunctionProtocol = 0,
	/* .iFunction =         DYNAMIC */
};

static struct usb_interface_assoc_descriptor iap_desc /*__initdata*/  = {
	.bLength = sizeof iap_desc,
	.bDescriptorType = USB_DT_INTERFACE_ASSOCIATION,

	/* .bFirstInterface =   DYNAMIC, */
	.bInterfaceCount = 1,	/*iap+ control + data */
	.bFunctionClass = 0xff,
	.bFunctionSubClass = 0xf0,
	.bFunctionProtocol = 0,
	/* .iFunction =         DYNAMIC */
};

/* interface descriptor: */
static struct usb_interface_descriptor iap_intf /*__initdata*/  = {
	.bLength = sizeof iap_intf,
	.bDescriptorType = USB_DT_INTERFACE,

	/* .bInterfaceNumber = DYNAMIC */
	.bNumEndpoints = 2,
	.bInterfaceClass = 0xff,
	.bInterfaceSubClass = 0xf0,
	.bInterfaceProtocol = 0,
	.iInterface = 4,
};

static struct usb_endpoint_descriptor fs_iap_in_desc /*__initdata*/  = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,

	.bEndpointAddress = USB_DIR_IN | 8,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
	.bInterval = 1,
};

static struct usb_endpoint_descriptor fs_iap_out_desc /*__initdata*/  = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,

	.bEndpointAddress = USB_DIR_OUT | 9,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
	.bInterval = 1,
};

static struct usb_interface_assoc_descriptor audio_desc /*__initdata*/  = {
	.bLength = sizeof audio_desc,
	.bDescriptorType = USB_DT_INTERFACE_ASSOCIATION,
	.bFirstInterface = 0x1,
	/* .bFirstInterface =   DYNAMIC, */
	.bInterfaceCount = 3, /**/.bFunctionClass = 0x1,
	.bFunctionSubClass = 0x0,
	.bFunctionProtocol = 0x20,
	/* .iFunction =         DYNAMIC */
};

static struct usb_interface_descriptor audio_intf /*__initdata*/  = {
	.bLength = sizeof audio_intf,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = 1,
	/* .bInterfaceNumber = DYNAMIC */
	.bNumEndpoints = 0,
	.bInterfaceClass = 1,
	.bInterfaceSubClass = 1,
	.bInterfaceProtocol = 0x20,
	/* .iInterface = DYNAMIC */
};

struct usb_interface_descriptor cs_audio_intf = {
	0x9, 0x24, 0x01, 00, 0x02, 4, 0x53, 0, 0,
};

EXPORT_SYMBOL(cs_audio_intf);

static struct usb_interface_descriptor ncm_control_intf /*__initdata*/  = {
	.bLength = sizeof ncm_control_intf,
	.bDescriptorType = USB_DT_INTERFACE,

	/* .bInterfaceNumber = DYNAMIC */
	.bNumEndpoints = 1,
	.bInterfaceClass = USB_CLASS_COMM,
	.bInterfaceSubClass = USB_CDC_SUBCLASS_NCM,
	.bInterfaceProtocol = USB_CDC_PROTO_NONE,
	/* .iInterface = DYNAMIC */
};

static struct usb_cdc_header_desc ncm_header_desc /*__initdata*/  = {
	.bLength = sizeof ncm_header_desc,
	.bDescriptorType = USB_DT_CS_INTERFACE,
	.bDescriptorSubType = USB_CDC_HEADER_TYPE,

	.bcdCDC = cpu_to_le16(0x0110),
};

static struct usb_cdc_union_desc ncm_union_desc /*__initdata*/  = {
	.bLength = sizeof(ncm_union_desc),
	.bDescriptorType = USB_DT_CS_INTERFACE,
	.bDescriptorSubType = USB_CDC_UNION_TYPE,
	/* .bMasterInterface0 = DYNAMIC */
	/* .bSlaveInterface0 =  DYNAMIC */
};

static struct usb_cdc_ether_desc ecm_desc /*__initdata*/  = {
	.bLength = sizeof ecm_desc,
	.bDescriptorType = USB_DT_CS_INTERFACE,
	.bDescriptorSubType = USB_CDC_ETHERNET_TYPE,

	/* this descriptor actually adds value, surprise! */
	/* .iMACAddress = DYNAMIC */
	.bmEthernetStatistics = cpu_to_le32(0),	/* no statistics */
	.wMaxSegmentSize = cpu_to_le16(ETH_FRAME_LEN),
	.wNumberMCFilters = cpu_to_le16(0),
	.bNumberPowerFilters = 0,
};

#define NCAPS	(USB_CDC_NCM_NCAP_ETH_FILTER | USB_CDC_NCM_NCAP_CRC_MODE)

static struct usb_cdc_ncm_desc ncm_desc /*__initdata*/  = {
	.bLength = sizeof ncm_desc,
	.bDescriptorType = USB_DT_CS_INTERFACE,
	.bDescriptorSubType = USB_CDC_NCM_TYPE,

	.bcdNcmVersion = cpu_to_le16(0x0100),
	/* can process SetEthernetPacketFilter */
	.bmNetworkCapabilities = 0,//NCAPS,
};

/* the default data interface has no endpoints ... */

static struct usb_interface_descriptor ncm_data_nop_intf /*__initdata*/  = {
	.bLength = sizeof ncm_data_nop_intf,
	.bDescriptorType = USB_DT_INTERFACE,
#ifdef INTF_BUG
	.bInterfaceNumber = 0,
#else
	.bInterfaceNumber = 1,
#endif
	.bAlternateSetting = 0,
	.bNumEndpoints = 0,
	.bInterfaceClass = USB_CLASS_CDC_DATA,
	.bInterfaceSubClass = 0,
	.bInterfaceProtocol = USB_CDC_NCM_PROTO_NTB,
	/* .iInterface = DYNAMIC */
};

/* ... but the "real" data interface has two bulk endpoints */

static struct usb_interface_descriptor ncm_data_intf /*__initdata*/  = {
	.bLength = sizeof ncm_data_intf,
	.bDescriptorType = USB_DT_INTERFACE,

#ifdef INTF_BUG
	.bInterfaceNumber = 0,
#else
	.bInterfaceNumber = 1,
#endif
	.bAlternateSetting = 1,
	.bNumEndpoints = 2,
	.bInterfaceClass = USB_CLASS_CDC_DATA,
	.bInterfaceSubClass = 0,
	.bInterfaceProtocol = USB_CDC_NCM_PROTO_NTB,
	/* .iInterface = DYNAMIC */
};

/* full speed support: */

static struct usb_endpoint_descriptor fs_ncm_notify_desc /*__initdata*/  = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,

	.bEndpointAddress = USB_DIR_IN,
	.bmAttributes = USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize = cpu_to_le16(NCM_STATUS_BYTECOUNT),
	.bInterval = 1 << LOG2_STATUS_INTERVAL_MSEC,
};

static struct usb_endpoint_descriptor fs_ncm_in_desc /*__initdata*/  = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,

	.bEndpointAddress = USB_DIR_IN,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
};

static struct usb_endpoint_descriptor fs_ncm_out_desc /*__initdata*/  = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,

	.bEndpointAddress = USB_DIR_OUT,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
};

static struct usb_descriptor_header *ncm_fs_function[] /*__initdata*/  = {

	/* iap descriptors */
#ifdef IAP
	(struct usb_descriptor_header *)&iap_desc,
	(struct usb_descriptor_header *)&iap_intf,
	(struct usb_descriptor_header *)&fs_iap_in_desc,
	(struct usb_descriptor_header *)&fs_iap_out_desc,
#if 0
	(struct usb_descriptor_header *)&audio_desc,
	(struct usb_descriptor_header *)&audio_intf,
	(struct usb_descriptor_header *)&cs_audio_intf,
#endif
#endif
	(struct usb_descriptor_header *)&ncm_iad_desc,
	/* CDC NCM control descriptors */
	(struct usb_descriptor_header *)&ncm_control_intf,
	(struct usb_descriptor_header *)&ncm_header_desc,
	(struct usb_descriptor_header *)&ncm_union_desc,
	(struct usb_descriptor_header *)&ecm_desc,
	(struct usb_descriptor_header *)&ncm_desc,
	(struct usb_descriptor_header *)&fs_ncm_notify_desc,
	/* data interface, altsettings 0 and 1 */
	(struct usb_descriptor_header *)&ncm_data_nop_intf,
	(struct usb_descriptor_header *)&ncm_data_intf,
	(struct usb_descriptor_header *)&fs_ncm_in_desc,
	(struct usb_descriptor_header *)&fs_ncm_out_desc,

	NULL,
};

/* high speed support: */
#if 0
static struct usb_interface_descriptor iap_intf __initdata = {
	.bLength = sizeof iap_intf,
	.bDescriptorType = USB_DT_INTERFACE,

	/* .bInterfaceNumber = DYNAMIC */
	.bNumEndpoints = 2,
	.bInterfaceClass = 0xff,
	.bInterfaceSubClass = 0xf0,
	.bInterfaceProtocol = 0,
	/* .iInterface = DYNAMIC */
};
#endif
static struct usb_endpoint_descriptor hs_iap_in_desc /*__initdata*/  = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,

	.bEndpointAddress = USB_DIR_IN | 8,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
	.bInterval = 1,
	.wMaxPacketSize = cpu_to_le16(512),
};

static struct usb_endpoint_descriptor hs_iap_out_desc /*__initdata*/  = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,

	.bEndpointAddress = USB_DIR_OUT | 9,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
	.bInterval = 1,
	.wMaxPacketSize = cpu_to_le16(512),
};

static struct usb_endpoint_descriptor hs_ncm_notify_desc /*__initdata*/  = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,

	.bEndpointAddress = USB_DIR_IN,
	.bmAttributes = USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize = cpu_to_le16(NCM_STATUS_BYTECOUNT),
	.bInterval = LOG2_STATUS_INTERVAL_MSEC + 4,
};

static struct usb_endpoint_descriptor hs_ncm_in_desc /*__initdata*/  = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,

	.bEndpointAddress = USB_DIR_IN,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize = cpu_to_le16(512),
};

static struct usb_endpoint_descriptor hs_ncm_out_desc /*__initdata*/  = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,

	.bEndpointAddress = USB_DIR_OUT,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize = cpu_to_le16(512),
};

static struct usb_descriptor_header *ncm_hs_function[] /*__initdata*/  = {
	/* iap descriptors */

#ifdef IAP
	(struct usb_descriptor_header *)&iap_desc,
	(struct usb_descriptor_header *)&iap_intf,
	(struct usb_descriptor_header *)&hs_iap_in_desc,
	(struct usb_descriptor_header *)&hs_iap_out_desc,

#if 0
	(struct usb_descriptor_header *)&audio_desc,
	(struct usb_descriptor_header *)&audio_intf,
	(struct usb_descriptor_header *)&cs_audio_intf,
#endif

#endif
	(struct usb_descriptor_header *)&ncm_iad_desc,
	/* CDC NCM control descriptors */
	(struct usb_descriptor_header *)&ncm_control_intf,
	(struct usb_descriptor_header *)&ncm_header_desc,
	(struct usb_descriptor_header *)&ncm_union_desc,
	(struct usb_descriptor_header *)&ecm_desc,
	(struct usb_descriptor_header *)&ncm_desc,
	(struct usb_descriptor_header *)&hs_ncm_notify_desc,
	/* data interface, altsettings 0 and 1 */
	(struct usb_descriptor_header *)&ncm_data_nop_intf,
	(struct usb_descriptor_header *)&ncm_data_intf,
	(struct usb_descriptor_header *)&hs_ncm_in_desc,
	(struct usb_descriptor_header *)&hs_ncm_out_desc,
	NULL,
};

/* string descriptors: */

#define STRING_CTRL_IDX	0
#define STRING_MAC_IDX	1
#define STRING_DATA_IDX	2
#define STRING_IAD_IDX	3
#define STRING_IAP_IDX	4

static struct usb_string ncm_string_defs[] = {
	[STRING_CTRL_IDX].s = "CDC NCM Comm Interface",
	[STRING_MAC_IDX].s = NULL /* DYNAMIC */ ,
	[STRING_DATA_IDX].s = "CDC NCM Data Interface",
	[STRING_IAD_IDX].s = "CDC NCM",
	[STRING_IAP_IDX].s = "iAP Interface",

	{}			/* end of list */
};

static struct usb_gadget_strings ncm_string_table = {
	.language = 0x0409,	/* en-us */
	.strings = ncm_string_defs,
};

static struct usb_gadget_strings *ncm_strings[] = {
	&ncm_string_table,
	NULL,
};

/*
 * Here are options for NCM Datagram Pointer table (NDP) parser.
 * There are 2 different formats: NDP16 and NDP32 in the spec (ch. 3),
 * in NDP16 offsets and sizes fields are 1 16bit word wide,
 * in NDP32 -- 2 16bit words wide. Also signatures are different.
 * To make the parser code the same, put the differences in the structure,
 * and switch pointers to the structures when the format is changed.
 */

struct ndp_parser_opts {
	u32 nth_sign;
	u32 ndp_sign;
	unsigned nth_size;
	unsigned ndp_size;
	unsigned dpe_size;
	unsigned ndplen_align;
	/* sizes in u16 units */
	unsigned dgram_item_len;	/* index or length */
	unsigned block_length;
	unsigned ndp_index;
	unsigned fp_index;
	unsigned reserved1;
	unsigned reserved2;
	unsigned next_fp_index;
	unsigned next_ndp_index;
};

#define INIT_NDP16_OPTS {					\
		.nth_sign = USB_CDC_NCM_NTH16_SIGN,		\
		.ndp_sign = USB_CDC_NCM_NDP16_NOCRC_SIGN,	\
		.nth_size = sizeof(struct usb_cdc_ncm_nth16),	\
		.ndp_size = sizeof(struct usb_cdc_ncm_ndp16),	\
		.dpe_size = sizeof(struct usb_cdc_ncm_dpe16),	\
		.ndplen_align = 4,				\
		.dgram_item_len = 1,				\
		.block_length = 1,				\
		.ndp_index = 1, 				\
		.fp_index = 1,					\
		.reserved1 = 0,					\
		.reserved2 = 0,					\
		.next_fp_index = 1,				\
	}

#define INIT_NDP32_OPTS {					\
		.nth_sign = USB_CDC_NCM_NTH32_SIGN,		\
		.ndp_sign = USB_CDC_NCM_NDP32_NOCRC_SIGN,	\
		.nth_size = sizeof(struct usb_cdc_ncm_nth32),	\
		.ndp_size = sizeof(struct usb_cdc_ncm_ndp32),	\
		.ndplen_align = 8,				\
		.dgram_item_len = 2,				\
		.block_length = 2,				\
		.fp_index = 2,					\
		.reserved1 = 1,					\
		.reserved2 = 2,					\
		.next_fp_index = 2,				\
	}

static struct ndp_parser_opts ndp16_opts = INIT_NDP16_OPTS;
static struct ndp_parser_opts ndp32_opts = INIT_NDP32_OPTS;

static inline void put_ncm(__le16 ** p, unsigned size, unsigned val)
{
	switch (size) {
	case 1:
		put_unaligned_le16((u16) val, *p);
		break;
	case 2:
		put_unaligned_le32((u32) val, *p);

		break;
	default:
		BUG();
	}

	*p += size;
}

static inline unsigned get_ncm(__le16 ** p, unsigned size)
{
	unsigned tmp;

	switch (size) {
	case 1:
		tmp = get_unaligned_le16(*p);
		break;
	case 2:
		tmp = get_unaligned_le32(*p);
		break;
	default:
		BUG();
	}

	*p += size;
	return tmp;
}

static struct timespec tv;
void current_time(char *s)
{
	return;
	getnstimeofday(&tv);
	printk("%s timer %lu %lu\n", s, tv.tv_sec, tv.tv_nsec);
}

/*-------------------------------------------------------------------------*/

static inline void ncm_reset_values(struct f_ncm *ncm)
{
	ncm->parser_opts = &ndp16_opts;
	ncm->is_crc = false;
	ncm->port.cdc_filter = DEFAULT_FILTER;
	ncm->ndp_sign = ncm->parser_opts->ndp_sign;
	/* doesn't make sense for ncm, fixed size used */
	ncm->port.header_len = 0;

	ncm->port.fixed_out_len = le32_to_cpu(ntb_parameters.dwNtbOutMaxSize);
	ncm->port.fixed_in_len = NTB_DEFAULT_IN_SIZE;
}

/*
 * Context: ncm->lock held
 */
static void ncm_do_notify(struct f_ncm *ncm)
{
	struct usb_request *req = ncm->notify_req;
	struct usb_cdc_notification *event;
	struct usb_composite_dev *cdev = ncm->port.func.config->cdev;
	__le32 *data;
	int status;

	/* notification already in flight? */
	if (!req)
		return;

	event = req->buf;
	switch (ncm->notify_state) {
	case NCM_NOTIFY_NONE:
		return;

	case NCM_NOTIFY_CONNECT:
		event->bNotificationType = USB_CDC_NOTIFY_NETWORK_CONNECTION;
		if (ncm->is_open)
			event->wValue = cpu_to_le16(1);
		else
			event->wValue = cpu_to_le16(0);
		event->wLength = 0;
		req->length = sizeof *event;

		DBG(cdev, "notify connect %s\n", ncm->is_open ? "true" : "false");
		ncm->notify_state = NCM_NOTIFY_NONE;
		break;

	case NCM_NOTIFY_SPEED:
		event->bNotificationType = USB_CDC_NOTIFY_SPEED_CHANGE;
		event->wValue = cpu_to_le16(0);
		event->wLength = cpu_to_le16(8);
		req->length = NCM_STATUS_BYTECOUNT;

		/* SPEED_CHANGE data is up/down speeds in bits/sec */
		data = req->buf + sizeof *event;
		data[0] = cpu_to_le32(ncm_bitrate(cdev->gadget));
		data[1] = data[0];

		DBG(cdev, "notify speed %d\n", ncm_bitrate(cdev->gadget));
		ncm->notify_state = NCM_NOTIFY_CONNECT;
		break;
	}
	event->bmRequestType = 0xA1;
	event->wIndex = cpu_to_le16(ncm->ctrl_id);

	ncm->notify_req = NULL;
	/*
	 * In double buffering if there is a space in FIFO,
	 * completion callback can be called right after the call,
	 * so unlocking
	 */
	spin_unlock(&ncm->lock);
	status = usb_ep_queue(ncm->notify, req, GFP_ATOMIC);
	spin_lock(&ncm->lock);
	if (status < 0) {
		ncm->notify_req = req;
		DBG(cdev, "notify --> %d\n", status);
	}
}

/*
 * Context: ncm->lock held
 */
static void ncm_notify(struct f_ncm *ncm)
{
	/*
	 * NOTE on most versions of Linux, host side cdc-ethernet
	 * won't listen for notifications until its netdevice opens.
	 * The first notification then sits in the FIFO for a long
	 * time, and the second one is queued.
	 *
	 * If ncm_notify() is called before the second (CONNECT)
	 * notification is sent, then it will reset to send the SPEED
	 * notificaion again (and again, and again), but it's not a problem
	 */
	ncm->notify_state = NCM_NOTIFY_SPEED;
	ncm_do_notify(ncm);
}

static void ncm_notify_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct f_ncm *ncm = req->context;
	struct usb_composite_dev *cdev = ncm->port.func.config->cdev;
	struct usb_cdc_notification *event = req->buf;

	spin_lock(&ncm->lock);
	switch (req->status) {
	case 0:
		VDBG(cdev, "Notification %02x sent\n", event->bNotificationType);
		break;
	case -ECONNRESET:
	case -ESHUTDOWN:
		ncm->notify_state = NCM_NOTIFY_NONE;
		break;
	default:
		DBG(cdev, "event %02x --> %d\n", event->bNotificationType, req->status);
		break;
	}
	ncm->notify_req = req;
	ncm_do_notify(ncm);
	spin_unlock(&ncm->lock);
}

static void ncm_ep0out_complete(struct usb_ep *ep, struct usb_request *req)
{
	/* now for SET_NTB_INPUT_SIZE only */
	unsigned in_size;
	struct usb_function *f = req->context;
	struct f_ncm *ncm = func_to_ncm(f);
	struct usb_composite_dev *cdev = ep->driver_data;

	req->context = NULL;
	if (req->status || req->actual != req->length) {
		DBG(cdev, "Bad control-OUT transfer\n");
		goto invalid;
	}

	in_size = get_unaligned_le32(req->buf);
	if (in_size < USB_CDC_NCM_NTB_MIN_IN_SIZE || in_size > le32_to_cpu(ntb_parameters.dwNtbInMaxSize)) {
		DBG(cdev, "Got wrong INPUT SIZE (%d) from host\n", in_size);
		goto invalid;
	}

	ncm->port.fixed_in_len = in_size;
	VDBG(cdev, "Set NTB INPUT SIZE %d\n", in_size);
	return;

invalid:
	usb_ep_set_halt(ep);
	return;
}

static int ncm_setup(struct usb_function *f, const struct usb_ctrlrequest *ctrl)
{
	struct f_ncm *ncm = func_to_ncm(f);
	struct usb_composite_dev *cdev = f->config->cdev;
	struct usb_request *req = cdev->req;
	int value = -EOPNOTSUPP;
	u16 w_index = le16_to_cpu(ctrl->wIndex);
	u16 w_value = le16_to_cpu(ctrl->wValue);
	u16 w_length = le16_to_cpu(ctrl->wLength);

	/*
	 * composite driver infrastructure handles everything except
	 * CDC class messages; interface activation uses set_alt().
	 */
	switch ((ctrl->bRequestType << 8) | ctrl->bRequest) {
		#if 0
		case ((USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8)
	| USB_CDC_SET_ETHERNET_PACKET_FILTER:
		/*
		 * see 6.2.30: no data, wIndex = interface,
		 * wValue = packet filter bitmap
		 */
		if (w_length != 0 || w_index != ncm->ctrl_id)
			goto invalid;
		DBG(cdev, "packet filter %02x\n", w_value);
		/*
		 * REVISIT locking of cdc_filter.  This assumes the UDC
		 * driver won't have a concurrent packet TX irq running on
		 * another CPU; or that if it does, this write is atomic...
		 */
		ncm->port.cdc_filter = w_value;
		value = 0;
		break;
		#endif
		/*
		 * and optionally:
		 * case USB_CDC_SEND_ENCAPSULATED_COMMAND:
		 * case USB_CDC_GET_ENCAPSULATED_RESPONSE:
		 * case USB_CDC_SET_ETHERNET_MULTICAST_FILTERS:
		 * case USB_CDC_SET_ETHERNET_PM_PATTERN_FILTER:
		 * case USB_CDC_GET_ETHERNET_PM_PATTERN_FILTER:
		 * case USB_CDC_GET_ETHERNET_STATISTIC:
		 */

		case ((USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8)
	| USB_CDC_GET_NTB_PARAMETERS:
		VDBG(cdev, "wei ctrl_id= %d\n", ncm->ctrl_id);
		if (w_length == 0 || w_value != 0 || w_index != ncm->ctrl_id)
			goto invalid;
		value = w_length > sizeof ntb_parameters ? sizeof ntb_parameters : w_length;
		memcpy(req->buf, &ntb_parameters, value);
		VDBG(cdev, "Host asked NTB parameters\n");
		break;

		case ((USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8)
	| USB_CDC_GET_NTB_INPUT_SIZE:

		if (w_length < 4 || w_value != 0 || w_index != ncm->ctrl_id)
			goto invalid;
		put_unaligned_le32(ncm->port.fixed_in_len, req->buf);
		value = 4;
		VDBG(cdev, "Host asked INPUT SIZE, sending %d\n", ncm->port.fixed_in_len);
		break;

		case ((USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8)
	| USB_CDC_SET_NTB_INPUT_SIZE:
		{
			if (w_length != 4 || w_value != 0 || w_index != ncm->ctrl_id)
				goto invalid;
			req->complete = ncm_ep0out_complete;
			req->length = w_length;
			req->context = f;

			value = req->length;
			break;
		}

		case ((USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8)
	| USB_CDC_GET_NTB_FORMAT:
		{
			uint16_t format;

			if (w_length < 2 || w_value != 0 || w_index != ncm->ctrl_id)
				goto invalid;
			format = (ncm->parser_opts == &ndp16_opts) ? 0x0000 : 0x0001;
			put_unaligned_le16(format, req->buf);
			value = 2;
			VDBG(cdev, "Host asked NTB FORMAT, sending %d\n", format);
			break;
		}

		case ((USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8)
	| USB_CDC_SET_NTB_FORMAT:
		{
			if (w_length != 0 || w_index != ncm->ctrl_id)
				goto invalid;
			switch (w_value) {
			case 0x0000:
				ncm->parser_opts = &ndp16_opts;
				DBG(cdev, "NCM16 selected\n");
				break;
			case 0x0001:
				ncm->parser_opts = &ndp32_opts;
				DBG(cdev, "NCM32 selected\n");
				break;
			default:
				goto invalid;
			}
			value = 0;
			break;
		}
		case ((USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8)
	| USB_CDC_GET_CRC_MODE:
		{
			uint16_t is_crc;

			if (w_length < 2 || w_value != 0 || w_index != ncm->ctrl_id)
				goto invalid;
			is_crc = ncm->is_crc ? 0x0001 : 0x0000;
			put_unaligned_le16(is_crc, req->buf);
			value = 2;
			VDBG(cdev, "Host asked CRC MODE, sending %d\n", is_crc);
			break;
		}

		case ((USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8)
	| USB_CDC_SET_CRC_MODE:
		{
			int ndp_hdr_crc = 0;

			if (w_length != 0 || w_index != ncm->ctrl_id)
				goto invalid;
			switch (w_value) {
			case 0x0000:
				ncm->is_crc = false;
				ndp_hdr_crc = NCM_NDP_HDR_NOCRC;
				DBG(cdev, "non-CRC mode selected\n");
				break;
			case 0x0001:
				ncm->is_crc = true;
				ndp_hdr_crc = NCM_NDP_HDR_CRC;
				DBG(cdev, "CRC mode selected\n");
				break;
			default:
				goto invalid;
			}
			ncm->parser_opts->ndp_sign &= ~NCM_NDP_HDR_CRC_MASK;
			ncm->parser_opts->ndp_sign |= ndp_hdr_crc;
			ncm->ndp_sign = ncm->parser_opts->ndp_sign;
			value = 0;
			break;
		}

		/* and disabled in ncm descriptor: */
		/* case USB_CDC_GET_NET_ADDRESS: */
		/* case USB_CDC_SET_NET_ADDRESS: */
		/* case USB_CDC_GET_MAX_DATAGRAM_SIZE: */
		/* case USB_CDC_SET_MAX_DATAGRAM_SIZE: */

	default:
invalid:
		DBG(cdev, "invalid control req%02x.%02x v%04x i%04x l%d\n", ctrl->bRequestType, ctrl->bRequest, w_value, w_index, w_length);
	}

	/* respond with data transfer or status phase? */
	if (value >= 0) {
		DBG(cdev, "ncm req%02x.%02x v%04x i%04x l%d\n", ctrl->bRequestType, ctrl->bRequest, w_value, w_index, w_length);
		req->zero = 0;
		req->length = value;
		value = usb_ep_queue(cdev->gadget->ep0, req, GFP_ATOMIC);
		if (value < 0)
			ERROR(cdev, "ncm req %02x.%02x response err %d\n", ctrl->bRequestType, ctrl->bRequest, value);
	}

	/* device either stalls (value < 0) or reports success */
	return value;
}

static int ncm_set_alt(struct usb_function *f, unsigned intf, unsigned alt)
{
	struct f_ncm *ncm = func_to_ncm(f);
	struct usb_composite_dev *cdev = f->config->cdev;
	printk("wei: intf=%d data_id=%d alt = %d \n", intf, ncm->data_id, alt);
	carplay_f = f;
	/* Control interface has only altsetting 0 */
	if (intf == ncm->ctrl_id) {
		if (alt != 0)
			goto fail;

		if (ncm->notify->driver_data) {
			DBG(cdev, "reset ncm control %d\n", intf);
			usb_ep_disable(ncm->notify);
		}

		if (!(ncm->notify->desc)) {
			DBG(cdev, "init ncm ctrl %d\n", intf);
			if (config_ep_by_speed(cdev->gadget, f, ncm->notify))
				goto fail;
		}
		usb_ep_enable(ncm->notify);
		//      usb_ep_enable(
		ncm->notify->driver_data = ncm;

		/* Data interface has two altsettings, 0 and 1 */
	} else if (intf == ncm->data_id) {

		/*to enable ep3-int */
		if (alt == 1) {
			if (ncm->notify->driver_data) {
				DBG(cdev, "reset ncm control %d\n", intf);
				usb_ep_disable(ncm->notify);
			}

			if (!(ncm->notify->desc)) {
				DBG(cdev, "init ncm ctrl %d\n", intf);
				if (config_ep_by_speed(cdev->gadget, f, ncm->notify))
					goto fail;
			}
			usb_ep_enable(ncm->notify);
			ncm->notify->driver_data = ncm;
		}
		if (alt > 1)
			goto fail;

		if (ncm->port.in_ep->driver_data) {
			DBG(cdev, "reset ncm\n");
			ncm->timer_stopping = true;
			gether_disconnect(&ncm->port);
			ncm_reset_values(ncm);
		}

		/*
		 * CDC Network only sends data in non-default altsettings.
		 * Changing altsettings resets filters, statistics, etc.
		 */
		if (alt == 1) {
			struct net_device *net;

			if (!ncm->port.in_ep->desc || !ncm->port.out_ep->desc) {
				DBG(cdev, "init ncm\n");
				if (config_ep_by_speed(cdev->gadget, f, ncm->port.in_ep) || config_ep_by_speed(cdev->gadget, f, ncm->port.out_ep)) {
					ncm->port.in_ep->desc = NULL;
					ncm->port.out_ep->desc = NULL;
					goto fail;
				}
			}

			/* TODO */
			/* Enable zlps by default for NCM conformance;
			 * override for musb_hdrc (avoids txdma ovhead)
			 */
			ncm->port.is_zlp_ok = !(gadget_is_musbhdrc(cdev->gadget)
			    );
			ncm->port.cdc_filter = DEFAULT_FILTER;
			DBG(cdev, "activate ncm\n");
			ncm->timer_stopping = false;
			net = gether_connect(&ncm->port);
			ncm->netdev = net;
			if (IS_ERR(net))
				return PTR_ERR(net);
		}

		spin_lock(&ncm->lock);
		ncm_notify(ncm);
		ncm_intf = 1;
		spin_unlock(&ncm->lock);
	} else
		goto fail;

	return 0;
fail:
	return -EINVAL;
}

/*
 * Because the data interface supports multiple altsettings,
 * this NCM function *MUST* implement a get_alt() method.
 */
static int ncm_get_alt(struct usb_function *f, unsigned intf)
{
	struct f_ncm *ncm = func_to_ncm(f);

	if (intf == ncm->ctrl_id)
		return 0;
	return ncm->port.in_ep->driver_data ? 1 : 0;
}

static struct sk_buff *package_for_tx(struct f_ncm *ncm)
{
	__le16 *ntb_iter;
	struct sk_buff *skb2 = NULL;
	unsigned ndp_pad;
	unsigned ndp_index;
	unsigned new_len;

	const struct ndp_parser_opts *opts = ncm->parser_opts;
	const int ndp_align = le16_to_cpu(ntb_parameters.wNdpInAlignment);
	const int dgram_idx_len = 2 * 2 * opts->dgram_item_len;
	/* Stop the timer */
#ifdef HR_TIMER
	hrtimer_try_to_cancel(&ncm->task_timer);
#endif
	ndp_pad = ALIGN(ncm->skb_tx_data->len, ndp_align) - ncm->skb_tx_data->len;
	ndp_index = ncm->skb_tx_data->len + ndp_pad;
	new_len = ndp_index + dgram_idx_len + ncm->skb_tx_ndp->len;
	/* Set the final BlockLength and wNdpIndex */
	ntb_iter = (void *)ncm->skb_tx_data->data;

	/* Increment pointer to BlockLength */
	ntb_iter += 2 + 1 + 1;
	put_ncm(&ntb_iter, opts->block_length, new_len);

	put_ncm(&ntb_iter, opts->ndp_index, ndp_index);
	/* Set the final NDP wLength */
	new_len = opts->ndp_size + (ncm->ndp_dgram_count * dgram_idx_len);
	ncm->ndp_dgram_count = 0;
	/* Increment from start to wLength */
	ntb_iter = (void *)ncm->skb_tx_ndp->data;
	ntb_iter += 2;
	put_unaligned_le16(new_len, ntb_iter);
	/* Merge the skbs */
	swap(skb2, ncm->skb_tx_data);
	if (ncm->skb_tx_data) {
		dev_kfree_skb_any(ncm->skb_tx_data);
		ncm->skb_tx_data = NULL;
	}
	/* Insert NDP alignment. */
	ntb_iter = (void *)skb_put(skb2, ndp_pad);
	memset(ntb_iter, 0, ndp_pad);
	/* Copy NTB across. */
	ntb_iter = (void *)skb_put(skb2, ncm->skb_tx_ndp->len);
	memcpy(ntb_iter, ncm->skb_tx_ndp->data, ncm->skb_tx_ndp->len);
	dev_kfree_skb_any(ncm->skb_tx_ndp);
	ncm->skb_tx_ndp = NULL;
	/* Insert zero'd datagram. */
	ntb_iter = (void *)skb_put(skb2, dgram_idx_len);
	memset(ntb_iter, 0, dgram_idx_len);

	return skb2;
}

/*
 * This transmits the NTB if there are frames waiting.
 */
static void ncm_tx_tasklet(unsigned long data)
{
	struct f_ncm *ncm = (void *)data;

	if (ncm->timer_stopping)
		return;

	/* Only send if data is available. */
	if (ncm->skb_tx_data) {
		ncm->timer_force_tx = true;

		/* XXX This allowance of a NULL skb argument to ndo_start_xmit
		 * XXX is not sane.  The gadget layer should be redesigned so
		 * XXX that the dev->wrap() invocations to build SKBs is transparent
		 * XXX and performed in some way outside of the ndo_start_xmit
		 * XXX interface.
		 */
		ncm->netdev->netdev_ops->ndo_start_xmit(NULL, ncm->netdev);

		ncm->timer_force_tx = false;
	}
}

#if 1
static struct sk_buff *ncm_wrap_ntb(struct gether *port, struct sk_buff *skb)
{
	struct f_ncm *ncm = func_to_ncm(&port->func);
	struct sk_buff *skb2 = NULL;
	int ncb_len = 0;
	__le16 *ntb_data;
	__le16 *ntb_ndp;
	int dgram_pad;

	unsigned max_size = ncm->port.fixed_in_len;
	const struct ndp_parser_opts *opts = ncm->parser_opts;
	const int ndp_align = le16_to_cpu(ntb_parameters.wNdpInAlignment);
	const int div = le16_to_cpu(ntb_parameters.wNdpInDivisor);
	const int rem = le16_to_cpu(ntb_parameters.wNdpInPayloadRemainder);
	const int dgram_idx_len = 2 * 2 * opts->dgram_item_len;

	if (!skb && !ncm->skb_tx_data)
		return NULL;

	if (skb) {
		//printk(KERN_DEBUG "send skblen = %d", skb->len);
		/* Add the CRC if required up front */
		if (ncm->is_crc) {
			uint32_t crc;
			__le16 *crc_pos;

			crc = ~crc32_le(~0, skb->data, skb->len);
			crc_pos = (void *)skb_put(skb, sizeof(uint32_t));
			put_unaligned_le32(crc, crc_pos);
		}

		/* If the new skb is too big for the current NCM NTB then
		 * set the current stored skb to be sent now and clear it
		 * ready for new data.
		 * NOTE: Assume maximum align for speed of calculation.
		 */
		if (ncm->skb_tx_data
		    && (ncm->ndp_dgram_count >= TX_MAX_NUM_DPE || (ncm->skb_tx_data->len + div + rem + skb->len + ncm->skb_tx_ndp->len + ndp_align + (2 * dgram_idx_len))
			> max_size)) {
			skb2 = package_for_tx(ncm);
			if (!skb2)
				goto err;
		}

		if (!ncm->skb_tx_data) {
			ncb_len = opts->nth_size;
			dgram_pad = ALIGN(ncb_len, div) + rem - ncb_len;
			ncb_len += dgram_pad;

			/* Create a new skb for the NTH and datagrams. */
			ncm->skb_tx_data = alloc_skb(max_size, GFP_ATOMIC);
			if (!ncm->skb_tx_data)
				goto err;
			ntb_data = (void *)skb_put(ncm->skb_tx_data, ncb_len);
			memset(ntb_data, 0, ncb_len);
			/* dwSignature */
			put_unaligned_le32(opts->nth_sign, ntb_data);
			ntb_data += 2;
			/* wHeaderLength */
			put_unaligned_le16(opts->nth_size, ntb_data++);

			/* Allocate an skb for storing the NDP,
			 * TX_MAX_NUM_DPE should easily suffice for a
			 * 16k packet.
			 */
			ncm->skb_tx_ndp = alloc_skb((int)(opts->ndp_size + opts->dpe_size * TX_MAX_NUM_DPE), GFP_ATOMIC);
			if (!ncm->skb_tx_ndp)
				goto err;

			ntb_ndp = (void *)skb_put(ncm->skb_tx_ndp, opts->ndp_size);
			memset(ntb_ndp, 0, ncb_len);
			/* dwSignature */
			put_unaligned_le32(ncm->ndp_sign, ntb_ndp);
			ntb_ndp += 2;

			/* There is always a zeroed entry */
			ncm->ndp_dgram_count = 1;

			/* Note: we skip opts->next_ndp_index */
		}

		/* Delay the timer. */
#ifdef HR_TIMER
		hrtimer_start(&ncm->task_timer, ktime_set(0, TX_TIMEOUT_NSECS), HRTIMER_MODE_REL);
#else
		mod_timer(&(ncm->task_timer), jiffies + 1);
#endif
		current_time("ncm_wrap_ntb");

		/* Add the datagram position entries */
		ntb_ndp = (void *)skb_put(ncm->skb_tx_ndp, dgram_idx_len);
		memset(ntb_ndp, 0, dgram_idx_len);

		ncb_len = ncm->skb_tx_data->len;
		dgram_pad = ALIGN(ncb_len, div) + rem - ncb_len;
		ncb_len += dgram_pad;

		/* (d)wDatagramIndex */
		put_ncm(&ntb_ndp, opts->dgram_item_len, ncb_len);
		/* (d)wDatagramLength */
		put_ncm(&ntb_ndp, opts->dgram_item_len, skb->len);
		ncm->ndp_dgram_count++;

		/* Add the new data to the skb */
		ntb_data = (void *)skb_put(ncm->skb_tx_data, dgram_pad);
		memset(ntb_data, 0, dgram_pad);
		ntb_data = (void *)skb_put(ncm->skb_tx_data, skb->len);
		memcpy(ntb_data, skb->data, skb->len);
		dev_kfree_skb_any(skb);
		skb = NULL;

	} else if (ncm->skb_tx_data && ncm->timer_force_tx) {
		/* If the tx was requested because of a timeout then send */
		skb2 = package_for_tx(ncm);
		if (!skb2)
			goto err;
	}

	return skb2;

err:
	ncm->netdev->stats.tx_dropped++;

	if (skb)
		dev_kfree_skb_any(skb);
	if (ncm->skb_tx_data)
		dev_kfree_skb_any(ncm->skb_tx_data);
	if (ncm->skb_tx_ndp)
		dev_kfree_skb_any(ncm->skb_tx_ndp);

	return NULL;
}
#else
static struct sk_buff *ncm_wrap_ntb(struct gether *port, struct sk_buff *skb)
{
	struct f_ncm *ncm = func_to_ncm(&port->func);
	struct sk_buff *skb2;
	int ncb_len = 0;
	__le16 *tmp;
	int div = ntb_parameters.wNdpInDivisor;
	int rem = ntb_parameters.wNdpInPayloadRemainder;
	int pad;
	int ndp_align = ntb_parameters.wNdpInAlignment;
	int ndp_pad;
	unsigned max_size = ncm->port.fixed_in_len;
	struct ndp_parser_opts *opts = ncm->parser_opts;
	unsigned crc_len = ncm->is_crc ? sizeof(uint32_t) : 0;

	ncb_len += opts->nth_size;
	ndp_pad = ALIGN(ncb_len, ndp_align) - ncb_len;
	ncb_len += ndp_pad;
	ncb_len += opts->ndp_size;
	ncb_len += 2 * 2 * opts->dgram_item_len;	/* Datagram entry */
	ncb_len += 2 * 2 * opts->dgram_item_len;	/* Zero datagram entry */
	pad = ALIGN(ncb_len, div) + rem - ncb_len;
	ncb_len += pad;

#if 0
	printk(KERN_DEBUG "send skblen = %d", skb->len);
	//print_hex_dump(KERN_DEBUG, "send data: ", DUMP_PREFIX_ADDRESS,
	//      32, 1, skb->data, skb->len, false);
#endif
	if (ncb_len + skb->len + crc_len > max_size) {
		dev_kfree_skb_any(skb);
		return NULL;
	}

	skb2 = skb_copy_expand(skb, ncb_len, max_size - skb->len - ncb_len - crc_len, GFP_ATOMIC);
	dev_kfree_skb_any(skb);
	if (!skb2)
		return NULL;

	skb = skb2;

	tmp = (void *)skb_push(skb, ncb_len);
	memset(tmp, 0, ncb_len);

	put_unaligned_le32(opts->nth_sign, tmp);	/* dwSignature */
	tmp += 2;
	/* wHeaderLength */
	put_unaligned_le16(opts->nth_size, tmp++);
	tmp++;			/* skip wSequence */
	put_ncm(&tmp, opts->block_length, skb->len);	/* (d)wBlockLength */
	/* (d)wFpIndex */
	/* the first pointer is right after the NTH + align */
	put_ncm(&tmp, opts->fp_index, opts->nth_size + ndp_pad);

	tmp = (void *)tmp + ndp_pad;

	/* NDP */
	put_unaligned_le32(opts->ndp_sign, tmp);	/* dwSignature */
	tmp += 2;
	/* wLength */
	put_unaligned_le16(ncb_len - opts->nth_size - pad, tmp++);

	tmp += opts->reserved1;
	tmp += opts->next_fp_index;	/* skip reserved (d)wNextFpIndex */
	tmp += opts->reserved2;

	if (ncm->is_crc) {
		uint32_t crc;

		crc = ~crc32_le(~0, skb->data + ncb_len, skb->len - ncb_len);
		put_unaligned_le32(crc, skb->data + skb->len);
		skb_put(skb, crc_len);
	}

	/* (d)wDatagramIndex[0] */
	put_ncm(&tmp, opts->dgram_item_len, ncb_len);
	/* (d)wDatagramLength[0] */
	put_ncm(&tmp, opts->dgram_item_len, skb->len - ncb_len);
	/* (d)wDatagramIndex[1] and  (d)wDatagramLength[1] already zeroed */

	if (skb->len > MAX_TX_NONFIXED)
		memset(skb_put(skb, max_size - skb->len), 0, max_size - skb->len);

	return skb;
}
#endif
/*
 * The transmit should only be run if no skb data has been sent
 * for a certain duration.
 */
#ifdef HR_TIMER
static enum hrtimer_restart ncm_tx_timeout(struct hrtimer *data)
{
	struct f_ncm *ncm = container_of(data, struct f_ncm, task_timer);
	tasklet_schedule(&ncm->tx_tasklet);
	current_time("ncm_tx_timeout");
	return HRTIMER_NORESTART;
}
#else
static void ncm_tx_timeout(unsigned long data)
{
	struct f_ncm *ncm = (struct f_ncm *)data;
	tasklet_schedule(&ncm->tx_tasklet);
	current_time("ncm_tx_timeout");
}
#endif

static int ncm_unwrap_ntb(struct gether *port, struct sk_buff *skb, struct sk_buff_head *list)
{
	struct f_ncm *ncm = func_to_ncm(&port->func);
	__le16 *tmp = (void *)skb->data;
	unsigned index, index2;
	unsigned dg_len, dg_len2;
	unsigned ndp_len;
	struct sk_buff *skb2;
	int ret = -EINVAL;
	unsigned max_size = le32_to_cpu(ntb_parameters.dwNtbOutMaxSize);
	struct ndp_parser_opts *opts = ncm->parser_opts;
	unsigned crc_len = ncm->is_crc ? sizeof(uint32_t) : 0;
	int dgram_counter;

	/* dwSignature */
	if (get_unaligned_le32(tmp) != opts->nth_sign) {
		INFO(port->func.config->cdev, "Wrong NTH SIGN, skblen %d\n", skb->len);
		print_hex_dump(KERN_INFO, "HEAD:", DUMP_PREFIX_ADDRESS, 32, 1, skb->data, 32, false);

		goto err;
	}
	tmp += 2;
	/* wHeaderLength */
	if (get_unaligned_le16(tmp++) != opts->nth_size) {
		INFO(port->func.config->cdev, "Wrong NTB headersize\n");
		goto err;
	}
	tmp++;			/* skip wSequence */

	/* (d)wBlockLength */
	if (get_ncm(&tmp, opts->block_length) > max_size) {
		INFO(port->func.config->cdev, "OUT size exceeded %d > %d\n", get_ncm(&tmp, opts->block_length), max_size);
		goto err;
	}

	index = get_ncm(&tmp, opts->fp_index);
	/* NCM 3.2 */
	if (((index % 4) != 0) && (index < opts->nth_size)) {
		INFO(port->func.config->cdev, "Bad index: %x\n", index);
		goto err;
	}

	/* walk through NDP */
	tmp = ((void *)skb->data) + index;
	if (get_unaligned_le32(tmp) != opts->ndp_sign) {
		INFO(port->func.config->cdev, "Wrong NDP SIGN\n");
		goto err;
	}
	tmp += 2;

	ndp_len = get_unaligned_le16(tmp++);
	/*
	 * NCM 3.3.1
	 * entry is 2 items
	 * item size is 16/32 bits, opts->dgram_item_len * 2 bytes
	 * minimal: struct usb_cdc_ncm_ndpX + normal entry + zero entry
	 */
	if ((ndp_len < opts->ndp_size + 2 * 2 * (opts->dgram_item_len * 2))
	    || (ndp_len % opts->ndplen_align != 0)) {
		INFO(port->func.config->cdev, "Bad NDP length: %x\n", ndp_len);
		goto err;
	}
	tmp += opts->reserved1;
	tmp += opts->next_fp_index;	/* skip reserved (d)wNextFpIndex */
	tmp += opts->reserved2;

	ndp_len -= opts->ndp_size;
	index2 = get_ncm(&tmp, opts->dgram_item_len);
	dg_len2 = get_ncm(&tmp, opts->dgram_item_len);
	dgram_counter = 0;

	do {
		index = index2;
		dg_len = dg_len2;
		if (dg_len < 14 + crc_len) {	/* ethernet header + crc */
			INFO(port->func.config->cdev, "Bad dgram length: %x\n", dg_len);
			goto err;
		}
		if (ncm->is_crc) {
			uint32_t crc, crc2;

			crc = get_unaligned_le32(skb->data + index + dg_len - crc_len);
			crc2 = ~crc32_le(~0, skb->data + index, dg_len - crc_len);
			if (crc != crc2) {
				INFO(port->func.config->cdev, "Bad CRC\n");
				goto err;
			}
		}

		index2 = get_ncm(&tmp, opts->dgram_item_len);
		dg_len2 = get_ncm(&tmp, opts->dgram_item_len);

		if (index2 == 0 || dg_len2 == 0) {
			skb2 = skb;
		} else {
			skb2 = skb_clone(skb, GFP_ATOMIC);
			if (skb2 == NULL)
				goto err;
		}

		if (!skb_pull(skb2, index)) {
			ret = -EOVERFLOW;
			goto err;
		}

		skb_trim(skb2, dg_len - crc_len);
		skb_queue_tail(list, skb2);

		ndp_len -= 2 * (opts->dgram_item_len * 2);

		dgram_counter++;

		if (index2 == 0 || dg_len2 == 0)
			break;
	} while (ndp_len > 2 * (opts->dgram_item_len * 2));	/* zero entry */

	//VDBG(port->func.config->cdev,
	//     "Parsed NTB with %d frames\n", dgram_counter);
#if 0
	if (skb2->len % 4 == 3) {
		printk(KERN_DEBUG "\t skblen = %d", skb2->len);
		print_hex_dump(KERN_DEBUG, "raw data: ", DUMP_PREFIX_ADDRESS, 32, 1, skb2->data, skb2->len, false);
	}
#endif
	return 0;
err:
	skb_queue_purge(list);
	dev_kfree_skb_any(skb);
	return ret;
}

static void ncm_disable(struct usb_function *f)
{
	struct f_ncm *ncm = func_to_ncm(f);
	struct usb_composite_dev *cdev = f->config->cdev;

	DBG(cdev, "ncm deactivated\n");

	if (ncm->port.in_ep->driver_data) {
		ncm->timer_stopping = true;
		gether_disconnect(&ncm->port);
	} else {
		printk(" wei >>>>> \n");
	}
	if (ncm->notify->driver_data) {
		usb_ep_disable(ncm->notify);
		ncm->notify->driver_data = NULL;
		ncm->notify->desc = NULL;
	}
	/*iAP dev remove */
#ifdef CONFIG_USB_G_ANDROID
	iap_disc();
#endif

}

/*-------------------------------------------------------------------------*/

/*
 * Callbacks let us notify the host about connect/disconnect when the
 * net device is opened or closed.
 *
 * For testing, note that link states on this side include both opened
 * and closed variants of:
 *
 *   - disconnected/unconfigured
 *   - configured but inactive (data alt 0)
 *   - configured and active (data alt 1)
 *
 * Each needs to be tested with unplug, rmmod, SET_CONFIGURATION, and
 * SET_INTERFACE (altsetting).  Remember also that "configured" doesn't
 * imply the host is actually polling the notification endpoint, and
 * likewise that "active" doesn't imply it's actually using the data
 * endpoints for traffic.
 */

static void ncm_open(struct gether *geth)
{
	struct f_ncm *ncm = func_to_ncm(&geth->func);

	DBG(ncm->port.func.config->cdev, "%s\n", __func__);

	spin_lock(&ncm->lock);
	ncm->is_open = true;
	//printk("ncm_open call ncm_notify \n");
	ncm_notify(ncm);
	spin_unlock(&ncm->lock);
}

static void ncm_close(struct gether *geth)
{
	struct f_ncm *ncm = func_to_ncm(&geth->func);

	DBG(ncm->port.func.config->cdev, "%s\n", __func__);

	spin_lock(&ncm->lock);
	ncm->is_open = false;
	ncm_notify(ncm);
	spin_unlock(&ncm->lock);
}

/*-------------------------------------------------------------------------*/
#if 0
struct hrtimer task_timer;
static enum hrtimer_restart hrt_test_timeout(unsigned long data)
{
	current_time("hrt_test_timeout");
	hrtimer_start(&task_timer, ktime_set(0, TX_TIMEOUT_NSECS), HRTIMER_MODE_REL);
	return HRTIMER_NORESTART;
}
#endif
#if 0
void hrtimer_test(void)
{
	hrtimer_init(&task_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	task_timer.function = hrt_test_timeout;

	hrtimer_start(&task_timer, ktime_set(0, TX_TIMEOUT_NSECS), HRTIMER_MODE_REL);
	while (1) {
		mdelay(2000);
		printk(".");
	}
}
#endif
/* ethernet function driver setup/binding */

static int /*__init*/ ncm_bind(struct usb_configuration *c,
			       struct usb_function *f)
{
	struct usb_composite_dev *cdev = c->cdev;
	struct f_ncm *ncm = func_to_ncm(f);
	int status;
	struct usb_ep *ep;
	carplay_f = f;
	/*iap to alloc ep used */
	gadget_iap = cdev->gadget;

#ifdef IAP
	status = usb_interface_id(c, NULL);
	if (status < 0)
		goto fail;
	iap_intf.bInterfaceNumber = status;
#endif
	/* allocate instance-specific interface IDs */
	status = usb_interface_id(c, f);
	if (status < 0)
		goto fail;

#ifdef INTF_BUG
	status = 1;
#endif
	ncm->ctrl_id = status;
	ncm_iad_desc.bFirstInterface = status;

	ncm_control_intf.bInterfaceNumber = status;
	ncm_union_desc.bMasterInterface0 = status;

	status = usb_interface_id(c, f);
	if (status < 0)
		goto fail;

#ifdef INTF_BUG
	status = 0;
#endif
	ncm->data_id = status;	/*should equal to [set_intf cmd] */
	ncm_data_nop_intf.bInterfaceNumber = status;
	ncm_data_intf.bInterfaceNumber = status;
	ncm_union_desc.bSlaveInterface0 = status;

	status = -ENODEV;

	/* allocate instance-specific endpoints */
	ep = usb_ep_autoconfig(cdev->gadget, &fs_ncm_in_desc);
	if (!ep)
		goto fail;
	ncm->port.in_ep = ep;
	ep->driver_data = cdev;	/* claim */

	ep = usb_ep_autoconfig(cdev->gadget, &fs_ncm_out_desc);
	if (!ep)
		goto fail;
	ncm->port.out_ep = ep;
	ep->driver_data = cdev;	/* claim */

	ep = usb_ep_autoconfig(cdev->gadget, &fs_ncm_notify_desc);
	if (!ep)
		goto fail;
	ncm->notify = ep;
	ep->driver_data = cdev;	/* claim */

	status = -ENOMEM;

	/* allocate notification request and buffer */
	ncm->notify_req = usb_ep_alloc_request(ep, GFP_KERNEL);
	if (!ncm->notify_req)
		goto fail;
	ncm->notify_req->buf = kmalloc(NCM_STATUS_BYTECOUNT, GFP_KERNEL);
	if (!ncm->notify_req->buf)
		goto fail;
	ncm->notify_req->context = ncm;
	ncm->notify_req->complete = ncm_notify_complete;

	/* copy descriptors, and track endpoint copies */
	f->descriptors = usb_copy_descriptors(ncm_fs_function);
	if (!f->descriptors)
		goto fail;

	/*
	 * support all relevant hardware speeds... we expect that when
	 * hardware is dual speed, all bulk-capable endpoints work at
	 * both speeds
	 */
	if (gadget_is_dualspeed(c->cdev->gadget)) {
		hs_ncm_in_desc.bEndpointAddress = fs_ncm_in_desc.bEndpointAddress;
		hs_ncm_out_desc.bEndpointAddress = fs_ncm_out_desc.bEndpointAddress;
		hs_ncm_notify_desc.bEndpointAddress = fs_ncm_notify_desc.bEndpointAddress;

		/* copy descriptors, and track endpoint copies */
		f->hs_descriptors = usb_copy_descriptors(ncm_hs_function);
		if (!f->hs_descriptors)
			goto fail;
	}

	/*
	 * NOTE:  all that is done without knowing or caring about
	 * the network link ... which is unavailable to this code
	 * until we're activated via set_alt().
	 */

	ncm->port.open = ncm_open;
	ncm->port.close = ncm_close;
	tasklet_init(&ncm->tx_tasklet, ncm_tx_tasklet, (unsigned long)ncm);
#ifndef HR_TIMER
	t_task = &ncm->tx_tasklet;
#endif
#ifdef HR_TIMER
	hrtimer_init(&ncm->task_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	ncm->task_timer.function = ncm_tx_timeout;
#else
	ncm->task_timer.data = (unsigned long)ncm;
	init_timer(&ncm->task_timer);
	ncm->task_timer.expires = jiffies + 1;
	ncm->task_timer.function = ncm_tx_timeout;
	add_timer(&ncm->task_timer);
#endif

	DBG(cdev, "CDC Network: %s speed IN/%s OUT/%s NOTIFY/%s\n",
	    gadget_is_dualspeed(c->cdev->gadget) ? "dual" : "full", ncm->port.in_ep->name, ncm->port.out_ep->name, ncm->notify->name);
	return 0;

fail:
	if (f->descriptors)
		usb_free_descriptors(f->descriptors);

	if (ncm->notify_req) {
		kfree(ncm->notify_req->buf);
		usb_ep_free_request(ncm->notify, ncm->notify_req);
	}

	/* we might as well release our claims on endpoints */
	if (ncm->notify)
		ncm->notify->driver_data = NULL;
	if (ncm->port.out_ep->desc)
		ncm->port.out_ep->driver_data = NULL;
	if (ncm->port.in_ep->desc)
		ncm->port.in_ep->driver_data = NULL;

	ERROR(cdev, "%s: can't bind, err %d\n", f->name, status);

	return status;
}

static void ncm_unbind(struct usb_configuration *c, struct usb_function *f)
{
	struct f_ncm *ncm = func_to_ncm(f);

	DBG(c->cdev, "ncm unbind\n");

	if (gadget_is_dualspeed(c->cdev->gadget))
		usb_free_descriptors(f->hs_descriptors);
	usb_free_descriptors(f->descriptors);

	kfree(ncm->notify_req->buf);
	usb_ep_free_request(ncm->notify, ncm->notify_req);

	ncm_string_defs[1].s = NULL;
	kfree(ncm);
}

/**
 * ncm_bind_config - add CDC Network link to a configuration
 * @c: the configuration to support the network link
 * @ethaddr: a buffer in which the ethernet address of the host side
 *	side of the link was recorded
 * Context: single threaded during gadget setup
 *
 * Returns zero on success, else negative errno.
 *
 * Caller must have called @gether_setup().  Caller is also responsible
 * for calling @gether_cleanup() before module unload.
 */
int /*__init*/ ncm_bind_config(struct usb_configuration *c,
			       u8 ethaddr[ETH_ALEN])
{
	struct f_ncm *ncm;
	int status;

	if (!can_support_ecm(c->cdev->gadget) || !ethaddr)
		return -EINVAL;

	/* maybe allocate device-global string IDs */
	if (ncm_string_defs[0].id == 0) {

		/* control interface label */
		status = usb_string_id(c->cdev);
		if (status < 0)
			return status;
		ncm_string_defs[STRING_CTRL_IDX].id = status;
		ncm_control_intf.iInterface = status;

		/* data interface label */
		status = usb_string_id(c->cdev);
		if (status < 0)
			return status;
		ncm_string_defs[STRING_DATA_IDX].id = status;
		ncm_data_nop_intf.iInterface = status;
		ncm_data_intf.iInterface = status;

		/* MAC address */
		status = usb_string_id(c->cdev);
		if (status < 0)
			return status;
		ncm_string_defs[STRING_MAC_IDX].id = status;
		ecm_desc.iMACAddress = status;

		/* IAD */
		status = usb_string_id(c->cdev);
		if (status < 0)
			return status;
		ncm_string_defs[STRING_IAD_IDX].id = status;
		ncm_iad_desc.iFunction = status;

		status = usb_string_id(c->cdev);
		ncm_string_defs[STRING_IAP_IDX].id = status;
		iap_intf.iInterface = status;
	}

	/* allocate and initialize one new instance */
	ncm = kzalloc(sizeof *ncm, GFP_KERNEL);
	if (!ncm)
		return -ENOMEM;

	/* export host's Ethernet address in CDC format */
	snprintf(ncm->ethaddr, sizeof ncm->ethaddr, "%02X%02X%02X%02X%02X%02X", ethaddr[0], ethaddr[1], ethaddr[2], ethaddr[3], ethaddr[4], ethaddr[5]);
	ncm_string_defs[1].s = ncm->ethaddr;

	spin_lock_init(&ncm->lock);
	ncm_reset_values(ncm);
	ncm->port.is_fixed = true;
	ncm->port.supports_multi_frame = true;

	ncm->port.func.name = "cdc_network";
	ncm->port.func.strings = ncm_strings;
	/* descriptors are per-instance copies */
	ncm->port.func.bind = ncm_bind;
	ncm->port.func.unbind = ncm_unbind;
	ncm->port.func.set_alt = ncm_set_alt;
	ncm->port.func.get_alt = ncm_get_alt;
	ncm->port.func.setup = ncm_setup;
	ncm->port.func.disable = ncm_disable;

	ncm->port.wrap = ncm_wrap_ntb;
	ncm->port.unwrap = ncm_unwrap_ntb;

	status = usb_add_function(c, &ncm->port.func);
	if (status) {
		ncm_string_defs[1].s = NULL;
		kfree(ncm);
	}
	return status;
}