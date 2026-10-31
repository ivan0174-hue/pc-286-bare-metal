#!/bin/sh
# Builds pi/kernel8-rpi4.img with Circle (64 bit, Raspberry Pi 4, multicore).
# Requires: gcc-aarch64-linux-gnu, g++-aarch64-linux-gnu, git, make
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
C=$ROOT/circle
if [ ! -d $C ]; then
  git clone https://github.com/rsta2/circle.git $C
  git -C $C checkout 7a81e1b480fe7d19fc1fc695b89bff0a0848f44b
fi
cp $ROOT/scripts/circle-Config.mk $C/Config.mk
(cd $C && ./makeall -j4 && make -C addon/SDCard -j4 && make -C addon/fatfs -j4)
make -C $ROOT/pi -j4
ls -la $ROOT/pi/kernel8-rpi4.img
