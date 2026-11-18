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

#include "host.h"

// The PlayStation button symbols in the font (glyphs, written as two-byte codes): circle, cross,
// triangle, square. Prompts naming them show the button of the device in use (host_button_label).
const uint32_t VWF_PS_GLYPH[4] = { 0xea, 0xe3, 0xee, 0xec };
int vwf_button_action(uint32_t code, int two_byte) {
    if (!two_byte) return -1;
    for (int k = 0; k < 4; k++) if ((code & 0x7ffu) == VWF_PS_GLYPH[k]) return k;
    return -1;
}

int vwf_active;      // 1 while English is on (story dialogue)
int vwf_on[3];       // per interpreter copy: 1 when its English data is installed and verified
int vwf_two_byte;    // the glyph being drawn came from a two-byte code (for the rewind)

// ---------------------------------------------------------------- the interpreters' data
// Each copy of the interpreter has its own FONT.BIN pointer, VRAM upload queue and map routine.
struct interp_cfg { uint32_t font_ptr, queue_count, queue, queue_max, activate; };
static const struct interp_cfg CFG[3] = {
    [VWF_ADV] = { 0x618f80u, 0x61ec28u, 0x61ecb8u, 64, 0x479610u },
    [VWF_B]   = { 0x5e0e8cu, 0x5e5478u, 0x5e4f18u, 64, 0x4212b0u },
    [VWF_C]   = { 0x62985cu, 0x62a248u, 0x62a278u, 32, 0x4e20f0u },
};
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
struct engine {
    uint8_t box[4][16][BOX_W + 32];       // colour index per pixel, one image per box line
    int cur_line, pen;
    uint32_t ring;                        // guest buffer: one 128-byte cell per box position
};
static struct engine engines[3] = { [0] = { .cur_line = -1 }, [1] = { .cur_line = -1 }, [2] = { .cur_line = -1 } };
static struct engine *E_;                 // the engine of the interpreter being served
static const struct interp_cfg *C_;
#define box (E_->box)
#define cur_line (E_->cur_line)
#define pen (E_->pen)
#define ring (E_->ring)

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
    uint32_t n = RD32(C_->queue_count);
    if (n < C_->queue_max) {
        uint32_t e = C_->queue + 12u * n;
        WR32(e, src);
        WR16(e + 4, TEXT_VRAM_X + ((P + 1) & 15u) * 4u);
        WR16(e + 6, TEXT_VRAM_Y + ((P + 1) & 0xf0u));
        WR16(e + 8, 4);
        WR16(e + 10, 16);
        WR32(C_->queue_count, n + 1);
    }
    uint32_t save_ecx = ECX, save_edx = EDX, save_eax = EAX;
    ECX = P;
    rt_call(C_->activate, MAGIC_RET);
    ECX = save_ecx; EDX = save_edx; EAX = save_eax;
}

// A glyph's rows (narrow English: 1 byte per row, or wide original: 2 bytes) and its advance.
struct glyph { int adv, wide; const uint8_t *narrow; uint32_t wide_src; };
static void glyph_of(uint32_t code, int two_byte, struct glyph *g) {
    if (code >= 0x88 && code <= 0xfe && !two_byte) {
        int k = (int)code - ENGLISH_BIAS - font_first;
        if (k < 0 || k >= font_count) k = ' ' - font_first;   // unknown code: a space
        g->narrow = font_rows[k]; g->adv = font_width[k]; g->wide = 0; g->wide_src = 0;
    } else {
        g->narrow = 0; g->wide_src = RD32(C_->font_ptr) + (code & 0x7ffu) * 32u; g->adv = 16; g->wide = 1;
    }
}

// compose: ink in the main colour, a shadow one pixel right and down
static void draw_glyph(int line, const struct glyph *g) {
    for (int r = 0; r < 16; r++) {
        uint32_t bits = g->wide ? (uint32_t)RD8(g->wide_src + 2u * r) << 8 | RD8(g->wide_src + 2u * r + 1)
                                : (uint32_t)g->narrow[r] << 8;
        for (int b = 0; bits && b < 16; b++)
            if (bits & (0x8000u >> b)) {
                plot(line, pen + b, r, COLOR_MAIN, 0);
                plot(line, pen + b + 1, r + 1, COLOR_SHADOW, 1);
            }
    }
}

// Draw the glyph the interpreter pushed (its argument is at [ESP]) at the pen. With a text speed
// above 1, the next plain characters of the same line are drawn in the same call: the read
// pointer moves past them, so the interpreter carries on after them next frame. Anything that is
// not a plain one-byte character (a control code, a two-byte glyph) ends the batch, so waits,
// choices and pauses behave exactly as before.
void vwf_emit(int interp, int name_path) {
    E_ = &engines[interp];
    C_ = &CFG[interp];
    if (!ring) ring = hostobj_alloc(4 * COLS * 128);
    uint32_t st = ESI;
    uint32_t code = RD32(ESP) & 0xffffu;
    uint32_t pos = RD16(st + 0xa);
    int line = (int)(pos / COLS) & 3, col = (int)(pos % COLS);
    if (col == 0 || line != cur_line) {          // a fresh line: start at its left edge
        cur_line = line;
        pen = 0;
        memset(box[line], 0, sizeof box[line]);
    }
    // the glyph, or a button prompt's label (several narrow glyphs)
    struct glyph gs[8];
    int ng = 0, adv = 0;
    const char *label = 0;
    int act = vwf_button_action(code, vwf_two_byte), ps;
    if (act >= 0) {
        label = host_button_label(act, &ps);
        if (!label) code = VWF_PS_GLYPH[ps];
    }
    if (label) {
        for (const char *c = label; *c && ng < 8; c++) {
            glyph_of((uint32_t)(unsigned char)*c + ENGLISH_BIAS, 0, &gs[ng]);
            adv += gs[ng++].adv;
        }
    } else {
        glyph_of(code, vwf_two_byte, &gs[0]);
        adv = gs[0].adv;
        ng = 1;
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

    int start = pen;
    for (int k = 0; k < ng; k++) {
        draw_glyph(line, &gs[k]);
        pen += gs[k].adv;
    }
    // more characters this frame (text speed); only on the plain text path
    for (int extra = name_path ? 0 : cfg.text_speed - 1; extra > 0 && pen < BOX_W; extra--) {
        uint32_t b = RD8(EDI);
        if (b == 0xff || (b >= 0x80 && b <= 0x87)) break;     // control code or two-byte glyph
        struct glyph n;
        glyph_of(b, 0, &n);
        if (pen + n.adv > BOX_W) break;                        // the interpreter wraps it itself
        draw_glyph(line, &n);
        pen += n.adv;
        EDI += 1;
    }
    int c0 = start >> 4, c1 = (pen + 2) >> 4;
    if (c1 > COLS - 1) c1 = COLS - 1;
    for (int c = c0; c <= c1; c++) upload_cell(line, c);

    // the cell position follows the pen
    if (pen >= BOX_W) {
        WR16(st + 0xa, (uint32_t)(((line + 1) & 3) * COLS));
        return;
    }
    int cells = (pen + 15) >> 4;
    if (cells < 1) cells = 1;
    if (cells > COLS - 1) cells = COLS - 1;
    WR16(st + 0xa, (uint32_t)(line * COLS + cells));
}

// an ASCII character's rows in the English font, for other text (movie subtitles); returns its advance
int vwf_glyph(int ch, const uint8_t **rows) {
    static const uint8_t blank[16];
    int k = ch - font_first;
    if (k < 0 || k >= font_count) { *rows = blank; return 4; }
    *rows = font_rows[k];
    return font_width[k];
}
