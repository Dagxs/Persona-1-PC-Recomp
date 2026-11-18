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
