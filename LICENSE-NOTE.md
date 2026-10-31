# Licenze e materiale di terzi

- Il codice di questo progetto non ha ancora una licenza scelta dall'autore:
  aggiungi un file LICENSE (es. MIT o GPL) se vuoi permetterne il riuso.
- `core/opl3.c`, `core/opl3.h`: Nuked OPL3 di Nuke.YKT, LGPL-2.1-or-later.
- Circle (scaricato da `scripts/build_pi.sh`): GPL-3.0. Il kernel binario che si
  ottiene collegandolo va quindi distribuito secondo la GPL-3.0.
- MS-DOS 4.00 (scaricato e compilato da `scripts/build_dos.sh`): licenza MIT,
  https://github.com/microsoft/MS-DOS
- Firmware Raspberry Pi (scaricato da `scripts/make_sd.sh`): licenza Broadcom
  redistribuibile, vedi LICENCE.broadcom.
- **ROM non incluse**: il BIOS Quadtel 286 e il BIOS AMI/Diamond Stealth VRAM
  sono protetti da copyright e non sono nel repository. Usa i dump delle tue schede.
- `test/`: la suite SingleStepTests 80286 (https://github.com/SingleStepTests/80286)
  va scaricata a parte.
