#!/usr/bin/env python3
"""Convert the runtime's frame dumps (32-bit top-down BMP) to PNG. usage: bmp2png.py in.bmp... """
import struct
import sys
import zlib


def convert(src, dst):
    d = open(src, 'rb').read()
    w, h = struct.unpack_from('<ii', d, 18)
    h = abs(h)
    px = d[54:]
    rows = []
    for y in range(h):
        row = px[y * w * 4:(y + 1) * w * 4]
        rgb = bytearray(w * 3)
        rgb[0::3] = row[2::4]
        rgb[1::3] = row[1::4]
        rgb[2::3] = row[0::4]
        rows.append(b'\x00' + bytes(rgb))

    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))
    open(dst, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                          chunk(b'IDAT', zlib.compress(b''.join(rows), 6)) + chunk(b'IEND', b''))


for f in sys.argv[1:]:
    convert(f, f[:-4] + '.png')
