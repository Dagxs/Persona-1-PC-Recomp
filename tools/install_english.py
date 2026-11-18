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


def main():
    repo = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, '_tl'))
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

    for name in ('LICENSE', 'contributors.md'):
        src = os.path.join(repo, name)
        if os.path.exists(src):
            shutil.copyfile(src, os.path.join(out, 'TRANSLATION_' + name))
    print(f'English data written to {out}: script files, font ({top - first + 1} glyphs)')


if __name__ == '__main__':
    main()
