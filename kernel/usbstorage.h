#ifndef __USBSTORAGE_H__
#define __USBSTORAGE_H__

#define	USBSTORAGE_OK			0
#define	USBSTORAGE_ENOINTERFACE		-10000
#define	USBSTORAGE_ESENSE		-10001
#define	USBSTORAGE_ESHORTWRITE		-10002
#define	USBSTORAGE_ESHORTREAD		-10003
#define	USBSTORAGE_ESIGNATURE		-10004
#define	USBSTORAGE_ETAG			-10005
#define	USBSTORAGE_ESTATUS		-10006
#define	USBSTORAGE_EDATARESIDUE		-10007
#define	USBSTORAGE_ETIMEDOUT		-10008
#define	USBSTORAGE_EINIT		-10009
#define USBSTORAGE_PROCESSING	-10010

#define B_RAW_DEVICE_DATA_IN 0x01
#define B_RAW_DEVICE_COMMAND 0

typedef struct {
   uint8_t         command[16];
   uint8_t         command_length;
   uint8_t         flags;
   uint8_t         scsi_status;
   void*           data;
   size_t          data_length;
} raw_device_command;

// Slot 0 is mounted as "usb:", slot 1 as "usb2:".
#define USB_SLOT_MAIN	0
#define USB_SLOT_2		1
#define USB_SLOT_COUNT	2

/**
 * @param hotswap Enable hot-plug detection for the replay drive.
 * @param replaySlot Slot the hot-plugged replay drive is mounted into.
 * @param replayIndex Which valid USB drive to use (0 = first), or -1 for the first one found.
 * @param replayHintId IOS device_id the loader saw for the replay drive, or -1.
 */
s32 USBStorage_Startup(bool hotswap, int replaySlot, int replayIndex, s32 replayHintId);
bool USBStorage_ReadSectors(int slot, u32 sector, u32 numSectors, void *buffer);
bool USBStorage_WriteSectors(int slot, u32 sector, u32 numSectors, const void *buffer);
u32 USBStorage_GetSectorSize(int slot);
void USBStorage_Shutdown(void);

void USBStorage_UpdateRegisters_MainThread(void);
bool USBStorage_IsInserted_SlippiThread(void);

#endif /* __USBSTORAGE_H__ */
