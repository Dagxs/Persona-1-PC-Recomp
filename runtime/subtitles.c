// English movie subtitles (Language = English): data/en/subtitles.tsv from the fan translation
// (translation/manual/fmv/subtitles.tsv: movie, start_s, end_s, text, note; "\n" breaks lines).
// movie.c calls subtitles_draw for every decoded frame; the text is drawn into the 320x240 movie
// frame in the English dialogue font, white with a black outline, centred near the bottom.
#include "rt.h"
#include "paths.h"
#include <stdlib.h>
#include <string.h>

struct sub { char movie[16]; double start, end; char text[200]; };
static struct sub *subs;
static int nsubs;

int vwf_glyph(int ch, const uint8_t **rows);   // vwf.c: rows of an ASCII character, returns advance

void subtitles_load(void) {
    char path[1200];
    snprintf(path, sizeof path, "%s/subtitles.tsv", path_en());
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[1024];
    int first = 1;
    while (fgets(line, sizeof line, f)) {
        if (first) { first = 0; continue; }                 // header
        char *col[5] = { 0 };
        int n = 0;
        for (char *p = line; n < 5; n++) {
            col[n] = p;
            char *tab = strchr(p, '\t');
            if (!tab) { char *e = strpbrk(p, "\r\n"); if (e) *e = 0; n++; break; }
            *tab = 0;
            p = tab + 1;
        }
        if (n < 4 || !col[3][0]) continue;
        struct sub *s = realloc(subs, sizeof *subs * (size_t)(nsubs + 1));
        if (!s) break;
        subs = s;
        struct sub *e = &subs[nsubs++];
        snprintf(e->movie, sizeof e->movie, "%s", col[0]);
        e->start = atof(col[1]);
        e->end = atof(col[2]);
        // copy the text: "\n" -> newline, typographic quotes and dashes -> ASCII
        size_t o = 0;
        for (const unsigned char *p = (const unsigned char *)col[3]; *p && o + 4 < sizeof e->text; p++) {
            if (p[0] == '\\' && p[1] == 'n') { e->text[o++] = '\n'; p++; }
            else if (p[0] == 0xe2 && p[1] == 0x80 && (p[2] == 0x98 || p[2] == 0x99)) { e->text[o++] = '\''; p += 2; }
            else if (p[0] == 0xe2 && p[1] == 0x80 && (p[2] == 0x9c || p[2] == 0x9d)) { e->text[o++] = '"'; p += 2; }
            else if (p[0] == 0xe2 && p[1] == 0x80 && (p[2] == 0x93 || p[2] == 0x94)) { e->text[o++] = '-'; p += 2; }
            else if (p[0] == 0xe2 && p[1] == 0x80 && p[2] == 0xa6) { e->text[o++] = '.'; e->text[o++] = '.'; e->text[o++] = '.'; p += 2; }
            else if (*p < 0x80) e->text[o++] = (char)*p;
        }
        e->text[o] = 0;
    }
    fclose(f);
    if (rt_trace) fprintf(stderr, "subtitles: %d lines\n", nsubs);
}

static int line_width(const char *s, size_t n) {
    int w = 0;
    for (size_t k = 0; k < n; k++) { const uint8_t *r; w += vwf_glyph((unsigned char)s[k], &r); }
    return w;
}

static void put(uint32_t pixels, uint32_t pitch, int w, int h, int x, int y, uint16_t c) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    WR16(pixels + (uint32_t)y * pitch + 2u * (uint32_t)x, c);
}

// draw the subtitle active at time t (seconds) of `movie` into a 565 frame
void subtitles_draw(const char *movie, double t, uint32_t pixels, uint32_t pitch, int w, int h) {
    for (int k = 0; k < nsubs; k++) {
        struct sub *s = &subs[k];
        if (t < s->start || t >= s->end || strcmp(s->movie, movie)) continue;
        int lines = 1;
        for (const char *p = s->text; *p; p++) lines += *p == '\n';
        // the game shows the movie shifted down, cutting its bottom rows: keep the text above them
        int y = h - 32 - lines * 16;
        const char *p = s->text;
        while (*p) {
            const char *e = strchr(p, '\n');
            size_t n = e ? (size_t)(e - p) : strlen(p);
            int x = (w - line_width(p, n)) / 2;
            for (int pass = 0; pass < 2; pass++)             // outline first, then the ink
                for (size_t i = 0, px = (size_t)x; i < n; i++) {
                    const uint8_t *rows;
                    int adv = vwf_glyph((unsigned char)p[i], &rows);
                    for (int r = 0; r < 16; r++)
                        for (int b = 0; b < 8; b++)
                            if (rows[r] & (0x80 >> b)) {
                                if (pass == 0) {
                                    for (int dy = -1; dy <= 1; dy++)
                                        for (int dx = -1; dx <= 1; dx++)
                                            put(pixels, pitch, w, h, (int)px + b + dx, y + r + dy, 0x0000);
                                } else put(pixels, pitch, w, h, (int)px + b, y + r, 0xffff);
                            }
                    px += (size_t)adv;
                }
            y += 16;
            p += n;
            if (*p == '\n') p++;
        }
        return;
    }
}

// ---------------------------------------------------------------- the opening quote (MV1C)
// The opening movie shows the Zhuangzi quote as Japanese text burned into the video, in four
// cards. The translation's English (data/en/opening.tsv, 10 lines) replaces it: each Japanese
// line's rows are blanked and the English line drawn there, as bright as the original text was
// in that frame, so the English fades in and out with the movie. Line bands are the PS1
// translation's measurements (src/injection_tools/fmv_layout.py there); the card frames (30 fps)
// were measured on the PC movie with P1_QUOTE_SCAN=1 (the PS1 numbers are 15 fps, and the PC
// movie fades the moon in behind the last card from frame 945 on: see MOON_FIRST).
static char quote[10][64];
static int quote_loaded;

static void quote_load(void) {
    quote_loaded = 1;
    char path[1200], line[512];
    snprintf(path, sizeof path, "%s/opening.tsv", path_en());
    FILE *f = fopen(path, "rb");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t');
        if (!tab || line[0] < '0' || line[0] > '9') continue;
        int k = atoi(line);
        char *text = strrchr(line, '\t') + 1;
        char *e = strpbrk(text, "\r\n");
        if (e) *e = 0;
        if (k >= 0 && k < 10) snprintf(quote[k], sizeof quote[k], "%s", text);
    }
    fclose(f);
}

#define MOON_FIRST 945
#define MOON_FADE 966
#define MOON_LAST 984

struct band { int y0, y1; };
static const struct {
    int first, last, nbands;
    struct band band[4];
    int line_of_band[4];        // English line drawn on each band, -1 = none
} CARDS[] = {
    { 40, 196, 1, { { 107, 120 } }, { 0, -1, -1, -1 } },
    { 204, 456, 4, { { 79, 92 }, { 99, 112 }, { 119, 132 }, { 139, 152 } }, { 1, 2, 3, -1 } },
    { 460, 676, 2, { { 85, 98 }, { 125, 138 } }, { 4, 5, -1, -1 } },
    { 680, MOON_LAST, 4, { { 83, 95 }, { 102, 115 }, { 122, 135 }, { 165, 178 } }, { 6, 7, 8, 9 } },
};
#define ATTR_RIGHT 286

// a + (b - a) * f / 256, per 565 channel (defined below)
static uint32_t mix565(uint32_t a, uint32_t b, int f);

// average 565 colour of rows y..y+2, columns x-3..x+3
static uint32_t avg565(uint32_t pixels, uint32_t pitch, int w, int x, int y) {
    int r = 0, g = 0, b = 0, n = 0;
    for (int yy = y; yy < y + 3; yy++)
        for (int xx = x - 3; xx <= x + 3; xx++) {
            if (xx < 0 || xx >= w) continue;
            uint32_t v = RD16(pixels + (uint32_t)yy * pitch + 2u * (uint32_t)xx);
            r += (int)(v >> 11); g += (int)((v >> 5) & 63); b += (int)(v & 31); n++;
        }
    return (uint32_t)((r / n) << 11 | (g / n) << 5 | (b / n));
}

// over the moon: fill the last card's text block (rows 77..140) and attribution (161..183) by
// interpolating between smoothed rows just outside them
static void repaint(uint32_t pixels, uint32_t pitch, int w, int h) {
    static const int span[2][2] = { { 76, 141 }, { 160, 184 } };     // exclusive ends
    if (h < 190) return;
    static uint32_t top[640], bot[640];
    for (int k = 0; k < 2; k++) {
        int a = span[k][0], z = span[k][1];
        for (int x = 0; x < w && x < 640; x++) {
            top[x] = avg565(pixels, pitch, w, x, a - 3);
            bot[x] = avg565(pixels, pitch, w, x, z + 1);
        }
        for (int y = a; y < z; y++)
            for (int x = 0; x < w && x < 640; x++)
                WR16(pixels + (uint32_t)y * pitch + 2u * (uint32_t)x,
                     mix565(top[x], bot[x], 256 * (y - a + 1) / (z - a + 1)));
    }
}

// a + (b - a) * f / 256, per 565 channel
static uint32_t mix565(uint32_t a, uint32_t b, int f) {
    int r = (int)(a >> 11) + (((int)(b >> 11) - (int)(a >> 11)) * f >> 8);
    int g = (int)((a >> 5) & 63) + (((int)((b >> 5) & 63) - (int)((a >> 5) & 63)) * f >> 8);
    int bl = (int)(a & 31) + (((int)(b & 31) - (int)(a & 31)) * f >> 8);
    return (uint32_t)(r << 11 | g << 5 | bl);
}

static void blend(uint32_t pixels, uint32_t pitch, int w, int h, int x, int y, uint16_t c, int alpha) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    uint32_t at = pixels + (uint32_t)y * pitch + 2u * (uint32_t)x;
    WR16(at, (uint16_t)(alpha >= 256 ? c : mix565(RD16(at), c, alpha)));
}

void quote_draw(const char *movie, double t, uint32_t pixels, uint32_t pitch, int w, int h) {
    if (strcmp(movie, "MV1C")) return;
    if (!quote_loaded) quote_load();
    int frame = (int)(t * 30.0 + 0.5);
    if (getenv("P1_QUOTE_SCAN")) {                          // debug: print the text rows of each frame
        fprintf(stderr, "QS %d", frame);
        int in = 0, start = 0, peak = 0;
        for (int y = 0; y <= h; y++) {
            int m = 0;
            if (y < h)
                for (int x = 0; x < w; x++) {
                    int g = (int)((RD16(pixels + (uint32_t)y * pitch + 2u * (uint32_t)x) >> 5) & 63);
                    if (g > m) m = g;
                }
            if (m > 12 && !in) { in = 1; start = y; peak = m; }
            else if (m > 12) { if (m > peak) peak = m; }
            else if (in) { in = 0; fprintf(stderr, " %d-%d:%d", start, y - 1, peak); }
        }
        int white = 0;                                      // near-white pixels (the text) in 80..180
        for (int y = 80; y < 180 && y < h; y++)
            for (int x = 0; x < w; x++) {
                uint32_t v = RD16(pixels + (uint32_t)y * pitch + 2u * (uint32_t)x);
                white += ((v >> 11) & 31) > 26 && ((v >> 5) & 63) > 54 && (v & 31) > 26;
            }
        fprintf(stderr, " W%d", white);
        fprintf(stderr, "\n");
        return;
    }
    for (size_t c = 0; c < sizeof CARDS / sizeof CARDS[0]; c++) {
        if (frame < CARDS[c].first || frame > CARDS[c].last) continue;
        // from MOON_FIRST the moon fades in behind the last card, whose text fades out by
        // MOON_LAST: there the text rows are repainted from the background above and below them
        int moon = frame >= MOON_FIRST;
        if (moon) repaint(pixels, pitch, w, h);
        int alpha = !moon ? 256 : frame <= MOON_FADE ? 256
                  : 256 * (MOON_LAST - frame) / (MOON_LAST - MOON_FADE);
        for (int b = 0; b < CARDS[c].nbands; b++) {
            int y0 = CARDS[c].band[b].y0, y1 = CARDS[c].band[b].y1;
            int peak = 0;
            if (moon)
                peak = 63;                                   // the rows were repainted above
            else
                // brightness of the Japanese line in this frame, then blank it
                for (int y = y0; y <= y1 && y < h; y++)
                    for (int x = 0; x < w; x++) {
                        uint32_t v = RD16(pixels + (uint32_t)y * pitch + 2u * (uint32_t)x);
                        int g = (int)((v >> 5) & 63);
                        if (g > peak) peak = g;
                        WR16(pixels + (uint32_t)y * pitch + 2u * (uint32_t)x, 0);
                    }
            int k = CARDS[c].line_of_band[b];
            if (k < 0 || !quote[k][0] || alpha <= 0) continue;
            const char *s = quote[k];
            int right = k == 9;                              // the attribution, right-aligned
            int width = line_width(s, strlen(s));
            int x = right ? ATTR_RIGHT - width : (w - width) / 2;
            int ty = y0 - 2;
            uint16_t r5 = (uint16_t)(peak * 31 / 63), g6 = (uint16_t)peak;
            uint16_t col = (uint16_t)(r5 << 11 | g6 << 5 | r5);
            for (int pass = moon ? 0 : 1; pass < 2; pass++)  // over the moon: outline, then ink
                for (size_t i = 0, px = (size_t)x; s[i]; i++) {
                    const uint8_t *rows;
                    int adv = vwf_glyph((unsigned char)s[i], &rows);
                    for (int r = 0; r < 16; r++)
                        for (int bit = 0; bit < 8; bit++) {
                            if (!(rows[r] & (0x80 >> bit))) continue;
                            for (int dy = pass ? 0 : -1; dy <= (pass ? 0 : 1); dy++)
                                for (int dx = pass ? 0 : -1; dx <= (pass ? 0 : 1); dx++)
                                    blend(pixels, pitch, w, h, (int)px + bit + dx, ty + r + dy,
                                          pass ? col : 0, alpha);
                        }
                    px += (size_t)adv;
                }
        }
        return;
    }
}
