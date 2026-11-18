// English dialogue: a variable-width glyph engine for the ADV text interpreter (f_00478cf0).
//
// Same design as the PS1 fan translation's dialogue engine (src/asm/vwf/vwf_dialog.asm there),
// rebuilt in C for the Windows port, whose interpreter is the same code compiled for x86:
//   * The text box is a 15x4 map of 16x16 cells (4 bits per pixel) in emulated VRAM. Retail draws
//     one glyph per cell. The engine instead keeps a pen position in pixels per line, composes
//     glyphs at their real width into an image of the box, and uploads every cell a glyph touched.
//   * Glyph codes: 00-7F and two-byte 80-87xx are the original font (FONT.BIN, 16 px wide);
//     88-E6 are English characters (ASCII + 0x68) from data/en/vwf_font.bin.
//   * The engine owns the cell position (state+0xA): line*15 + clamp(ceil(px/16), 1, 14), so it
//     is a multiple of 15 only at a real line start, or the next line's start when the line is
//     full. A glyph that does not fit hands its bytes back (rewinds the read pointer) so the
//     interpreter re-reads it on the new line after its own scroll and wait checks.
// The interpreter's call sites are patched in tools/recomp.py (PATCHES).
#include "rt.h"
#include "settings.h"
#include <stdlib.h>
#include <string.h>

int vwf_active;      // 1 while the English dialogue engine is in use
int vwf_two_byte;    // the glyph being drawn came from a two-byte code (for the rewind)

// ---------------------------------------------------------------- the ADV interpreter's data
#define ADV_FONT_PTR      0x618f80u   // u32: guest address of FONT.BIN (32 bytes per glyph)
#define ADV_QUEUE_COUNT   0x61ec28u   // u32: entries in the VRAM upload queue
#define ADV_QUEUE         0x61ecb8u   // 12-byte entries {u32 src; u16 x, y, w, h}
#define ADV_QUEUE_MAX     64          // the next variable starts at 0x61efb8
#define ADV_ACTIVATE      0x479610u   // void(ecx = cell position): map entry + cell setup
#define TEXT_VRAM_X       0x3c0u
#define TEXT_VRAM_Y       0x100u

#define COLS 15
#define BOX_W (COLS * 16)
#define COLOR_MAIN 2
#define COLOR_SHADOW 1
#define ENGLISH_BIAS 0x68             // byte = ASCII + bias

// ---------------------------------------------------------------- the English font
static uint8_t font_first;
static int font_count;
static uint8_t font_width[256];
static uint8_t font_rows[256][16];

int vwf_load_font(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t hdr[7];
    int ok = fread(hdr, 1, 7, f) == 7 && !memcmp(hdr, "P1VF", 4);
    if (ok) {
        font_first = hdr[4];
        font_count = hdr[5] | hdr[6] << 8;
        if (font_count > 256 - font_first) font_count = 256 - font_first;
        for (int k = 0; k < font_count && ok; k++) {
            ok = fread(&font_width[k], 1, 1, f) == 1 && fread(font_rows[k], 1, 16, f) == 16;
        }
    }
    fclose(f);
    return ok;
}

// ---------------------------------------------------------------- engine state
static uint8_t box[4][16][BOX_W + 32];   // colour index per pixel, one image per box line
static int cur_line = -1, pen;
static uint32_t ring;                     // guest buffer: one 128-byte cell per box position

static void plot(int line, int x, int y, uint8_t c, int under) {
    if (x < 0 || x >= BOX_W + 32 || y < 0 || y >= 16) return;
    uint8_t *p = &box[line][y][x];
    if (under && *p) return;              // the shadow never covers ink
    *p = c;
}

// upload one cell of the box image and show it in the map
static void upload_cell(int line, int cell) {
    uint32_t P = (uint32_t)(line * COLS + cell);
    uint32_t src = ring + P * 128u;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x += 2)
            WR8(src + (uint32_t)y * 8u + (uint32_t)x / 2u,
                box[line][y][cell * 16 + x] | box[line][y][cell * 16 + x + 1] << 4);
    uint32_t n = RD32(ADV_QUEUE_COUNT);
    if (n < ADV_QUEUE_MAX) {
        uint32_t e = ADV_QUEUE + 12u * n;
        WR32(e, src);
        WR16(e + 4, TEXT_VRAM_X + ((P + 1) & 15u) * 4u);
        WR16(e + 6, TEXT_VRAM_Y + ((P + 1) & 0xf0u));
        WR16(e + 8, 4);
        WR16(e + 10, 16);
        WR32(ADV_QUEUE_COUNT, n + 1);
    }
    uint32_t save_ecx = ECX, save_edx = EDX, save_eax = EAX;
    ECX = P;
    rt_call(ADV_ACTIVATE, MAGIC_RET);
    ECX = save_ecx; EDX = save_edx; EAX = save_eax;
}

// Draw the glyph the interpreter pushed (its argument is at [ESP]) at the pen.
void vwf_emit(int interp, int name_path) {
    (void)interp;
    if (!ring) ring = hostobj_alloc(4 * COLS * 128);
    uint32_t st = ESI;
    uint32_t glyph = RD32(ESP) & 0xffffu;
    uint32_t pos = RD16(st + 0xa);
    int line = (int)(pos / COLS) & 3, col = (int)(pos % COLS);
    if (col == 0 || line != cur_line) {          // a fresh line: start at its left edge
        cur_line = line;
        pen = 0;
        memset(box[line], 0, sizeof box[line]);
    }

    // glyph rows and advance
    int adv, wide;
    const uint8_t *narrow = 0;
    uint32_t wide_src = 0;
    if (glyph >= 0x88 && glyph <= 0xfe && !vwf_two_byte) {
        int k = (int)glyph - ENGLISH_BIAS - font_first;
        if (k < 0 || k >= font_count) k = ' ' - font_first;   // unknown code: a space
        narrow = font_rows[k];
        adv = font_width[k];
        wide = 0;
    } else {
        wide_src = RD32(ADV_FONT_PTR) + (glyph & 0x7ffu) * 32u;
        adv = 16;
        wide = 1;
    }

    // does it fit on this line?
    if (pen > 0 && pen + adv > BOX_W) {
        if (!name_path) EDI -= vwf_two_byte ? 2u : 1u;         // re-read it on the next line
        else {
            if (RD32(st) & 0x200000u) EDI += 1; else EDI -= 1;  // name read backwards / forwards
            WR8(st + 0x35, RD8(st + 0x35) + 1);                 // the caller counts it again
        }
        WR16(st + 0xa, (uint32_t)(((line + 1) & 3) * COLS));
        return;
    }

    // compose: ink in the main colour, a shadow one pixel right and down
    for (int r = 0; r < 16; r++) {
        uint32_t bits = wide ? (uint32_t)RD8(wide_src + 2u * r) << 8 | RD8(wide_src + 2u * r + 1)
                             : (uint32_t)narrow[r] << 8;
        for (int b = 0; bits && b < 16; b++)
            if (bits & (0x8000u >> b)) {
                plot(line, pen + b, r, COLOR_MAIN, 0);
                plot(line, pen + b + 1, r + 1, COLOR_SHADOW, 1);
            }
    }
    int c0 = pen >> 4, c1 = (pen + adv + 2) >> 4;
    if (c1 > COLS - 1) c1 = COLS - 1;
    for (int c = c0; c <= c1; c++) upload_cell(line, c);

    // advance the pen; the cell position follows it
    pen += adv;
    if (pen >= BOX_W) {
        WR16(st + 0xa, (uint32_t)(((line + 1) & 3) * COLS));
        return;
    }
    int cells = (pen + 15) >> 4;
    if (cells < 1) cells = 1;
    if (cells > COLS - 1) cells = COLS - 1;
    WR16(st + 0xa, (uint32_t)(line * COLS + cells));
}
