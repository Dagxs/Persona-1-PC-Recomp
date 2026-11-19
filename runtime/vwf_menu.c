// English names in the menus at their real width (the PS1 patch's SF_VWF, done on the PC).
//
// The menus print item / skill / Persona / demon names into background maps: grids of 16-bit
// cell indices, one 8x12 small-font cell per glyph (TileMapWriteRow, PC 0x421490: src, dst,
// base, count), which libgs' GsSortFastBg (PC 0x5496d0: GsBG, GsOT, otz) turns into one textured
// quad per cell. A name field is ten cells, so the English names, written into the name tables
// as their first ten glyphs (english.c), were cut there.
//
// Here the row writer is watched: when its source is a name's head (data/en/sf_names.bin: by
// the head's address in the name tables, else by its bytes when only one name has that head),
// the row is remembered with the cells it wrote. When the map holding the row is drawn and the
// cells are still those, the cells are hidden for that draw and the whole name is drawn instead
// as quads of the same packet type, each glyph cut to its ink (left column and width from the
// font page), squeezed horizontally when the name is wider than its field. The quads are linked
// into the layer's OT entry before the layer's own cells, so they are drawn after them.
#include "rt.h"
#include "paths.h"
#include <stdlib.h>
#include <string.h>

int vwf_menu_on;
static int sf_log;          // P1_SFLOG: log the rows found and drawn

#define F_SORTBG   0x5496d0u
#define WORK_PTR   0xa65288u     // libgs packet work pointer
#define DRAW_OFSX  0xa65330u
#define DRAW_OFSY  0xa65334u
#define PKT        0x2cu         // the textured quad GsSortFastBg emits per cell
#define HEAD       10
#define NGLYPH     215           // one bank of the small font (base = bank * 215)
#define MAXROWS    512
#define MAXTEXT    64

struct name { uint32_t addr; uint8_t head[HEAD], hlen, len; uint8_t text[MAXTEXT]; };
static struct name *names;
static int nnames;
static uint8_t metric[NGLYPH];   // left << 4 | width (0 = blank)

struct row {
    uint32_t dst;                // guest address of the first cell
    uint16_t count, n;           // the field (cells) and the cells written
    int32_t base;
    uint16_t vals[HEAD];
    const struct name *name;
};
static struct row rows[MAXROWS];
static int next_slot;

void sf_names_load(void) {
    char path[1200];
    snprintf(path, sizeof path, "%s/sf_names.bin", path_en());
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint8_t magic[4];
    uint32_t n = 0;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "P1SF", 4) || fread(metric, 1, NGLYPH, f) != NGLYPH ||
        fread(&n, 4, 1, f) != 1 || n > 4096) { fclose(f); return; }
    names = calloc(n ? n : 1, sizeof *names);
    for (uint32_t k = 0; names && k < n; k++) {
        struct name *m = &names[nnames];
        if (fread(&m->addr, 4, 1, f) != 1 || fread(m->head, 1, HEAD, f) != HEAD || fread(&m->len, 1, 1, f) != 1) break;
        if (m->len > MAXTEXT || fread(m->text, 1, m->len, f) != m->len) break;
        m->hlen = 0;
        while (m->hlen < HEAD && m->head[m->hlen] != 0xff) m->hlen++;
        if (m->len) nnames++;
    }
    fclose(f);
    sf_log = getenv("P1_SFLOG") != NULL;
    if (rt_trace || sf_log) fprintf(stderr, "menu names: %d\n", nnames);
}

static const struct name *find_name(uint32_t src) {
    for (int k = 0; k < nnames; k++)
        if (names[k].addr == src) return &names[k];
    uint8_t b[HEAD];
    int n = 0;
    while (n < HEAD && (b[n] = RD8(src + (uint32_t)n)) != 0xff) n++;
    if (!n) return 0;
    const struct name *hit = 0;
    for (int k = 0; k < nnames; k++) {
        const struct name *m = &names[k];
        if (m->hlen != n || memcmp(m->head, b, (size_t)n)) continue;
        if (hit && (hit->len != m->len || memcmp(hit->text, m->text, m->len))) return 0;   // ambiguous
        hit = m;
    }
    return hit;
}

// TileMapWriteRow's entry (ecx src, edx dst, [esp+4] base, [esp+8] count): remember name rows
void sf_record(void) {
    uint32_t src = ECX, dst = EDX;
    int32_t base = (int32_t)RD32(ESP + 4u);
    uint16_t count = (uint16_t)RD32(ESP + 8u);
    if (!count) return;
    uint16_t written = 0;                   // the cells this call writes (it stops at FF)
    while (written < count && RD8(src + written) != 0xff) written++;
    uint32_t end = dst + 2u * written;
    for (int k = 0; k < MAXROWS; k++)       // a write over a remembered row retires it
        if (rows[k].dst && rows[k].dst < end && dst < rows[k].dst + 2u * rows[k].count) rows[k].dst = 0;
    const struct name *m = find_name(src);
    if (!m) return;
    if (sf_log && getenv("P1_SFSWAP")) {            // test: draw another name (by index) in its place
        int k = atoi(getenv("P1_SFSWAP"));
        if (k >= 0 && k < nnames) m = &names[k];
    }
    int slot = -1;
    for (int k = 0; k < MAXROWS && slot < 0; k++)
        if (!rows[k].dst) slot = k;
    if (slot < 0) { slot = next_slot; next_slot = (next_slot + 1) % MAXROWS; }
    struct row *r = &rows[slot];
    r->dst = dst; r->count = count; r->base = base; r->name = m;
    r->n = 0;
    while (r->n < count && r->n < HEAD) {
        uint8_t c = RD8(src + r->n);
        if (c == 0xff) break;
        r->vals[r->n++] = (uint16_t)(c + base);
    }
    if (count > HEAD && r->n == HEAD) r->dst = 0;    // a wider field than a name's: not a name row
    if (sf_log && r->dst) {
        extern uint8_t vwf_name_ascii[256];
        char t[MAXTEXT + 1];
        for (int i = 0; i <= m->len; i++) t[i] = i < m->len ? (char)(vwf_name_ascii[m->text[i]] ? vwf_name_ascii[m->text[i]] : ' ') : 0;
        fprintf(stderr, "sf_record: '%s' src %08x dst %08x base %d count %u by %s (caller %08x)\n", t,
                src, dst, base, count, m->addr == src ? "address" : "bytes", RD32(ESP));
    }
}

struct bgdraw {
    uint32_t attr, cells, entry;
    int cellw, cellh, clipx, clipy, clipw, cliph;
    uint8_t r, g, b;
};

static void quad(const struct bgdraw *d, int x0, int x1, int y0, int y1, uint32_t value, int u0, int u1, int v0, int v1) {
    uint32_t c = d->cells + value * 8u;
    int cu = RD8(c), cv = RD8(c + 1);
    uint32_t w = RD32(WORK_PTR);
    WR32(WORK_PTR, w + PKT);
    WR32(w, RD32(w) | 0x80000000u);
    if (d->attr & 0x40) WR8(w + 7, 0xf1);
    else { WR8(w + 4, d->r); WR8(w + 5, d->g); WR8(w + 6, d->b); WR8(w + 7, 0xf0); }
    if (d->attr & 0x40000000u) WR8(w + 7, RD8(w + 7) | 2);
    static const uint8_t X[4] = { 0x08, 0x12, 0x1c, 0x24 };
    for (int k = 0; k < 4; k++) {
        WR16(w + X[k], (uint16_t)((k & 1) ? x1 : x0));
        WR16(w + X[k] + 2, (uint16_t)((k & 2) ? y1 : y0));
    }
    // texture coordinates: v0 +0c/+0e, v1 +16/+18, v2 +20/+22, v3 +28/+2a
    WR16(w + 0x0c, (uint16_t)(cu + u0)); WR16(w + 0x0e, (uint16_t)(cv + v0));
    WR16(w + 0x16, (uint16_t)(cu + u1)); WR16(w + 0x18, (uint16_t)(cv + v0));
    WR16(w + 0x20, (uint16_t)(cu + u0)); WR16(w + 0x22, (uint16_t)(cv + v1));
    WR16(w + 0x28, (uint16_t)(cu + u1)); WR16(w + 0x2a, (uint16_t)(cv + v1));
    WR16(w + 0x10, RD16(c + 2));                                    // clut
    WR16(w + 0x1a, (uint16_t)(RD8(c + 6) & 0x1f));                  // tpage
    if (d->attr & 0x2000000u) WR8(w + 0x1b, RD8(w + 0x1b) | 1);
    else if (d->attr & 0x1000000u) WR8(w + 0x1a, RD8(w + 0x1a) | 0x80);
    uint32_t e = RD32(d->entry), t = RD32(w);
    WR32(w, (t & 0x80000000u) | (e & 0x7fffffffu));
    WR32(d->entry, (e & 0x80000000u) | (w & 0x7fffffffu));
}

static int advance(uint8_t code, int gap, int space) {
    if (code >= NGLYPH || !metric[code]) return space;
    return (metric[code] & 15) + gap;
}

// one remembered row, at screen (x, y)
static void draw_row(const struct bgdraw *d, const struct row *r, int x, int y) {
    const struct name *m = r->name;
    int field = r->count * d->cellw, gap = 1, space = 4, w = 0;
    for (int tier = 0; tier < 3; tier++) {
        gap = tier ? 0 : 1;
        space = tier == 2 ? 2 : tier ? 3 : 4;
        w = 0;
        for (int i = 0; i < m->len; i++) w += advance(m->text[i], gap, space);
        w -= gap;
        if (w <= field) break;
    }
    int y0 = y, y1 = y + d->cellh, v0 = 0;
    if (y0 < d->clipy) { v0 = d->clipy - y0; y0 = d->clipy; }
    if (y1 > d->clipy + d->cliph) y1 = d->clipy + d->cliph;
    if (y1 <= y0) return;
    int v1 = v0 + (y1 - y0);
    int pen = 0;
    for (int i = 0; i < m->len; i++) {
        uint8_t code = m->text[i];
        int a = advance(code, gap, space);
        if (code < NGLYPH && metric[code]) {
            int left = metric[code] >> 4, wd = metric[code] & 15;
            int gx0 = x + (w > field ? pen * field / w : pen);
            int gx1 = x + (w > field ? (pen + wd) * field / w : pen + wd);
            if (gx1 <= gx0) gx1 = gx0 + 1;
            if (gx0 >= d->clipx && gx1 <= d->clipx + d->clipw)
                quad(d, gx0, gx1, y0, y1, (uint32_t)(code + r->base), left, left + wd, v0, v1);
        }
        pen += a;
    }
}

// GsSortFastBg's entry (ecx GsBG, edx GsOT, [esp+4] otz): 1 = drawn here (with the original)
int sf_sort(void) {
    static int busy;
    if (busy || !nnames) return 0;
    uint32_t bg = ECX, ot = EDX, otz = RD32(ESP + 4u);
    struct bgdraw d;
    d.attr = RD32(bg);
    if (d.attr & 0x80000000u || d.attr & 0xc00000u) return 0;   // hidden, or flipped
    d.attr &= ~0xc00000u;
    uint32_t map = RD32(bg + 0x14u);
    d.cellw = RD8(map); d.cellh = RD8(map + 1);
    int ncw = RD16(map + 2), nch = RD16(map + 4);
    d.cells = RD32(map + 8u);
    uint32_t idx = RD32(map + 0xcu), idx_end = idx + 2u * (uint32_t)ncw * (uint32_t)nch;
    if (d.cellw != 8 || d.cellh != 12 || !ncw || !nch) return 0;

    static int mine[MAXROWS];
    int nmine = 0;
    for (int k = 0; k < MAXROWS; k++) {
        struct row *r = &rows[k];
        if (!r->dst || r->dst < idx || r->dst + 2u * r->n > idx_end || (r->dst - idx) & 1) continue;
        int same = 1;
        for (int i = 0; i < r->n && same; i++) same = RD16(r->dst + 2u * i) == r->vals[i];
        if (!same) { r->dst = 0; continue; }
        mine[nmine++] = k;
    }
    if (!nmine) return 0;

    // the OT entry, as GsSortFastBg finds it
    uint32_t len = RD32(ot), z = otz & 0xffffu;
    if (len < 31 && z >= (1u << len)) z = (1u << len) - 1u;
    z = (z - RD16(ot + 8u)) & 0xffffu;
    d.entry = RD32(ot + 4u) + z * 4u;
    d.clipx = (int16_t)RD16(bg + 4) - (int16_t)RD16(bg + 0x18) + (int32_t)RD32(DRAW_OFSX);
    d.clipy = (int16_t)RD16(bg + 6) - (int16_t)RD16(bg + 0x1a) + (int32_t)RD32(DRAW_OFSY);
    d.clipw = (int16_t)RD16(bg + 8); d.cliph = (int16_t)RD16(bg + 0xa);
    d.r = RD8(bg + 0x10); d.g = RD8(bg + 0x11); d.b = RD8(bg + 0x12);
    int scx = (int16_t)RD16(bg + 0xc), scy = (int16_t)RD16(bg + 0xe);
    int tw = ncw * d.cellw, th = nch * d.cellh;

    for (int j = 0; j < nmine; j++) {
        const struct row *r = &rows[mine[j]];
        uint32_t p = (r->dst - idx) / 2u;
        int sx = (int)(p % (uint32_t)ncw) * d.cellw - scx, sy = (int)(p / (uint32_t)ncw) * d.cellh - scy;
        sx = ((sx % tw) + tw) % tw;
        sy = ((sy % th) + th) % th;
        if (sx >= d.clipw + d.cellw) sx -= tw;
        if (sy >= d.cliph + d.cellh) sy -= th;
        draw_row(&d, r, d.clipx + sx, d.clipy + sy);
        if (sf_log) fprintf(stderr, "sf_sort: bg %08x row %08x at %d,%d (%d glyphs)\n", bg, r->dst, d.clipx + sx, d.clipy + sy, r->name->len);
    }

    // the layer itself, with the rows' cells hidden
    static uint16_t saved[MAXROWS][HEAD];
    for (int j = 0; j < nmine; j++) {
        const struct row *r = &rows[mine[j]];
        for (int i = 0; i < r->n; i++) { saved[j][i] = RD16(r->dst + 2u * i); WR16(r->dst + 2u * i, 0xffff); }
    }
    busy = 1;
    PUSH32(otz);
    ECX = bg; EDX = ot;
    rt_call(F_SORTBG, MAGIC_RET);
    busy = 0;
    for (int j = 0; j < nmine; j++) {
        const struct row *r = &rows[mine[j]];
        for (int i = 0; i < r->n; i++) WR16(r->dst + 2u * i, saved[j][i]);
    }
    return 1;
}

// The dialogue interpreters' name inserts (MsgStep cases 8-11: item, skill, demon, Persona; PC
// 0x47941a / 0x4210c3 / 0x4e1f03, after `left = 10`): edi points at the record's ten-glyph head.
// A longer English name is copied whole into a scratch buffer and read from there.
void sf_insert(int copy) {
    static uint32_t scratch[3];
    if (!nnames) return;
    const struct name *m = 0;
    for (int k = 0; k < nnames && !m; k++)
        if (names[k].addr == EDI) m = &names[k];
    if (!m || m->len <= HEAD) return;
    if (!scratch[copy]) scratch[copy] = hostobj_alloc(MAXTEXT + 4);
    for (int i = 0; i < m->len; i++) WR8(scratch[copy] + (uint32_t)i, m->text[i]);
    WR8(scratch[copy] + m->len, 0xff);
    EDI = scratch[copy];
    WR8(ESI + 0x35u, m->len);
    if (sf_log) fprintf(stderr, "sf_insert: copy %d, name at %08x, %d glyphs\n", copy, m->addr, m->len);
}

// Battle messages (the BTLP text system, vwf_btl.c): a name insert is copied into one of seven
// 24-byte slots at 0x6391e0 (0x52e6c0: bytes >= 0x80 get an 0x80 lead; 0x52e6f0: as they are),
// at most 8 or 10 glyphs, and the reader is pointed at the slot (`mov [esi+0xc], slot`,
// 0x52f97f..0x52fa03). A long name is also written whole, in the same encoding, to a buffer of
// our own, and the reader is pointed there while the slot still holds that name's head.
#define BSLOTS     0x6391e0u
#define BSLOT_SIZE 0x18u
#define BSLOT_N    7
#define BBUF       96u
static uint32_t bbuf;
static struct { int on; uint8_t head[4]; } bslot[BSLOT_N];

void btl_name_copy(int lead) {
    uint32_t dst = ECX, src = EDX;
    if (dst < BSLOTS || dst >= BSLOTS + BSLOT_N * BSLOT_SIZE || (dst - BSLOTS) % BSLOT_SIZE) return;
    int k = (int)((dst - BSLOTS) / BSLOT_SIZE);
    bslot[k].on = 0;
    if (!nnames) return;
    const struct name *m = find_name(src);
    if (!m || m->len <= HEAD) return;
    if (!bbuf) bbuf = hostobj_alloc(BSLOT_N * BBUF);
    uint32_t o = bbuf + (uint32_t)k * BBUF, n = 0;
    for (int i = 0; i < m->len && n + 3 < BBUF; i++) {
        if (lead && m->text[i] >= 0x80) WR8(o + n++, 0x80);
        WR8(o + n++, m->text[i]);
    }
    WR8(o + n, 0xff);
    for (int i = 0; i < 4; i++) bslot[k].head[i] = RD8(o + (uint32_t)i);
    bslot[k].on = 1;
    if (sf_log) fprintf(stderr, "btl_name_copy: slot %d, name at %08x, %d glyphs\n", k, m->addr, m->len);
}

// the reader entering a slot: the whole name when the slot still holds its head
uint32_t btl_name_slot(uint32_t slot) {
    int k = (int)((slot - BSLOTS) / BSLOT_SIZE);
    if (k < 0 || k >= BSLOT_N || !bslot[k].on || !bbuf) return slot;
    for (int i = 0; i < 4; i++)
        if (RD8(slot + (uint32_t)i) != bslot[k].head[i]) { bslot[k].on = 0; return slot; }
    return bbuf + (uint32_t)k * BBUF;
}

// vwf_btl.c: is p inside one of those whole-name buffers?
int btl_name_buf(uint32_t p) { return bbuf && p >= bbuf && p < bbuf + BSLOT_N * BBUF; }
