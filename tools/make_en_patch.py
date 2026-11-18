#!/usr/bin/env python3
"""Make assets/english.p1patch: the English overlay as a patch against the player's own disc.

data/en (built by tools/install_english.py from the fan translation and YOUR disc) cannot be
published, because the rebuilt files contain the game's own data. This patch stores, for each
overlay file, only what is new (the English text, rebuilt tables, the font) plus instructions to
copy everything else from the original file on the disc. The runtime (runtime/enpatch.c) applies
it to the installed disc files, so English works without Python or the translation repository.

Format (little endian):
  "P1EN" u32 version=1 u32 file_count, then per file:
    u16 name_len, name          overlay path, e.g. "ADV/E0.BIN" (relative to data/en)
    u16 src_len, src            disc file it is built from ("" = none, the file is all new data)
    u32 src_size, u32 src_crc32 the disc file the patch was made against
    u32 dst_size, u32 dst_crc32 the result
    u32 op_count, then ops:     u8 0 = COPY u32 src_offset u32 length
                                u8 1 = DATA u32 length, bytes
usage: make_en_patch.py [data/cd] [data/en] [assets/english.p1patch]
"""
import os
import struct
import sys
import zlib

BLOCK = 32
STEP = 4


def find_src(cd, rel):
    """the disc file with the same path, ignoring case"""
    cur = cd
    for part in rel.split('/'):
        if not os.path.isdir(cur):
            return None
        names = {n.upper(): n for n in os.listdir(cur)}
        if part.upper() not in names:
            return None
        cur = os.path.join(cur, names[part.upper()])
    return cur if os.path.isfile(cur) else None


def diff(src, dst):
    """COPY/DATA ops rebuilding dst from src (greedy block matching, extended both ways)"""
    index = {}
    for o in range(0, len(src) - BLOCK + 1, STEP):
        index.setdefault(src[o:o + BLOCK], o)
    ops, lit, i = [], bytearray(), 0
    n = len(dst)
    while i < n:
        o = index.get(dst[i:i + BLOCK]) if i + BLOCK <= n else None
        if o is None:
            lit.append(dst[i])
            i += 1
            continue
        # extend backwards into pending literals, then forwards
        back = 0
        while back < len(lit) and o - back - 1 >= 0 and src[o - back - 1] == lit[-1 - back]:
            back += 1
        if back:
            del lit[len(lit) - back:]
        if lit:
            ops.append((1, bytes(lit)))
            lit = bytearray()
        start_src, start_dst = o - back, i - back
        j = i + BLOCK
        k = o + BLOCK
        while j < n and k < len(src) and dst[j] == src[k]:
            j += 1
            k += 1
        ops.append((0, start_src, j - start_dst))
        i = j
    if lit:
        ops.append((1, bytes(lit)))
    return ops


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    cd = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, 'data', 'cd')
    en = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, 'data', 'en')
    out = sys.argv[3] if len(sys.argv) > 3 else os.path.join(root, 'assets', 'english.p1patch')
    files = []
    for dp, _, fns in os.walk(en):
        for fn in sorted(fns):
            rel = os.path.relpath(os.path.join(dp, fn), en).replace('\\', '/')
            if rel.endswith('.asm') or rel == 'patch.id':
                continue
            files.append(rel)
    blob = bytearray(b'P1EN' + struct.pack('<II', 1, len(files)))
    total_data = 0
    for rel in sorted(files):
        dst = open(os.path.join(en, rel), 'rb').read()
        srcp = find_src(cd, rel)
        src = open(srcp, 'rb').read() if srcp else b''
        ops = diff(src, dst) if src else [(1, dst)]
        srcrel = os.path.relpath(srcp, cd).replace('\\', '/') if srcp else ''
        blob += struct.pack('<H', len(rel)) + rel.encode() + struct.pack('<H', len(srcrel)) + srcrel.encode()
        blob += struct.pack('<IIIII', len(src), zlib.crc32(src), len(dst), zlib.crc32(dst), len(ops))
        data = 0
        for op in ops:
            if op[0] == 0:
                blob += struct.pack('<BII', 0, op[1], op[2])
            else:
                blob += struct.pack('<BI', 1, len(op[1])) + op[1]
                data += len(op[1])
        total_data += data
        print(f'{rel}: {len(ops)} ops, {data} new bytes of {len(dst)}')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, 'wb').write(blob)
    print(f'{out}: {len(blob)} bytes ({total_data} new)')


if __name__ == '__main__':
    main()
