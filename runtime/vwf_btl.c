// English for the battle text system (the PS1 BTLP overlay: battle messages, the Triangle help of
// the battle menus, demon conversations from TALK\*.BIN, scripted story-battle lines).
//
// Its interpreter (0x52f850, PS1 0x8007C9E0) draws one 16x16 glyph per cell through the glyph
// function 0x52fbb0(ecx = text struct, edx = read pointer) -> next pointer (PS1 0x8007C754): it
// decodes the FONT.BIN glyph into a 128-byte 4bpp cell (ink 1, shadow 2), queues it for VRAM at
// texture slot k = [struct+0x20] (x = texX + (k % 15) * 4, y = texY + (k / 15) * 16) through the
// game's upload queue (0x5212d0, 72 entries, flushed once a frame after the queued clears) and
// adds sprite k {x, y, 0, 0, attr} at struct+0x28 + k*12 at the cell [struct+0x1C].
//
// With English on, 0x52fbb0 comes here instead (tools/recomp.py). As in the PS1 patch
// (vwf/vwf_btlp*.asm), a pen in pixels per line composes glyphs at their real width; every cell
// a glyph touches is uploaded to texture slot P = its cell and shown by sprite P (sprites skipped
// over are parked off screen). A glyph that does not fit hands itself back: the cell moves to the
// next line and the read pointer stays, so the interpreter's own page wait and scroll run first.
// Codes: 00-7F one-byte glyph, 80-87 xx two-byte glyph, 88-E6 English (ASCII + 0x68).
#include "rt.h"
#include "host.h"
#include "paths.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int vwf_btl_on;           // English battle text on

int vwf_glyph(int ch, const uint8_t **rows);

#define T_ATTR     0x10u
#define T_CELL     0x1cu
#define T_SPRITES  0x20u
#define T_LIST     0x28u
#define T_TEX_X    0x26cu
#define T_TEX_Y    0x26eu
#define MAX_CELLS  45            // 3 lines of 15: the sprite list holds 48
#define FONT_BASE  0x5daaa8u     // [FONT_BASE] + 0x1e0000 = FONT.BIN (PS1 0x801E0000)
#define Q_COUNT    0x638454u
#define Q_ENTRIES  0x637f18u     // {u16 x, y, w, h; u32 src}
#define Q_MAX      72
#define F_QUEUE    0x5212d0u     // (ecx src, dx x, stack y, w, h; ret 0xc)
#define F_LOADIMAGE 0x54c380u    // immediate (ecx rect, edx src)
#define COLS 15
#define NAME_BUFS     0x6391e0u  // the interpreter's name / number insert buffers (7 x 0x18)
#define NAME_BUFS_END 0x639288u
#define BOX_W (COLS * 16)
#define INK 1
#define SHADOW 2
#define ENGLISH_BIAS 0x68

struct btl_engine {
    uint32_t st;                  // the text struct this engine serves
    uint8_t img[4][16][BOX_W + 32];
    int cur_line, pen;
    uint32_t ring;                // guest memory: one 128-byte cell per text cell
    uint32_t rect;                // guest memory: a RECT for the immediate path
};
#define NENG 8
static struct btl_engine eng[NENG];
static int next_victim;

static struct btl_engine *engine_of(uint32_t st) {
    for (int k = 0; k < NENG; k++)
        if (eng[k].st == st) return &eng[k];
    struct btl_engine *e = 0;
    for (int k = 0; k < NENG && !e; k++)
        if (!eng[k].st) e = &eng[k];
    if (!e) { e = &eng[next_victim]; next_victim = (next_victim + 1) % NENG; }
    uint32_t ring = e->ring, rect = e->rect;
    memset(e, 0, sizeof *e);
    e->st = st;
    e->cur_line = -1;
    e->ring = ring ? ring : hostobj_alloc(MAX_CELLS * 128);
    e->rect = rect ? rect : hostobj_alloc(8);
    return e;
}

static void plot(struct btl_engine *e, int line, int x, int y, uint8_t c, int under) {
    if (x < 0 || x >= BOX_W + 32 || y < 0 || y >= 16) return;
    uint8_t *p = &e->img[line][y][x];
    if (under && *p) return;
    *p = c;
}

// upload cell P of the text and make sprite P show it
static void activate(struct btl_engine *e, int line, int cell) {
    uint32_t st = e->st;
    int P = line * COLS + cell;
    if (P >= MAX_CELLS) return;
    uint32_t src = e->ring + (uint32_t)P * 128u;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x += 2)
            WR8(src + (uint32_t)y * 8u + (uint32_t)x / 2u,
                (uint8_t)(e->img[line][y][cell * 16 + x] | e->img[line][y][cell * 16 + x + 1] << 4));
    uint16_t vx = (uint16_t)(RD16(st + T_TEX_X) + (P % COLS) * 4);
    uint16_t vy = (uint16_t)(RD16(st + T_TEX_Y) + (P / COLS) * 16);

    // the game's queue: a cell already queued this frame just points at the newer pixels
    uint32_t n = RD32(Q_COUNT), k;
    for (k = 0; k < n && k < Q_MAX; k++) {
        uint32_t q = Q_ENTRIES + 12u * k;
        if (RD16(q) == vx && RD16(q + 2) == vy) { WR32(q + 8, src); break; }
    }
    uint32_t a = EAX, c = ECX, d = EDX;
    if (k == n && n < Q_MAX - 1) {
        PUSH32(16u);
        PUSH32(4u);
        PUSH32(vy);
        ECX = src;
        EDX = (EDX & 0xffff0000u) | vx;
        rt_call(F_QUEUE, MAGIC_RET);
    } else if (k == n) {                    // queue full: upload now
        WR16(e->rect, vx); WR16(e->rect + 2, vy); WR16(e->rect + 4, 4); WR16(e->rect + 6, 16);
        ECX = e->rect;
        EDX = src;
        rt_call(F_LOADIMAGE, MAGIC_RET);
    }
    EAX = a; ECX = c; EDX = d;

    // sprites: park the ones skipped over, then sprite P at cell P
    uint32_t count = RD32(st + T_SPRITES), attr = RD32(st + T_ATTR);
    for (uint32_t s = count; s < (uint32_t)P; s++) {
        uint32_t sp = st + T_LIST + 12u * s;
        WR16(sp, 0); WR16(sp + 2, (uint16_t)-300); WR32(sp + 4, 0); WR32(sp + 8, attr);
    }
    uint32_t sp = st + T_LIST + 12u * (uint32_t)P;
    WR16(sp, (uint16_t)((P % COLS) * 16));
    WR16(sp + 2, (uint16_t)((P / COLS) * 16));
    WR32(sp + 4, 0);
    WR32(sp + 8, attr);
    if (count < (uint32_t)P + 1) WR32(st + T_SPRITES, (uint32_t)P + 1);
}

// 0x52fbb0 with English on
void vwf_btl_glyph(void) {
    uint32_t st = ECX, p = EDX;
    struct btl_engine *e = engine_of(st);
    uint32_t code = RD8(p), next = p + 1;
    int two = 0;
    if (code >= 0x80 && code <= 0x87) { code = (code & 0x7f) << 8 | RD8(p + 1); next = p + 2; two = 1; }

    // a name copied into the message's name buffers (0x52e6c0: 0x6391e0..): English names
    extern uint8_t vwf_name_ascii[256];
    extern int btl_name_buf(uint32_t p);
    if (((p >= NAME_BUFS && p < NAME_BUFS_END) || btl_name_buf(p)) && code < 256 && vwf_name_ascii[code]) {
        code = vwf_name_ascii[code] + ENGLISH_BIAS;
        two = 0;
    }

    int cell = (int)RD32(st + T_CELL);
    if (cell < 0 || cell >= MAX_CELLS) { EAX = next; return; }   // the interpreter pages first
    int line = cell / COLS, col = cell % COLS;
    if (col == 0 || line != e->cur_line) {
        e->cur_line = line;
        e->pen = 0;
        memset(e->img[line], 0, sizeof e->img[0]);
    }

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
    else { wide = RD32(FONT_BASE) + 0x1e0000u + (code & 0x7fffu) * 32u; adv = gadv[0] = 16; ng = 1; }

    if (e->pen > 0 && e->pen + adv > BOX_W) {     // does not fit: hand it back on the next line
        WR32(st + T_CELL, (uint32_t)((line + 1) * COLS));
        EAX = p;
        return;
    }
    int start = e->pen;
    for (int k = 0; k < ng; k++) {
        for (int r = 0; r < 16; r++) {
            uint32_t bits = wide ? (uint32_t)RD8(wide + 2u * r) << 8 | RD8(wide + 2u * r + 1)
                                 : (uint32_t)narrow[k][r] << 8;
            for (int b = 0; bits && b < 16; b++)
                if (bits & (0x8000u >> b)) {
                    plot(e, line, e->pen + b, r, INK, 0);
                    plot(e, line, e->pen + b + 1, r + 1, SHADOW, 1);
                }
        }
        e->pen += gadv[k];
    }
    int c0 = start >> 4, c1 = (e->pen + 1) >> 4;
    if (c1 > COLS - 1) c1 = COLS - 1;
    for (int c = c0; c <= c1; c++) activate(e, line, c);

    // the cell follows the pen
    int cells;
    if (e->pen >= BOX_W) cells = COLS;
    else {
        cells = (e->pen + 15) >> 4;
        if (cells < 1) cells = 1;
        if (cells > COLS - 1) cells = COLS - 1;
    }
    WR32(st + T_CELL, (uint32_t)(line * COLS + cells));
    EAX = next;
}

// ---------------------------------------------------------------- the 16 px string renderer
// 0x52f2c0(ecx = descriptor {.., +4 x, +6 y}, edx = string; FF / F5 end), PS1 BTLP 0x8007BF44: the
// battle command windows (CONTACT commands, the "LV name" header...). Retail queues one glyph per
// cell to x = (desc.x + 0x1c0 + 2 * cell) * 2, y = desc.y + 0x150; the caller owns the sprites. With
// English on the string is composed at real widths and every cell it covers is queued.
#define S16_RING 256              // cells of upload buffers, reused round robin (queued until the frame ends)
uint32_t btl_text(uint32_t p);
static uint32_t s16_ring;
static int s16_next;

void vwf_btl_str16(void) {
    uint32_t desc = ECX, p = btl_text(EDX);
    static uint8_t row[16][BOX_W + 32];
    memset(row, 0, sizeof row);
    if (!s16_ring) s16_ring = hostobj_alloc(S16_RING * 128);
    int pen = 0, n = 0;
    for (;;) {                                        // retail stops at 15 glyphs; English at 15 cells
        uint32_t code = RD8(p);
        if (code == 0xff || code == 0xf5 || RD8(p + 1) == 0xf5) break;
        int two = 0;
        uint32_t next = p + 1;
        if (code >= 0x80 && code <= 0x87) { code = (code & 0x7f) << 8 | RD8(p + 1); next = p + 2; two = 1; }
        const uint8_t *narrow = 0;
        uint32_t wide = 0;
        int adv;
        if (!two && code >= 0x88 && code <= 0xfe) adv = vwf_glyph((int)code - ENGLISH_BIAS, &narrow);
        else { wide = RD32(FONT_BASE) + 0x1e0000u + (code & 0x7fffu) * 32u; adv = 16; }
        if (pen + adv > BOX_W || (wide && n >= COLS)) break;
        for (int r = 0; r < 16; r++) {
            uint32_t bits = wide ? (uint32_t)RD8(wide + 2u * r) << 8 | RD8(wide + 2u * r + 1) : (uint32_t)narrow[r] << 8;
            for (int b = 0; bits && b < 16; b++)
                if (bits & (0x8000u >> b)) {
                    row[r][pen + b] = INK;
                    if (r + 1 < 16 && !row[r + 1][pen + b + 1]) row[r + 1][pen + b + 1] = SHADOW;
                }
        }
        pen += adv;
        p = next;
        n++;
    }
    int cells = (pen + 15) >> 4;
    if (cells > COLS) cells = COLS;
    uint32_t a = EAX, c = ECX, d = EDX;
    for (int cell = 0; cell < cells; cell++) {
        if (RD32(Q_COUNT) >= Q_MAX - 1) break;       // the queue is full this frame
        uint32_t src = s16_ring + (uint32_t)s16_next * 128u;
        s16_next = (s16_next + 1) % S16_RING;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x += 2)
                WR8(src + (uint32_t)y * 8u + (uint32_t)x / 2u, (uint8_t)(row[y][cell * 16 + x] | row[y][cell * 16 + x + 1] << 4));
        PUSH32(16u);
        PUSH32(4u);
        PUSH32((uint16_t)(RD16(desc + 6) + 0x150));
        ECX = src;
        EDX = (uint16_t)((RD16(desc + 4) + 0x1c0 + 2 * cell) * 2);
        rt_call(F_QUEUE, MAGIC_RET);
    }
    EAX = a; ECX = c; EDX = d;
}

// ---------------------------------------------------------------- English strings
// data/en/btl_text.bin (tools/btl_map.py): {PC address of a battle string, its English}. When the
// interpreter (0x52fd6d, every text struct) or the command renderer starts reading one of those
// strings, it reads the English copy instead; the original bytes stay (the compiler stores
// identical strings once, and other code may share them).
static struct btl_str { uint32_t pc, en; } *btl_strs;
static int nbtl_strs;

void btl_text_load(void) {
    char path[1200];
    snprintf(path, sizeof path, "%s/btl_text.bin", path_en());
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint8_t hdr[8];
    uint32_t n = 0;
    if (fread(hdr, 1, 8, f) == 8 && !memcmp(hdr, "P1BT", 4)) memcpy(&n, hdr + 4, 4);
    if (n > 8192) n = 0;
    btl_strs = calloc(n ? n : 1, sizeof *btl_strs);
    static uint8_t buf[4096];
    uint32_t prev = 0;
    for (uint32_t k = 0; btl_strs && k < n; k++) {
        uint32_t h[2];
        if (fread(h, 4, 2, f) != 2 || h[1] > sizeof buf || fread(buf, 1, h[1], f) != h[1] || h[0] <= prev) break;
        uint32_t en = hostobj_alloc(h[1] + 4);
        for (uint32_t i = 0; i < h[1]; i++) WR8(en + i, buf[i]);
        btl_strs[nbtl_strs].pc = prev = h[0];
        btl_strs[nbtl_strs++].en = en;
    }
    fclose(f);
    if (rt_trace) fprintf(stderr, "battle text: %d strings\n", nbtl_strs);
}

uint32_t btl_text(uint32_t p) {
    int lo = 0, hi = nbtl_strs - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (btl_strs[m].pc == p) return btl_strs[m].en;
        if (btl_strs[m].pc < p) lo = m + 1; else hi = m - 1;
    }
    return p;
}
