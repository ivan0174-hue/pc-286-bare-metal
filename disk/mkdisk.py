#!/usr/bin/env python3
"""Build a bootable MS-DOS 4.00 hard disk image (BIOS type 46: 969/9/34)."""
import os, struct, subprocess, sys, shutil, tempfile

CYL, HEADS, SPT = 969, 9, 34
TOTAL = CYL * HEADS * SPT
START = SPT                      # cylinder 0, head 1, sector 1
PSECT = TOTAL - START

def chs(lba):
    c = lba // (HEADS * SPT)
    h = (lba // SPT) % HEADS
    s = lba % SPT + 1
    return bytes([h, s | ((c >> 8) << 6), c & 0xFF])

def main(out, dosdir, extra):
    here = os.path.dirname(os.path.abspath(__file__))
    mbr = open(os.path.join(here, 'mbr.bin'), 'rb').read()
    boot = open(os.path.join(dosdir, 'BOOT', 'MSBOOT.BIN'), 'rb').read()[0x7C00:0x7E00]
    tmp = tempfile.mkdtemp()
    bfile = os.path.join(tmp, 'boot.bin')
    open(bfile, 'wb').write(boot)
    part = os.path.join(tmp, 'part.img')
    with open(part, 'wb') as f:
        f.truncate(PSECT * 512)
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1', MTOOLS_NO_VFAT='1')
    subprocess.check_call(['mformat', '-i', part, '-t', str(CYL), '-h', str(HEADS), '-s', str(SPT),
                           '-H', str(START), '-c', '8', '-r', '32', '-m', '0xF8', '-B', bfile,
                           '-N', '28601989', '::'], env=env)
    # patch BPB details expected by the DOS 4 boot code
    with open(part, 'r+b') as f:
        bs = bytearray(f.read(512))
        bs[0:3] = boot[0:3]
        bs[3:11] = b'MSDOS4.0'
        struct.pack_into('<H', bs, 0x18, SPT)
        struct.pack_into('<H', bs, 0x1A, HEADS)
        struct.pack_into('<I', bs, 0x1C, START)
        struct.pack_into('<H', bs, 0x13, 0)
        struct.pack_into('<I', bs, 0x20, PSECT)
        bs[0x24] = 0x80
        bs[0x26] = 0x29
        bs[0x36:0x3E] = b'FAT16   '
        bs[0x3E:0x1FE] = boot[0x3E:0x1FE]
        bs[0x1FE:0x200] = b'\x55\xAA'
        f.seek(0)
        f.write(bs)

    def mcopy(src, dst, attrs=None):
        subprocess.check_call(['mcopy', '-o', '-i', part, src, '::' + dst], env=env)
        if attrs:
            subprocess.check_call(['mattrib', '-i', part] + attrs + ['::' + dst], env=env)

    # IO.SYS and MSDOS.SYS must be the first two directory entries, contiguous
    mcopy(os.path.join(dosdir, 'BIOS', 'IO.SYS'), 'IO.SYS', ['+h', '+s', '+r'])
    mcopy(os.path.join(dosdir, 'DOS', 'MSDOS.SYS'), 'MSDOS.SYS', ['+h', '+s', '+r'])
    mcopy(os.path.join(dosdir, 'CMD', 'COMMAND', 'COMMAND.COM'), 'COMMAND.COM')
    subprocess.check_call(['mlabel', '-i', part, '::PC286'], env=env)
    for src, dst in extra:
        if dst.endswith('/'):
            subprocess.call(['mmd', '-i', part, '::' + dst.rstrip('/')], env=env,
                            stderr=subprocess.DEVNULL)
            for fn in sorted(os.listdir(src)):
                mcopy(os.path.join(src, fn), dst + fn)
        else:
            mcopy(src, dst)

    with open(out, 'wb') as f:
        mbrsec = bytearray(512)
        mbrsec[0:len(mbr)] = mbr
        entry = bytes([0x80]) + chs(START) + bytes([0x06]) + chs(TOTAL - 1) + \
            struct.pack('<II', START, PSECT)
        mbrsec[446:462] = entry
        mbrsec[510:512] = b'\x55\xAA'
        f.write(mbrsec)
        f.write(b'\0' * 512 * (START - 1))
        with open(part, 'rb') as p:
            shutil.copyfileobj(p, f, 1 << 20)
    shutil.rmtree(tmp)
    print('wrote', out, TOTAL * 512 // (1024 * 1024), 'MB')

if __name__ == '__main__':
    out, dosdir = sys.argv[1], sys.argv[2]
    extra = []
    for a in sys.argv[3:]:
        s, d = a.split('=', 1)
        extra.append((s, d))
    main(out, dosdir, extra)
