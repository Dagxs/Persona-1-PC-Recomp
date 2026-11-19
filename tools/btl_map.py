"""English for the battle text strings (the PS1 BTLP overlay's messages, negotiation lines,
story-battle dialogue, action names) on the PC.

The translation's BTLP rows are keyed by their offset in the PS1 overlay file, which the PC disc
does not have; Persona.exe has the same strings, but the compiler laid them out on its own (in
blocks, with its own alignment, identical strings stored once). Each row is matched to its PC
string by:
  * anchors: rows whose Japanese is known (the manual table's notes) and found once in the exe;
  * fingerprints: runs of K consecutive row slots (PS1 slot size = string + 0-3 bytes padding)
    that only one run of consecutive PC strings fits;
  * chains: from a matched row, the next / previous PC string must fill the row's PS1 slot
    exactly (untranslated, both disks hold the same bytes).
With the PS1 overlay at hand (psx: BTLP.BIN of the first release), a row's Japanese is read from
it and found byte for byte in the exe instead: every copy of it gets the English when the exe
holds it more than once, unless rows with the same Japanese differ in English (then the slot
chains decide). Rows are dropped when two rows with different English claim one PC string, when the matched
string's insert codes differ from the English's, and in the debug-menu block (0x800DD000 on,
strings separated by FF 00). Returns {PC address: English bytes}.
"""
import bisect
import collections
import re

B = 0x800643C4                     # PS1 overlay load address: RAM = file offset + B
EXCLUDE = (0x800DD000, 0x800E0000)  # debug menus
MSG_ZONE = (0x800D1D4C, 0x800D222C)
K = 5                              # fingerprint length
LAYOUT = {0xF6, 0xFC, 0xF5, 0xEF, 0xFB, 0xF7, 0x02}


def build(img, base, sections, rows, manual, etext, vwf_wrap, psx=None, log=print):
    """img: the exe's memory image (bytes), base: image base, sections: [(start, end)] of the data
    sections (image offsets), rows: {file offset: row}, manual: {pos string: manual row}."""
    P = sorted(rows)

    def pcstr(a):
        o = a - base
        for i in range(o, min(o + 700, len(img) - 1)):
            if img[i] == 0xff and img[i + 1] in (0xf5, 0xef):
                return img[o:i + 2]
        return None

    # anchors from the known Japanese
    anch = {}
    for pos, r in rows.items():
        m = re.search(r'ja (.*)$', (manual.get(r['pos']) or {}).get('note', '') or '')
        if not m:
            continue
        try:
            b = etext.encode(m.group(1), end=None)
        except Exception:
            continue
        if len(b) < 4:
            continue
        i = img.find(b)
        if i >= 0 and img.find(b, i + 1) < 0 and (img[i - 1] == 0 or img[i - 2:i] in (b'\xff\xf5', b'\xff\xef')):
            anch[pos] = base + i
    n_known = len(anch)

    # every PC string start in the data sections
    starts = []
    for d0, d1 in sections:
        i = d0
        while i < d1 - 2:
            if img[i] == 0xff and img[i + 1] in (0xf5, 0xef):
                j = i + 2
                while j < d1 and img[j] == 0 and j - i < 18:
                    j += 1
                starts.append(j)
                i = j
            else:
                i += 1
    lens = {}
    for st in starts:
        t = pcstr(base + st)
        if t is not None:
            lens[st] = len(t)
    seq = [st for st in starts if st in lens]
    by = collections.defaultdict(list)
    for n in range(len(seq) - K):
        by[lens[seq[n]]].append(n)
    gaps = [P[i + 1] - P[i] for i in range(len(P) - 1)]
    for r in range(len(P) - K):
        g = gaps[r:r + K]
        if max(g) > 700 or P[r] in anch:
            continue
        cands = []
        for L0 in range(g[0] - 3, g[0] + 1):
            for n in by.get(L0, []):
                ok = True
                for t in range(K):
                    st = seq[n + t]
                    L = lens[st]
                    if not 0 <= g[t] - L <= 3:
                        ok = False
                        break
                    if t + 1 < K and not 0 <= seq[n + t + 1] - st - L <= 16:
                        ok = False
                        break
                if ok:
                    cands.append(n)
        if len(cands) == 1:
            anch[P[r]] = base + seq[cands[0]]

    def skip0(e):
        z = e
        while img[z - base] == 0 and z - e < 16:
            z += 1
        return z

    def walk(pos, a):
        out = []
        while True:
            s = pcstr(a)
            if s is None:
                break
            end = pos + len(s)
            k = bisect.bisect_left(P, end)
            if k >= len(P) or P[k] - end > 3:
                break
            pos, a = P[k], skip0(a + len(s))
            out.append((pos, a))
        return out

    def prev_start(a):
        e = a - base
        z = 0
        while z < 16 and img[e - 1] == 0:
            e -= 1
            z += 1
        if img[e - 2] != 0xff or img[e - 1] not in (0xf5, 0xef):
            return None
        for i in range(e - 3, e - 700, -1):
            if img[i - 1] == 0xff and img[i] in (0xf5, 0xef):
                j = i + 1
                while img[j] == 0:
                    j += 1
                return base + j
        return None

    def walk_back(pos, a):
        out = []
        while True:
            k = bisect.bisect_left(P, pos)
            if k == 0:
                break
            st = prev_start(a)
            if st is None:
                break
            s = pcstr(st)
            if s is None or st + len(s) > a or not 0 <= (pos - P[k - 1]) - len(s) <= 3:
                break
            pos, a = P[k - 1], st
            out.append((pos, a))
        return out

    mapping = {}
    todo = sorted(anch.items())
    seen = set()
    while todo:
        p, a = todo.pop()
        if (p, a) in seen:
            continue
        seen.add((p, a))
        mapping.setdefault(p, a)
        for q, b in walk(p, a) + walk_back(p, a):
            if q in mapping:
                if mapping[q] != b:
                    mapping[q] = None
            else:
                mapping[q] = b
                todo.append((q, b))

    def codes(b):
        c = collections.Counter()
        i = 0
        while i < len(b) - 1:
            if b[i] == 0xff:
                if b[i + 1] not in LAYOUT:
                    c[b[i + 1]] += 1
                i += 2
            elif 0x80 <= b[i] <= 0x87:
                i += 2
            else:
                i += 1
        return c

    def english(pos, t):
        """the row's English as the PS1 family builders would ship it, or None"""
        t = t.replace('<FF03>', '<FFF6>')
        ram = pos + B
        if MSG_ZONE[0] <= ram < MSG_ZONE[1]:
            t = re.sub(r'(?:<FF02>|<FF05><[0-9A-Fa-f]{2}><[0-9A-Fa-f]{2}>)(?=(?:<FF01>)?\s*$)', '', t)
            if re.search(r'<FF0[245]>|<FFF6>', t):
                return None
        for page in re.split(r'<FFFB><FFF7>|<FF04>', t):
            lines = page.split('<FFF6>')
            if len(lines) > 4:
                return None
            for ln in lines:
                if vwf_wrap.text_width(ln) > 240:
                    return None
        try:
            return etext.encode(t, end=b'\xff\xf5')
        except Exception:
            return None

    exact = 0
    if psx:
        # every PC string start, for exact lookups
        pc_starts = collections.defaultdict(list)
        for st in seq:
            pc_starts[img[st:st + lens[st]]].append(base + st)
        ja_of = {}
        for p in rows:
            e = psx.find(bytes([0xff, 0xf5]), p, p + 700)
            f = psx.find(bytes([0xff, 0xef]), p, p + 700)
            ends = [x for x in (e, f) if x >= 0]
            if ends:
                ja_of[p] = psx[p:min(ends) + 2]
        same_ja = collections.defaultdict(set)
        for p, j in ja_of.items():
            same_ja[j].add((rows[p].get('en') or '').strip())
        for p, j in ja_of.items():
            hits = pc_starts.get(j, [])
            if not hits:
                continue
            if len(same_ja[j]) == 1:
                exact += 1
                mapping[p] = hits[0]
                for extra in hits[1:]:
                    mapping[(p, extra)] = extra          # the other copies, same English
            elif mapping.get(p) in hits:
                exact += 1
    by_pc = collections.defaultdict(list)
    for p, a in mapping.items():
        q = p[0] if isinstance(p, tuple) else p
        if a and not EXCLUDE[0] <= q + B < EXCLUDE[1]:
            by_pc[a].append(q)
    out, dropped = {}, collections.Counter()
    for a, ps in by_pc.items():
        texts = {(rows[p].get('en') or '').strip() for p in ps}
        if len(texts) != 1:
            dropped['shared string, different English'] += len(ps)
            continue
        t = texts.pop()
        if not t:
            continue
        s = pcstr(a)
        b = english(ps[0], t)
        if s is None:
            dropped['no string at the match'] += 1
        elif b is None:
            dropped['does not fit the box'] += 1
        elif codes(b) != codes(s):
            dropped['insert codes differ'] += 1
        else:
            out[a] = b
    log(f'battle text: {len(out)} PC strings for {len(rows)} rows ({exact} rows found byte for byte, '
        f'{n_known} known anchors, {len(anch) - n_known} fingerprints); dropped: {dict(dropped)}')
    return out
