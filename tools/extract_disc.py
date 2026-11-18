#!/usr/bin/env python3
"""Extract the Persona (Windows, 1999) disc into a folder.

Accepts a .cue, a raw .bin (2352-byte MODE1 sectors) or a plain .iso (2048-byte sectors).
usage: extract_disc.py DISC OUTDIR      (also copies acc/persona/Program/Persona.exe to OUTDIR/../)
"""
import os
import re
import struct
import sys


def open_disc(path):
    if path.lower().endswith('.cue'):
        txt = open(path, encoding='latin1').read()
        m = re.search(r'FILE\s+"([^"]+)"', txt)
        binpath = os.path.join(os.path.dirname(path), m.group(1))
        if not os.path.exists(binpath):           # cue sheets often disagree on case
            for f in os.listdir(os.path.dirname(path) or '.'):
                if f.lower() == m.group(1).lower():
                    binpath = os.path.join(os.path.dirname(path), f)
        path = binpath
    f = open(path, 'rb')
    size = os.path.getsize(path)
    if size % 2352 == 0 and f.read(12) == b'\x00' + b'\xff' * 10 + b'\x00':
        return f, 2352, 16
    return f, 2048, 0


def main():
    disc, out = sys.argv[1], sys.argv[2]
    f, ss, off = open_disc(disc)

    def sector(n, count=1):
        buf = bytearray()
        for k in range(count):
            f.seek((n + k) * ss + off)
            buf += f.read(2048)
        return bytes(buf)

    pvd = sector(16)
    if pvd[1:6] != b'CD001':
        sys.exit('not an ISO 9660 disc')
    total = [0]

    def walk(lba, size, dst):
        os.makedirs(dst, exist_ok=True)
        data = sector(lba, (size + 2047) // 2048)
        i = 0
        while i < len(data):
            n = data[i]
            if n == 0:
                i = (i // 2048 + 1) * 2048
                continue
            rec = data[i:i + n]
            i += n
            elba, esz = struct.unpack_from('<I', rec, 2)[0], struct.unpack_from('<I', rec, 10)[0]
            flags, nl = rec[25], rec[32]
            name = rec[33:33 + nl]
            if name in (b'\x00', b'\x01'):
                continue
            name = name.decode('latin1').split(';')[0].rstrip('.')
            p = os.path.join(dst, name)
            if flags & 2:
                walk(elba, esz, p)
            else:
                with open(p, 'wb') as o:
                    left, s = esz, elba
                    while left > 0:
                        chunk = sector(s, min(256, (left + 2047) // 2048))
                        o.write(chunk[:left])
                        left -= len(chunk)
                        s += 256
                total[0] += esz

    root = pvd[156:190]
    walk(struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0], out)
    print('extracted %.1f MB to %s' % (total[0] / 1e6, out))
    exe = os.path.join(out, 'acc', 'persona', 'Program', 'Persona.exe')
    if os.path.exists(exe):
        dst = os.path.join(os.path.dirname(os.path.abspath(out)), 'Persona.exe')
        open(dst, 'wb').write(open(exe, 'rb').read())
        print('copied Persona.exe to', dst)


if __name__ == '__main__':
    main()
