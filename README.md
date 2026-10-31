# pi286 – emulatore PC 286 @ 25 MHz bare metal per Raspberry Pi 3

Nessun sistema operativo: `kernel8.img` parte direttamente dal firmware del Pi e fa girare un PC AT 286 emulato.

## Cosa emula
| Componente | Stato |
|---|---|
| CPU 80286 | modo **reale** completo (incl. PUSHA/POPA, ENTER/LEAVE, BOUND, IMUL imm, shift imm, INS/OUTS). **Modo protetto NON implementato** (niente HIMEM/DOS-extender/Win 3.x standard). Nessun 287. |
| Velocità | 25 MHz, sincronizzata al timer reale; regolabile con `CYC_PER_INSN` in `emu.h` |
| RAM | 640K + 15 MB estesa (INT 15h/87h, A20 via porta 92h / 8042) |
| BIOS | emulato ad alto livello: INT 08/09/10/11/12/13/15/16/18/19/1A |
| Floppy A: | file `FLOPPY.IMG` (160K…2.88M, geometria dalla dimensione) |
| HDD C: | file `HDD.IMG` (immagine raw, fino a 384 MB; CHS dedotta dalla tabella partizioni; scritture rimandate alla SD se il file è contiguo) |
| VGA | testo 80x25, **mode 13h 320x200x256**, **mode X** (unchained, map mask, latch copy), DAC 256 colori, 3DA retrace |
| Audio | **Sound Blaster 2.0** (A220 I5 D1): DSP, DMA 8 bit single/auto-init, high-speed, time constant; **AdLib/OPL2 con sintesi FM** (9 canali, 4 forme d onda, ADSR, feedback, tremolo/vibrato; senza KSL/KSR e ritmica); speaker PC. Uscita sul jack 3.5 mm (PWM 44.1 kHz) |
| Altro | PIC 8259, PIT 8254, DMA 8237, CMOS, tastiera 8042 |

Nel DOS: `SET BLASTER=A220 I5 D1 T2`

## SD card
Prima partizione **FAT32** (quella di boot del Pi) con:
```
bootcode.bin start.elf fixup.dat (dal repo raspberrypi/firmware)
config.txt      (questo)
kernel8.img     (compilato)
FLOPPY.IMG      (opzionale, bootabile)
HDD.IMG         (opzionale; partizione grande abbastanza!)
(VGAFONT.BIN non serve più: il font 8x16 CP437 è incorporato)
```
Nomi **8.3 in maiuscolo nella radice**. Boot: prima il floppy (se ha firma 55AA), altrimenti l'HDD.

## Compilare
```
sudo apt install gcc-aarch64-linux-gnu
make CROSS=aarch64-linux-gnu-
```

## Input / debug
**Tastiera USB** (`usb.c`): driver host DWC2 in DMA/polling, enumerazione dell'hub LAN9514 del Pi 3, split transaction per i device low/full-speed, tastiera HID *boot protocol* (US layout) convertita in scancode AT (set 1, con prefisso E0 per frecce/Ins/Canc/Home/Fine/PgSu/PgGiù). Collegala a una qualsiasi porta USB del Pi. Se non viene trovata, resta attiva la **seriale** (mini UART GPIO14/15, 115200 8N1) come tastiera di ripiego, e la UART stampa sempre il log `usb: ...` dell'enumerazione. Video: HDMI 640x480.

## Limiti noti / da fare
- **Il driver USB è la parte più a rischio** (registri DWC2, timing delle split transaction, FIFO): se non trova la tastiera, leggi il log `usb:` sulla UART e dimmi dove si ferma. Un solo canale, nessun hot-plug, nessun layout non-US, niente LED (Bloc Maiusc) né tastiere con hub integrato.
- **Non è stato provato su hardware** (nessun Pi in questo ambiente): compilazione/sintassi e test del core CPU fatti solo su host x86. Aspettati di dover fare debug (UART stampa gli errori SD/FAT).
- Se i colori sono invertiti: scambia R e B in `fb_set_palette()` (plat.c).
- Nessun modo protetto, nessun EGA planare completo (modi 0Dh/10h/12h), nessun write-mode VGA 2/3, testo grafico INT 10h in mode 13h non disegnato.
- OPL2 approssimato (non cycle-accurate: timbro simile ma non identico a un vero YM3812; modo ritmico a percussioni non emulato), nessun SB Pro stereo/SB16, nessun mouse/seriale/stampante.
- FAT32 solo radice e nomi 8.3; SD con bus 4 bit a 25 MHz; l'HDD è interamente caricato in RAM all'avvio.
- Per attivare il jack audio: GPIO40/41, il Pi 3 lo usa già di default.
