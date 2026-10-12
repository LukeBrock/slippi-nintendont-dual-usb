/*-------------------------------------------------------------

usbstorage.c -- Bulk-only USB mass storage support

Copyright (C) 2008
Sven Peter (svpe) <svpe@gmx.net>
Copyright (C) 2009-2010
tueidj, rodries, Tantric

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any
damages arising from the use of this software.

Permission is granted to anyone to use this software for any
purpose, including commercial applications, and to alter it and
redistribute it freely, subject to the following restrictions:

1.	The origin of this software must not be misrepresented; you
must not claim that you wrote the original software. If you use
this software in a product, an acknowledgment in the product
documentation would be appreciated but is not required.

2.	Altered source versions must be plainly marked as such, and
must not be misrepresented as being the original software.

3.	This notice may not be removed or altered from any source
distribution.

-------------------------------------------------------------*/
/* Stripped down nintendont port by FIX94 */
#include "Config.h"
#include "debug.h"
#include "usbstorage.h"
#include "usb.h"

#define ROUNDDOWN32(v)				(((u32)(v)-0x1f)&~0x1f)

#define	HEAP_SIZE					(18*1024)
#define	TAG_START					0x0BADC0DE

#define	CBW_SIZE					31
#define	CBW_SIGNATURE				0x43425355
#define	CBW_IN						(1 << 7)
#define	CBW_OUT						0

#define	CSW_SIZE					13
#define	CSW_SIGNATURE				0x53425355

#define	SCSI_TEST_UNIT_READY		0x00
#define	SCSI_REQUEST_SENSE			0x03
#define	SCSI_INQUIRY				0x12
#define SCSI_START_STOP				0x1B
#define	SCSI_READ_CAPACITY			0x25
#define	SCSI_READ_10				0x28
#define	SCSI_WRITE_10				0x2A

#define	SCSI_SENSE_REPLY_SIZE		18
#define	SCSI_SENSE_NOT_READY		0x02
#define	SCSI_SENSE_MEDIUM_ERROR		0x03
#define	SCSI_SENSE_HARDWARE_ERROR	0x04

#define	USB_CLASS_MASS_STORAGE		0x08
#define	MASS_STORAGE_RBC_COMMANDS		0x01
#define	MASS_STORAGE_ATA_COMMANDS		0x02
#define	MASS_STORAGE_QIC_COMMANDS		0x03
#define	MASS_STORAGE_UFI_COMMANDS		0x04
#define	MASS_STORAGE_SFF8070_COMMANDS	0x05
#define	MASS_STORAGE_SCSI_COMMANDS		0x06
#define	MASS_STORAGE_BULK_ONLY		0x50

#define USBSTORAGE_GET_MAX_LUN		0xFE
#define USBSTORAGE_RESET			0xFF

#define	USB_ENDPOINT_BULK			0x02

#define USBSTORAGE_CYCLE_RETRIES	3

#define INVALID_LUN					-2

#define MAX_TRANSFER_SIZE_V5		(16*1024)

#define DEVLIST_MAXSIZE				8

#define GETDEVPARAMS_DESC_OFFSET	20
#define GETDEVPARAMS_OUT_SIZE		0xC0

typedef struct _usbendpointdesc
{
	u8 bLength;
	u8 bDescriptorType;
	u8 bEndpointAddress;
	u8 bmAttributes;
	u16 wMaxPacketSize;
	u8 bInterval;
} __attribute__((packed)) usb_endpointdesc;

typedef struct _usbinterfacedesc
{
	u8 bLength;
	u8 bDescriptorType;
	u8 bInterfaceNumber;
	u8 bAlternateSetting;
	u8 bNumEndpoints;
	u8 bInterfaceClass;
	u8 bInterfaceSubClass;
	u8 bInterfaceProtocol;
	u8 iInterface;
	u8 *extra;
	u16 extra_size;
	struct _usbendpointdesc *endpoints;
} __attribute__((packed)) usb_interfacedesc;

typedef struct _usbconfdesc
{
	u8 bLength;
	u8 bDescriptorType;
	u16 wTotalLength;
	u8 bNumInterfaces;
	u8 bConfigurationValue;
	u8 iConfiguration;
	u8 bmAttributes;
	u8 bMaxPower;
	struct _usbinterfacedesc *interfaces;
} __attribute__((packed)) usb_configurationdesc;

typedef struct _usbdevdesc
{
	u8  bLength;
	u8  bDescriptorType;
	u16 bcdUSB;
	u8  bDeviceClass;
	u8  bDeviceSubClass;
	u8  bDeviceProtocol;
	u8  bMaxPacketSize0;
	u16 idVendor;
	u16 idProduct;
	u16 bcdDevice;
	u8  iManufacturer;
	u8  iProduct;
	u8  iSerialNumber;
	u8  bNumConfigurations;
	struct _usbconfdesc *configurations;
} __attribute__((packed)) usb_devdesc;

typedef struct
{
	u32 sector_size;
	u32 sector_count;

	u32 lun;
	u32 vid;
	u32 pid;
	u32 tag;
	u32 interface;
	s32 usb_fd;

	u8 ep_in;
	u8 ep_out;
} important_storage_data;

extern u32 usb_s_size, usb_s_cnt;

typedef struct
{
	important_storage_data d;
	bool mounted;
	// Per-slot buffers: the main thread and the Slippi thread can access different slots at once.
	u8 *cbw;
	u8 *xfer;
} usb_slot;

static bool __inited = false;
static usb_slot slots[USB_SLOT_COUNT];

static int __replay_slot = USB_SLOT_MAIN;
static int __replay_index = -1;
static s32 __replay_hint_id = -1;

static s32 ven_fd = -1;
static usb_device_entry AttachedDevices[32] ALIGNED(32);

static struct ipcmessage *venchangemsg = NULL;
static u32 venchange_thread = 0;
static u8 *venchangeheap = NULL;
static s32 venchangequeue = -1;
extern char __ven_change_stack_addr, __ven_change_stack_size;

static bool __ioctl_running = false;
static bool __main_thread_dirty = false;
static bool __slippi_thread_dirty = false;

static s32 __usbstorage_reset(usb_slot *slot);

static bool __slot_alloc_buffers(usb_slot *slot)
{
	if (slot->cbw == NULL)
		slot->cbw = (u8*)malloca(32, 32);
	if (slot->xfer == NULL)
		slot->xfer = (u8*)malloca(MAX_TRANSFER_SIZE_V5, 32);
	return slot->cbw != NULL && slot->xfer != NULL;
}

static void __slot_free_buffers(usb_slot *slot)
{
	if (slot->cbw != NULL)
	{
		free(slot->cbw);
		slot->cbw = NULL;
	}
	if (slot->xfer != NULL)
	{
		free(slot->xfer);
		slot->xfer = NULL;
	}
}

static s32 __send_cbw(usb_slot *slot, u8 lun, u32 len, u8 flags, const u8 *cb, u8 cbLen)
{
	s32 retval = USBSTORAGE_OK;
	important_storage_data *dev = &slot->d;
	u8 *cbw_buffer = slot->cbw;

	if(cbLen == 0 || cbLen > 16)
		return IPC_EINVAL;

	memset(cbw_buffer, 0, CBW_SIZE);

	write32(((u32)cbw_buffer),bswap32(CBW_SIGNATURE));
	write32(((u32)cbw_buffer)+4,bswap32(++dev->tag));
	write32(((u32)cbw_buffer)+8,bswap32(len));
	cbw_buffer[12] = flags;
	cbw_buffer[13] = lun;
	cbw_buffer[14] = (cbLen > 6 ? 10 : 6);

	memcpy(cbw_buffer + 15, cb, cbLen);

	retval = USB_WriteBlkMsg(dev->usb_fd, dev->ep_out, CBW_SIZE, (void *)cbw_buffer);

	if(retval == CBW_SIZE) return USBSTORAGE_OK;
	else if(retval > 0) return USBSTORAGE_ESHORTWRITE;

	return retval;
}

static s32 __read_csw(usb_slot *slot, u8 *status, u32 *dataResidue)
{
	s32 retval = USBSTORAGE_OK;
	u32 signature, tag, _dataResidue, _status;
	important_storage_data *dev = &slot->d;
	u8 *cbw_buffer = slot->cbw;

	retval = USB_WriteBlkMsg(dev->usb_fd, dev->ep_in, CSW_SIZE, cbw_buffer);
	if(retval > 0 && retval != CSW_SIZE) return USBSTORAGE_ESHORTREAD;
	else if(retval < 0) return retval;

	signature = bswap32(read32(((u32)cbw_buffer)));
	tag = bswap32(read32(((u32)cbw_buffer)+4));
	_dataResidue = bswap32(read32(((u32)cbw_buffer)+8));
	_status = cbw_buffer[12];

	if(signature != CSW_SIGNATURE) return USBSTORAGE_ESIGNATURE;

	if(dataResidue != NULL)
		*dataResidue = _dataResidue;
	if(status != NULL)
		*status = _status;

	if(tag != dev->tag) return USBSTORAGE_ETAG;

	return USBSTORAGE_OK;
}

static s32 __cycle(usb_slot *slot, u8 lun, u8 *buffer, u32 len, u8 *cb, u8 cbLen, u8 write, u8 *_status, u32 *_dataResidue)
{
	s32 retval = USBSTORAGE_OK;
	important_storage_data *dev = &slot->d;
	u8 *transferbuffer = slot->xfer;

	u8 status=0;
	u32 dataResidue = 0;
	u32 max_size = MAX_TRANSFER_SIZE_V5;
	u8 ep = write ? dev->ep_out : dev->ep_in;
	s8 retries = USBSTORAGE_CYCLE_RETRIES + 1;

	do
	{
		u8 *_buffer = buffer;
		u32 _len = len;
		retries--;

		if(retval == USBSTORAGE_ETIMEDOUT)
			break;

		retval = __send_cbw(slot, lun, len, (write ? CBW_OUT:CBW_IN), cb, cbLen);

		while(_len > 0 && retval >= 0)
		{
			u32 thisLen = _len > max_size ? max_size : _len;

			if ((u32)_buffer&0x1F || !((u32)_buffer&0x10000000))
			{
				if(write) memcpy(transferbuffer, _buffer, thisLen);
				retval = USB_WriteBlkMsg(dev->usb_fd, ep, thisLen, transferbuffer);
				if (!write && retval > 0) memcpy(_buffer, transferbuffer, retval);
			}
			else
				retval = USB_WriteBlkMsg(dev->usb_fd, ep, thisLen, _buffer);
			if (retval == thisLen)
			{
				_len -= retval;
				_buffer += retval;
			}
			else if (retval != USBSTORAGE_ETIMEDOUT)
				retval = USBSTORAGE_EDATARESIDUE;
		}

		if (retval >= 0)
			retval = __read_csw(slot, &status, &dataResidue);

		if (retval < 0) {
			if (__usbstorage_reset(slot) == USBSTORAGE_ETIMEDOUT)
				retval = USBSTORAGE_ETIMEDOUT;
		}
	} while (retval < 0 && retries > 0);

	if(_status != NULL)
		*_status = status;
	if(_dataResidue != NULL)
		*_dataResidue = dataResidue;

	return retval;
}

static s32 __usbstorage_reset(usb_slot *slot)
{
	important_storage_data *dev = &slot->d;
	u8 bmRequestType = USB_CTRLTYPE_DIR_HOST2DEVICE | USB_CTRLTYPE_TYPE_CLASS | USB_CTRLTYPE_REC_INTERFACE;
	s32 retval = USB_WriteCtrlMsg(dev->usb_fd, bmRequestType, USBSTORAGE_RESET, 0, dev->interface, 0, NULL);

	udelay(60*1000);
	USB_ClearHalt(dev->usb_fd, dev->ep_in);udelay(10000); //from http://www.usb.org/developers/devclass_docs/usbmassbulk_10.pdf
	USB_ClearHalt(dev->usb_fd, dev->ep_out);udelay(10000);
	return retval;
}

void USBStorage_Open()
{
	sync_before_read((void*)0x132C1000, sizeof(important_storage_data));
	important_storage_data *d = (important_storage_data*)0x132C1000;
	usb_slot *slot = &slots[USB_SLOT_MAIN];

	slot->d.sector_size = d->sector_size;
	slot->d.sector_count = d->sector_count;
	slot->d.lun = d->lun;
	slot->d.vid = d->vid;
	slot->d.pid = d->pid;
	slot->d.tag = d->tag;
	slot->d.interface = d->interface;
	slot->d.usb_fd = d->usb_fd;
	slot->d.ep_in = d->ep_in;
	slot->d.ep_out = d->ep_out;

	usb_s_size = slot->d.sector_size;
	usb_s_cnt = slot->d.sector_count;

	slot->mounted = slot->d.vid != 0 || slot->d.pid != 0;

	__slot_alloc_buffers(slot);
}

static u32 __ven_change_thread()
{
	struct ipcmessage *msg = NULL;
	while(1)
	{
		mqueue_recv(venchangequeue, &msg, 0);
		mqueue_ack(msg, 0);
		__ioctl_running = false;

		// order actually matters here for thread safety
		__slippi_thread_dirty = true;
		__main_thread_dirty = true;
	}
	return 0;
}

s32 USBStorage_Startup(bool hotswap, int replaySlot, int replayIndex, s32 replayHintId)
{
	if(__inited)
		return 0;

	ven_fd = USB_Initialize();
	if(ven_fd < 0)
		return ven_fd;

	memset(slots, 0, sizeof(slots));
	USBStorage_Open();

	__replay_slot = (replaySlot == USB_SLOT_2) ? USB_SLOT_2 : USB_SLOT_MAIN;
	__replay_index = replayIndex;
	__replay_hint_id = replayHintId;

	if (hotswap)
	{
		if (__replay_slot == USB_SLOT_2)
			__slot_alloc_buffers(&slots[USB_SLOT_2]);
		else if (__replay_hint_id >= 0 && slots[USB_SLOT_MAIN].d.usb_fd != __replay_hint_id)
			// The loader left a different drive mounted; let hot-plug detection pick the replay drive.
			slots[USB_SLOT_MAIN].mounted = false;


		memset32(AttachedDevices, 0, sizeof(usb_device_entry)*32);
		venchangeheap = (u8*)malloca(32,32);
		venchangequeue = mqueue_create(venchangeheap, 1);
		venchangemsg = (struct ipcmessage*)malloca(sizeof(struct ipcmessage), 32);
		venchange_thread = do_thread_create(__ven_change_thread, ((u32*)&__ven_change_stack_addr), ((u32)(&__ven_change_stack_size)), 0x78);
		thread_continue(venchange_thread);
	}

	__inited = true;
	return 0;
}

bool USBStorage_ReadSectors(int slotIndex, u32 sector, u32 numSectors, void *buffer)
{
	if (slotIndex < 0 || slotIndex >= USB_SLOT_COUNT || !slots[slotIndex].mounted)
		return false;

	usb_slot *slot = &slots[slotIndex];
	u8 status = 0;
	s32 retval;
	u8 cmd[] = {
		SCSI_READ_10,
		slot->d.lun << 5,
		sector >> 24,
		sector >> 16,
		sector >>  8,
		sector,
		0,
		numSectors >> 8,
		numSectors,
		0
	};

	retval = __cycle(slot, slot->d.lun, buffer, numSectors * slot->d.sector_size, cmd, sizeof(cmd), 0, &status, NULL);
	if(retval > 0 && status != 0)
		retval = USBSTORAGE_ESTATUS;

	return retval >= 0;
}

bool USBStorage_WriteSectors(int slotIndex, u32 sector, u32 numSectors, const void *buffer)
{
	if (slotIndex < 0 || slotIndex >= USB_SLOT_COUNT || !slots[slotIndex].mounted)
		return false;

	usb_slot *slot = &slots[slotIndex];
	u8 status = 0;
	s32 retval;
	u8 cmd[] = {
		SCSI_WRITE_10,
		slot->d.lun << 5,
		sector >> 24,
		sector >> 16,
		sector >> 8,
		sector,
		0,
		numSectors >> 8,
		numSectors,
		0
	};

	retval = __cycle(slot, slot->d.lun, (u8*)buffer, numSectors * slot->d.sector_size, cmd, sizeof(cmd), 1, &status, NULL);
	if(retval > 0 && status != 0)
		retval = USBSTORAGE_ESTATUS;

	return retval >= 0;
}

u32 USBStorage_GetSectorSize(int slotIndex)
{
	if (slotIndex < 0 || slotIndex >= USB_SLOT_COUNT)
		return 0;
	return slots[slotIndex].d.sector_size;
}

static void USBStorage_Close(usb_slot *slot)
{
	slot->d.lun = 0;
	slot->d.vid = 0;
	slot->d.pid = 0;
	slot->mounted = false;
}

void USBStorage_Shutdown(void)
{
	if(__inited == false)
		return;

	int i;
	for (i = 0; i < USB_SLOT_COUNT; i++)
	{
		USBStorage_Close(&slots[i]);
		__slot_free_buffers(&slots[i]);
	}

	USB_Deinitialize();
	__inited = false;
}

// see libogc/usb.c: __find_next_endpoint
static u32 __find_next_endpoint(u8 *buffer,s32 size,u8 align)
{
	u8 *ptr = buffer;

	while(size>2 && buffer[0]) { // abort if buffer[0]==0 to avoid getting stuck
		if(buffer[1]==USB_DT_ENDPOINT || buffer[1]==USB_DT_INTERFACE)
			break;

		size -= (buffer[0]+align)&~align;
		buffer += (buffer[0]+align)&~align;
	}

	return (buffer - ptr);
}

static bool __setValidLun(usb_slot *probe, int max_lun)
{
	important_storage_data *dev = &probe->d;
	s32 retval;
	int lun;

	// max_lun is the maximum LUN index, not the number of LUNs
	for (lun = 0; lun <= max_lun; lun++)
	{
		udelay(50);

		// see libogc/usbstorage.c: __usbstorage_clearerrors
		u8 test_cmd[] = {SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0};
		retval = __cycle(probe, lun, NULL, 0, test_cmd, 6, 0, NULL, NULL);
		if (retval < 0)
			continue;

		u8 sense_cmd[] = {SCSI_REQUEST_SENSE, lun << 5, 0, 0, SCSI_SENSE_REPLY_SIZE, 0};
		u8 sense_response[SCSI_SENSE_REPLY_SIZE];
		memset(sense_response, 0, SCSI_SENSE_REPLY_SIZE);
		retval = __cycle(probe, lun, sense_response, SCSI_SENSE_REPLY_SIZE, sense_cmd, 6, 0, NULL, NULL);
		if (retval < 0)
			continue;
		u8 sense_key = sense_response[2] & 0xF;
		if (sense_key == SCSI_SENSE_NOT_READY || sense_key == SCSI_SENSE_MEDIUM_ERROR || sense_key == SCSI_SENSE_HARDWARE_ERROR)
			continue;

		// see libogc/usbstorage.c: USBStorage_Inquiry
		u8 inquiry_cmd[] = {SCSI_INQUIRY, lun << 5,0,0,36,0};
		u8 inquiry_response[36];
		int j;
		for (j = 0; j < 2; j++)
		{
			memset(inquiry_response, 0, 36);
			retval = __cycle(probe, lun, inquiry_response, 36, inquiry_cmd, 6, 0, NULL, NULL);
			if (retval >= 0) break;
		}

		// see libogc/usbstorage.c: USBStorage_ReadCapacity
		u8 read_capacity_cmd[10] = {SCSI_READ_CAPACITY, lun << 5, 0, 0, 0, 0, 0, 0, 0, 0};
		u32 read_capacity_response[2];
		memset(read_capacity_response, 0, 8);
		retval = __cycle(probe, lun, (u8*)read_capacity_response, 8, read_capacity_cmd, 10, 0, NULL, NULL);

		if (retval >= 0 && read_capacity_response[0] > 0 && read_capacity_response[1] >= 512)
		{
			dev->sector_count = read_capacity_response[0];
			dev->sector_size = read_capacity_response[1];
			dev->lun = lun;
			return true;
		}				
	}
	return false;
}

/**
 * Probe AttachedDevices[i] and, if it is a usable mass storage device,
 * fill probe->d with its parameters.
 * probe->cbw/xfer must point at buffers not in use by another thread.
 */
static bool __probe_device(int i, usb_slot *probe)
{
	// known device USB LAN
	if (AttachedDevices[i].vid == 0x0b95 && AttachedDevices[i].pid == 0x7720)
		return false;

	s32 suspend_resume_buf[8] ALIGNED(32);
	memset(suspend_resume_buf, 0, sizeof(s32) * 8);
	u32 get_dev_params_in[8] ALIGNED(32);
	memset(get_dev_params_in, 0, sizeof(u32) * 8);
	u8 get_dev_params_out[GETDEVPARAMS_OUT_SIZE] ALIGNED(32);
	usb_devdesc *udd = NULL;
	usb_configurationdesc *ucd = NULL;
	usb_interfacedesc *uid = NULL;
	usb_endpointdesc *ued = NULL;
	{
		// see libogc/usb.c: USBV5_SuspendResume
		suspend_resume_buf[0] = AttachedDevices[i].device_id;
		suspend_resume_buf[2] = 1;
		IOS_Ioctl(ven_fd, USBV5_IOCTL_SUSPEND_RESUME, suspend_resume_buf, 32, NULL, 0);

		// see libogc/usb.c: USBV5_GetDescriptors
		get_dev_params_in[0] = AttachedDevices[i].device_id;
		get_dev_params_in[2] = 0;
		memset(get_dev_params_out, 0, GETDEVPARAMS_OUT_SIZE);
		s32 retval = IOS_Ioctl(ven_fd, USBV5_IOCTL_GETDEVPARAMS, get_dev_params_in, 32, get_dev_params_out, GETDEVPARAMS_OUT_SIZE);
		if (retval == IPC_OK)
		{
			u8 *next = get_dev_params_out + GETDEVPARAMS_DESC_OFFSET;
			udd = (usb_devdesc*)next;
			next += (udd->bLength+3)&~3;

			// "very few devices have more than 1 configuration" - https://www.beyondlogic.org/usbnutshell/usb5.shtml
			// also, GETDEVPARAMS_OUT_SIZE 0xC0 is sized exactly for 1 configuration with 1 interface with up to 16 endpoints
			// so assume only one configuration
			ucd = (usb_configurationdesc*)next;
			next += (ucd->bLength+3)&~3;
			if (ucd->bNumInterfaces == 0)
				return false;

			// "IOS presents each interface as a different device" - libogc/usb.c
			// so assume only one interface
			uid = (usb_interfacedesc*)next;
			next += (uid->bLength+3)&~3;

			if (uid->bInterfaceClass == USB_CLASS_MASS_STORAGE && uid->bInterfaceProtocol == MASS_STORAGE_BULK_ONLY && uid->bNumEndpoints >= 2)
			{
				u16 extra_size = __find_next_endpoint(next, get_dev_params_out + GETDEVPARAMS_OUT_SIZE - next, 3);
				if (extra_size > 0)
					next += extra_size;

				u8 endpoint_in = 0;
				u8 endpoint_out = 0;
				int iEndpoint;
				for (iEndpoint = 0; iEndpoint < uid->bNumEndpoints; iEndpoint++)
				{
					ued = (usb_endpointdesc*)next;
					next += (ued->bLength+3)&~3;
					if (ued->bmAttributes != USB_ENDPOINT_BULK)
						continue;

					if (ued->bEndpointAddress & USB_ENDPOINT_IN)
						endpoint_in = ued->bEndpointAddress;
					else
						endpoint_out = ued->bEndpointAddress;
				}

				if (endpoint_in != 0 && endpoint_out != 0)
				{
					important_storage_data new_device;
					new_device.vid = AttachedDevices[i].vid;
					new_device.pid = AttachedDevices[i].pid;
					new_device.tag = TAG_START;
					new_device.interface = uid->bInterfaceNumber;
					new_device.usb_fd = AttachedDevices[i].device_id;
					new_device.ep_in = endpoint_in;
					new_device.ep_out = endpoint_out;

					// Even though (we assume) the device has only one configuration, we need to explicitly select it.
					u8 bmRequestType = USB_CTRLTYPE_DIR_HOST2DEVICE | USB_CTRLTYPE_TYPE_STANDARD | USB_CTRLTYPE_REC_DEVICE;
					retval = USB_WriteCtrlMsg(new_device.usb_fd, bmRequestType, USB_REQ_SETCONFIG, ucd->bConfigurationValue, 0, 0, NULL);

					bmRequestType = USB_CTRLTYPE_DIR_DEVICE2HOST | USB_CTRLTYPE_TYPE_CLASS | USB_CTRLTYPE_REC_INTERFACE;
					u8 max_lun = 0;
					retval = USB_ReadCtrlMsg(new_device.usb_fd, bmRequestType, USBSTORAGE_GET_MAX_LUN, 0, new_device.interface, 1, &max_lun);
					probe->d = new_device;
					if (__setValidLun(probe, max_lun))
						return true;
				}
			}
		}
	}
	return false;
}

static bool __mount_probed(usb_slot *target, usb_slot *probe)
{
	target->d = probe->d;
	if (target == &slots[USB_SLOT_MAIN])
	{
		usb_s_size = target->d.sector_size;
		usb_s_cnt = target->d.sector_count;
	}
	target->mounted = true;
	udelay(10000);
	return true;
}

// see libogc/usbstorage.c: __usbstorage_IsInserted
bool __has_device_after_change()
{
	int i;
	u32 num_attached_devices = venchangemsg->result;
	usb_slot *target = &slots[__replay_slot];

	// When replays go to slot 2, slot 0 is the game drive and must not be touched.
	s32 game_fd = (__replay_slot == USB_SLOT_2 && slots[USB_SLOT_MAIN].mounted) ? slots[USB_SLOT_MAIN].d.usb_fd : -1;

	if (num_attached_devices == 0)
	{
		target->mounted = false;
		return false;
	}

	// If already have a device, return if it's still present
	if (target->mounted)
	{
		for (i = 0; i < num_attached_devices; i++)
		{
			if (AttachedDevices[i].device_id == target->d.usb_fd) {
				udelay(50);
				return true;
			}
		}
		target->mounted = false;
	}

	usb_slot probe;
	memset(&probe, 0, sizeof(probe));
	probe.cbw = target->cbw;
	probe.xfer = target->xfer;

	// Prefer the exact drive the loader identified.
	if (__replay_hint_id >= 0 && __replay_hint_id != game_fd)
	{
		for (i = 0; i < num_attached_devices; i++)
		{
			if (AttachedDevices[i].device_id == __replay_hint_id && __probe_device(i, &probe))
				return __mount_probed(target, &probe);
		}
	}

	// Otherwise pick the Nth usable drive. The game drive counts toward N
	// so the numbering matches the loader's USB 1 / USB 2.
	int valid_index = 0;
	for (i = 0; i < num_attached_devices; i++)
	{
		if (AttachedDevices[i].device_id == game_fd)
		{
			valid_index++;
			continue;
		}
		if (!__probe_device(i, &probe))
			continue;
		if (__replay_index < 0 || valid_index == __replay_index)
			return __mount_probed(target, &probe);
		valid_index++;
	}
	return false;
}

// Call periodically from only the main thread
void USBStorage_UpdateRegisters_MainThread(void)
{
	if (__main_thread_dirty)
	{
		IOS_Ioctl(ven_fd, USBV5_IOCTL_ATTACHFINISH, NULL, 0, NULL, 0);
		__main_thread_dirty = false;
	}

	if (!__main_thread_dirty && !__slippi_thread_dirty && !__ioctl_running)
	{
		IOS_IoctlAsync(ven_fd, USBV5_IOCTL_GETDEVICECHANGE, NULL, 0, AttachedDevices, 0x180, venchangequeue, venchangemsg);
		__ioctl_running = true;
	}
}

// Call periodically from only the slippi thread
bool USBStorage_IsInserted_SlippiThread(void)
{
	if (__slippi_thread_dirty)
	{
		bool retval = __has_device_after_change();

		__slippi_thread_dirty = false;
		return retval;
	}

	return slots[__replay_slot].mounted;
}
