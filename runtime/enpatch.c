// Builds the English overlay (path_en()) from assets/english.p1patch and the installed disc files,
// so English needs no Python and no copy of the translation repository (tools/make_en_patch.py
// describes the format). Runs at startup; a patch.id file records which patch built the overlay,
// so it is only rebuilt when the patch changes.
#include "rt.h"
#include "paths.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc32_buf(const uint8_t *p, size_t n) {
    uint32_t c = 0xffffffffu;
    for (size_t k = 0; k < n; k++) {
        c ^= p[k];
        for (int b = 0; b < 8; b++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

// assets/english.p1patch: in the working directory, next to the program, or one level up
// (a build directory inside the source tree)
static int find_patch(char *out, size_t cap) {
    const char *base = SDL_GetBasePath();
    const char *cands[3] = { "assets/english.p1patch", "assets/english.p1patch", "../assets/english.p1patch" };
    for (int k = 0; k < 3; k++) {
        if (k == 0) snprintf(out, cap, "%s", cands[0]);
        else snprintf(out, cap, "%s%s", base ? base : "", cands[k]);
        if (SDL_GetPathInfo(out, NULL)) return 1;
    }
    return 0;
}

int english_patch_present(void) {
    char p[1100];
    return find_patch(p, sizeof p);
}

// Apply the patch if the overlay is missing or was built from another patch. Returns 1 when the
// overlay is ready.
int english_prepare(void) {
    char ppath[1100];
    if (!find_patch(ppath, sizeof ppath)) return SDL_GetPathInfo(path_en(), NULL);
    size_t n;
    uint8_t *p = SDL_LoadFile(ppath, &n);
    if (!p || n < 12 || memcmp(p, "P1EN", 4)) { SDL_free(p); return 0; }
    char idpath[1200], id[32];
    snprintf(idpath, sizeof idpath, "%s/patch.id", path_en());
    snprintf(id, sizeof id, "%08x %zu", crc32_buf(p, n), n);
    size_t idn;
    char *old = SDL_LoadFile(idpath, &idn);
    int current = old && idn == strlen(id) && !memcmp(old, id, idn);
    SDL_free(old);
    if (current) { SDL_free(p); return 1; }

    uint32_t files = u32(p + 8);
    size_t pos = 12;
    int ok = 1;
    for (uint32_t f = 0; ok && f < files; f++) {
        char name[512], src[512];
        uint16_t L = u16(p + pos); pos += 2;
        snprintf(name, sizeof name, "%.*s", L, (const char *)p + pos); pos += L;
        L = u16(p + pos); pos += 2;
        snprintf(src, sizeof src, "%.*s", L, (const char *)p + pos); pos += L;
        uint32_t ssize = u32(p + pos), scrc = u32(p + pos + 4), dsize = u32(p + pos + 8), dcrc = u32(p + pos + 12);
        uint32_t nops = u32(p + pos + 16);
        pos += 20;
        uint8_t *sdata = NULL;
        size_t slen = 0;
        if (src[0]) {
            char sp[1200];
            snprintf(sp, sizeof sp, "%s/%s", path_cd(), src);
            sdata = SDL_LoadFile(sp, &slen);
            if (!sdata || slen != ssize || crc32_buf(sdata, slen) != scrc) {
                fprintf(stderr, "English patch: %s on the disc is not the expected version\n", src);
                ok = 0;
            }
        }
        uint8_t *out = malloc(dsize ? dsize : 1);
        size_t o = 0;
        for (uint32_t k = 0; k < nops; k++) {
            uint8_t t = p[pos++];
            if (t == 0) {
                uint32_t so = u32(p + pos), len = u32(p + pos + 4);
                pos += 8;
                if (ok && sdata && so + len <= slen && o + len <= dsize) memcpy(out + o, sdata + so, len);
                o += len;
            } else {
                uint32_t len = u32(p + pos);
                pos += 4;
                if (o + len <= dsize) memcpy(out + o, p + pos, len);
                pos += len;
                o += len;
            }
        }
        if (ok && (o != dsize || crc32_buf(out, dsize) != dcrc)) {
            fprintf(stderr, "English patch: %s did not rebuild correctly\n", name);
            ok = 0;
        }
        if (ok) {
            char dp[1200];
            snprintf(dp, sizeof dp, "%s/%s", path_en(), name);
            char dir[1200];
            snprintf(dir, sizeof dir, "%s", dp);
            char *slash = strrchr(dir, '/');
            if (slash) { *slash = 0; SDL_CreateDirectory(dir); }
            if (!SDL_SaveFile(dp, out, dsize)) { fprintf(stderr, "English patch: cannot write %s\n", dp); ok = 0; }
        }
        free(out);
        SDL_free(sdata);
    }
    SDL_free(p);
    if (ok) SDL_SaveFile(idpath, id, strlen(id));
    return ok;
}
