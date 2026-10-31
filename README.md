# PC286 – emulatore PC/AT 80286 bare metal per Raspberry Pi 4

Emulatore di un PC AT 286 che gira **senza sistema operativo** sul Raspberry Pi 4
(libreria [Circle](https://github.com/rsta2/circle), AArch64). Si accende
direttamente nel BIOS del PC emulato e avvia **MS-DOS 4.00 compilato dai sorgenti
pubblicati da Microsoft** (licenza MIT).

## Macchina emulata

| Componente | Dettagli |
|---|---|
| CPU | 80286, modo reale e protetto (descrittori, gate, task switch, LOADALL), da 6 a 25 MHz |
| RAM | 640 KB, 1, 2 o 4 MB (A20 tramite 8042) |
| Chipset | 8259 ×2, 8254, 8042, MC146818 (CMOS/RTC), 8237 ×2 |
| BIOS | Quadtel Enhanced 286 v3.04.02 (**ROM non incluse**) |
| Video | Diamond Stealth VRAM (S3 86C911, 1 MB): VGA, SVGA, VESA con banking (**ROM non inclusa**) |
| Disco | controller AT/IDE, disco da 145 MB (tipo BIOS 46: 969/9/34) |
| Audio | Sound Blaster 2.0 (DSP 2.01, A220 I7 D1) + OPL2 ([Nuked-OPL3](https://github.com/nukeykt/Nuked-OPL3)), PC speaker |
| Uscite | HDMI (immagine 4:3 scalata), audio HDMI o jack 3,5 mm, tastiera USB |

Menu con **F12**: velocità CPU, RAM, Sound Blaster, uscita audio, volume,
avvio veloce del POST, salvataggio impostazioni, reset.

## Struttura

```
core/    emulatore portabile in C (cpu, pc, vga+S3, ide, sb, opl3)
pi/      frontend bare metal Circle: video su core 1, menu, USB, audio, SD
host/    harness Linux headless per i test (screenshot PPM, WAV, tasti)
test/    runner per la suite SingleStepTests 80286 (130.400 test, 99,8% ok)
disk/    MBR, creazione immagine disco, programmi di prova SBTEST/GFXTEST
dos/     configurazioni DOSBox per compilare MS-DOS 4.00
sd/      config.txt e LEGGIMI.TXT per la scheda SD
scripts/ script di build
```

## Compilazione (Linux, Ubuntu 24.04)

```sh
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu mtools nasm dosbox python3 curl git
scripts/build_dos.sh      # compila MS-DOS 4.00 dai sorgenti Microsoft (in DOSBox)
scripts/build_disk.sh     # crea build/c.img con DOS installato
scripts/build_pi.sh       # scarica Circle e compila pi/kernel8-rpi4.img
# metti le tue ROM in roms/: QUADT89L.ROM, QUADT89H.ROM, vgabios.bin
scripts/make_sd.sh        # prepara build/sd: copiane il contenuto su una SD FAT32
```

Test sul PC: `make -f Makefile.host`, poi
`./pc286 -bios bios.bin -vga vgabios.bin -hd build/c.img -chs 969,9,34 -t 20 -shot s.ppm`.

## Stato

Collaudato in emulazione (harness Linux e QEMU `raspi3b` con una build Circle per
Pi 3): POST completo, DOS 4.00, tastiera USB, menu, Sound Blaster, modi grafici
13h/12h/VESA 101h. **Non ancora provato su un Raspberry Pi 4 reale.**

Vedi `sd/LEGGIMI.TXT` per l'uso e le limitazioni.
