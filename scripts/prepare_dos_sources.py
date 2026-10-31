#!/usr/bin/env python3
"""Copy MS-DOS 4.0 sources (microsoft/MS-DOS v4.0/src) into a build dir and fix
them so they build with the original tools: CP437 encoding, CRLF line endings,
include/lib paths in SETENV.BAT."""
import os, shutil, sys
src, dst = sys.argv[1], sys.argv[2]
if os.path.exists(dst):
    shutil.rmtree(dst)
shutil.copytree(src, dst)
binext = {'EXE', 'LIB', 'OBJ', 'CPI', 'SYS', 'COM', 'BIN', 'DAT'}
for root, _, files in os.walk(dst):
    for f in files:
        p = os.path.join(root, f)
        ext = f.rsplit('.', 1)[-1].upper() if '.' in f else ''
        b = open(p, 'rb').read()
        if ext in binext or b'\x00' in b:
            continue
        try:
            t = b.decode('utf-8')
            if any(ord(c) > 127 for c in t):
                b = t.encode('cp437', errors='replace')
        except UnicodeDecodeError:
            pass
        b = b.replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')
        open(p, 'wb').write(b)
p = os.path.join(dst, 'SETENV.BAT')
s = open(p, 'rb').read().decode('latin-1')
lines = []
for l in s.split('\r\n'):
    if l.lower().startswith('set lib='):
        l = 'set LIB=%BAKROOT%\\src\\tools\\bld\\lib'
    elif l.lower().startswith('set include='):
        l = 'set INCLUDE=%BAKROOT%\\src\\tools\\bld\\inc'
    lines.append(l)
open(p, 'wb').write('\r\n'.join(lines).encode('latin-1'))
print('sources ready in', dst)
