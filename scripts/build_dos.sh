#!/bin/sh
# Builds MS-DOS 4.00 from the Microsoft sources inside DOSBox (headless).
# Requires: git, python3, dosbox.  Output: build/dos/SRC/...
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=$ROOT/build/dos
mkdir -p $ROOT/build
[ -d $ROOT/build/MS-DOS ] || git clone --depth 1 https://github.com/microsoft/MS-DOS.git $ROOT/build/MS-DOS
python3 $ROOT/scripts/prepare_dos_sources.py $ROOT/build/MS-DOS/v4.0/src $B/SRC
for c in build build2; do
  sed "s#@DOSBUILD@#$B#" $ROOT/dos/$c.conf > $B/$c.conf
done
# The full build stops in DEV\DISPLAY (not needed): run it with a timeout,
# then build the remaining drivers separately.
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 1800 dosbox -conf $B/build.conf -noconsole >/dev/null 2>&1 || true
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 900 dosbox -conf $B/build2.conf -noconsole >/dev/null 2>&1 || true
ls -la $B/SRC/BIOS/IO.SYS $B/SRC/DOS/MSDOS.SYS $B/SRC/CMD/COMMAND/COMMAND.COM
