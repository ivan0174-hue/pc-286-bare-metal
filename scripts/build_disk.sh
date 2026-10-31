#!/bin/sh
# Creates build/c.img (145 MB, BIOS type 46) with MS-DOS 4.00 installed.
# Requires: mtools, nasm, python3 and a completed scripts/build_dos.sh
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=$ROOT/build/dos/SRC
ST=$ROOT/build/stage
rm -rf $ST && mkdir -p $ST/DOS $ST/UTIL
nasm -f bin -o $ROOT/disk/mbr.bin $ROOT/disk/mbr.asm
nasm -f bin -o $ST/UTIL/SBTEST.COM $ROOT/disk/tools/sbtest.asm
nasm -f bin -o $ST/UTIL/GFXTEST.COM $ROOT/disk/tools/gfxtest.asm
for f in $(tr -d '\r' < $SRC/CPY.BAT | grep -i copy | awk '{print $2}' | sed 's#\\#/#g; s#^\./##' | tr a-z A-Z); do
  case "$f" in BIOS/IO.SYS|DOS/MSDOS.SYS|CMD/COMMAND/COMMAND.COM|SELECT/*) continue;; esac
  [ -f "$SRC/$f" ] && cp "$SRC/$f" $ST/DOS/ || true
done
python3 $ROOT/disk/mkdisk.py $ROOT/build/c.img $SRC \
  $ROOT/disk/stage/root/CONFIG.SYS=CONFIG.SYS $ROOT/disk/stage/root/AUTOEXEC.BAT=AUTOEXEC.BAT \
  $ST/DOS=DOS/ $ST/UTIL=UTIL/
MTOOLS_SKIP_CHECK=1 mmd -i $ROOT/build/c.img@@17408 ::TEMP
MTOOLS_SKIP_CHECK=1 mmd -i $ROOT/build/c.img@@17408 ::GIOCHI
