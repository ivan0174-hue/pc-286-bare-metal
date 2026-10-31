// PC286 bare metal frontend for Raspberry Pi 4 (Circle)
#include "kernel.h"
#include "keymap.h"
#include <circle/bcmpropertytags.h>
#include <circle/chargenerator.h>
#include <circle/sound/pwmsoundbasedevice.h>
#include <circle/sound/hdmisoundbasedevice.h>
#include <circle/util.h>
#include <circle/alloc.h>
#include <circle/string.h>

extern "C" {
#include "../core/pc.h"
#include "../core/cpu.h"
#include "../core/vga.h"
#include "../core/ide.h"
}

#define DRIVE		"SD:"
#define FILE_BIOS	DRIVE "/pc286/bios.bin"
#define FILE_VGABIOS	DRIVE "/pc286/vgabios.bin"
#define FILE_DISK	DRIVE "/pc286/c.img"
#define FILE_CONFIG	DRIVE "/pc286/pc286.cfg"

#define DISK_CYL	969
#define DISK_HEADS	9
#define DISK_SPT	34

static const char FromKernel[] = "pc286";

CKernel *CKernel::s_pThis = 0;

// ---------------------------------------------------------------------------
// state shared between the cores

enum TScreenMode { ScreenLoading, ScreenEmu, ScreenMenu, ScreenError };

static volatile int g_ScreenMode = ScreenLoading;
static CBcmFrameBuffer *g_pFrameBuffer = 0;

// simple 80x25 text canvas used for the loading screen and the menu
static u8 g_Canvas[25][80][2];
static volatile int g_CanvasSerial;

static void CanvasClear (u8 attr)
{
	for (int y = 0; y < 25; y++)
		for (int x = 0; x < 80; x++)
		{
			g_Canvas[y][x][0] = ' ';
			g_Canvas[y][x][1] = attr;
		}
}

static void CanvasPrint (int x, int y, u8 attr, const char *s)
{
	while (*s && x < 80)
	{
		if (x >= 0 && y >= 0 && y < 25)
		{
			g_Canvas[y][x][0] = (u8) *s;
			g_Canvas[y][x][1] = attr;
		}
		x++;
		s++;
	}
}

static void CanvasFill (int x, int y, int w, u8 attr)
{
	for (int i = 0; i < w; i++)
		if (x + i >= 0 && x + i < 80)
		{
			g_Canvas[y][x + i][0] = ' ';
			g_Canvas[y][x + i][1] = attr;
		}
}

static const u32 cga_pal[16] = {
	0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
	0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};

static const u8 *s_pFont8x16;

static void FindFont (void)
{
	static const u8 pat[16] = { 0x00, 0x00, 0x10, 0x38, 0x6C, 0xC6, 0xC6, 0xFE,
				    0xC6, 0xC6, 0xC6, 0xC6, 0x00, 0x00, 0x00, 0x00 };
	for (unsigned i = 0x410; vga_rom && i + 16 + 0xBF0 <= 32768; i++)
		if (!memcmp (vga_rom + i, pat, 16))
		{
			s_pFont8x16 = vga_rom + i - 0x410;
			return;
		}
}

static void CanvasRender (VGAFrame *f)
{
	static CCharGenerator Font;
	unsigned cw = Font.GetCharWidth (), ch = Font.GetCharHeight ();
	f->w = 640;
	f->h = 400;
	f->text_mode = 1;
	f->blank = 0;
	for (int i = 0; i < 256; i++)
		f->pal[i] = cga_pal[i & 15];
	const u8 *font = s_pFont8x16;
	for (int row = 0; row < 25; row++)
	{
		for (int col = 0; col < 80; col++)
		{
			u8 c = g_Canvas[row][col][0];
			u8 a = g_Canvas[row][col][1];
			for (unsigned yy = 0; yy < 16; yy++)
			{
				u8 *p = f->pix + (row * 16 + yy) * 640 + col * 8;
				if (font)
				{
					u8 bits = font[c * 16 + yy];
					for (unsigned xx = 0; xx < 8; xx++)
						p[xx] = (bits & (0x80 >> xx)) ? (a & 15) : (a >> 4);
					continue;
				}
				CCharGenerator::TPixelLine line = Font.GetPixelLine ((char) c, yy * ch / 16);
				for (unsigned xx = 0; xx < 8; xx++)
					p[xx] = Font.GetPixel (xx * cw / 8, line) ? (a & 15) : (a >> 4);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// video core (core 1): renders the emulated display and scales it to HDMI

static u16 s_Pal16[256];
static unsigned s_xmap[2048];
static unsigned s_ymap[2048];

static void Blit (VGAFrame *f)
{
	CBcmFrameBuffer *fb = g_pFrameBuffer;
	if (!fb)
		return;
	static int lw = -1, lh = -1;
	unsigned W = fb->GetWidth (), H = fb->GetHeight ();
	unsigned pitch = fb->GetPitch () / 2;
	u16 *base = (u16 *) (uintptr) fb->GetBuffer ();

	// display area: 4:3 inside the framebuffer
	unsigned dh = H;
	unsigned dw = H * 4 / 3;
	if (dw > W)
	{
		dw = W;
		dh = W * 3 / 4;
	}
	unsigned ox = (W - dw) / 2, oy = (H - dh) / 2;

	if (f->w != lw || f->h != lh)
	{
		lw = f->w;
		lh = f->h;
		for (unsigned x = 0; x < dw && x < 2048; x++)
			s_xmap[x] = x * f->w / dw;
		for (unsigned y = 0; y < dh && y < 2048; y++)
			s_ymap[y] = y * f->h / dh;
		for (unsigned y = 0; y < H; y++)
			memset (base + y * pitch, 0, W * 2);
	}
	for (int i = 0; i < 256; i++)
	{
		u32 c = f->pal[i];
		s_Pal16[i] = ((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F);
	}
	for (unsigned y = 0; y < dh; y++)
	{
		const u8 *src = f->pix + s_ymap[y] * f->w;
		u16 *dst = base + (oy + y) * pitch + ox;
		if (f->blank)
		{
			memset (dst, 0, dw * 2);
			continue;
		}
		for (unsigned x = 0; x < dw; x++)
			dst[x] = s_Pal16[src[s_xmap[x]]];
	}
}

void CVideoCore::Run (unsigned nCore)
{
	if (nCore != 1)
	{
		for (;;)
			asm volatile ("wfi");
	}
	static VGAFrame fr;
	fr.pix = new u8[VGA_MAX_W * VGA_MAX_H];
	u64 next = CTimer::GetClockTicks64 ();
	for (;;)
	{
		int mode = g_ScreenMode;
		if (mode == ScreenEmu)
			vga_render (&fr);
		else
			CanvasRender (&fr);
		Blit (&fr);
		next += 1000000 / 60;
		u64 now = CTimer::GetClockTicks64 ();
		if (now < next)
			CTimer::SimpleusDelay ((unsigned) (next - now));
		else if (now > next + 100000)
			next = now;
	}
}

// ---------------------------------------------------------------------------
// keyboard events (filled in interrupt context, consumed by the main loop)

static volatile unsigned s_KeyQueue[256];
static volatile unsigned s_KeyHead, s_KeyTail;
static unsigned char s_LastMods, s_LastKeys[6];

static void QueueKey (unsigned nUsage, boolean bPressed)
{
	unsigned n = (s_KeyHead + 1) & 255;
	if (n == s_KeyTail)
		return;
	s_KeyQueue[s_KeyHead] = nUsage | (bPressed ? 0x1000 : 0);
	s_KeyHead = n;
}

void CKernel::KeyStatusHandlerRaw (unsigned char ucModifiers, const unsigned char RawKeys[6])
{
	// modifiers: usage codes 0xE0..0xE7
	for (int i = 0; i < 8; i++)
	{
		boolean was = !!(s_LastMods & (1 << i)), now = !!(ucModifiers & (1 << i));
		if (was != now)
			QueueKey (0xE0 + i, now);
	}
	s_LastMods = ucModifiers;
	// released keys
	for (int i = 0; i < 6; i++)
	{
		unsigned char k = s_LastKeys[i];
		if (k < 4)
			continue;
		boolean still = FALSE;
		for (int j = 0; j < 6; j++)
			if (RawKeys[j] == k)
				still = TRUE;
		if (!still)
			QueueKey (k, FALSE);
	}
	// pressed keys
	for (int i = 0; i < 6; i++)
	{
		unsigned char k = RawKeys[i];
		if (k < 4)
			continue;
		boolean was = FALSE;
		for (int j = 0; j < 6; j++)
			if (s_LastKeys[j] == k)
				was = TRUE;
		if (!was)
			QueueKey (k, TRUE);
	}
	memcpy (s_LastKeys, RawKeys, 6);
}

void CKernel::KeyboardRemovedHandler (CDevice *pDevice, void *pContext)
{
	s_pThis->m_pKeyboard = 0;
	s_LastMods = 0;
	memset (s_LastKeys, 0, sizeof s_LastKeys);
}

// ---------------------------------------------------------------------------
// menu

static const int cpu_speeds[] = { 6, 8, 10, 12, 16, 20, 25 };
static const int ram_sizes[] = { 640, 1024, 2048, 4096 };
static const char *ram_names[] = { "640 KB", "1 MB", "2 MB", "4 MB" };

enum { MI_CPU, MI_RAM, MI_SB, MI_AUDIO, MI_VOLUME, MI_FASTBOOT, MI_SAVE, MI_RESET, MI_BACK, MI_COUNT };

static int s_MenuSel;
static int s_MenuRam = 4096;
static int s_FastBoot = 1;
static int s_BootTurbo;
static char s_MenuMsg[80];

static int IndexOf (const int *list, int n, int v)
{
	for (int i = 0; i < n; i++)
		if (list[i] == v)
			return i;
	return n - 1;
}

static void MenuDraw (void)
{
	CanvasClear (0x17);
	CanvasFill (0, 0, 80, 0x70);
	CanvasPrint (2, 0, 0x70, "PC286 - Emulatore 80286 bare metal per Raspberry Pi 4");
	CanvasPrint (66, 0, 0x70, "F12: chiudi");

	CanvasPrint (4, 2, 0x1F, "Configurazione");
	const char *labels[MI_COUNT] = {
		"Velocita' CPU", "Memoria RAM", "Sound Blaster 2.0", "Uscita audio",
		"Volume", "Avvio veloce (POST)", "Salva impostazioni", "Riavvia il PC (reset)",
		"Torna al DOS"
	};
	for (int i = 0; i < MI_COUNT; i++)
	{
		int y = 4 + i * 2;
		u8 attr = (i == s_MenuSel) ? 0x70 : 0x17;
		CanvasFill (4, y, 72, attr);
		CanvasPrint (6, y, attr, labels[i]);
		CString val;
		switch (i)
		{
		case MI_CPU:	val.Format ("< %d MHz >", pc_cfg.cpu_mhz); break;
		case MI_RAM:
			val.Format ("< %s >%s", ram_names[IndexOf (ram_sizes, 4, s_MenuRam)],
				    s_MenuRam != pc_cfg.ram_kb ? "  (al riavvio)" : "");
			break;
		case MI_SB:	val = pc_cfg.sb_enabled ? "< Attiva (A220 I7 D1) >" : "< Disattivata >"; break;
		case MI_AUDIO:	val = pc_cfg.audio_out ? "< Jack 3.5 mm >" : "< HDMI >"; break;
		case MI_VOLUME:	val.Format ("< %d >", pc_cfg.volume); break;
		case MI_FASTBOOT: val = s_FastBoot ? "< Si' >" : "< No >"; break;
		default: break;
		}
		CanvasPrint (36, y, attr, val);
	}
	CanvasPrint (4, 23, 0x1E, s_MenuMsg);
	CanvasFill (0, 24, 80, 0x70);
	CanvasPrint (2, 24, 0x70, "Frecce: scegli/modifica   Invio: conferma   Esc o F12: torna al DOS");
}

void CKernel::MenuKey (unsigned nUsage)
{
	s_MenuMsg[0] = 0;
	int dir = 0;
	switch (nUsage)
	{
	case 0x52: s_MenuSel = (s_MenuSel + MI_COUNT - 1) % MI_COUNT; break;	// up
	case 0x51: s_MenuSel = (s_MenuSel + 1) % MI_COUNT; break;		// down
	case 0x50: dir = -1; break;						// left
	case 0x4F: dir = 1; break;						// right
	case 0x28: case 0x58: dir = 2; break;					// enter
	case 0x29: case 0x45:							// esc, F12
		g_ScreenMode = ScreenEmu;
		return;
	}
	if (dir == 0)
	{
		MenuDraw ();
		return;
	}
	int step = dir == 2 ? 1 : dir;
	switch (s_MenuSel)
	{
	case MI_CPU: {
		int i = IndexOf (cpu_speeds, 7, pc_cfg.cpu_mhz) + step;
		if (dir == 2 && i >= 7) i = 0;
		if (i < 0) i = 0;
		if (i > 6) i = 6;
		pc_set_speed (cpu_speeds[i]);
		break; }
	case MI_RAM: {
		int i = IndexOf (ram_sizes, 4, s_MenuRam) + step;
		if (dir == 2 && i >= 4) i = 0;
		if (i < 0) i = 0;
		if (i > 3) i = 3;
		s_MenuRam = ram_sizes[i];
		break; }
	case MI_SB:
		pc_cfg.sb_enabled = !pc_cfg.sb_enabled;
		break;
	case MI_AUDIO:
		StopSound ();
		pc_cfg.audio_out = !pc_cfg.audio_out;
		StartSound ();
		break;
	case MI_VOLUME:
		pc_cfg.volume += step;
		if (dir == 2 && pc_cfg.volume > 10) pc_cfg.volume = 0;
		if (pc_cfg.volume < 0) pc_cfg.volume = 0;
		if (pc_cfg.volume > 10) pc_cfg.volume = 10;
		break;
	case MI_FASTBOOT:
		s_FastBoot = !s_FastBoot;
		break;
	case MI_SAVE:
		if (dir == 2)
		{
			FlushDisk (TRUE);
			strcpy (s_MenuMsg, SaveConfig () ? "Impostazioni salvate su scheda SD."
							 : "ERRORE: impossibile salvare pc286.cfg");
		}
		break;
	case MI_RESET:
		if (dir == 2)
		{
			pc_cfg.ram_kb = s_MenuRam;
			pc_hard_reset ();
			ide_mbr_reads = 0;
			s_BootTurbo = s_FastBoot;
			g_ScreenMode = ScreenEmu;
			return;
		}
		break;
	case MI_BACK:
		if (dir == 2)
		{
			g_ScreenMode = ScreenEmu;
			return;
		}
		break;
	}
	MenuDraw ();
}

// ---------------------------------------------------------------------------

CKernel::CKernel (void)
:	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_CPUThrottle (CPUSpeedMaximum),
	m_USBHCI (&m_Interrupt, &m_Timer, TRUE),
	m_EMMC (&m_Interrupt, &m_Timer, &m_ActLED),
	m_VideoCore (CMemorySystem::Get ()),
	m_pKeyboard (0),
	m_pSound (0),
	m_nSoundOut (-1),
	m_bDiskOpen (FALSE),
	m_pDisk (0),
	m_nDiskSize (0),
	m_nLastDirtyCheck (0),
	m_LastLEDs (0xFF)
{
	s_pThis = this;
	memset (m_PressedKeys, 0, sizeof m_PressedKeys);
}

CKernel::~CKernel (void)
{
	s_pThis = 0;
}

boolean CKernel::Initialize (void)
{
	boolean bOK = TRUE;

	if (bOK) bOK = m_Serial.Initialize (115200);
	if (bOK) bOK = m_Logger.Initialize (&m_Serial);
	if (bOK) bOK = m_Interrupt.Initialize ();
	if (bOK) bOK = m_Timer.Initialize ();

	if (bOK)
	{
		unsigned w = 1280, h = 720;
		CBcmPropertyTags Tags;
		TPropertyTagDisplayDimensions Dim;
		if (Tags.GetTag (PROPTAG_GET_DISPLAY_DIMENSIONS, &Dim, sizeof Dim)
		    && Dim.nWidth >= 640 && Dim.nHeight >= 480)
		{
			w = Dim.nWidth;
			h = Dim.nHeight;
		}
		if (w > 1920 || h > 1080)
		{
			w = 1920;
			h = 1080;
		}
		g_pFrameBuffer = new CBcmFrameBuffer (w, h, 16);
		bOK = g_pFrameBuffer->Initialize ();
		if (!bOK)
		{
			delete g_pFrameBuffer;
			g_pFrameBuffer = new CBcmFrameBuffer (1280, 720, 16);
			bOK = g_pFrameBuffer->Initialize ();
		}
	}

	if (bOK)
	{
		CanvasClear (0x07);
		CanvasPrint (2, 1, 0x0F, "PC286 - avvio in corso...");
		bOK = m_VideoCore.Initialize ();
	}

	if (bOK) bOK = m_EMMC.Initialize ();
	if (bOK) bOK = m_USBHCI.Initialize ();

	return bOK;
}

void CKernel::Fatal (const char *pMsg)
{
	CanvasClear (0x4F);
	CanvasPrint (2, 2, 0x4F, "PC286 - ERRORE");
	CanvasPrint (2, 5, 0x4F, pMsg);
	CanvasPrint (2, 8, 0x4F, "Controlla la cartella pc286 sulla scheda SD e riavvia il Raspberry Pi.");
	g_ScreenMode = ScreenError;
	m_Logger.Write (FromKernel, LogError, "%s", pMsg);
	for (;;)
		m_Timer.MsDelay (1000);
}

boolean CKernel::LoadFile (const char *pName, u8 *pBuffer, unsigned nMax, unsigned *pSize)
{
	FIL File;
	if (f_open (&File, pName, FA_READ | FA_OPEN_EXISTING) != FR_OK)
		return FALSE;
	UINT nRead = 0;
	FRESULT r = f_read (&File, pBuffer, nMax, &nRead);
	f_close (&File);
	if (pSize)
		*pSize = nRead;
	return r == FR_OK;
}

boolean CKernel::LoadDisk (void)
{
	if (f_open (&m_DiskFile, FILE_DISK, FA_READ | FA_WRITE | FA_OPEN_EXISTING) != FR_OK)
		return FALSE;
	m_bDiskOpen = TRUE;
	m_nDiskSize = (unsigned) f_size (&m_DiskFile);
	unsigned nNeed = DISK_CYL * DISK_HEADS * DISK_SPT * 512;
	unsigned nAlloc = m_nDiskSize > nNeed ? m_nDiskSize : nNeed;
	m_pDisk = (u8 *) malloc (nAlloc);
	if (!m_pDisk)
		return FALSE;
	memset (m_pDisk, 0, nAlloc);
	const unsigned CHUNK = 1024 * 1024;
	for (unsigned off = 0; off < m_nDiskSize; off += CHUNK)
	{
		UINT n = 0;
		unsigned want = m_nDiskSize - off < CHUNK ? m_nDiskSize - off : CHUNK;
		if (f_read (&m_DiskFile, m_pDisk + off, want, &n) != FR_OK || n != want)
			return FALSE;
		CString s;
		s.Format ("Caricamento disco C: %3u%%", (unsigned) ((u64) off * 100 / m_nDiskSize));
		CanvasPrint (2, 4, 0x07, s);
	}
	CanvasPrint (2, 4, 0x07, "Caricamento disco C: completato");
	ide_attach (m_pDisk, nNeed, DISK_CYL, DISK_HEADS, DISK_SPT);
	return TRUE;
}

void CKernel::FlushDisk (boolean bForce)
{
	if (!m_bDiskOpen)
		return;
	u32 lba;
	unsigned n = 0;
	boolean any = FALSE;
	while (ide_next_dirty (&lba))
	{
		if ((u64) lba * 512 + 512 > m_nDiskSize)
			continue;
		if (f_lseek (&m_DiskFile, (FSIZE_t) lba * 512) != FR_OK)
			break;
		UINT w;
		f_write (&m_DiskFile, m_pDisk + (size_t) lba * 512, 512, &w);
		any = TRUE;
		if (++n >= 2048 && !bForce)
			break;
	}
	if (any)
		f_sync (&m_DiskFile);
}

void CKernel::LoadConfig (void)
{
	static char buf[1024];
	unsigned n = 0;
	if (!LoadFile (FILE_CONFIG, (u8 *) buf, sizeof buf - 1, &n))
		return;
	buf[n] = 0;
	char *p = buf;
	while (*p)
	{
		char *line = p;
		while (*p && *p != '\n') p++;
		if (*p) *p++ = 0;
		char *eq = line;
		while (*eq && *eq != '=') eq++;
		if (!*eq) continue;
		*eq++ = 0;
		int v = 0;
		boolean isnum = (*eq >= '0' && *eq <= '9');
		for (char *q = eq; *q >= '0' && *q <= '9'; q++) v = v * 10 + (*q - '0');
		if (!strcmp (line, "cpu_mhz") && isnum) pc_cfg.cpu_mhz = cpu_speeds[IndexOf (cpu_speeds, 7, v)];
		else if (!strcmp (line, "ram_kb") && isnum) pc_cfg.ram_kb = ram_sizes[IndexOf (ram_sizes, 4, v)];
		else if (!strcmp (line, "soundblaster") && isnum) pc_cfg.sb_enabled = !!v;
		else if (!strcmp (line, "audio")) pc_cfg.audio_out = (eq[0] == 'j' || eq[0] == 'J');
		else if (!strcmp (line, "volume") && isnum) pc_cfg.volume = v > 10 ? 10 : v;
		else if (!strcmp (line, "fastboot") && isnum) s_FastBoot = !!v;
	}
}

boolean CKernel::SaveConfig (void)
{
	CString s;
	s.Format ("cpu_mhz=%d\nram_kb=%d\nsoundblaster=%d\naudio=%s\nvolume=%d\nfastboot=%d\n",
		  pc_cfg.cpu_mhz, s_MenuRam, pc_cfg.sb_enabled, pc_cfg.audio_out ? "jack" : "hdmi",
		  pc_cfg.volume, s_FastBoot);
	FIL File;
	if (f_open (&File, FILE_CONFIG, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
		return FALSE;
	UINT w;
	FRESULT r = f_write (&File, (const char *) s, s.GetLength (), &w);
	f_close (&File);
	return r == FR_OK;
}

void CKernel::StartSound (void)
{
	if (pc_cfg.audio_out)
		m_pSound = new CPWMSoundBaseDevice (&m_Interrupt, AUDIO_RATE, 2048);
	else
		m_pSound = new CHDMISoundBaseDevice (&m_Interrupt, AUDIO_RATE, 384 * 10);
	if (!m_pSound->AllocateQueue (120))
	{
		delete m_pSound;
		m_pSound = 0;
		return;
	}
	m_pSound->SetWriteFormat (SoundFormatSigned16, 2);
	static s16 silence[2 * 2048];
	memset (silence, 0, sizeof silence);
	m_pSound->Write (silence, sizeof silence);
	if (!m_pSound->Start ())
	{
		delete m_pSound;
		m_pSound = 0;
	}
}

void CKernel::StopSound (void)
{
	if (!m_pSound)
		return;
	m_pSound->Cancel ();
	for (int i = 0; i < 50 && m_pSound->IsActive (); i++)
		m_Timer.MsDelay (5);
	delete m_pSound;
	m_pSound = 0;
}

void CKernel::PumpAudio (void)
{
	static s16 buf[2 * 1024];
	if (!m_pSound)
	{
		while (audio_read (buf, 1024) > 0) {}
		return;
	}
	unsigned avail = m_pSound->GetQueueSizeFrames () - m_pSound->GetQueueFramesAvail ();
	while (avail > 0)
	{
		unsigned want = avail > 1024 ? 1024 : avail;
		int n = audio_read (buf, want);
		if (n <= 0)
			break;
		m_pSound->Write (buf, n * 4);
		avail -= n;
	}
}

void CKernel::SendScan (unsigned nCode, boolean bPressed)
{
	if (!nCode)
		return;
	if (nCode == 0x137)
	{
		// Print Screen
		if (bPressed) { kbd_put_scancode (0xE0); kbd_put_scancode (0x2A); kbd_put_scancode (0xE0); kbd_put_scancode (0x37); }
		else { kbd_put_scancode (0xE0); kbd_put_scancode (0xB7); kbd_put_scancode (0xE0); kbd_put_scancode (0xAA); }
		return;
	}
	if (nCode & 0x100)
		kbd_put_scancode (0xE0);
	kbd_put_scancode ((nCode & 0x7F) | (bPressed ? 0 : 0x80));
}

void CKernel::HandleKey (unsigned nUsage, boolean bPressed)
{
	if (nUsage == 0x45)	// F12: menu
	{
		if (bPressed)
		{
			ReleaseAllKeys ();
			s_MenuRam = s_MenuRam ? s_MenuRam : pc_cfg.ram_kb;
			s_MenuMsg[0] = 0;
			MenuDraw ();
			g_ScreenMode = ScreenMenu;
		}
		return;
	}
	if (nUsage == 0x48)	// Pause
	{
		if (bPressed)
		{
			static const u8 seq[] = { 0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5 };
			for (unsigned i = 0; i < sizeof seq; i++)
				kbd_put_scancode (seq[i]);
		}
		return;
	}
	unsigned code = (nUsage >= 0xE0 && nUsage <= 0xE7) ? hidmod2set1[nUsage - 0xE0]
		      : (nUsage < 256 ? hid2set1[nUsage] : 0);
	if (!code)
		return;
	m_PressedKeys[nUsage & 0xFF] = bPressed;
	SendScan (code, bPressed);
}

void CKernel::ReleaseAllKeys (void)
{
	for (unsigned u = 0; u < 256; u++)
	{
		if (m_PressedKeys[u])
		{
			m_PressedKeys[u] = 0;
			unsigned code = (u >= 0xE0 && u <= 0xE7) ? hidmod2set1[u - 0xE0] : hid2set1[u];
			SendScan (code, FALSE);
		}
	}
}

TShutdownMode CKernel::Run (void)
{
	m_Logger.Write (FromKernel, LogNotice, "PC286 starting");

	if (f_mount (&m_FileSystem, DRIVE, 1) != FR_OK)
		Fatal ("Impossibile leggere la scheda SD (FAT32).");

	bios_rom = (u8 *) malloc (65536);
	vga_rom = (u8 *) malloc (32768);
	memset (vga_rom, 0xFF, 32768);
	unsigned n;
	if (!LoadFile (FILE_BIOS, bios_rom, 65536, &n) || n != 65536)
		Fatal ("File mancante o errato: pc286/bios.bin (BIOS 286 da 64 KB)");
	if (!LoadFile (FILE_VGABIOS, vga_rom, 32768, &n) || n < 16384)
		Fatal ("File mancante o errato: pc286/vgabios.bin (BIOS Diamond Stealth)");
	FindFont ();

	LoadConfig ();
	s_MenuRam = pc_cfg.ram_kb;

	CanvasPrint (2, 3, 0x07, "BIOS caricati.");
	if (!LoadDisk ())
		Fatal ("File mancante o errato: pc286/c.img (disco fisso)");

	if (pc_init () != 0)
		Fatal ("Memoria insufficiente.");
	pc_hard_reset ();
	ide_mbr_reads = 0;
	s_BootTurbo = s_FastBoot;

	StartSound ();
	g_ScreenMode = ScreenEmu;

	u64 t0 = CTimer::GetClockTicks64 ();
	u64 emu_us = 0;
	u64 last_usb = 0, last_flush = 0, led_off = 0;
	int last_mode = ScreenEmu;

	for (;;)
	{
		u64 now = CTimer::GetClockTicks64 ();

		// USB plug and play (keyboard)
		if (now - last_usb > 100000)
		{
			last_usb = now;
			if (m_USBHCI.UpdatePlugAndPlay () && m_pKeyboard == 0)
			{
				CUSBKeyboardDevice *pKbd =
					(CUSBKeyboardDevice *) m_DeviceNameService.GetDevice ("ukbd1", FALSE);
				if (pKbd)
				{
					pKbd->RegisterRemovedHandler (KeyboardRemovedHandler);
					pKbd->RegisterKeyStatusHandlerRaw (KeyStatusHandlerRaw);
					m_pKeyboard = pKbd;
					m_LastLEDs = 0xFF;
				}
			}
			// mirror the guest keyboard LEDs
			if (m_pKeyboard && kbd_leds != m_LastLEDs)
			{
				m_LastLEDs = kbd_leds;
				u8 usb = 0;
				if (kbd_leds & 2) usb |= LED_NUM_LOCK;
				if (kbd_leds & 4) usb |= LED_CAPS_LOCK;
				if (kbd_leds & 1) usb |= LED_SCROLL_LOCK;
				m_pKeyboard->SetLEDs (usb);
			}
		}

		// keyboard events
		while (s_KeyTail != s_KeyHead)
		{
			unsigned ev = s_KeyQueue[s_KeyTail];
			s_KeyTail = (s_KeyTail + 1) & 255;
			if (g_ScreenMode == ScreenMenu)
			{
				if (ev & 0x1000)
					MenuKey (ev & 0xFFF);
			}
			else
			{
				HandleKey (ev & 0xFFF, !!(ev & 0x1000));
			}
		}

		if (g_ScreenMode == ScreenMenu)
		{
			last_mode = ScreenMenu;
			PumpAudio ();
			m_Timer.MsDelay (5);
			continue;
		}
		if (last_mode == ScreenMenu)
		{
			// resynchronise emulated time after the pause
			last_mode = ScreenEmu;
			t0 = CTimer::GetClockTicks64 () - emu_us;
			now = CTimer::GetClockTicks64 ();
		}

		boolean turbo = s_BootTurbo && ide_mbr_reads == 0;
		u64 real_us = now - t0;
		if (!turbo)
		{
			if (s_BootTurbo)
			{
				// boot sector reached: back to real time
				s_BootTurbo = 0;
				t0 = now - emu_us;
				real_us = emu_us;
			}
			if (emu_us > real_us + 1000)
			{
				unsigned wait = (unsigned) (emu_us - real_us - 500);
				if (wait > 2000) wait = 2000;
				CTimer::SimpleusDelay (wait);
				PumpAudio ();
				continue;
			}
			if (real_us > emu_us + 100000)
				emu_us = real_us - 100000;	// too slow: skip ahead
		}

		pc_run_us (1000);
		emu_us += 1000;
		if (turbo)
		{
			t0 = CTimer::GetClockTicks64 () - emu_us;
			static s16 trash[2 * 512];
			while (audio_read (trash, 512) > 0) {}
		}
		else
		{
			PumpAudio ();
		}

		// disk activity LED and write-back
		if (ide_activity)
		{
			ide_activity = 0;
			m_ActLED.On ();
			led_off = now + 50000;
		}
		else if (led_off && now > led_off)
		{
			m_ActLED.Off ();
			led_off = 0;
		}
		if (now - last_flush > 2000000)
		{
			last_flush = now;
			FlushDisk (FALSE);
		}
	}

	return ShutdownHalt;
}
