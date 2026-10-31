#!/bin/sh
# Assembles the SD card contents in build/sd.
# The ROMs are NOT in this repository: put your own dumps in roms/
#   roms/QUADT89L.ROM, roms/QUADT89H.ROM   (Quadtel 286 BIOS, even/odd 32 KB)
#   roms/vgabios.bin                       (Diamond Stealth VRAM BIOS v2.31)
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SD=$ROOT/build/sd
FW=https://raw.githubusercontent.com/raspberrypi/firmware/0641c5bf25d185d5bf25d8dcafd1da6dc8fdbf68/boot
rm -rf $SD && mkdir -p $SD/pc286
for f in start4.elf fixup4.dat bcm2711-rpi-4-b.dtb bcm2711-rpi-400.dtb LICENCE.broadcom; do
  curl -sSfL -o $SD/$f $FW/$f
done
make -C $ROOT/circle/boot armstub64
cp $ROOT/circle/boot/armstub8-rpi4.bin $ROOT/pi/kernel8-rpi4.img $ROOT/sd/config.txt $ROOT/sd/LEGGIMI.TXT $SD/
python3 $ROOT/scripts/join_bios.py $ROOT/roms/QUADT89L.ROM $ROOT/roms/QUADT89H.ROM $SD/pc286/bios.bin
cp $ROOT/roms/vgabios.bin $SD/pc286/vgabios.bin
cp $ROOT/build/c.img $SD/pc286/c.img
printf 'cpu_mhz=25\nram_kb=4096\nsoundblaster=1\naudio=hdmi\nvolume=8\nfastboot=1\n' > $SD/pc286/pc286.cfg
echo "Copia il contenuto di $SD nella radice di una SD FAT32."
