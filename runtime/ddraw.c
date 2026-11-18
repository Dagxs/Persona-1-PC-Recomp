// DirectDraw (DirectX 5 interfaces). The display is 640x480 at 16 bits (RGB565); every surface's
// pixels live in guest memory, so Lock hands the game a real pointer and the Direct3D rasterizer
// (d3d.c) draws straight into the render target. Flip converts the back buffer and presents it.
#include "com.h"
#include "ddraw.h"
#include "host.h"
#include <stdlib.h>
#include <string.h>

static const char *const DD_METHODS[] = {
    "QueryInterface", "AddRef", "Release", "Compact", "CreateClipper", "CreatePalette", "CreateSurface",
    "DuplicateSurface", "EnumDisplayModes", "EnumSurfaces", "FlipToGDISurface", "GetCaps", "GetDisplayMode",
    "GetFourCCCodes", "GetGDISurface", "GetMonitorFrequency", "GetScanLine", "GetVerticalBlankStatus",
    "Initialize", "RestoreDisplayMode", "SetCooperativeLevel", "SetDisplayMode", "WaitForVerticalBlank",
    "GetAvailableVidMem" };
static const char *const DDS_METHODS[] = {
    "QueryInterface", "AddRef", "Release", "AddAttachedSurface", "AddOverlayDirtyRect", "Blt", "BltBatch",
    "BltFast", "DeleteAttachedSurface", "EnumAttachedSurfaces", "EnumOverlayZOrders", "Flip",
    "GetAttachedSurface", "GetBltStatus", "GetCaps", "GetClipper", "GetColorKey", "GetDC", "GetFlipStatus",
    "GetOverlayPosition", "GetPalette", "GetPixelFormat", "GetSurfaceDesc", "Initialize", "IsLost", "Lock",
    "ReleaseDC", "Restore", "SetClipper", "SetColorKey", "SetOverlayPosition", "SetPalette", "Unlock",
    "UpdateOverlay", "UpdateOverlayDisplay", "UpdateOverlayZOrder",
    "GetDDInterface", "PageLock", "PageUnlock", "SetSurfaceDesc" };
static const char *const CLIP_METHODS[] = {
    "QueryInterface", "AddRef", "Release", "GetClipList", "GetHWnd", "Initialize", "IsClipListChanged",
    "SetClipList", "SetHWnd" };
static const char *const PAL_METHODS[] = {
    "QueryInterface", "AddRef", "Release", "GetCaps", "GetEntries", "Initialize", "SetEntries" };
#define N(x) ((int)(sizeof x / sizeof x[0]))

static uint32_t vt_dd, vt_dd2, vt_surf[3], vt_clip, vt_pal;
static struct dd *the_dd;
struct dd_surface *dd_primary;

// ---------------------------------------------------------------- pixel formats
const struct pixfmt PF_565 = { 16, 0xf800, 0x07e0, 0x001f, 0, 0 };
static void write_pixfmt(uint32_t p, const struct pixfmt *f) {
    memset(MEM + p, 0, 32);
    WR32(p, 32);
    WR32(p + 4, f->pal8 ? 0x20u : 0x40u | (f->a ? 1u : 0u));      // DDPF_PALETTEINDEXED8 / DDPF_RGB | ALPHAPIXELS
    WR32(p + 12, f->bpp);
    WR32(p + 16, f->r); WR32(p + 20, f->g); WR32(p + 24, f->b); WR32(p + 28, f->a);
}
static void read_pixfmt(uint32_t p, struct pixfmt *f) {
    memset(f, 0, sizeof *f);
    uint32_t flags = RD32(p + 4);
    f->bpp = RD32(p + 12);
    if (flags & 0x20u) { f->pal8 = 1; f->bpp = 8; return; }
    f->r = RD32(p + 16); f->g = RD32(p + 20); f->b = RD32(p + 24);
    f->a = (flags & 1u) ? RD32(p + 28) : 0;
}

// ---------------------------------------------------------------- surfaces
static struct dd_surface *surf_of(uint32_t obj) { return com_host(obj); }

static struct dd_surface *surface_new(uint32_t w, uint32_t h, const struct pixfmt *f, uint32_t caps) {
    struct dd_surface *s = calloc(1, sizeof *s);
    s->w = w; s->h = h; s->fmt = *f; s->caps = caps;
    uint32_t bpp = (caps & DDSCAPS_ZBUFFER) ? 16 : f->bpp;
    s->pitch = (w * (bpp / 8) + 3u) & ~3u;
    s->pixels = guest_heap_alloc(s->pitch * h, 1);
    for (int k = 0; k < 3; k++) s->obj[k] = com_new(vt_surf[k], s);
    if (rt_trace) fprintf(stderr, "surface %ux%u bpp %u caps %x at %08x\n", w, h, bpp, caps, s->pixels);
    return s;
}

static void fill_desc(uint32_t p, struct dd_surface *s) {
    memset(MEM + p + 4, 0, 104);
    WR32(p, 108);
    WR32(p + 4, 0x1u | 0x2u | 0x4u | 0x8u | 0x1000u | 0x800u);
    WR32(p + 8, s->h); WR32(p + 12, s->w); WR32(p + 16, s->pitch);
    WR32(p + 36, s->pixels);
    if (s->caps & DDSCAPS_ZBUFFER) { WR32(p + 24, 16); WR32(p + 4, RD32(p + 4) | 0x40u); }
    if (s->ck_src_set) { WR32(p + 64, s->ck_src_lo); WR32(p + 68, s->ck_src_hi); WR32(p + 4, RD32(p + 4) | 0x10000u); }
    write_pixfmt(p + 72, &s->fmt);
    WR32(p + 104, s->caps);
}

static uint32_t dd_CreateSurface(const uint32_t *a) {
    uint32_t d = a[1];
    uint32_t flags = RD32(d + 4), caps = (flags & 1u) ? RD32(d + 104) : 0;
    uint32_t w = RD32(d + 12), h = RD32(d + 8);
    struct pixfmt f = PF_565;
    if (flags & 0x1000u) read_pixfmt(d + 72, &f);
    struct dd_surface *s;
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        s = surface_new(640, 480, &PF_565, caps | DDSCAPS_FRONTBUFFER | DDSCAPS_VIDEOMEMORY);
        dd_primary = s;
        if (flags & 0x20u && RD32(d + 20)) {            // flipping chain with a back buffer
            uint32_t bc = (caps & ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER)) | DDSCAPS_BACKBUFFER | DDSCAPS_VIDEOMEMORY;
            s->back = surface_new(640, 480, &PF_565, bc);
        }
    } else {
        if (caps & DDSCAPS_ZBUFFER) caps |= DDSCAPS_VIDEOMEMORY;
        else if (!(caps & DDSCAPS_SYSTEMMEMORY)) caps |= DDSCAPS_VIDEOMEMORY;
        s = surface_new(w, h, &f, caps);
    }
    if (flags & 0x10000u) { s->ck_src_set = 1; s->ck_src_lo = RD32(d + 64); s->ck_src_hi = RD32(d + 68); }
    WR32(a[2], s->obj[0]);
    return DD_OK;
}

static uint32_t surf_QueryInterface(const uint32_t *a) {
    struct dd_surface *s = surf_of(a[0]);
    uint32_t r = 0;
    if (guid_eq(a[1], IID_IDirectDrawSurface)) r = s->obj[0];
    else if (guid_eq(a[1], IID_IDirectDrawSurface2)) r = s->obj[1];
    else if (guid_eq(a[1], IID_IDirectDrawSurface3)) r = s->obj[2];
    else if (guid_eq(a[1], IID_IDirect3DTexture2) || guid_eq(a[1], IID_IDirect3DTexture)) r = d3d_texture_of(s);
    else if (guid_eq(a[1], IID_IDirect3DHALDevice) || guid_eq(a[1], IID_IDirect3DRGBDevice)) r = 0;
    if (rt_trace) { char g[40]; guid_str(a[1], g); fprintf(stderr, "IDirectDrawSurface::QueryInterface(%s) -> %08x\n", g, r); }
    WR32(a[2], r);
    return r ? DD_OK : E_NOINTERFACE;
}
static uint32_t surf_Release(const uint32_t *a) { return com_release(a); }  // memory is never reclaimed

static uint32_t surf_GetAttachedSurface(const uint32_t *a) {
    struct dd_surface *s = surf_of(a[0]);
    uint32_t want = RD32(a[1]);
    struct dd_surface *r = 0;
    if (want & DDSCAPS_BACKBUFFER) r = s->back;
    else if (want & DDSCAPS_ZBUFFER) r = s->zbuf;
    if (!r) { WR32(a[2], 0); return 0x887600ffu; }      // DDERR_NOTFOUND
    WR32(a[2], r->obj[0]);
    return DD_OK;
}
static uint32_t surf_AddAttachedSurface(const uint32_t *a) {
    struct dd_surface *s = surf_of(a[0]), *t = surf_of(a[1]);
    if (t->caps & DDSCAPS_ZBUFFER) s->zbuf = t;
    else rt_fatal("AddAttachedSurface of a non-z-buffer surface");
    return DD_OK;
}
static uint32_t surf_GetSurfaceDesc(const uint32_t *a) { fill_desc(a[1], surf_of(a[0])); return DD_OK; }
static uint32_t surf_GetPixelFormat(const uint32_t *a) { write_pixfmt(a[1], &surf_of(a[0])->fmt); return DD_OK; }
static uint32_t surf_GetCaps(const uint32_t *a) { WR32(a[1], surf_of(a[0])->caps); return DD_OK; }
static uint32_t surf_Lock(const uint32_t *a) {
    struct dd_surface *s = surf_of(a[0]);
    fill_desc(a[2], s);
    if (a[1]) {                                 // lock a sub-rectangle: point at its top-left
        uint32_t x = RD32(a[1]), y = RD32(a[1] + 4);
        WR32(a[2] + 36, s->pixels + y * s->pitch + x * (s->fmt.bpp / 8));
    }
    return DD_OK;
}
static uint32_t surf_Unlock(const uint32_t *a) { (void)a; return DD_OK; }
static uint32_t surf_IsLost(const uint32_t *a) { (void)a; return DD_OK; }
static uint32_t surf_Restore(const uint32_t *a) { (void)a; return DD_OK; }
static uint32_t surf_SetColorKey(const uint32_t *a) {
    struct dd_surface *s = surf_of(a[0]);
    if (a[1] & 8u) {                              // DDCKEY_SRCBLT
        if (a[2]) { s->ck_src_set = 1; s->ck_src_lo = RD32(a[2]); s->ck_src_hi = RD32(a[2] + 4); }
        else s->ck_src_set = 0;
    }
    return DD_OK;
}
static uint32_t surf_GetColorKey(const uint32_t *a) {
    struct dd_surface *s = surf_of(a[0]);
    if (!(a[1] & 8u) || !s->ck_src_set) return 0x88760226u;  // DDERR_NOCOLORKEY
    WR32(a[2], s->ck_src_lo); WR32(a[2] + 4, s->ck_src_hi);
    return DD_OK;
}
static uint32_t surf_SetPalette(const uint32_t *a) { surf_of(a[0])->palette = a[1] ? com_host(a[1]) : 0; return DD_OK; }
static uint32_t surf_SetClipper(const uint32_t *a) { (void)a; return DD_OK; }
static uint32_t surf_GetDC(const uint32_t *a) { WR32(a[1], 0x20002u); return DD_OK; }
static uint32_t surf_ReleaseDC(const uint32_t *a) { (void)a; return DD_OK; }
static uint32_t surf_GetBltStatus(const uint32_t *a) { (void)a; return DD_OK; }
static uint32_t surf_GetFlipStatus(const uint32_t *a) { (void)a; return DD_OK; }
static uint32_t surf_PageLock(const uint32_t *a) { (void)a; return DD_OK; }

// ---------------------------------------------------------------- blits
static inline uint32_t px_get(struct dd_surface *s, uint32_t x, uint32_t y) {
    uint32_t p = s->pixels + y * s->pitch;
    switch (s->fmt.bpp) { case 8: return RD8(p + x); case 16: return RD16(p + 2 * x); default: return RD32(p + 4 * x); }
}
static inline void px_put(struct dd_surface *s, uint32_t x, uint32_t y, uint32_t v) {
    uint32_t p = s->pixels + y * s->pitch;
    switch (s->fmt.bpp) { case 8: WR8(p + x, v); break; case 16: WR16(p + 2 * x, v); break; default: WR32(p + 4 * x, v); }
}
static void rect_of(uint32_t p, struct dd_surface *s, int32_t *r) {
    if (p) { r[0] = (int32_t)RD32(p); r[1] = (int32_t)RD32(p + 4); r[2] = (int32_t)RD32(p + 8); r[3] = (int32_t)RD32(p + 12); }
    else { r[0] = 0; r[1] = 0; r[2] = (int32_t)s->w; r[3] = (int32_t)s->h; }
}
static void blit(struct dd_surface *d, const int32_t *dr, struct dd_surface *s, const int32_t *sr, int key) {
    int32_t dw = dr[2] - dr[0], dh = dr[3] - dr[1], sw = sr[2] - sr[0], sh = sr[3] - sr[1];
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    for (int32_t y = 0; y < dh; y++) {
        int32_t dy = dr[1] + y, sy = sr[1] + (int32_t)((int64_t)y * sh / dh);
        if (dy < 0 || dy >= (int32_t)d->h || sy < 0 || sy >= (int32_t)s->h) continue;
        for (int32_t x = 0; x < dw; x++) {
            int32_t dx = dr[0] + x, sx = sr[0] + (int32_t)((int64_t)x * sw / dw);
            if (dx < 0 || dx >= (int32_t)d->w || sx < 0 || sx >= (int32_t)s->w) continue;
            uint32_t v = px_get(s, (uint32_t)sx, (uint32_t)sy);
            if (key && v >= s->ck_src_lo && v <= s->ck_src_hi) continue;
            px_put(d, (uint32_t)dx, (uint32_t)dy, v);
        }
    }
}
static uint32_t surf_Blt(const uint32_t *a) {
    struct dd_surface *d = surf_of(a[0]);
    uint32_t flags = a[4];
    int32_t dr[4]; rect_of(a[1], d, dr);
    if (flags & 0x400u) {                         // DDBLT_COLORFILL
        uint32_t c = RD32(a[5] + 80);
        for (int32_t y = dr[1] < 0 ? 0 : dr[1]; y < dr[3] && y < (int32_t)d->h; y++)
            for (int32_t x = dr[0] < 0 ? 0 : dr[0]; x < dr[2] && x < (int32_t)d->w; x++) px_put(d, (uint32_t)x, (uint32_t)y, c);
        return DD_OK;
    }
    if (flags & 0x800u && !a[2]) {                // DDBLT_DEPTHFILL on a z-buffer
        uint32_t c = RD32(a[5] + 80);
        for (uint32_t y = 0; y < d->h; y++) for (uint32_t x = 0; x < d->w; x++) WR16(d->pixels + y * d->pitch + 2 * x, c);
        return DD_OK;
    }
    if (!a[2]) rt_fatal("Blt without a source, flags %x", flags);
    struct dd_surface *s = surf_of(a[2]);
    int32_t sr[4]; rect_of(a[3], s, sr);
    blit(d, dr, s, sr, (flags & 0x8000u) != 0);   // DDBLT_KEYSRC
    return DD_OK;
}
static uint32_t surf_BltFast(const uint32_t *a) {
    struct dd_surface *d = surf_of(a[0]), *s = surf_of(a[3]);
    int32_t sr[4]; rect_of(a[4], s, sr);
    int32_t dr[4] = { (int32_t)a[1], (int32_t)a[2], (int32_t)a[1] + sr[2] - sr[0], (int32_t)a[2] + sr[3] - sr[1] };
    blit(d, dr, s, sr, (a[5] & 1u) != 0);         // DDBLTFAST_SRCCOLORKEY
    return DD_OK;
}

// ---------------------------------------------------------------- presentation
static uint32_t present_buf[640 * 480];
void dd_present(struct dd_surface *s) {
    for (uint32_t y = 0; y < s->h && y < 480; y++) {
        const uint8_t *row = MEM + s->pixels + y * s->pitch;
        for (uint32_t x = 0; x < s->w && x < 640; x++) {
            uint32_t v = (uint32_t)row[2 * x] | ((uint32_t)row[2 * x + 1] << 8);
            uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
            present_buf[y * 640 + x] = ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
        }
    }
    host_present(present_buf, 640, 480);
    host_pump_events();
    frame_counter++;
    dump_frame_if_requested(present_buf, 640, 480);
}
uint32_t frame_counter;
static uint32_t surf_Flip(const uint32_t *a) {
    struct dd_surface *s = surf_of(a[0]);
    dd_present(s->back ? s->back : s);
    rt_yield();
    return DD_OK;
}

static const struct com_method SURF_IMPL[] = {
    { "QueryInterface", surf_QueryInterface, 3 }, { "AddRef", com_addref, 1 }, { "Release", surf_Release, 1 },
    { "AddAttachedSurface", surf_AddAttachedSurface, 2 }, { "Blt", surf_Blt, 6 }, { "BltFast", surf_BltFast, 6 },
    { "Flip", surf_Flip, 3 }, { "GetAttachedSurface", surf_GetAttachedSurface, 3 },
    { "GetBltStatus", surf_GetBltStatus, 2 }, { "GetCaps", surf_GetCaps, 2 }, { "GetColorKey", surf_GetColorKey, 3 },
    { "GetDC", surf_GetDC, 2 }, { "GetFlipStatus", surf_GetFlipStatus, 2 }, { "GetPixelFormat", surf_GetPixelFormat, 2 },
    { "GetSurfaceDesc", surf_GetSurfaceDesc, 2 }, { "IsLost", surf_IsLost, 1 }, { "Lock", surf_Lock, 5 },
    { "ReleaseDC", surf_ReleaseDC, 2 }, { "Restore", surf_Restore, 1 }, { "SetClipper", surf_SetClipper, 2 },
    { "SetColorKey", surf_SetColorKey, 3 }, { "SetPalette", surf_SetPalette, 2 }, { "Unlock", surf_Unlock, 2 },
    { "PageLock", surf_PageLock, 2 }, { "PageUnlock", surf_PageLock, 2 },
    { 0 } };

// ---------------------------------------------------------------- clipper, palette
static uint32_t dd_CreateClipper(const uint32_t *a) { WR32(a[2], com_new(vt_clip, 0)); return DD_OK; }
static uint32_t clip_SetHWnd(const uint32_t *a) { (void)a; return DD_OK; }
static const struct com_method CLIP_IMPL[] = {
    { "AddRef", com_addref, 1 }, { "Release", com_release, 1 }, { "SetHWnd", clip_SetHWnd, 3 }, { 0 } };

struct dd_palette { uint32_t entries[256]; };
static uint32_t dd_CreatePalette(const uint32_t *a) {
    struct dd_palette *p = calloc(1, sizeof *p);
    if (a[2]) memcpy(p->entries, MEM + a[2], 1024);
    WR32(a[3], com_new(vt_pal, p));
    return DD_OK;
}
static uint32_t pal_SetEntries(const uint32_t *a) {
    struct dd_palette *p = com_host(a[0]);
    for (uint32_t k = 0; k < a[3] && a[2] + k < 256; k++) p->entries[a[2] + k] = RD32(a[4] + 4 * k);
    return DD_OK;
}
static uint32_t pal_GetEntries(const uint32_t *a) {
    struct dd_palette *p = com_host(a[0]);
    for (uint32_t k = 0; k < a[3] && a[2] + k < 256; k++) WR32(a[4] + 4 * k, p->entries[a[2] + k]);
    return DD_OK;
}
const uint32_t *dd_palette_entries(void *pal) { return pal ? ((struct dd_palette *)pal)->entries : 0; }
static const struct com_method PAL_IMPL[] = {
    { "AddRef", com_addref, 1 }, { "Release", com_release, 1 }, { "SetEntries", pal_SetEntries, 5 },
    { "GetEntries", pal_GetEntries, 5 }, { 0 } };

// ---------------------------------------------------------------- IDirectDraw
struct dd { uint32_t obj_dd, obj_dd2, obj_d3d2; };

static uint32_t dd_QueryInterface(const uint32_t *a) {
    struct dd *d = com_host(a[0]);
    uint32_t r = 0;
    if (guid_eq(a[1], IID_IDirectDraw)) r = d->obj_dd;
    else if (guid_eq(a[1], IID_IDirectDraw2)) r = d->obj_dd2;
    else if (guid_eq(a[1], IID_IDirect3D2)) r = d->obj_d3d2;
    if (rt_trace) { char g[40]; guid_str(a[1], g); fprintf(stderr, "IDirectDraw::QueryInterface(%s) -> %08x\n", g, r); }
    WR32(a[2], r);
    if (!r) return E_NOINTERFACE;
    return DD_OK;
}
static uint32_t dd_SetCooperativeLevel(const uint32_t *a) { (void)a; return DD_OK; }
static uint32_t dd_SetDisplayMode(const uint32_t *a) {
    if (rt_trace) fprintf(stderr, "SetDisplayMode %ux%u %u bpp\n", a[1], a[2], a[3]);
    return (a[1] == 640 && a[2] == 480 && a[3] == 16) ? DD_OK : 0x88760226u;   // DDERR_INVALIDMODE
}
static uint32_t dd_RestoreDisplayMode(const uint32_t *a) { (void)a; return DD_OK; }
static uint32_t dd_GetCaps(const uint32_t *a) {
    for (int k = 1; k <= 2; k++) if (a[k]) {
        uint32_t p = a[k], sz = RD32(p);
        memset(MEM + p + 4, 0, sz > 4 ? sz - 4 : 0);
        // DDCAPS_3D | BLT | BLTSTRETCH | COLORKEY | BLTCOLORFILL
        WR32(p + 4, 0x1u | 0x40u | 0x200u | 0x400000u | 0x4000000u);
        WR32(p + 12, 0x200u);                                     // DDCKEYCAPS_SRCBLT
        WR32(p + 56, 0x400u);                                     // DDBD_16 z-buffers
        WR32(p + 60, 16u << 20);                                  // dwVidMemTotal
        WR32(p + 64, 16u << 20);                                  // dwVidMemFree
        if (sz >= 0x88) WR32(p + 0x84, DDSCAPS_3DDEVICE | DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER | DDSCAPS_FLIP | DDSCAPS_PRIMARYSURFACE);
    }
    return DD_OK;
}
static uint32_t dd_GetAvailableVidMem(const uint32_t *a) {
    if (a[2]) WR32(a[2], 16u << 20);
    if (a[3]) WR32(a[3], 16u << 20);
    return DD_OK;
}
static uint32_t dd_GetDisplayMode(const uint32_t *a) {
    struct dd_surface tmp = { .w = 640, .h = 480, .pitch = 1280, .fmt = PF_565, .caps = 0 };
    fill_desc(a[1], &tmp);
    WR32(a[1] + 36, 0);
    return DD_OK;
}
static uint32_t dd_WaitForVerticalBlank(const uint32_t *a) { (void)a; return DD_OK; }

static const struct com_method DD_IMPL[] = {
    { "QueryInterface", dd_QueryInterface, 3 }, { "AddRef", com_addref, 1 }, { "Release", com_release, 1 },
    { "CreateClipper", dd_CreateClipper, 4 }, { "CreatePalette", dd_CreatePalette, 5 },
    { "CreateSurface", dd_CreateSurface, 4 }, { "GetCaps", dd_GetCaps, 3 }, { "GetDisplayMode", dd_GetDisplayMode, 2 },
    { "RestoreDisplayMode", dd_RestoreDisplayMode, 1 }, { "SetCooperativeLevel", dd_SetCooperativeLevel, 3 },
    { "SetDisplayMode", dd_SetDisplayMode, 4 }, { "WaitForVerticalBlank", dd_WaitForVerticalBlank, 3 },
    { "GetAvailableVidMem", dd_GetAvailableVidMem, 4 },
    { 0 } };

static void make_vtables(void) {
    if (vt_dd) return;
    vt_dd = com_vtable("IDirectDraw", DD_METHODS, 23, DD_IMPL);
    vt_dd2 = com_vtable("IDirectDraw2", DD_METHODS, N(DD_METHODS), DD_IMPL);
    vt_surf[0] = com_vtable("IDirectDrawSurface", DDS_METHODS, 36, SURF_IMPL);
    vt_surf[1] = com_vtable("IDirectDrawSurface2", DDS_METHODS, 39, SURF_IMPL);
    vt_surf[2] = com_vtable("IDirectDrawSurface3", DDS_METHODS, 40, SURF_IMPL);
    vt_clip = com_vtable("IDirectDrawClipper", CLIP_METHODS, N(CLIP_METHODS), CLIP_IMPL);
    vt_pal = com_vtable("IDirectDrawPalette", PAL_METHODS, N(PAL_METHODS), PAL_IMPL);
}

DEF(DirectDrawCreate) {
    make_vtables();
    struct dd *d = calloc(1, sizeof *d);
    d->obj_dd = com_new(vt_dd, d);
    d->obj_dd2 = com_new(vt_dd2, d);
    d->obj_d3d2 = d3d_create();
    the_dd = d;
    WR32(a[1], d->obj_dd);
    return DD_OK;
}

struct dd_surface *dd_surface_of(uint32_t obj) { return obj ? com_host(obj) : 0; }

const struct impl DDRAW_IMPL[] = { API("DDRAW.DLL", DirectDrawCreate, 3), { 0 } };

// an offscreen RGB565 surface for host-side producers (movie frames)
struct dd_surface *dd_new_surface(uint32_t w, uint32_t h) {
    return surface_new(w, h, &PF_565, 0x40u | DDSCAPS_SYSTEMMEMORY);   // DDSCAPS_OFFSCREENPLAIN
}
