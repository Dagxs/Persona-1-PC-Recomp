// English dialogue: variable-width glyph engine for the ADV text interpreter.
//
// Work in progress. The hook points are in place (tools/recomp.py PATCHES) and the English
// data builds (tools/install_english.py), but the engine itself is not written yet, so English
// stays switched off and the patched code runs the original Japanese paths.
//
// Plan (mirrors the PS1 translation's vwf_dialog.asm):
//   * glyph codes 88-E6 are English characters (ASCII + 0x68), drawn from data/en/vwf_font.bin
//     at their own width; original glyphs (00-7F, 80-87xx) are drawn 16 px wide from FONT.BIN
//   * glyphs are composed at pixel granularity into the 15x4-cell box (16x16 4bpp cells, main
//     colour 2, shadow 1 at +1,+1); each touched cell is queued for upload to VRAM
//     x = 0x3C0 + ((P+1) & 15) * 4, y = 0x100 + ((P+1) & 0xF0), with P = line*15 + cell,
//     through the game's queue (entries at 0x61ECB8, 12 bytes, 64 slots; count at 0x61EC28),
//     then activated with the game's own routine at 0x479610 (ecx = P)
//   * the engine owns the cell position (state+0xA): line*15 + clamp(ceil(px/16), 1, 14), or
//     the next line's start when the line is full; a glyph that does not fit rewinds the read
//     pointer (edi) so the interpreter re-reads it on the new line
//   * when English is on: script files come from data/en/ADV and the sector tables of
//     E0-E3 in Persona.exe (0x57946C, 0x579630, 0x5797F4, 0x5799D8) are replaced
#include "rt.h"

int vwf_active;      // 1 while the English dialogue engine is in use
int vwf_two_byte;    // the glyph being drawn came from a two-byte code (for the rewind)

void vwf_emit(int interp, int name_path) {
    (void)interp; (void)name_path;
    rt_fatal("English dialogue engine is not implemented yet");
}
