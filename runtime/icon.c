// The window icon: the original port's own icon, read from the player's Persona.exe (its first
// RT_ICON resource: a BITMAPINFOHEADER image, palette, XOR pixels, AND mask) when the window is
// made. Nothing of it is in this repository.
#include "paths.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

// the resource directory entry of `id` (or the first one when id < 0) -> its offset field
static const uint8_t *rsrc_entry(const uint8_t *rs, size_t n, uint32_t dir, int id) {
    if (dir + 16 > n) return 0;
    int named = u16(rs + dir + 12), ids = u16(rs + dir + 14);
    for (int k = 0; k < named + ids; k++) {
        uint32_t e = dir + 16 + 8u * (uint32_t)k;
        if (e + 8 > n) return 0;
        if (k < named) continue;
        if (id < 0 || u32(rs + e) == (uint32_t)id) return rs + e + 4;
    }
    return 0;
}

void host_window_icon(SDL_Window *w) {
    FILE *f = fopen(path_exe(), "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *pe = size > 0 && size < (64 << 20) ? malloc((size_t)size) : 0;
    if (!pe || fread(pe, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(pe); return; }
    fclose(f);
    SDL_Surface *s = 0;
    do {
        if (size < 0x40 || pe[0] != 'M' || pe[1] != 'Z') break;
        uint32_t nt = u32(pe + 0x3c);
        if (nt + 0x100 > (uint32_t)size || memcmp(pe + nt, "PE\0\0", 4)) break;
        uint32_t nsec = u16(pe + nt + 6), opt = nt + 24, optsz = u16(pe + nt + 20);
        uint32_t rva = u32(pe + opt + 96 + 8 * 2);                   // IMAGE_DIRECTORY_ENTRY_RESOURCE
        uint32_t sec = opt + optsz, raw = 0, vbase = 0, vsize = 0;
        for (uint32_t k = 0; k < nsec; k++, sec += 40) {
            uint32_t va = u32(pe + sec + 12), vs = u32(pe + sec + 8), rp = u32(pe + sec + 20);
            if (rva >= va && rva < va + vs) { raw = rp; vbase = va; vsize = vs; break; }
        }
        if (!raw || raw + vsize > (uint32_t)size) break;
        const uint8_t *rs = pe + raw + (rva - vbase);
        size_t n = vsize - (rva - vbase);
        const uint8_t *e = rsrc_entry(rs, n, 0, 3);                  // RT_ICON
        if (!e || !(u32(e) & 0x80000000u)) break;
        e = rsrc_entry(rs, n, u32(e) & 0x7fffffffu, -1);             // the first icon
        if (!e || !(u32(e) & 0x80000000u)) break;
        e = rsrc_entry(rs, n, u32(e) & 0x7fffffffu, -1);             // its first language
        if (!e || (u32(e) & 0x80000000u) || u32(e) + 16 > n) break;
        uint32_t drva = u32(rs + u32(e)), dsize = u32(rs + u32(e) + 4);
        if (drva < vbase || drva - vbase + dsize > vsize) break;
        const uint8_t *bmp = pe + raw + (drva - vbase);
        int iw = (int)u32(bmp + 4), ih = (int)u32(bmp + 8) / 2, bpp = u16(bmp + 14);
        uint32_t hdr = u32(bmp), ncol = u32(bmp + 32);
        if (iw <= 0 || iw > 256 || ih <= 0 || ih > 256 || (bpp != 4 && bpp != 8)) break;
        if (!ncol) ncol = 1u << bpp;
        const uint8_t *pal = bmp + hdr, *xor_ = pal + 4 * ncol;
        uint32_t xstride = (uint32_t)((iw * bpp + 31) / 32 * 4), astride = (uint32_t)((iw + 31) / 32 * 4);
        const uint8_t *and_ = xor_ + xstride * (uint32_t)ih;
        if (and_ + astride * (uint32_t)ih > bmp + dsize) break;
        s = SDL_CreateSurface(iw, ih, SDL_PIXELFORMAT_RGBA32);
        if (!s) break;
        for (int y = 0; y < ih; y++) {
            const uint8_t *xr = xor_ + xstride * (uint32_t)(ih - 1 - y), *ar = and_ + astride * (uint32_t)(ih - 1 - y);
            uint8_t *o = (uint8_t *)s->pixels + (size_t)y * (size_t)s->pitch;
            for (int x = 0; x < iw; x++) {
                int c = bpp == 8 ? xr[x] : (xr[x / 2] >> (x & 1 ? 0 : 4)) & 15;
                const uint8_t *p = pal + 4 * (uint32_t)c;
                o[4 * x] = p[2]; o[4 * x + 1] = p[1]; o[4 * x + 2] = p[0];
                o[4 * x + 3] = (ar[x / 8] >> (7 - x % 8)) & 1 ? 0 : 255;
            }
        }
    } while (0);
    free(pe);
    if (s) { SDL_SetWindowIcon(w, s); SDL_DestroySurface(s); }
}
