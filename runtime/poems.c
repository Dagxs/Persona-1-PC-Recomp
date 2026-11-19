// English for the ending poems (MV1E, MV25): Japanese text burned into the video over a still
// picture that a black (MV1E) or grey (MV25) layer darkens while the text shows.
//
// As in the PS1 translation (docs/research/fmv-subs-in-engine.md there), the text block is
// rebuilt from a clean "plate" frame of the same still picture: every frame is fitted as
// frame = k * plate + lift on the rows above and below the text, and the block is painted as
// k * plate + lift. The English (data/en/poems.tsv, re-flowed to this font) is drawn over it, fading
// in and out with the original text. The plate is decoded from the movie file with a second
// decoder when the movie starts (MV25's clean frame comes after the text). Frame numbers (30 fps)
// were measured on the PC movies.
#include "rt.h"
#include "paths.h"
#include "../third_party/pl_mpeg/pl_mpeg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int vwf_glyph(int ch, const uint8_t **rows);

static const struct poem {
    const char *name;
    int plate;                 // a clean frame of the still picture
    int first, in_end;         // the text fades in
    int out_start, last;       // and out
    int y0, y1;                // the rows the Japanese text covers (with margin)
} POEMS[] = {
    { "MV1E", 85, 88, 125, 405, 452, 53, 185 },
    { "MV25", 388, 62, 90, 334, 372, 66, 172 },
};
#define NPOEMS (int)(sizeof POEMS / sizeof POEMS[0])
#define LINE_H 18
#define MAXL 12

static char text[NPOEMS][1024];          // the poem as one paragraph
static int loaded;
static uint8_t *plate[NPOEMS];           // RGBA, w * h
static int plate_w[NPOEMS], plate_h[NPOEMS], plate_tried[NPOEMS];

static void load(void) {
    loaded = 1;
    char path[1200], line[1024];
    snprintf(path, sizeof path, "%s/poems.tsv", path_en());
    FILE *f = fopen(path, "rb");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        *t1 = 0;
        char *t2 = strchr(t1 + 1, '\t');
        if (!t2) continue;
        char *s = t2 + 1, *e = strpbrk(s, "\r\n");
        if (e) *e = 0;
        for (int k = 0; k < NPOEMS; k++)
            if (!strcmp(line, POEMS[k].name) && strlen(text[k]) + strlen(s) + 2 < sizeof text[k]) {
                if (text[k][0]) strcat(text[k], " ");
                strcat(text[k], s);
            }
    }
    fclose(f);
}

static void get_plate(int k, const char *host) {
    plate_tried[k] = 1;
    plm_t *p = plm_create_with_filename(host);
    if (!p) return;
    plm_set_audio_enabled(p, 0);
    plm_set_loop(p, 0);
    plm_frame_t *f;
    while ((f = plm_decode_video(p))) {
        if ((int)(f->time * 30.0 + 0.5) >= POEMS[k].plate) {
            plate_w[k] = (int)f->width; plate_h[k] = (int)f->height;
            plate[k] = malloc((size_t)plate_w[k] * (size_t)plate_h[k] * 4);
            if (plate[k]) plm_frame_to_rgba(f, plate[k], plate_w[k] * 4);
            break;
        }
    }
    plm_destroy(p);
}

static int width_of(const char *s, size_t n) {
    int w = 0;
    for (size_t i = 0; i < n; i++) { const uint8_t *r; w += vwf_glyph((unsigned char)s[i], &r); }
    return w;
}

// word-wrap the paragraph to `room` pixels
static int wrap(const char *s, int room, const char *start[MAXL], size_t len[MAXL]) {
    int n = 0;
    while (*s && n < MAXL) {
        while (*s == ' ') s++;
        size_t best = 0, i = 0;
        for (;;) {
            while (s[i] && s[i] != ' ') i++;
            if (best && width_of(s, i) > room) break;
            best = i;
            if (!s[i]) break;
            i++;
        }
        if (!best) break;
        start[n] = s; len[n++] = best;
        s += best;
    }
    return n;
}

static void blend(uint8_t *rgba, int w, int h, int x, int y, int v, int a) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    uint8_t *p = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4;
    for (int c = 0; c < 3; c++) p[c] = (uint8_t)(p[c] + ((v - p[c]) * a >> 8));
}

// movie.c: every decoded frame, before it is shown (RGBA, w x h); host = the movie file
void poems_draw(const char *movie, const char *host, double t, uint8_t *rgba, int w, int h) {
    int k = 0;
    while (k < NPOEMS && strcmp(movie, POEMS[k].name)) k++;
    if (k == NPOEMS) return;
    if (!loaded) load();
    if (!text[k][0]) return;
    if (!plate_tried[k]) get_plate(k, host);
    const struct poem *P = &POEMS[k];
    int frame = (int)(t * 30.0 + 0.5);
    if (!plate[k] || plate_w[k] != w || plate_h[k] != h || frame < P->first || frame > P->last) return;
    const uint8_t *pl = plate[k];

    // fit frame = k * plate + lift on the rows clear of the text
    double sp = 0, sx = 0, spp = 0, spx = 0, n = 0;
    for (int y = 0; y < h; y += 2) {
        if (y >= P->y0 - 4 && y <= P->y1 + 4) continue;
        for (int x = 0; x < w; x += 2)
            for (int c = 0; c < 3; c++) {
                double a = pl[((size_t)y * w + x) * 4 + c], b = rgba[((size_t)y * w + x) * 4 + c];
                sp += a; sx += b; spp += a * a; spx += a * b; n++;
            }
    }
    double den = n * spp - sp * sp, kk = 1, lift = 0;
    if (den > 1e-6) { kk = (n * spx - sp * sx) / den; lift = (sx - kk * sp) / n; }
    for (int y = P->y0; y <= P->y1 && y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 3; c++) {
                double v = kk * pl[((size_t)y * w + x) * 4 + c] + lift;
                rgba[((size_t)y * w + x) * 4 + c] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5);
            }

    // the English, faded like the original text
    int a = 256;
    if (frame < P->in_end) a = 256 * (frame - P->first) / (P->in_end - P->first);
    if (frame > P->out_start) a = 256 * (P->last - frame) / (P->last - P->out_start);
    if (a <= 0) return;
    const char *ls[MAXL];
    size_t ll[MAXL];
    int nl = wrap(text[k], w - 16, ls, ll);
    int top = (P->y0 + P->y1) / 2 - nl * LINE_H / 2;
    static uint8_t mask[18][1024];           // per line: 2 = ink, 1 = outline
    for (int li = 0; li < nl; li++) {
        memset(mask, 0, sizeof mask);
        int x0 = (w - width_of(ls[li], ll[li])) / 2, y0 = top + li * LINE_H - 1;
        for (size_t i = 0, x = 1; i < ll[li]; i++) {
            const uint8_t *rows;
            int adv = vwf_glyph((unsigned char)ls[li][i], &rows);
            for (int r = 0; r < 16; r++)
                for (int b = 0; b < 8; b++)
                    if (rows[r] & (0x80 >> b))
                        for (int dy = 0; dy <= 2; dy++)
                            for (int dx = -1; dx <= 1; dx++) {
                                size_t xx = x + (size_t)b + (size_t)(dx + 1);
                                if (xx >= 1024) continue;
                                uint8_t v = dx == 0 && dy == 1 ? 2 : 1;
                                if (mask[r + dy][xx] < v) mask[r + dy][xx] = v;
                            }
            x += (size_t)adv;
        }
        for (int r = 0; r < 18; r++)
            for (int xx = 0; xx < 1024 && xx < w + 2; xx++)
                if (mask[r][xx]) blend(rgba, w, h, x0 + xx - 2, y0 + r, mask[r][xx] == 2 ? 255 : 0,
                                       mask[r][xx] == 2 ? a : a * 3 / 4);
    }
}

// debug: P1_POEMTEST="cd/relative/movie.STR|dir|f1,f2,..." writes those frames, composited, as BMPs
void poems_test(void) {
    const char *spec = getenv("P1_POEMTEST");
    if (!spec) return;
    char rel[256], dir[512], frames[512];
    if (sscanf(spec, "%255[^|]|%511[^|]|%511s", rel, dir, frames) != 3) return;
    char host[1200], name[16];
    snprintf(host, sizeof host, "%s/%s", path_cd(), rel);
    const char *b = rel;
    for (const char *q = rel; *q; q++) if (*q == '/' || *q == '\\') b = q + 1;
    size_t j = 0;
    for (; b[j] && b[j] != '.' && j + 1 < sizeof name; j++) name[j] = b[j];
    name[j] = 0;
    plm_t *p = plm_create_with_filename(host);
    if (!p) { fprintf(stderr, "poems_test: cannot open %s\n", host); exit(1); }
    plm_set_audio_enabled(p, 0);
    plm_frame_t *f;
    uint8_t *rgba = 0;
    int nf = 0;
    double last = 0;
    while ((f = plm_decode_video(p))) {
        nf++; last = f->time;
        int w = (int)f->width, h = (int)f->height, fr = (int)(f->time * 30.0 + 0.5);
        char key[16];
        snprintf(key, sizeof key, "%d", fr);
        int want = 0;
        for (const char *q = frames; *q;) {
            size_t n = strcspn(q, ",");
            if (n == strlen(key) && !strncmp(q, key, n)) want = 1;
            q += n + (q[n] == ',');
        }
        if (!want) continue;
        if (!rgba) rgba = malloc((size_t)w * h * 4);
        plm_frame_to_rgba(f, rgba, w * 4);
        poems_draw(name, host, f->time, rgba, w, h);
        char out[700];
        snprintf(out, sizeof out, "%s/%s_%04d.bmp", dir, name, fr);
        FILE *o = fopen(out, "wb");
        fprintf(stderr, "poems_test: frame %d -> %s%s\n", fr, out, o ? "" : " (cannot write)");
        if (!o) continue;
        uint32_t row = (uint32_t)w * 3, sz = 54 + row * (uint32_t)h;
        uint8_t hd[54] = { 'B', 'M' };
        memcpy(hd + 2, &sz, 4); hd[10] = 54; hd[14] = 40;
        memcpy(hd + 18, &w, 4); memcpy(hd + 22, &h, 4); hd[26] = 1; hd[28] = 24;
        fwrite(hd, 1, 54, o);
        for (int y = h - 1; y >= 0; y--)
            for (int x = 0; x < w; x++) {
                uint8_t *px = rgba + ((size_t)y * w + x) * 4, bgr[3] = { px[2], px[1], px[0] };
                fwrite(bgr, 1, 3, o);
            }
        fclose(o);
    }
    fprintf(stderr, "poems_test: %s: %d frames, last at %.3f s\n", host, nf, last);
    plm_destroy(p);
    exit(0);
}
