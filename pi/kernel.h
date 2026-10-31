// PC286 bare metal frontend for Raspberry Pi 4 (Circle)
#ifndef _kernel_h
#define _kernel_h

#include <circle/actled.h>
#include <circle/koptions.h>
#include <circle/devicenameservice.h>
#include <circle/serial.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include <circle/cputhrottle.h>
#include <circle/memory.h>
#include <circle/multicore.h>
#include <circle/bcmframebuffer.h>
#include <circle/usb/usbhcidevice.h>
#include <circle/usb/usbkeyboard.h>
#include <circle/sound/soundbasedevice.h>
#include <SDCard/emmc.h>
#include <fatfs/ff.h>
#include <circle/types.h>

enum TShutdownMode
{
	ShutdownNone,
	ShutdownHalt,
	ShutdownReboot
};

class CVideoCore : public CMultiCoreSupport
{
public:
	CVideoCore (CMemorySystem *pMemorySystem) : CMultiCoreSupport (pMemorySystem) {}
	void Run (unsigned nCore);
};

class CKernel
{
public:
	CKernel (void);
	~CKernel (void);

	boolean Initialize (void);
	TShutdownMode Run (void);

private:
	static void KeyStatusHandlerRaw (unsigned char ucModifiers, const unsigned char RawKeys[6]);
	static void KeyboardRemovedHandler (CDevice *pDevice, void *pContext);

	boolean LoadFile (const char *pName, u8 *pBuffer, unsigned nMax, unsigned *pSize);
	boolean LoadDisk (void);
	void FlushDisk (boolean bForce);
	void LoadConfig (void);
	boolean SaveConfig (void);
	void StartSound (void);
	void StopSound (void);
	void PumpAudio (void);
	void HandleKey (unsigned nUsage, boolean bPressed);
	void MenuKey (unsigned nUsage);
	void SendScan (unsigned nCode, boolean bPressed);
	void ReleaseAllKeys (void);
	void Fatal (const char *pMsg);

private:
	CActLED			m_ActLED;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CSerialDevice		m_Serial;
	CExceptionHandler	m_ExceptionHandler;
	CInterruptSystem	m_Interrupt;
	CTimer			m_Timer;
	CLogger			m_Logger;
	CCPUThrottle		m_CPUThrottle;
	CUSBHCIDevice		m_USBHCI;
	CEMMCDevice		m_EMMC;
	FATFS			m_FileSystem;
	CVideoCore		m_VideoCore;

	CUSBKeyboardDevice * volatile m_pKeyboard;
	CSoundBaseDevice	*m_pSound;
	int			m_nSoundOut;

	FIL			m_DiskFile;
	boolean			m_bDiskOpen;
	u8			*m_pDisk;
	unsigned		m_nDiskSize;
	unsigned		m_nLastDirtyCheck;

	u8			m_PressedKeys[256];
	u8			m_LastLEDs;

	static CKernel *s_pThis;
};

#endif
