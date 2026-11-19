// English for the dungeon field script's message box (the text inside the dungeon packs
// D0?/D??.BIN: locked doors, party remarks, lever prompts...). This is not the MsgStep dialogue
// interpreter (vwf.c) but the field script interpreter (PS1 FieldRunScript, here f_0042cd..), which
// prints one glyph at a time through FieldMsgPutGlyph (0x42da00): three lines of fifteen 16x16
// 4bpp cells at VRAM x = 0x140 + col*4, y = 0x18 + (line % 3)*16.
//
// With English on, 0x42da00 comes here instead (tools/recomp.py). Like the dialogue engine, this
// keeps a pen in pixels per line, composes glyphs at their real width into an image of the line and
// uploads every cell a glyph touched; the cursor (col) follows the pen. Codes 88-E6 are English
// (ASCII + 0x68); the lead-byte tests of the interpreter and its string printers are patched so
// only 80-87 start a two-byte glyph (vwf_field_two says which kind the current code is).
//
// Boxes whose English does not fit in place in the pack (data/en/dng_spill.bin) are matched by the
// 24 bytes at the interpreter's read pointer and redirected to the English text; the box's FF01
// returns through the interpreter's own resume pointer, as in the PS1 patch (dng/spill.asm).
#include "rt.h"
#include "paths.h"
#include "host.h"
#include <stdlib.h>
#include <string.h>

int vwf_field_on;         // English dungeon text on
int vwf_field_two;        // the code passed to 0x42da00 came from a two-byte glyph

int vwf_glyph(int ch, const uint8_t **rows);

#define F_COL       0x5e46e8u    // cursor cell in the line
#define F_LINE      0x5e46ecu    // line counter (the row on screen is line % 3)
#define F_COLOR     0x5e474cu    // text colour (palette index for the tint)
#define F_FONT      0x5e110cu    // pointer to the font (32 bytes per glyph)
#define F_RECT      0x5e47b0u    // the upload rectangle {x, y, w, h}
#define F_NEWLINE   0x42db00u
#define F_TINT      0x42dc80u    // (cells, colour, col, line, count)
#define F_LOADIMAGE 0x54c380u    // (rect, src)
#define COLS 15
#define BOX_W (COLS * 16)
#define INK 1
#define SHADOW 2
#define ENGLISH_BIAS 0x68

static uint8_t img[3][16][BOX_W + 32];
static int cur_line = -1, pen;
static uint32_t ring;     // guest memory: one 128-byte 4bpp cell per box position

static void call_saved(uint32_t fn) {
    uint32_t a = EAX, c = ECX, d = EDX;
    rt_call(fn, MAGIC_RET);
    EAX = a; ECX = c; EDX = d;
}

static int row_of(int line) { return ((line % 3) + 3) % 3; }

static void fresh_line(int line) {
    cur_line = line;
    pen = 0;
    memset(img[row_of(line)], 0, sizeof img[0]);
}

static void plot(int row, int x, int y, uint8_t c, int under) {
    if (x < 0 || x >= BOX_W + 32 || y < 0 || y >= 16) return;
    uint8_t *p = &img[row][y][x];
    if (under && *p) return;
    *p = c;
}

static void upload(int line, int cell) {
    int row = row_of(line);
    uint32_t src = ring + (uint32_t)(row * COLS + cell) * 128u;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x += 2)
            WR8(src + (uint32_t)y * 8u + (uint32_t)x / 2u,
                (uint8_t)(img[row][y][cell * 16 + x] | img[row][y][cell * 16 + x + 1] << 4));
    // the cell's palette, as the original does for every glyph
    uint32_t a = EAX, c = ECX, d = EDX;
    PUSH32(1u);
    PUSH32((uint32_t)line);
    PUSH32((uint32_t)cell);
    ECX = RD32(RD32(0x5e1038u) + 0xcu) + RD32(0x5daaa8u) + 0x130000u;
    EDX = (EDX & 0xffffff00u) | RD8(F_COLOR);
    rt_call(F_TINT, MAGIC_RET);                 // callee pops its three arguments
    WR16(F_RECT, (uint16_t)(0x140 + cell * 4));
    WR16(F_RECT + 2, (uint16_t)(0x18 + row * 16));
    WR16(F_RECT + 4, 4);
    WR16(F_RECT + 6, 16);
    ECX = F_RECT;
    EDX = src;
    rt_call(F_LOADIMAGE, MAGIC_RET);
    EAX = a; ECX = c; EDX = d;
}

// 0x42da00 with English on: ECX = the glyph code
void vwf_field_put(void) {
    uint32_t code = ECX & 0xffffu;
    int two = vwf_field_two;
    vwf_field_two = 0;
    if (!ring) ring = hostobj_alloc(3 * COLS * 128);
    if ((int32_t)RD32(F_COL) >= COLS) call_saved(F_NEWLINE);
    int line = (int)RD32(F_LINE), col = (int)RD32(F_COL);
    if (col == 0 || line != cur_line) fresh_line(line);

    // the glyph, or a button prompt's label (several narrow glyphs)
    const uint8_t *narrow[8] = { 0 };
    uint32_t wide = 0;
    int adv = 0, ng = 0, gadv[8];
    const char *label = 0;
    int act = vwf_button_action(code, two), ps;
    if (act >= 0) {
        label = host_button_label(act, &ps);
        if (!label) code = VWF_PS_GLYPH[ps];
    }
    if (label) {
        for (const char *c = label; *c && ng < 8; c++, ng++) adv += gadv[ng] = vwf_glyph((unsigned char)*c, &narrow[ng]);
    } else if (!two && code >= 0x88 && code <= 0xfe) { adv = gadv[0] = vwf_glyph((int)code - ENGLISH_BIAS, &narrow[0]); ng = 1; }
    else { wide = RD32(F_FONT) + code * 32u; adv = gadv[0] = 16; ng = 1; }

    if (pen > 0 && pen + adv > BOX_W) {         // does not fit: next line (the box scrolls itself)
        call_saved(F_NEWLINE);
        line = (int)RD32(F_LINE);
        fresh_line(line);
    }
    int row = row_of(line), start = pen;
    for (int k = 0; k < ng; k++) {
        for (int r = 0; r < 16; r++) {
            uint32_t bits = wide ? (uint32_t)RD8(wide + 2u * r) << 8 | RD8(wide + 2u * r + 1)
                                 : (uint32_t)narrow[k][r] << 8;
            for (int b = 0; bits && b < 16; b++)
                if (bits & (0x8000u >> b)) {
                    plot(row, pen + b, r, INK, 0);
                    plot(row, pen + b + 1, r + 1, SHADOW, 1);
                }
        }
        pen += gadv[k];
    }
    int c0 = start >> 4, c1 = (pen + 1) >> 4;
    if (c1 > COLS - 1) c1 = COLS - 1;
    for (int c = c0; c <= c1; c++) upload(line, c);

    int cells = pen >= BOX_W ? COLS : (pen + 15) >> 4;
    if (cells < 1) cells = 1;
    if (cells > COLS) cells = COLS;
    if (pen < BOX_W && cells > COLS - 1) cells = COLS - 1;
    WR32(F_COL, (uint32_t)cells);
    EAX = (uint32_t)cells;
}

// ---------------------------------------------------------------- spilled boxes
#define PREFIX 24
static struct spill { uint8_t pre[PREFIX]; uint32_t text; } *spills;
static int nspills;
int dng_spill_n;

void dng_spill_load(void) {
    char path[1200];
    snprintf(path, sizeof path, "%s/dng_spill.bin", path_en());
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint8_t hdr[12];
    uint32_t ne = 0, nt = 0;
    if (fread(hdr, 1, 12, f) == 12 && !memcmp(hdr, "P1DS", 4)) {
        memcpy(&ne, hdr + 4, 4);
        memcpy(&nt, hdr + 8, 4);
    }
    if (ne > 4096 || nt > 4096) ne = nt = 0;
    struct { uint8_t pre[PREFIX]; uint32_t t; } *raw = calloc(ne ? ne : 1, sizeof *raw);
    uint32_t *at = calloc(nt ? nt : 1, sizeof *at);
    int ok = raw && at;
    for (uint32_t k = 0; ok && k < ne; k++)
        ok = fread(raw[k].pre, 1, PREFIX, f) == PREFIX && fread(&raw[k].t, 4, 1, f) == 1 && raw[k].t < nt;
    for (uint32_t k = 0; ok && k < nt; k++) {
        uint32_t len;
        ok = fread(&len, 4, 1, f) == 1 && len < 65536;
        if (!ok) break;
        uint8_t *tmp = malloc(len ? len : 1);
        ok = tmp && fread(tmp, 1, len, f) == len;
        if (ok) {
            at[k] = hostobj_alloc(len + 4);
            for (uint32_t i = 0; i < len; i++) WR8(at[k] + i, tmp[i]);
        }
        free(tmp);
    }
    fclose(f);
    if (ok && ne) {
        spills = calloc(ne, sizeof *spills);
        for (uint32_t k = 0; spills && k < ne; k++) {
            memcpy(spills[k].pre, raw[k].pre, PREFIX);
            spills[k].text = at[raw[k].t];
        }
        if (spills) nspills = dng_spill_n = (int)ne;
    } else
        fprintf(stderr, "English: %s is damaged; long dungeon boxes stay Japanese\n", path);
    free(raw);
    free(at);
    if (rt_trace) fprintf(stderr, "dungeon spill: %d boxes\n", nspills);
}

// the field interpreter's read pointer at the top of its loop: a spilled box's head -> its English
uint32_t dng_spill(uint32_t p) {
    uint8_t first = RD8(p);
    for (int k = 0; k < nspills; k++) {
        const uint8_t *q = spills[k].pre;
        if (q[0] != first) continue;
        int i = 1;
        while (i < PREFIX && RD8(p + (uint32_t)i) == q[i]) i++;
        if (i == PREFIX) return spills[k].text;
    }
    return p;
}

// ---------------------------------------------------------------- map titles
// MapDrawName (0x4079e0, ecx = map index) copies the pack's 10-cell name field (20 bytes) to the
// message cells 0x5e0ae4, starts them as a script (0x4208b0) and sets the title strip. Names longer
// than 10 characters keep their Japanese field in the pack; with English on, the field is looked up
// in data/en/map_titles.bin and the full English shown instead, the strip hugging its width (the PS1
// patch's dng/map_title.asm).
#define MT_CELLS 0x5e0ae4u
#define MT_RECT  0x573edcu
#define MT_FLAGS 0x5e4cf8u
#define MT_INIT  0x4208b0u
#define MT_CX    0xaa
static struct map_title { uint8_t key[20]; uint16_t px, len; uint8_t text[40]; } *mts;
static int nmts;

void map_titles_load(void) {
    char path[1200];
    snprintf(path, sizeof path, "%s/map_titles.bin", path_en());
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint8_t hdr[8];
    uint32_t n = 0;
    if (fread(hdr, 1, 8, f) == 8 && !memcmp(hdr, "P1MT", 4)) memcpy(&n, hdr + 4, 4);
    if (n > 1024) n = 0;
    mts = calloc(n ? n : 1, sizeof *mts);
    for (uint32_t k = 0; mts && k < n; k++) {
        struct map_title *m = &mts[nmts];
        if (fread(m->key, 1, 20, f) != 20 || fread(&m->px, 2, 1, f) != 1 || fread(&m->len, 2, 1, f) != 1) break;
        if (m->len > sizeof m->text || m->len < 2 || fread(m->text, 1, m->len, f) != m->len) break;
        if (m->text[m->len - 2] != 0xff || m->text[m->len - 1] != 0x01 || m->px > 0xa0) continue;
        nmts++;
    }
    fclose(f);
    if (rt_trace) fprintf(stderr, "map titles: %d\n", nmts);
}

// 0x4079e0 with English on: 1 = shown here, 0 = the original code runs
int map_title(void) {
    if (!nmts) return 0;
    uint32_t base = RD32(0x5daaa8u);
    int32_t map = (int16_t)ECX;
    uint32_t field = RD32(base + 0x1dd008u + (uint32_t)map * 4u) + base + 0x1dd000u;
    const struct map_title *m = 0;
    for (int k = 0; k < nmts && !m; k++) {
        int i = 0;
        while (i < 20 && RD8(field + (uint32_t)i) == mts[k].key[i]) i++;
        if (i == 20) m = &mts[k];
    }
    if (!m) return 0;
    for (uint32_t i = 0; i < 0x22; i++) WR8(MT_CELLS + i, 0);
    for (uint32_t i = 0; i < m->len; i++) WR8(MT_CELLS + i, m->text[i]);
    uint32_t a = EAX, c = ECX, d = EDX;
    ECX = MT_CELLS;
    EDX = 0;
    rt_call(MT_INIT, MAGIC_RET);
    EAX = a; ECX = c; EDX = d;
    WR16(MT_RECT, (uint16_t)(MT_CX - m->px / 2));
    WR16(MT_RECT + 2, 0x12);
    WR16(MT_RECT + 4, m->px);
    WR16(MT_RECT + 6, 0x10);
    WR32(MT_FLAGS, RD32(MT_FLAGS) | 0x10u);
    return 1;
}
