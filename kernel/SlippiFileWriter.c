#include "SlippiFileWriter.h"
#include "SlippiMemory.h"
#include "alloc.h"
#include "debug.h"
#include "string.h"
#include "ff_utf8.h"
#include "net.h"

#include "Config.h"
#include "usbstorage.h"

// use common physical sector size so as to write efficiently
// and not excessively wear out the underlying flash storage
#define READ_BUF_SIZE 4096

#define THREAD_CYCLE_TIME_MS 100
#define THREAD_ERROR_TIME_MS 2000
#define LED_FLASH_TIME_MS 1000

#define FOOTER_BUFFER_LENGTH 200

static u32 SlippiHandlerThread(void *arg);

// Thread stuff
static u32 Slippi_Thread;
extern char __slippi_stack_addr, __slippi_stack_size;

// File writing stuff
extern u8 wifi_mac_address[6]; // Used to identify replays

// File object
FIL currentFile;

// vars for metadata generation
u32 gameStartTime;

// timer for drive led
u32 driveTimer;

// flag for drive led timer
bool driveTimerSet;

// replays LED setting
bool replaysLED;

extern FATFS *devices[3];

void SlippiFileWriterInit(bool led)
{
	replaysLED = led;

	Slippi_Thread = do_thread_create(
		SlippiHandlerThread,
		((u32 *)&__slippi_stack_addr),
		((u32)(&__slippi_stack_size)),
		0x78);
	thread_continue(Slippi_Thread);
}

void SlippiFileWriterUpdateRegisters()
{
	if (driveTimerSet && TimerDiffMs(driveTimer) >= LED_FLASH_TIME_MS)
	{
		clear32(HW_GPIO_OUT, GPIO_SLOT_LED);
		driveTimerSet = false;
	}
}

void SlippiFileWriterShutdown()
{
	thread_cancel(Slippi_Thread, 0);
}

void flashLED()
{
	driveTimer = read32(HW_TIMER);
	if (!driveTimerSet)
	{
		set32(HW_GPIO_OUT, GPIO_SLOT_LED);
		driveTimerSet = true;
	}
}

//we cant include time.h so hardcode what we need
struct tm
{
	int tm_sec;
	int tm_min;
	int tm_hour;
	int tm_mday;
	int tm_mon;
	int tm_year;
};
extern struct tm *gmtime(u32 *time);

char *generateFileName()
{
	// // Add game start time
	// u8 dateTimeStrLength = sizeof "20171015T095717";
	// char *dateTimeBuf = (char *)malloc(dateTimeStrLength);
	// strftime(&dateTimeBuf[0], dateTimeStrLength, "%Y%m%dT%H%M%S", localtime(&gameStartTime));

	// std::string str(&dateTimeBuf[0]);
	// return StringFromFormat("Slippi/Game_%s.slp", str.c_str());

	static char pathStr[50];
	struct tm *tmp = gmtime(&gameStartTime);

	_sprintf(
		&pathStr[0], "/Slippi/Game_%02X%02X%02X%02X%02X%02X_%04d%02d%02dT%02d%02d%02d.slp",
		wifi_mac_address[0], wifi_mac_address[1], wifi_mac_address[2], wifi_mac_address[3],
		wifi_mac_address[4], wifi_mac_address[5], tmp->tm_year + 1900, tmp->tm_mon + 1,
		tmp->tm_mday, tmp->tm_hour, tmp->tm_min, tmp->tm_sec);

	return pathStr;
}

FRESULT writeHeader(FIL *file)
{
	u8 header[] = {'{', 'U', 3, 'r', 'a', 'w', '[', '$', 'U', '#', 'l', 0, 0, 0, 0};

	u32 wrote;
	return f_write(file, header, sizeof(header), &wrote);
}

FRESULT completeFile(FIL *file, s32 lastFrame, u32 writtenByteCount)
{
	u8 footer[FOOTER_BUFFER_LENGTH];
	u32 writePos = 0;

	// Write opener
	u8 footerOpener[] = {'U', 8, 'm', 'e', 't', 'a', 'd', 'a', 't', 'a', '{'};
	u8 writeLen = sizeof(footerOpener);
	memcpy(&footer[writePos], footerOpener, writeLen);
	writePos += writeLen;

	// Write startAt
	// TODO: Figure out how to specify time zone
	char timeStr[] = "2011-10-08T07:07:09";
	int timeStrLen = strlen(timeStr);
	struct tm *tmp = gmtime(&gameStartTime);
	_sprintf(
		&timeStr[0], "%04d-%02d-%02dT%02d:%02d:%02d", tmp->tm_year + 1900,
		tmp->tm_mon + 1, tmp->tm_mday, tmp->tm_hour, tmp->tm_min, tmp->tm_sec);
	u8 startAtOpener[] = {'U', 7, 's', 't', 'a', 'r', 't', 'A', 't', 'S', 'U', (u8)timeStrLen};
	writeLen = sizeof(startAtOpener);
	memcpy(&footer[writePos], startAtOpener, writeLen);
	writePos += writeLen;
	writeLen = timeStrLen;
	memcpy(&footer[writePos], timeStr, writeLen);
	writePos += writeLen;

	// Write lastFrame
	u8 lastFrameOpener[] = {'U', 9, 'l', 'a', 's', 't', 'F', 'r', 'a', 'm', 'e', 'l'};
	writeLen = sizeof(lastFrameOpener);
	memcpy(&footer[writePos], lastFrameOpener, writeLen);
	writePos += writeLen;
	memcpy(&footer[writePos], &lastFrame, 4);
	writePos += 4;

	// Write console nickname
	u8 nickLen = strlen(SlippiGetConsoleNick());
	if (nickLen > 32) nickLen = 32;
	u8 consoleNickOpener[] = { 'U', 11, 'c', 'o', 'n', 's', 'o', 'l', 'e', 'N', 'i', 'c', 'k', 'S', 'U', nickLen };
	writeLen = sizeof(consoleNickOpener);
	memcpy(&footer[writePos], consoleNickOpener, writeLen);
	writePos += writeLen;
	memcpy(&footer[writePos], SlippiGetConsoleNick(), nickLen);
	writePos += nickLen;

	// Write closing
	u8 closing[] = {
		'U', 7, 'p', 'l', 'a', 'y', 'e', 'r', 's', '{', '}',
		'U', 8, 'p', 'l', 'a', 'y', 'e', 'd', 'O', 'n', 'S', 'U',
		10, 'n', 'i', 'n', 't', 'e', 'n', 'd', 'o', 'n', 't',
		'}', '}'};
	writeLen = sizeof(closing);
	memcpy(&footer[writePos], closing, writeLen);
	writePos += writeLen;

	// Write footer
	// Always seek first in case there was a previous failure with partial write
	FRESULT fRes = f_lseek(file, writtenByteCount + 15);
	if (fRes != FR_OK)
	{
		dbgprintf("Slippi: failed to seek before writing footer, errno: %d\r\n", fRes);
		return fRes;
	}

	u32 wrote;
	fRes = f_write(file, footer, writePos, &wrote);
	if (fRes != FR_OK)
	{
		dbgprintf("Slippi: failed to write footer, errno: %d\r\n", fRes);
		return fRes;
	}

	// Write length
	fRes = f_lseek(file, 11);
	if (fRes != FR_OK)
	{
		dbgprintf("Slippi: failed to seek before writing length, errno: %d\r\n", fRes);
		return fRes;
	}

	fRes = f_write(file, &writtenByteCount, 4, &wrote);
	if (fRes != FR_OK)
		dbgprintf("Slippi: failed to write length, errno: %d\r\n", fRes);
	
	return fRes;
}

/* Replay status file
 * dbgprintf() output is invisible unless a debug build/logging is in use, so
 * the Slippi thread also keeps a plain-text summary of what it is doing in
 * STATUS_FILE_PATH at the root of the replay drive. Only this thread touches
 * the replay drive, so writing it here adds no FatFs contention elsewhere.
 */
#define STATUS_FILE_PATH "/slippi_status.txt"
#define STATUS_GAME_HISTORY 4
#define STATUS_MIN_FLUSH_MS 1000
#define STATUS_HEARTBEAT_MS 10000
#define STATUS_BUF_SIZE 2048
#define STATUS_LINE_MAX 200
#define STATUS_NOT_RUN -1

#define LED_ERROR_BLINK_MS 250
#define LED_WAITING_BLINK_MS 1000

typedef struct StatusGame
{
	u32 number;
	char fileName[64];
	int openResult;
	u32 bytesWritten;
	const char *outcome;
	int lastResult;
} StatusGame;

static struct
{
	u32 startTime;
	const char *driveState;
	u32 mountAttempts;
	int mountResult;
	int mkdirResult;
	u32 memErrorCount;
	SlpMemError lastMemError;
	u32 gamesSeen;
	StatusGame games[STATUS_GAME_HISTORY];
	u32 writes;
	int lastWriteResult;
	bool dirty;
	u32 lastFlush;
	u32 lastHeartbeat;
	u32 lastBytesReceived;
} status;

static char statusBuf[STATUS_BUF_SIZE];
static u32 statusLen;
static FIL statusFile;

static u32 errorBlinkTimer;

static const char *fresultName(int res)
{
	static const char *names[] = {
		"FR_OK", "FR_DISK_ERR", "FR_INT_ERR", "FR_NOT_READY", "FR_NO_FILE",
		"FR_NO_PATH", "FR_INVALID_NAME", "FR_DENIED", "FR_EXIST",
		"FR_INVALID_OBJECT", "FR_WRITE_PROTECTED", "FR_INVALID_DRIVE",
		"FR_NOT_ENABLED", "FR_NO_FILESYSTEM", "FR_MKFS_ABORTED", "FR_TIMEOUT",
		"FR_LOCKED", "FR_NOT_ENOUGH_CORE", "FR_TOO_MANY_OPEN_FILES",
		"FR_INVALID_PARAMETER"};

	if (res == STATUS_NOT_RUN)
		return "not attempted";
	if (res < 0 || res >= (int)(sizeof(names) / sizeof(names[0])))
		return "unknown";
	return names[res];
}

static const char *memErrorName(SlpMemError err)
{
	switch (err)
	{
		case SLP_MEM_OK: return "none";
		case SLP_MEM_UNNEX_NG: return "unexpected new game";
		case SLP_PL_MISSING: return "payload size missing";
		case SLP_READ_OVERFLOW: return "read overflow (writer fell behind)";
		default: return "unknown";
	}
}

static const char *replayDeviceName(u32 dev)
{
	switch (dev)
	{
		case REPLAY_DEV_AUTO: return "Auto";
		case REPLAY_DEV_SD: return "SD";
		case REPLAY_DEV_USB1: return "USB 1";
		case REPLAY_DEV_USB2: return "USB 2";
		default: return "unknown";
	}
}

static const char *replayTargetName(u32 target)
{
	switch (target)
	{
		case REPLAY_TARGET_SD: return "sd: (SD card)";
		case REPLAY_TARGET_USB: return "usb: (USB slot 0)";
		case REPLAY_TARGET_USB2: return "usb2: (USB slot 1)";
		default: return "unknown";
	}
}

static void statusAppend(const char *fmt, ...)
{
	if (statusLen + STATUS_LINE_MAX >= STATUS_BUF_SIZE)
		return;

	va_list args;
	va_start(args, fmt);
	_vsprintf(&statusBuf[statusLen], fmt, args);
	va_end(args);
	statusLen += strlen(&statusBuf[statusLen]);
}

static void statusInit(void)
{
	memset(&status, 0, sizeof(status));
	status.startTime = read32(HW_TIMER);
	status.driveState = "starting";
	status.mountResult = STATUS_NOT_RUN;
	status.mkdirResult = STATUS_NOT_RUN;
	status.lastWriteResult = STATUS_NOT_RUN;
	status.lastMemError = SLP_MEM_OK;
	status.lastFlush = status.startTime;
	status.lastHeartbeat = status.startTime;
	status.dirty = true;
}

static void statusSetDriveState(const char *state)
{
	if (status.driveState != state)
	{
		status.driveState = state;
		status.dirty = true;
	}
}

static StatusGame *statusCurrentGame(void)
{
	if (status.gamesSeen == 0)
		return NULL;
	return &status.games[(status.gamesSeen - 1) % STATUS_GAME_HISTORY];
}

static StatusGame *statusGameStart(const char *fileName)
{
	StatusGame *game = &status.games[status.gamesSeen % STATUS_GAME_HISTORY];
	status.gamesSeen++;

	memset(game, 0, sizeof(StatusGame));
	game->number = status.gamesSeen;
	if (strlen(fileName) < sizeof(game->fileName))
		_sprintf(game->fileName, "%s", fileName);
	game->openResult = STATUS_NOT_RUN;
	game->lastResult = STATUS_NOT_RUN;
	game->outcome = "opening";
	status.dirty = true;
	return game;
}

static void statusGameOutcome(const char *outcome, int result)
{
	StatusGame *game = statusCurrentGame();
	if (game == NULL)
		return;

	game->outcome = outcome;
	if (result != STATUS_NOT_RUN)
		game->lastResult = result;
	status.dirty = true;
}

static void statusBuild(void)
{
	SlpMemStats stats;
	SlippiMemoryGetStats(&stats);

	statusLen = 0;
	statusBuf[0] = 0;

	statusAppend("Slippi Nintendont replay status\r\n");
	statusAppend("Version: %s\r\n", NIN_GIT_VERSION);
	statusAppend("Seconds since boot: %u\r\n", TimerDiffMs(status.startTime) / 1000);
	statusAppend("Status file writes: %u (previous write: %s)\r\n",
		status.writes + 1, fresultName(status.lastWriteResult));

	statusAppend("\r\n[Config]\r\n");
	if (ConfigGetUseUSB())
		statusAppend("Games loaded from: USB %u\r\n", ConfigGetUSBIndex() + 1);
	else
		statusAppend("Games loaded from: SD\r\n");
	statusAppend("Replay Drive setting: %s (%u)\r\n",
		replayDeviceName(ConfigGetReplayDevice()), ConfigGetReplayDevice());
	statusAppend("Replay target: %s\r\n", replayTargetName(GetReplayTarget()));
	statusAppend("Replay USB index: %d\r\n", GetReplayUSBIndex());
	statusAppend("Replays LED: %s\r\n", replaysLED ? "on" : "off");

	statusAppend("\r\n[Replay drive]\r\n");
	statusAppend("State: %s\r\n", status.driveState);
	statusAppend("Mount attempts by Slippi thread: %u\r\n", status.mountAttempts);
	statusAppend("Last mount result: %s (%d)\r\n", fresultName(status.mountResult), status.mountResult);
	statusAppend("/Slippi mkdir result: %s (%d)\r\n", fresultName(status.mkdirResult), status.mkdirResult);

	statusAppend("\r\n[Data from game]\r\n");
	statusAppend("Bytes received: %u\r\n", (u32)stats.bytesReceived);
	statusAppend("EXI transfers: %u\r\n", stats.writeCalls);
	statusAppend("Games started: %u\r\n", stats.gamesStarted);
	statusAppend("Games ended: %u\r\n", stats.gamesEnded);
	statusAppend("Replay buffer errors: %u (last: %s)\r\n",
		status.memErrorCount, memErrorName(status.lastMemError));

	statusAppend("\r\n[Recent replay files] (newest first)\r\n");
	if (status.gamesSeen == 0)
		statusAppend("none yet\r\n");

	u32 shown = 0;
	while (shown < status.gamesSeen && shown < STATUS_GAME_HISTORY)
	{
		const StatusGame *game = &status.games[(status.gamesSeen - 1 - shown) % STATUS_GAME_HISTORY];
		statusAppend("#%u %s\r\n", game->number, game->fileName);
		statusAppend("  open: %s, bytes written: %u\r\n",
			fresultName(game->openResult), game->bytesWritten);
		statusAppend("  result: %s, last error: %s (%d)\r\n",
			game->outcome, fresultName(game->lastResult), game->lastResult);
		shown++;
	}
}

static void statusFlush(void)
{
	statusBuild();

	FRESULT res = f_open_secondary_drive(&statusFile, STATUS_FILE_PATH, FA_CREATE_ALWAYS | FA_WRITE);
	if (res == FR_OK)
	{
		UINT wrote;
		res = f_write(&statusFile, statusBuf, statusLen, &wrote);
		FRESULT closeRes = f_close(&statusFile);
		if (res == FR_OK)
			res = closeRes;
	}
	if (res != FR_OK)
		dbgprintf("Slippi: failed to write %s, errno: %d\r\n", STATUS_FILE_PATH, res);

	status.lastWriteResult = res;
	status.writes++;
	status.dirty = false;
	status.lastFlush = read32(HW_TIMER);
}

// Call only while the replay drive is mounted
static void statusMaybeFlush(bool recording)
{
	if (!recording && TimerDiffMs(status.lastHeartbeat) >= STATUS_HEARTBEAT_MS)
	{
		SlpMemStats stats;
		SlippiMemoryGetStats(&stats);
		if ((u32)stats.bytesReceived != status.lastBytesReceived)
		{
			status.lastBytesReceived = (u32)stats.bytesReceived;
			status.dirty = true;
		}
		status.lastHeartbeat = read32(HW_TIMER);
	}

	if (status.dirty && TimerDiffMs(status.lastFlush) >= STATUS_MIN_FLUSH_MS)
		statusFlush();
}

static void blinkLED(u32 periodMs)
{
	if (!replaysLED || TimerDiffMs(errorBlinkTimer) < periodMs)
		return;

	errorBlinkTimer = read32(HW_TIMER);
	if (read32(HW_GPIO_OUT) & GPIO_SLOT_LED)
		clear32(HW_GPIO_OUT, GPIO_SLOT_LED);
	else
		set32(HW_GPIO_OUT, GPIO_SLOT_LED);
}

static u32 SlippiHandlerThread(void *arg)
{
	dbgprintf("Slippi Thread ID: %d\r\n", thread_get_id());

	static SlpGameReader reader;
	static u8 readBuf[READ_BUF_SIZE];
	static u64 memReadPos = 0;

	u32 writtenByteCount = 0;
	s32 lastFrame;
	driveTimer = read32(HW_TIMER);
	driveTimerSet = false;

	bool failedToMount = false;
	bool dirReady = false;
	bool dirFailed = false;
	bool currentFileOpen = false;
	bool currentFileValid = false;
	const u32 replay_target = GetReplayTarget();
	const bool use_usb = replay_target != REPLAY_TARGET_SD;
	const char *usb_mount_name = (replay_target == REPLAY_TARGET_USB2) ? "usb2:" : "usb:";
	FATFS *usb_fs = (replay_target == REPLAY_TARGET_USB2) ? devices[2] : devices[1];
	// "usb:" is mounted at boot by kernel/main.c; "usb2:" is always mounted by this thread.
	bool mounted;
	if (!use_usb)
		mounted = true;
	else if (replay_target == REPLAY_TARGET_USB2)
		mounted = false;
	else
		mounted = USBStorage_IsInserted_SlippiThread();

	statusInit();
	if (mounted)
		statusSetDriveState("mounted at boot");
	errorBlinkTimer = read32(HW_TIMER);

	while (1)
	{
		// Cycle time, look at const definition for more info
		mdelay(THREAD_CYCLE_TIME_MS);

		if (use_usb)
		{
			if (!USBStorage_IsInserted_SlippiThread())
			{
				if (mounted)
				{
					// unmount (cannot fail so no need to check return value)
					f_mount_char(NULL, usb_mount_name, 1);
				}

				if (currentFileOpen || currentFileValid)
					statusGameOutcome("drive removed during game", STATUS_NOT_RUN);
				statusSetDriveState("waiting for USB drive");
				blinkLED(LED_WAITING_BLINK_MS);

				failedToMount = false;
				dirReady = false;
				dirFailed = false;
				currentFileOpen = false;
				currentFileValid = false;
				mounted = false;
				continue;
			}
			else if (!mounted && !failedToMount)
			{
				status.mountAttempts++;
				FRESULT mountResult = f_mount_char(usb_fs, usb_mount_name, 1);
				status.mountResult = mountResult;
				status.dirty = true;
				if (mountResult != FR_OK)
				{
					dbgprintf("Slippi: failed to mount %s, errno: %d\r\n", usb_mount_name, mountResult);
					statusSetDriveState("mount failed (re-insert drive to retry)");

					// only attempt to mount once, user can retry by re-inserting the device.
					failedToMount = true;
					continue;
				}

				// ignore anything already in the buffer. users should not expect to record a
				// game if the usb device is inserted after game start.
				memReadPos = SlippiRestoreReadPos();
				mounted = true;
				statusSetDriveState("mounted by Slippi thread");
			}
			if (failedToMount)
			{
				blinkLED(LED_ERROR_BLINK_MS);
				continue;
			}
			if (!mounted)
				continue;
		}

		// Drives mounted at boot never pass through the mount branch above, so
		// make sure the replay folder exists before the first file is opened.
		if (!dirReady && !dirFailed)
		{
			FRESULT mkdirResult = f_mkdir_secondary_drive("/Slippi");
			status.mkdirResult = mkdirResult;
			status.dirty = true;
			if (mkdirResult == FR_OK || mkdirResult == FR_EXIST)
			{
				dirReady = true;
				statusSetDriveState("ready");
			}
			else
			{
				dbgprintf("Slippi: failed to mkdir: /Slippi, errno: %d\r\n", mkdirResult);
				dirFailed = true;
				statusSetDriveState("could not create /Slippi (re-insert drive to retry)");
			}
		}

		statusMaybeFlush(currentFileValid);

		if (dirFailed)
		{
			blinkLED(LED_ERROR_BLINK_MS);
			continue;
		}

		while (1)
		{
			// Read from memory and write to file
			SlpMemError err = SlippiMemoryRead(&reader, readBuf, READ_BUF_SIZE, memReadPos);
			if (err)
			{
				status.memErrorCount++;
				status.lastMemError = err;
				status.dirty = true;
				if (currentFileValid)
					statusGameOutcome("abandoned: replay buffer error", STATUS_NOT_RUN);

				// all possible errors render the current file incompletable, so let's jump ahead
				currentFileValid = false;
				memReadPos = SlippiRestoreReadPos();
				if (currentFileOpen)
				{
					FRESULT closeResult = f_close(&currentFile);
					if (closeResult != FR_OK)
					{
						dbgprintf("Slippi: failed to close incompletable file, errno: %d\r\n", closeResult);
					}
					else
					{
						currentFileOpen = false;
					}
				}
				break;
			}

			if (reader.lastReadResult.bytesAvailable < READ_BUF_SIZE && !(currentFileValid && reader.lastReadResult.isGameEnd))
			{
				if (replaysLED)
					flashLED();
				break;
			}

			if (reader.lastReadResult.isNewGame)
			{
				if (currentFileValid)
				{
					FRESULT completeResult = completeFile(&currentFile, lastFrame, writtenByteCount);
					if (completeResult != FR_OK)
					{
						dbgprintf("Slippi: failed to complete dangling file, errno: %d\r\n", completeResult);
						statusGameOutcome("failed to finish file (no game end seen)", completeResult);
						break;
					}
					currentFileValid = false;
					statusGameOutcome("finished without game end", STATUS_NOT_RUN);
				}
				if (currentFileOpen)
				{
					FRESULT closeResult = f_close(&currentFile);
					if (closeResult != FR_OK)
					{
						dbgprintf("Slippi: failed to close dangling file, errno: %d\r\n", closeResult);
						statusGameOutcome("failed to close previous file", closeResult);
						break;
					}
					currentFileOpen = false;
				}

				dbgprintf("Creating File...\r\n");
				gameStartTime = GetCurrentTime();
				char *fileName = generateFileName();
				StatusGame *statusGame = statusGameStart(fileName);
				// Maybe can remove FA_READ since network thread doesn't share &currentFile
				FRESULT fileOpenResult = f_open_secondary_drive(&currentFile, fileName, FA_CREATE_ALWAYS | FA_WRITE | FA_READ);
				statusGame->openResult = fileOpenResult;
				if (fileOpenResult != FR_OK)
				{
					dbgprintf("Slippi: failed to open file: %s, errno: %d\r\n", fileName, fileOpenResult);
					statusGameOutcome("could not create file", fileOpenResult);
					break;
				}

				currentFileOpen = true;
				writtenByteCount = 0;
				
				FRESULT writeHeaderResult = writeHeader(&currentFile);
				if (writeHeaderResult != FR_OK)
				{
					dbgprintf("Slippi: failed to write header, errno: %d\r\n", writeHeaderResult);
					statusGameOutcome("failed to write header", writeHeaderResult);
					break;
				}

				currentFileValid = true;
				statusGameOutcome("recording", STATUS_NOT_RUN);
			}

			if (!currentFileValid)
			{
				// we can reach this state if we SlippiRestoreReadPos into 
				// the middle of a game due to usb insertion or SlpMemError
				// skip over and don't write anything until we see the start of a new game
				if (replaysLED)
					flashLED();
				memReadPos += reader.lastReadResult.bytesRead;
				break;
			}

			// Always seek first in case there was a previous failure with partial write
			FRESULT seekResult = f_lseek(&currentFile, writtenByteCount + 15);
			if (seekResult != FR_OK)
			{
				dbgprintf("Slippi: failed to seek before writing data, errno: %d\r\n", seekResult);
				statusGameOutcome("seek failed (retrying)", seekResult);
				break;
			}

			UINT wrote;
			FRESULT writeResult = f_write(&currentFile, readBuf, reader.lastReadResult.bytesRead, &wrote);
			if (writeResult != FR_OK)
			{
				dbgprintf("Slippi: failed to write data, errno: %d\r\n", writeResult);
				statusGameOutcome("write failed (retrying)", writeResult);
				break;
			}
			else
			{
				// Only increment mem read position when the write fully succeeds
				memReadPos += wrote;
				writtenByteCount += wrote;

				StatusGame *statusGame = statusCurrentGame();
				if (statusGame != NULL)
				{
					statusGame->bytesWritten = writtenByteCount;
					statusGame->outcome = "recording";
				}

				if (reader.lastReadResult.isGameEnd)
				{
					dbgprintf("Completing File...\r\n");
					lastFrame = reader.metadata.lastFrame;
					FRESULT completeResult = completeFile(&currentFile, lastFrame, writtenByteCount);
					if (completeResult != FR_OK)
					{
						// error is logged in completeFile
						statusGameOutcome("failed to write footer (retrying)", completeResult);
						break;
					}

					currentFileValid = false;
					FRESULT closeResult = f_close(&currentFile);
					if (closeResult != FR_OK)
					{
						dbgprintf("Slippi: failed to close completed file, errno: %d\r\n", closeResult);
						statusGameOutcome("failed to close file", closeResult);
					}
					else
					{
						currentFileOpen = false;
						statusGameOutcome("completed", STATUS_NOT_RUN);
						if (replaysLED)
							flashLED();
					}

					break;
				}
				else if (replaysLED)
					flashLED();
			}
		}
	}

	return 0;
}
