#!/usr/bin/env python3
"""Interleave the even (L) and odd (H) 32 KB EPROM dumps into a 64 KB BIOS image."""
import sys
lo = open(sys.argv[1], 'rb').read()
hi = open(sys.argv[2], 'rb').read()
out = bytearray()
for a, b in zip(lo, hi):
    out += bytes([a, b])
open(sys.argv[3], 'wb').write(out)
print(sys.argv[3], len(out), 'bytes')
