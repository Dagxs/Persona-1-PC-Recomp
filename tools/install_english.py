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
    US localization staff, shown by new roll script steps) are left out; the logo sheet's names use
    the crop table Persona.exe keeps (0x5d81f8)."""
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
    # the logo sheet's crop table (END.EXE 0x8009A08C on the PS1) is at 0x5d81f8 in Persona.exe
    crops = bytes(img.mem[0x5d81f8 - img.base:0x5d81f8 - img.base + 32])
    logo = crops[:8] == bytes.fromhex('0000680068000000')
    if logo:
        endtext.logo_crops = lambda: [(crops[i], crops[8 + i], crops[16 + i], crops[24 + i]) for i in range(8)
                                      if crops[16 + i] and crops[24 + i]]
    endtext.load_pages = lambda: {k: v for k, v in pages().items()
                                  if k < endtext.END_SIZE and (logo or v[0] != 'l')}
    endtext.roll_asm = lambda new_pages: ('', 0)
    endtext.main()
    os.makedirs(os.path.join(out, 'EXE'), exist_ok=True)
    shutil.copyfile(os.path.join(repo, 'build', 'EXE', 'END.BIN'), os.path.join(out, 'EXE', 'END.BIN'))


# The PS1 main executable's data tables (items, skills, Personas, demons, nicknames) are in
# Persona.exe's data section in the same order and spacing: PC address = PS1 address + DELTA.
SLPS_BASE = 0x8000F800                  # PS1 SLPS_005.00 file offset = address - SLPS_BASE
DELTA = 0x5ce258 - 0x8004442C           # the first item name: PS1 0x8004442C, PC 0x5ce258


def fake_slps(repo, exe):
    """data/psx/SLPS_005.00 for the repo's tools, rebuilt from Persona.exe's copy of the PS1 data
    tables (the code differs and is left zero; the tools only read the tables)."""
    sys.path.insert(0, HERE)
    from image import Image
    img = Image(exe)
    lo, hi = 0x80040000, 0x80050000
    d = bytearray(hi - SLPS_BASE)
    d[0:8] = b'PS-X EXE'
    struct.pack_into('<I', d, 0x18, 0x80010000)
    for va in range(lo, hi):
        pc = va + DELTA - img.base
        if 0 <= pc < len(img.mem):
            d[va - SLPS_BASE] = img.mem[pc]
    # the description pointer table holds PC addresses: back to PS1 ones
    for va in range(0x8004AA7C, 0x8004B0CC, 4):
        w = struct.unpack_from('<I', d, va - SLPS_BASE)[0]
        if 0x5ce258 <= w < 0x5ce258 + 0x10000:
            struct.pack_into('<I', d, va - SLPS_BASE, (w - DELTA) & 0xffffffff)
    path =os.path.join(repo, 'data', 'psx', 'SLPS_005.00')
    open(path, 'wb').write(bytes(d))
    return img


def names(repo, out, img):
    """names.bin: English item, skill, Persona, demon and nickname names, written into the game's
    tables at startup (runtime/english.c). They are drawn by the original small-font renderer: one
    byte per glyph, ten glyphs per field (longer names are cut, as the PS1 patch does for items),
    lowercase through the Latin cells drawn into every copy of the font page (small_font()).
    Format: "P1NM", u32 count, then {u32 address, u16 length, bytes}."""
    import toolbelt
    import slps
    import personas
    import demons
    def enc(t, n=10, end=True):
        b = bytearray()
        for c in t:
            try:
                b.append(toolbelt.smallfont_char(c))
            except SystemExit:
                pass                        # a character the small font lacks
        b = bytes(b[:n])
        if len(b) < n:
            b += (bytes([0xff]) if end else b'') + bytes(n - len(b) - (1 if end else 0))
        return b
    def check(addr, n):
        a = addr - img.base
        return 0 <= a and a + n <= len(img.mem)
    patches = []
    full = []                               # (PC address, ten-glyph head, the whole name) for sf_names.bin
    def put(va, b, t=None):
        if check(va + DELTA, len(b)):
            patches.append((va + DELTA, b))
            if t is not None:
                full.append((va + DELTA, b, enc(t, n=64, end=False).rstrip(b'\0')))
    rows = toolbelt.merged_rows('SLPS_005.00')
    it0, sk0, nk0 = slps.IT_FIRST - SLPS_BASE, slps.SK_FIRST - SLPS_BASE, slps.NK_FIRST - SLPS_BASE
    for r in rows:
        t = (r.get('en') or '').strip()
        if not t:
            continue
        pos = int(r['pos'], 16)
        if it0 <= pos < it0 + slps.IT_STRIDE * slps.IT_COUNT and (pos - it0) % slps.IT_STRIDE == 0:
            put(pos + SLPS_BASE, enc(t), t)
        elif sk0 <= pos < sk0 + slps.SK_STRIDE * slps.SK_COUNT and (pos - sk0) % slps.SK_STRIDE == 0:
            put(pos + SLPS_BASE, enc(t), t)
        elif nk0 <= pos < nk0 + slps.NK_STRIDE * slps.NK_COUNT and (pos - nk0) % slps.NK_STRIDE == 0:
            put(pos + SLPS_BASE, enc(t[:slps.NK_MAX]))
    pen = personas.record_en()
    for k, va, _ja in personas.records():
        if pen.get(k):
            put(va + personas.NAME, enc(pen[k]), pen[k])     # the name sits 8 bytes into the record
    den = demons.record_en()
    for k, va, _ja in demons.records():
        if den.get(k):
            put(va, enc(den[k]), den[k])
    # name_map.bin: the glyph code of each small-font character as it reaches the dialogue and
    # battle text engines (names copied into a message: bytes 80-FF become two-byte codes of the
    # same value) -> its ASCII, so English names are drawn at the English font's widths there
    # (lowercase sits on rare hiragana codes, which the 16 px font would draw as kana)
    nmap = bytearray(256)
    import string
    for ch in string.ascii_letters + string.digits + " .,!'-/:":
        try:
            code = toolbelt.smallfont_char(ch)
        except SystemExit:
            continue
        if code < 256 and not nmap[code]:
            nmap[code] = ord(ch)
    open(os.path.join(out, 'name_map.bin'), 'wb').write(bytes(nmap))
    patches += fixed_tables(repo, img)
    patches += descriptions(repo, img)
    patches += menu_text(img, 'cells')
    blob = bytearray(b'P1NM') + struct.pack('<I', len(patches))
    for a, b in patches:
        blob += struct.pack('<IH', a, len(b)) + b
    open(os.path.join(out, 'names.bin'), 'wb').write(bytes(blob))
    print(f'names: {len(patches)} names')
    sf_names(repo, out, full)


def sf_names(repo, out, full):
    """sf_names.bin: the menus' proportional small font (runtime/vwf_menu.c). The menus write a
    name's ten-glyph head into a background map (TileMapWriteRow); the runtime finds the whole
    name - by the head's address in the name tables, or by its bytes when only one name has that
    head - and draws it proportionally, squeezed when it is wider than the field.
    Format: "P1SF", 215 glyph metrics (left column << 4 | width, from the shipped font page; the
    shadow column counts), u32 count, then {u32 address, 10-byte head, u8 length, bytes}."""
    import tim
    import menu_gfx
    d = open(os.path.join(repo, 'build', 'ADV', 'ADVCMD.BIN'), 'rb').read()
    t = tim.parse(d, menu_gfx.PAGE)
    page = d[t['at']:t['at'] + 128 * 256]
    pix = bytearray()
    for b in page:
        pix += bytes((b & 15, b >> 4))
    metrics = bytearray()
    for g in range(215):
        r, c = divmod(g, 31)
        xs = [x for y in range(12) for x in range(8) if pix[(r * 12 + y) * 256 + c * 8 + x]]
        metrics.append(min(xs) << 4 | (max(xs) + 1 - min(xs)) if xs else 0)
    seen = {}
    for a, head, whole in full:
        seen.setdefault(bytes(head), set()).add(bytes(whole))
    blob = bytearray(b'P1SF') + metrics + struct.pack('<I', len(full))
    for a, head, whole in full:
        blob += struct.pack('<I', a) + bytes(head) + bytes([len(whole)]) + bytes(whole)
    open(os.path.join(out, 'sf_names.bin'), 'wb').write(bytes(blob))
    shared = sum(1 for v in seen.values() if len(v) > 1)
    print(f'sf_names: {len(full)} names, {shared} heads shared by different names')


def map_titles(repo, cd, out):
    """map_titles.bin: dungeon map titles longer than the 10-cell field (the PS1 patch's
    dng/map_title.asm). The map packs keep those floors' Japanese; MapDrawName (0x4079e0) looks the
    field's 20 bytes up and shows the full English (runtime/vwf_field.c).
    Format: "P1MT", u32 count, then {20-byte field, u16 width px, u16 length, English bytes + FF 01}."""
    import glob
    import dngnames
    import vwf_wrap
    import toolbelt
    table = {}
    for f in sorted(glob.glob(os.path.join(cd, 'D0?', 'D*M.BIN'))):
        rel = os.path.relpath(f, cd).replace(os.sep, '/')
        orig = open(f, 'rb').read()
        built_path = os.path.join(out, rel)
        built = open(built_path, 'rb').read() if os.path.exists(built_path) else orig
        try:
            rows = toolbelt.merged_rows(rel[:-4])
        except SystemExit:
            continue
        for r in rows:
            en = (r.get('en') or '').strip()
            if len(en) <= dngnames.SLOTS:
                continue                    # fits the field itself
            pos = int(r['pos'], 16)
            key = orig[pos:pos + 20]
            if len(key) != 20 or built[pos:pos + 20] != key:
                continue                    # the pack's field was rewritten: not ours to match
            try:
                blob = dngnames.vwf_bytes(en)
            except SystemExit:
                continue
            px = vwf_wrap.text_width(en)
            if len(blob) - 2 > 0x20 or px > 0xA0:
                continue
            table.setdefault(key, (px, blob))
    out_b = bytearray(b'P1MT') + struct.pack('<I', len(table))
    for key, (px, blob) in sorted(table.items()):
        out_b += key + struct.pack('<HH', px, len(blob)) + blob
    open(os.path.join(out, 'map_titles.bin'), 'wb').write(bytes(out_b))
    print(f'map titles: {len(table)} long titles')


def talk(repo, cd, out):
    """Demon conversations: TALK/*.BIN are the PS1 files (loaded to PS1 RAM 0x801C0000, FONT.BIN
    at 0x801E0000 above them, so a file may grow to 128 KB). The repo rebuilds them in English
    (talk.py; etctalk.py for ETC.BIN); the battle text engine (runtime/vwf_btl.c) draws them."""
    import glob
    import subprocess
    src = os.path.join(repo, 'data', 'psx', 'TALK')
    os.makedirs(src, exist_ok=True)
    for f in glob.glob(os.path.join(cd, 'TALK', '*.BIN')):
        shutil.copyfile(f, os.path.join(src, os.path.basename(f)))
    dst = os.path.join(repo, 'build', 'TALK')
    tools = os.path.join(repo, 'src', 'injection_tools')
    for cmd in (['talk.py', 'build', dst], ['etctalk.py', 'build', dst]):
        r = subprocess.run([sys.executable, os.path.join(tools, cmd[0])] + cmd[1:], cwd=repo,
                           capture_output=True, text=True, encoding='utf-8', errors='replace')
        if r.returncode:
            print(f'talk: {cmd[0]} failed; demon conversations stay Japanese\n{r.stderr[-800:]}')
            return
    os.makedirs(os.path.join(out, 'TALK'), exist_ok=True)
    n = 0
    for f in glob.glob(os.path.join(dst, '*.BIN')):
        if os.path.getsize(f) > 0x20000:
            print(f'talk: {os.path.basename(f)} is over 128 KB; it stays Japanese')
            continue
        shutil.copyfile(f, os.path.join(out, 'TALK', os.path.basename(f)))
        n += 1
    print(f'talk: {n} files')


def battle_text(repo, out, img):
    """btl_text.bin: English battle messages, negotiation lines, story-battle dialogue and action
    names, matched to their strings in Persona.exe (tools/btl_map.py). The runtime hands the
    battle text system the English when it starts reading one of those strings (vwf_btl.c).
    Format: "P1BT", u32 count, then {u32 PC address, u32 length, bytes}."""
    import toolbelt
    import etext
    import vwf_wrap
    import btl_map
    rows = {int(r['pos'], 16): r for r in toolbelt.merged_rows('BTLP')}
    manual = {r['pos']: r for r in toolbelt.read_manual('BTLP')}
    data = [(a - img.base, e - img.base) for (nm, a, e, ch) in img.sections
            if not ch & 0x20000000 and nm.startswith('.data')]
    # the PS1 overlay (BTLP.BIN of the first release), when the developer has it in the repo's
    # data/psx: exact matches; without it, slot-size matching alone
    psx_path = os.path.join(repo, 'data', 'psx', 'BTLP.BIN')
    psx = open(psx_path, 'rb').read() if os.path.exists(psx_path) else None
    found = btl_map.build(img.mem, img.base, data, rows, manual, etext, vwf_wrap, psx=psx)
    blob = bytearray(b'P1BT') + struct.pack('<I', len(found))
    for a in sorted(found):
        blob += struct.pack('<II', a, len(found[a])) + found[a]
    open(os.path.join(out, 'btl_text.bin'), 'wb').write(bytes(blob))


def menu_text(img, kind):
    """tools/menu_text.tsv: this project's English for menu text the translation leaves in
    Japanese. kind 'msg' -> {PC address: English bytes} for msg_text.bin; kind 'cells' ->
    [(PC address, bytes)] in-place small-font labels for names.bin."""
    import etext
    import toolbelt
    mem = bytes(img.mem)
    msgs, cells = {}, []
    for line in open(os.path.join(HERE, 'menu_text.tsv'), encoding='utf-8'):
        if line.startswith('#') or not line.strip():
            continue
        k, ja, en = line.rstrip(chr(10)).split(chr(9))
        jb = bytes.fromhex(ja[4:]) if ja.startswith('hex:') else etext.encode(ja, end=None)
        hits = []
        i = mem.find(jb)
        while i >= 0:
            # in place: only a string start, never another string's ending (a message redirect
            # only takes effect where a read starts, so it may keep every copy)
            if k == 'msg' or mem[i - 1] == 0 or mem[i - 2] == 0xff:
                hits.append(i)
            i = mem.find(jb, i + 1)
        if not hits:
            print(f'menu text: not in Persona.exe: {ja}')
        if k == 'msg':
            eb = etext.encode(en, end=None)
            for i in hits:
                msgs[img.base + i] = eb
        elif k == 'inplace':                # a 16 px string other code copies: English in its bytes
            eb = etext.encode(en, end=None)
            if len(eb) > len(jb):
                sys.exit(f'menu text: {en!r} does not fit in place of {ja}')
            cells += [(img.base + i, eb) for i in hits]
        else:
            n = int(k.split(':')[1])
            eb = bytes(toolbelt.smallfont_char(c) for c in en)
            if len(eb) > n:
                sys.exit(f'menu text: {en!r} is longer than {n} cells')
            eb += bytes(n - len(eb))
            cells += [(img.base + i, eb) for i in hits]
    return msgs if kind == 'msg' else cells

def msg_text(repo, out, img):
    """msg_text.bin: English for the messages the PS1 overlays ADV, DNG and S2D keep in their own
    data (the town's remarks, "There seems to be nothing here.", save and Persona prompts), which
    the Windows port compiled into Persona.exe. Each row's Japanese is read from the first
    release's overlay (the developer's data/psx; skipped without it) and found byte for byte in
    the exe; the dialogue interpreters read the English instead when they start one of those
    strings (runtime/msg_text.c). Format: "P1MS", u32 count, then {u32 PC address, u32 length,
    bytes}."""
    import toolbelt
    import etext
    found, groups = {}, {}
    for name in ('ADV', 'DNG', 'S2D'):
        path = os.path.join(repo, 'data', 'psx', name + '.BIN')
        if not os.path.exists(path):
            print(f'msg text: no {path}; the {name} messages stay Japanese')
            continue
        psx = open(path, 'rb').read()
        for r in toolbelt.merged_rows(name):
            en = (r.get('en') or '').strip()
            if not en.endswith('<FF01>'):
                continue                    # fixed-size labels (the race tables: fixed_tables())
            p = int(r['pos'], 16)
            e = psx.find(bytes([0xff, 0x01]), p, p + 800)
            if e < 0:
                continue
            groups.setdefault(psx[p:e + 2], set()).add(en)
    mem = bytes(img.mem)
    for ja, ens in groups.items():
        if len(ens) != 1:
            continue
        try:
            b = etext.encode(ens.pop(), end=None)
        except Exception:
            continue
        i = mem.find(ja)
        while i >= 0:
            if mem[i - 1] == 0 or mem[i - 2] == 0xff:      # a string start
                found[img.base + i] = b
            i = mem.find(ja, i + 1)
    found.update(menu_text(img, 'msg'))
    blob = bytearray(b'P1MS') + struct.pack('<I', len(found))
    for a in sorted(found):
        blob += struct.pack('<II', a, len(found[a])) + found[a]
    open(os.path.join(out, 'msg_text.bin'), 'wb').write(bytes(blob))
    print(f'msg text: {len(found)} strings in Persona.exe for {len(groups)} messages')

def fixed_tables(repo, img):
    """Fixed-slot small-font tables of the PS1 overlays, found in Persona.exe by their Japanese
    (read from the first release's overlays in the repo's data/psx; skipped without them):
    the 26 race names (6 bytes each, one copy per field overlay), the battle ANALYZE race tags
    (27 x 8 bytes) and the AUTO battle modes (3 x 10 bytes). Every PC copy gets the English."""
    import demons
    import skills
    import btlui
    psx = os.path.join(repo, 'data', 'psx')
    out = []
    def every(orig, new, what):
        hits, i = [], img.mem.find(orig)
        while i >= 0:
            hits.append(img.base + i)
            i = img.mem.find(orig, i + 1)
        for a in hits:
            out.append((a, new))
        print(f'{what}: {len(hits)} cop{"y" if len(hits) == 1 else "ies"} in Persona.exe')
    adv = os.path.join(psx, 'ADV.BIN')
    if os.path.exists(adv):
        a = demons.RACE_TABLES['ADV'] - demons.OVL_BASE
        orig = open(adv, 'rb').read()[a:a + demons.RACE_COUNT * demons.RACE_LEN]
        new = bytearray(orig)
        ren = demons.race_en()
        for i, _ja in demons.races():
            t = ren.get(i + 1, '')
            if t and len(t) <= demons.RACE_LEN:
                b = skills.encode(i, t)
                new[i * demons.RACE_LEN:(i + 1) * demons.RACE_LEN] = b + bytes(demons.RACE_LEN - len(b))
        every(orig, bytes(new), 'race names')
    btlp = os.path.join(psx, 'BTLP.BIN')
    if os.path.exists(btlp):
        d = open(btlp, 'rb').read()
        B = btlui.B
        races = btlui.psp_races()
        a0 = 0x800E2068
        orig = d[a0 - B:a0 - B + 8 * len(races)]
        new = bytearray(orig)
        for k, name in enumerate(races, 1):
            b = btlui.sf(name) + bytes([0xCB, 0xFF])
            if len(b) <= 8:
                new[(k - 1) * 8:k * 8] = b + bytes(8 - len(b))
        every(orig, bytes(new), 'battle race tags')
        auto = btlui.AUTO
        lo = min(a for a, _ in auto)
        orig = d[lo - B:lo - B + 10 * len(auto)]
        new = bytearray(orig)
        for a, t in auto:
            b = btlui.sf(t) + b'\xff'
            if len(b) <= 10:
                new[a - lo:a - lo + 10] = b + bytes(10 - len(b))
        every(orig, bytes(new), 'AUTO modes')
    return out


def descriptions(repo, img):
    """Skill and item descriptions (the one-line help of the menus): the repo's descs tool re-packs
    the English into the PS1 string zone and rewrites the pointer table; Persona.exe holds the same
    zone and a table of PC pointers (all four readers, the three dialogue copies and the battle
    text system, draw English). Returns name-patch entries {PC address, bytes}."""
    import descs
    descs.build()
    z0, z1 = descs.ZONE
    t0, t1 = descs.TABLE
    zone = bytearray(z1 - z0)
    table = bytearray(t1 - t0)
    org = None
    for line in open(descs.OUT, encoding='utf-8'):
        w = line.split(None, 1)
        if not w or w[0].startswith(';'):
            continue
        args = [int(v.strip(), 0) for v in w[1].split(',')] if len(w) > 1 else []
        if w[0] == '.org':
            org = args[0]
        elif w[0] == '.fill':
            org += args[0]
        elif w[0] == '.byte':
            zone[org - z0:org - z0 + len(args)] = bytes(args)
            org += len(args)
        elif w[0] == '.word':
            assert t0 <= org < t1 and z0 <= args[0] < z1, hex(org)
            struct.pack_into('<I', table, org - t0, (args[0] + DELTA) & 0xffffffff)
            org += 4
    # the PC table must be the PS1 one moved by DELTA, or the layout is not the expected one
    old = struct.unpack_from('<I', bytes(img.mem), t0 + DELTA - img.base)[0]
    if old != z0 + DELTA:
        print('descriptions: Persona.exe does not have the expected table; they stay Japanese')
        return []
    return [(z0 + DELTA, bytes(zone)), (t0 + DELTA, bytes(table))]


def small_font(repo, cd, out):
    """Latin letters in the small font: the repo draws a..z (and a few punctuation marks) over rare
    hiragana cells in every copy of the small-font page - map packs (2D/, D0?/), the menu files
    (ADV/ADVCMD*.BIN, OPEN.BIN) and the battle field data (B/BF.BIN, compressed) - with its small-
    font VWF kit off: the original renderer then prints English from the name tables."""
    import glob
    import features
    features.on = lambda g: {'SF_VWF': False, 'SF_LATIN': True}.get(g, False)
    rels = ['ADV/ADVCMD.BIN', 'ADV/ADVCMD0.BIN', 'OPEN.BIN', 'B/BF.BIN']
    rels += [os.path.relpath(p, cd) for p in glob.glob(os.path.join(cd, '2D', '*.BIN'))]
    for rel in rels:
        dst = os.path.join(repo, 'data', 'psx', rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(os.path.join(cd, rel), dst)
    import smallfont_pages
    import smallfont_vwf
    import btlfont
    import menu_gfx
    smallfont_pages.main()                  # 2D/ and D0?/ (on top of the English dungeon packs)
    btlfont.main()
    page = smallfont_pages.reference()
    pix = smallfont_vwf.page_indices()[0]
    prior = smallfont_vwf.page_prior()
    ref = smallfont_vwf.unpack_page(page)
    for rel in ('ADV/ADVCMD.BIN', 'ADV/ADVCMD0.BIN', 'OPEN.BIN'):
        b = bytearray(open(os.path.join(repo, 'data', 'psx', rel), 'rb').read())
        for at, bpr in smallfont_pages.find_pages(bytes(b), page):
            old = bytes(b[at:at + bpr * smallfont_pages.ROWS])
            b[at:at + len(old)] = smallfont_vwf.page_copy(smallfont_vwf.unpack_page(old), ref, pix, bpr * 2,
                                                          smallfont_pages.ROWS, prior)
            menu_gfx.label_rows(b, at, bpr, page)  # the stat labels (攻撃 -> ATK...) as in the map packs
        os.makedirs(os.path.dirname(os.path.join(repo, 'build', rel)), exist_ok=True)
        open(os.path.join(repo, 'build', rel), 'wb').write(bytes(b))
    # every changed file goes to the overlay
    import filecmp
    n = 0
    for rel in rels + [os.path.relpath(p, cd) for p in glob.glob(os.path.join(cd, 'D0?', '*.BIN'))]:
        src = os.path.join(repo, 'build', rel)
        if os.path.exists(src) and not filecmp.cmp(src, os.path.join(cd, rel), shallow=False):
            os.makedirs(os.path.dirname(os.path.join(out, rel)), exist_ok=True)
            shutil.copyfile(src, os.path.join(out, rel))
            n += 1
    print(f'small font: Latin cells in {n} files')


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
    map_titles(repo, cd, out)
    talk(repo, cd, out)
    ending(repo, cd, out, os.path.join(os.path.dirname(cd), 'Persona.exe'))
    exe = os.path.join(os.path.dirname(cd), 'Persona.exe')
    if os.path.exists(exe):
        img = fake_slps(repo, exe)
        small_font(repo, cd, out)
        names(repo, out, img)
        battle_text(repo, out, img)
        msg_text(repo, out, img)

    # movie subtitles (the runtime draws them over the movies)
    subs = os.path.join(repo, 'translation', 'manual', 'fmv', 'subtitles.tsv')
    if os.path.exists(subs):
        shutil.copyfile(subs, os.path.join(out, 'subtitles.tsv'))
    poems = os.path.join(repo, 'translation', 'manual', 'fmv', 'poems.tsv')   # the ending poems
    if os.path.exists(poems):
        shutil.copyfile(poems, os.path.join(out, 'poems.tsv'))
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
