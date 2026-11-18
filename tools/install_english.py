#!/usr/bin/env python3
"""Build the English data overlay (data/en/) from the PS1 fan translation.

Source: https://github.com/ayla6/megami-ibunroku-persona-translation (MIT), a port of the PSP
script to the PS1 game. The Windows port keeps the PS1 data formats, so its own text tools
apply to the Windows files directly:
  * ADV/E0-E3.BIN  - the story scripts, rebuilt with the English boxes by the repo's etext.py
                     from YOUR disc's files (data/cd/ADV), so no game data comes from elsewhere
  * vwf_font.bin   - the repo's variable-width English font (16 rows x 8 px per glyph + widths)
The runtime (runtime/english.c, runtime/vwf.c) reads these when Language = English.

usage: install_english.py [path/to/translation-repo] [data/cd] [data/en]
"""
import os
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def dungeon(repo, cd, out):
    """The dungeon packs D0?/D??.BIN (the field script's boxes, built in place by the repo's
    dngpacks.py) and dng_spill.bin: the boxes whose English does not fit in place, matched at
    runtime by the 24 bytes at the box head (runtime/vwf_field.c; the PS1 patch's dng/spill.asm).
    Map-name fields only take the plain glyph-cell form: the longer pre-swapped VWF form needs the
    PS1 patch's map-title hook, which the runtime does not have."""
    import glob
    import filecmp
    for src in glob.glob(os.path.join(cd, 'D0?', 'D*.BIN')):
        rel = os.path.relpath(src, cd)
        dst = os.path.join(repo, 'data', 'psx', rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)
    import vwf_wrap
    decomp = os.environ.get('P1_DECOMP', os.path.join(ROOT, '_decomp'))
    if os.path.isdir(decomp):
        vwf_wrap.DECOMP_BTLP = os.path.join(decomp, 'src', 'btlp')
    import dngpacks
    import dngspill
    dngpacks.SCRIPT_MAX = 0
    dngpacks.build()
    n = 0
    for src in glob.glob(os.path.join(repo, 'build', 'D0?', 'D*.BIN')):
        rel = os.path.relpath(src, os.path.join(repo, 'build'))
        orig = os.path.join(cd, rel)
        if os.path.exists(orig) and not filecmp.cmp(src, orig, shallow=False):
            os.makedirs(os.path.dirname(os.path.join(out, rel)), exist_ok=True)
            shutil.copyfile(src, os.path.join(out, rel))
            n += 1
    dngspill.main()
    # the generated assembly -> P1DS: magic, u32 entries, u32 texts; entries {24-byte prefix,
    # u32 text index}; texts {u32 length, bytes}
    entries, texts, cur = [], {}, None
    for line in open(dngspill.OUT, encoding='utf-8'):
        line = line.split(';')[0].strip()
        if line.startswith('.word') and 'dng_spill_t' in line:
            parts = [p.strip() for p in line[5:].split(',')]
            entries.append((struct.pack('<6I', *(int(p, 16) for p in parts[:6])), int(parts[6][len('dng_spill_t'):])))
        elif line.startswith('dng_spill_t') and line.endswith(':') and line[11:-1].isdigit():
            cur = int(line[len('dng_spill_t'):-1])
            texts[cur] = bytearray()
        elif line.startswith('.byte') and cur is not None:
            texts[cur] += bytes(int(p, 16) for p in line[5:].split(','))
        elif line.startswith('dng_spill_end'):
            cur = None
    blob = bytearray(b'P1DS') + struct.pack('<II', len(entries), len(texts))
    for pre, t in entries:
        blob += pre + struct.pack('<I', t)
    for k in range(len(texts)):
        blob += struct.pack('<I', len(texts[k])) + texts[k]
    with open(os.path.join(out, 'dng_spill.bin'), 'wb') as f:
        f.write(blob)
    print(f'dungeon: {n} pack files, {len(entries)} spill entries, {len(texts)} spill texts')


def ending(repo, cd, out, exe):
    """EXE/END.BIN: the ending's epilogue and staff-roll pages, redrawn in English by the repo's
    endtext.py (in place, same size). It reads the page sector table from the PS1 END.EXE; the
    Windows port keeps the same table in Persona.exe (0x5d7802). The PS1 patch's extra pages (the
    US localization staff, shown by new roll script steps) and the logo sheet's names (which need
    END.EXE's crop tables) are left out."""
    src = os.path.join(cd, 'EXE', 'END.BIN')
    if not os.path.exists(src) or not os.path.exists(exe):
        return
    os.makedirs(os.path.join(repo, 'data', 'psx', 'EXE'), exist_ok=True)
    shutil.copyfile(src, os.path.join(repo, 'data', 'psx', 'EXE', 'END.BIN'))
    sys.path.insert(0, HERE)
    from image import Image
    import endtext
    img = Image(exe)
    table = list(struct.unpack_from('<86H', img.mem, 0x5d7802 - img.base))
    if table[0] != 0 or any(table[i] >= table[i + 1] for i in range(85)):
        print('ending: page table not found in Persona.exe; the ending stays Japanese')
        return
    endtext.end_sectors = lambda image: table
    endtext.end_exe = lambda: b''
    pages = endtext.load_pages
    endtext.load_pages = lambda: {k: v for k, v in pages().items() if k < endtext.END_SIZE and v[0] != 'l'}
    endtext.roll_asm = lambda new_pages: ('', 0)
    endtext.main()
    os.makedirs(os.path.join(out, 'EXE'), exist_ok=True)
    shutil.copyfile(os.path.join(repo, 'build', 'EXE', 'END.BIN'), os.path.join(out, 'EXE', 'END.BIN'))


def main():
    repo = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'third_party', 'persona-translation'))
    cd = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, 'data', 'cd'))
    out = os.path.abspath(sys.argv[3] if len(sys.argv) > 3 else os.path.join(ROOT, 'data', 'en'))
    tools = os.path.join(repo, 'src', 'injection_tools')
    if not os.path.isdir(tools):
        sys.exit(f'{repo} does not look like the translation repository (no src/injection_tools)')
    sys.path.insert(0, tools)
    os.chdir(repo)

    # the repo's tools read the original files from <repo>/data/psx: give them this disc's
    psx_adv = os.path.join(repo, 'data', 'psx', 'ADV')
    os.makedirs(psx_adv, exist_ok=True)
    for n in ('E0', 'E1', 'E2', 'E3'):
        shutil.copyfile(os.path.join(cd, 'ADV', f'{n}.BIN'), os.path.join(psx_adv, f'{n}.BIN'))

    import etext
    import make_vwf_font

    os.makedirs(os.path.join(out, 'ADV'), exist_ok=True)
    etext.cmd_build(os.path.join(repo, 'translation', 'manual', 'events'), os.path.join(out, 'ADV'),
                    asm=os.path.join(out, 'event_tables.asm'))

    # font: magic, first code, glyph count, then per glyph: advance (px) and 16 rows (MSB = left)
    glyphs = make_vwf_font.load_sheet()
    _, _, _, widths = make_vwf_font.build()
    first, top = make_vwf_font.FIRST, make_vwf_font.TOP
    blob = bytearray(b'P1VF') + struct.pack('<BH', first, top - first + 1)
    for c in range(first, top + 1):
        blob.append(widths[c - first])
        blob += bytes(glyphs.get(c, [0] * 16))
    with open(os.path.join(out, 'vwf_font.bin'), 'wb') as f:
        f.write(blob)

    dungeon(repo, cd, out)
    ending(repo, cd, out, os.path.join(os.path.dirname(cd), 'Persona.exe'))

    # movie subtitles (the runtime draws them over the movies)
    subs = os.path.join(repo, 'translation', 'manual', 'fmv', 'subtitles.tsv')
    if os.path.exists(subs):
        shutil.copyfile(subs, os.path.join(out, 'subtitles.tsv'))
    quote = os.path.join(repo, 'translation', 'manual', 'fmv', 'opening.tsv')   # the opening movie's quote
    if os.path.exists(quote):
        shutil.copyfile(quote, os.path.join(out, 'opening.tsv'))

    for name in ('LICENSE', 'contributors.md'):
        src = os.path.join(repo, name)
        if os.path.exists(src):
            shutil.copyfile(src, os.path.join(out, 'TRANSLATION_' + name))
    print(f'English data written to {out}: script files, font ({top - first + 1} glyphs)')


if __name__ == '__main__':
    main()
