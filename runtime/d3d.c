// Direct3D (DirectX 5: IDirect3D2 / IDirect3DDevice2) with a software rasterizer.
// Triangles are drawn into the render target surface in guest memory (RGB565) with a 16-bit
// z-buffer, Gouraud colour, perspective-correct point-sampled textures, colour keys and the
// D3D5 alpha-blend and texture-blend modes.
#include "com.h"
#include "ddraw.h"
#include "host.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const char *const D3D2_METHODS[] = {
    "QueryInterface", "AddRef", "Release", "EnumDevices", "CreateLight", "CreateMaterial", "CreateViewport",
    "FindDevice", "CreateDevice" };
static const char *const DEV2_METHODS[] = {
    "QueryInterface", "AddRef", "Release", "GetCaps", "SwapTextureHandles", "GetStats", "AddViewport",
    "DeleteViewport", "NextViewport", "EnumTextureFormats", "BeginScene", "EndScene", "GetDirect3D",
    "SetCurrentViewport", "GetCurrentViewport", "SetRenderTarget", "GetRenderTarget", "Begin", "BeginIndexed",
    "Vertex", "Index", "End", "GetRenderState", "SetRenderState", "GetLightState", "SetLightState",
    "SetTransform", "GetTransform", "MultiplyTransform", "DrawPrimitive", "DrawIndexedPrimitive",
    "SetClipStatus", "GetClipStatus" };
static const char *const VP2_METHODS[] = {
    "QueryInterface", "AddRef", "Release", "Initialize", "GetViewport", "SetViewport", "TransformVertices",
    "LightElements", "SetBackground", "GetBackground", "SetBackgroundDepth", "GetBackgroundDepth", "Clear",
    "AddLight", "DeleteLight", "NextLight", "GetViewport2", "SetViewport2" };
static const char *const MAT2_METHODS[] = { "QueryInterface", "AddRef", "Release", "SetMaterial", "GetMaterial", "GetHandle" };
static const char *const TEX2_METHODS[] = { "QueryInterface", "AddRef", "Release", "GetHandle", "PaletteChanged", "Load" };
#define N(x) ((int)(sizeof x / sizeof x[0]))

static uint32_t vt_d3d2, vt_dev2, vt_vp2, vt_mat2, vt_tex2;

// ---------------------------------------------------------------- state
struct material { float r, g, b, a; uint32_t handle; };
static struct material *materials[1024];
static uint32_t nmaterials = 1;
static struct dd_surface *textures[4096];
static uint32_t ntextures = 1;

struct viewport { int32_t x, y, w, h; uint32_t bg_material; };
struct device {
    uint32_t obj;
    struct dd_surface *target;
    struct viewport *vp;
    uint32_t rs[256];
    int in_scene;
};
static struct device *dev;

// ---------------------------------------------------------------- caps
static void fill_primcaps(uint32_t p) {
    WR32(p, 56);
    WR32(p + 4, 0x7fu);                 // misc: masks, cull modes
    WR32(p + 8, 0x0001ffffu);           // raster caps (dither, z test, fog...)
    WR32(p + 12, 0xffu);                // z compare
    WR32(p + 16, 0x1fffu);              // src blend
    WR32(p + 20, 0x1fffu);              // dest blend
    WR32(p + 24, 0xffu);                // alpha compare
    WR32(p + 28, 0x001fffffu);          // shade caps
    WR32(p + 32, 0x1u | 0x4u | 0x8u);   // texture: perspective, alpha, transparency
    WR32(p + 36, 0x3fu);                // filters
    WR32(p + 40, 0xffu);                // texture blend
    WR32(p + 44, 0x1fu);                // address modes
    WR32(p + 48, 32); WR32(p + 52, 32);
}
static void fill_devdesc(uint32_t p, int hal) {
    memset(MEM + p, 0, 204);
    WR32(p, 204);
    if (!hal) { WR32(p + 4, 1); return; }   // the HEL side of a HAL device: colour model 0
    WR32(p + 4, 0x7ffu);
    WR32(p + 8, 2);                      // D3DCOLOR_RGB
    WR32(p + 12, 0x1u | 0x10u | 0x40u | 0x100u | 0x200u | 0x400u | 0x800u | 0x80000u);
    WR32(p + 16, 8); WR32(p + 20, 1);    // transform caps
    WR32(p + 24, 1);                     // clipping
    WR32(p + 28, 16); WR32(p + 32, 7); WR32(p + 36, 1); WR32(p + 40, 8);
    fill_primcaps(p + 44);
    fill_primcaps(p + 100);
    WR32(p + 156, 0x400u);               // DDBD_16 render targets
    WR32(p + 160, 0x400u);               // DDBD_16 z-buffers
    WR32(p + 164, 0); WR32(p + 168, 65535);
    WR32(p + 172, 1); WR32(p + 176, 1); WR32(p + 180, 1024); WR32(p + 184, 1024);
}

// ---------------------------------------------------------------- IDirect3D2
static uint32_t d3d_EnumDevices(const uint32_t *a) {
    uint32_t cb = a[1], ctx = a[2];
    uint32_t hal = hostobj_alloc(204), hel = hostobj_alloc(204), guid = hostobj_alloc(16);
    static uint32_t desc, name;
    if (!desc) { desc = hostobj_str("Direct3D HAL"); name = hostobj_str("Direct3D HAL"); }
    // IID_IDirect3DHALDevice
    static const uint8_t HALG[16] = { 0xe0, 0x3d, 0xe6, 0x84, 0xaa, 0x46, 0xcf, 0x11, 0x81, 0x6f, 0x00, 0x00, 0xc0, 0x20, 0x15, 0x6e };
    memcpy(MEM + guid, HALG, 16);
    fill_devdesc(hal, 1);
    fill_devdesc(hel, 0);
    uint32_t args[6] = { guid, desc, name, hal, hel, ctx };
    guest_call(cb, 6, args);
    return DD_OK;
}
static uint32_t d3d_QueryInterface(const uint32_t *a) { WR32(a[2], 0); return E_NOINTERFACE; }

static uint32_t d3d_CreateMaterial(const uint32_t *a);
static uint32_t d3d_CreateViewport(const uint32_t *a);
static uint32_t d3d_CreateDevice(const uint32_t *a);

static const struct com_method D3D2_IMPL[] = {
    { "QueryInterface", d3d_QueryInterface, 3 }, { "AddRef", com_addref, 1 }, { "Release", com_release, 1 },
    { "EnumDevices", d3d_EnumDevices, 3 }, { "CreateMaterial", d3d_CreateMaterial, 3 },
    { "CreateViewport", d3d_CreateViewport, 3 }, { "CreateDevice", d3d_CreateDevice, 4 },
    { 0 } };

// ---------------------------------------------------------------- materials
static uint32_t d3d_CreateMaterial(const uint32_t *a) {
    struct material *m = calloc(1, sizeof *m);
    m->handle = nmaterials < 1024 ? nmaterials : 0;
    materials[nmaterials++] = m;
    WR32(a[1], com_new(vt_mat2, m));
    return DD_OK;
}
static uint32_t mat_SetMaterial(const uint32_t *a) {
    struct material *m = com_host(a[0]);
    uint32_t p = a[1];
    float f[4];
    for (int k = 0; k < 4; k++) { uint32_t v = RD32(p + 4 + 4 * (uint32_t)k); memcpy(&f[k], &v, 4); }
    m->r = f[0]; m->g = f[1]; m->b = f[2]; m->a = f[3];
    return DD_OK;
}
static uint32_t mat_GetHandle(const uint32_t *a) { WR32(a[2], ((struct material *)com_host(a[0]))->handle); return DD_OK; }
static const struct com_method MAT2_IMPL[] = {
    { "AddRef", com_addref, 1 }, { "Release", com_release, 1 }, { "SetMaterial", mat_SetMaterial, 2 },
    { "GetHandle", mat_GetHandle, 3 }, { 0 } };

// ---------------------------------------------------------------- viewports
static uint32_t d3d_CreateViewport(const uint32_t *a) {
    struct viewport *v = calloc(1, sizeof *v);
    v->w = 640; v->h = 480;
    WR32(a[1], com_new(vt_vp2, v));
    return DD_OK;
}
static uint32_t vp_SetViewport2(const uint32_t *a) {
    struct viewport *v = com_host(a[0]);
    v->x = (int32_t)RD32(a[1] + 4); v->y = (int32_t)RD32(a[1] + 8);
    v->w = (int32_t)RD32(a[1] + 12); v->h = (int32_t)RD32(a[1] + 16);
    return DD_OK;
}
static uint32_t vp_SetBackground(const uint32_t *a) { ((struct viewport *)com_host(a[0]))->bg_material = a[1]; return DD_OK; }
static uint16_t to565(float r, float g, float b) {
    int R = (int)(r * 31.0f + 0.5f), G = (int)(g * 63.0f + 0.5f), B = (int)(b * 31.0f + 0.5f);
    R = R < 0 ? 0 : R > 31 ? 31 : R; G = G < 0 ? 0 : G > 63 ? 63 : G; B = B < 0 ? 0 : B > 31 ? 31 : B;
    return (uint16_t)(R << 11 | G << 5 | B);
}
static inline uint32_t to888(float r, float g, float b) {
    int R = (int)(r * 255.0f + 0.5f), G = (int)(g * 255.0f + 0.5f), B = (int)(b * 255.0f + 0.5f);
    R = R < 0 ? 0 : R > 255 ? 255 : R; G = G < 0 ? 0 : G > 255 ? 255 : G; B = B < 0 ? 0 : B > 255 ? 255 : B;
    return (uint32_t)(R << 16 | G << 8 | B);
}
static uint32_t vp_Clear(const uint32_t *a) {
    struct viewport *v = com_host(a[0]);
    if (!dev || !dev->target) return DD_OK;
    struct dd_surface *t = dev->target;
    uint16_t c = 0;
    uint32_t c32 = 0;
    if (v->bg_material && v->bg_material < nmaterials) {
        struct material *m = materials[v->bg_material];
        c = to565(m->r, m->g, m->b);
        c32 = to888(m->r, m->g, m->b);
    }
    if (t->hi) {                              // the screen: clear its high-resolution buffer
        for (uint32_t k = 0; k < a[1]; k++) {
            uint32_t r = a[2] + 16 * k;
            dd_hires_fill(t, (int32_t)RD32(r), (int32_t)RD32(r + 4), (int32_t)RD32(r + 8), (int32_t)RD32(r + 12),
                          (a[3] & 1u) != 0, c32, (a[3] & 2u) != 0);
        }
        return DD_OK;
    }
    for (uint32_t k = 0; k < a[1]; k++) {
        uint32_t r = a[2] + 16 * k;
        int32_t x0 = (int32_t)RD32(r), y0 = (int32_t)RD32(r + 4), x1 = (int32_t)RD32(r + 8), y1 = (int32_t)RD32(r + 12);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (int32_t)t->w) x1 = (int32_t)t->w;
        if (y1 > (int32_t)t->h) y1 = (int32_t)t->h;
        for (int32_t y = y0; y < y1; y++) {
            if (a[3] & 1u) { uint16_t *row = (uint16_t *)(MEM + t->pixels + (uint32_t)y * t->pitch); for (int32_t x = x0; x < x1; x++) row[x] = c; }
            if ((a[3] & 2u) && t->zbuf) { uint16_t *z = (uint16_t *)(MEM + t->zbuf->pixels + (uint32_t)y * t->zbuf->pitch); for (int32_t x = x0; x < x1; x++) z[x] = 0xffff; }
        }
    }
    return DD_OK;
}
static uint32_t vp_ok(const uint32_t *a) { (void)a; return DD_OK; }
static const struct com_method VP2_IMPL[] = {
    { "AddRef", com_addref, 1 }, { "Release", com_release, 1 }, { "SetViewport2", vp_SetViewport2, 2 },
    { "SetBackground", vp_SetBackground, 2 }, { "Clear", vp_Clear, 4 }, { "AddLight", vp_ok, 2 },
    { 0 } };

// ---------------------------------------------------------------- textures
uint32_t d3d_texture_of(struct dd_surface *s) {
    if (!s->tex_obj) {
        s->tex_obj = com_new(vt_tex2, s);
        if (ntextures < 4096) { s->tex_handle = ntextures; textures[ntextures++] = s; }
    } else WR32(s->tex_obj + 8, RD32(s->tex_obj + 8) + 1);
    return s->tex_obj;
}
static uint32_t tex_GetHandle(const uint32_t *a) { WR32(a[2], ((struct dd_surface *)com_host(a[0]))->tex_handle); return DD_OK; }
static uint32_t tex_Load(const uint32_t *a) {
    struct dd_surface *d = com_host(a[0]), *s = com_host(a[1]);
    uint32_t rows = d->h < s->h ? d->h : s->h, n = d->pitch < s->pitch ? d->pitch : s->pitch;
    for (uint32_t y = 0; y < rows; y++) memcpy(MEM + d->pixels + y * d->pitch, MEM + s->pixels + y * s->pitch, n);
    dd_set_palette(d, s->palette_obj);
    if (s->ck_src_set) { d->ck_src_set = 1; d->ck_src_lo = s->ck_src_lo; d->ck_src_hi = s->ck_src_hi; }
    return DD_OK;
}
static uint32_t tex_PaletteChanged(const uint32_t *a) { (void)a; return DD_OK; }
static uint32_t tex_Release(const uint32_t *a) {
    uint32_t n = com_release(a);
    if (!n) { struct dd_surface *s = com_host(a[0]); s->tex_obj = 0; }
    return n;
}
static const struct com_method TEX2_IMPL[] = {
    { "AddRef", com_addref, 1 }, { "Release", tex_Release, 1 }, { "GetHandle", tex_GetHandle, 3 },
    { "Load", tex_Load, 2 }, { "PaletteChanged", tex_PaletteChanged, 3 }, { 0 } };

// ---------------------------------------------------------------- device
static uint32_t d3d_CreateDevice(const uint32_t *a) {
    struct device *d = calloc(1, sizeof *d);
    d->target = dd_surface_of(a[2]);
    // D3D5 defaults
    d->rs[3] = 1; d->rs[4] = 0; d->rs[7] = 1; d->rs[8] = 3; d->rs[9] = 2; d->rs[14] = 1; d->rs[16] = 1;
    d->rs[17] = 1; d->rs[18] = 1; d->rs[19] = 2; d->rs[20] = 1; d->rs[21] = 2; d->rs[22] = 3; d->rs[23] = 4;
    d->rs[25] = 8;
    d->obj = com_new(vt_dev2, d);
    dev = d;
    WR32(a[3], d->obj);
    if (rt_trace) fprintf(stderr, "Direct3D device on surface %ux%u\n", d->target ? d->target->w : 0, d->target ? d->target->h : 0);
    return DD_OK;
}
static uint32_t dev_GetCaps(const uint32_t *a) {
    if (a[1]) fill_devdesc(a[1], 1);
    if (a[2]) fill_devdesc(a[2], 0);
    return DD_OK;
}
static uint32_t dev_EnumTextureFormats(const uint32_t *a) {
    static const struct pixfmt fmts[] = {
        { 16, 0x7c00, 0x03e0, 0x001f, 0x8000, 0 },     // ARGB1555
        { 16, 0x0f00, 0x00f0, 0x000f, 0xf000, 0 },     // ARGB4444
        { 16, 0xf800, 0x07e0, 0x001f, 0, 0 },          // RGB565
        { 8, 0, 0, 0, 0, 1 },                          // 8-bit palettized
    };
    uint32_t d = hostobj_alloc(108);
    for (int k = 0; k < 4; k++) {
        memset(MEM + d, 0, 108);
        WR32(d, 108); WR32(d + 4, 0x1000u); WR32(d + 104, DDSCAPS_TEXTURE);
        uint32_t p = d + 72;
        WR32(p, 32);
        WR32(p + 4, fmts[k].pal8 ? 0x20u : 0x40u | (fmts[k].a ? 1u : 0u));
        WR32(p + 12, fmts[k].bpp);
        WR32(p + 16, fmts[k].r); WR32(p + 20, fmts[k].g); WR32(p + 24, fmts[k].b); WR32(p + 28, fmts[k].a);
        uint32_t args[2] = { d, a[2] };
        if (guest_call(a[1], 2, args) == 0) break;     // D3DENUMRET_CANCEL
    }
    return DD_OK;
}
static uint32_t dev_AddViewport(const uint32_t *a) { if (!dev->vp) dev->vp = com_host(a[1]); return DD_OK; }
static uint32_t dev_SetCurrentViewport(const uint32_t *a) { dev->vp = com_host(a[1]); return DD_OK; }
static uint32_t dev_BeginScene(const uint32_t *a) { (void)a; dev->in_scene = 1; return DD_OK; }
static uint32_t dev_EndScene(const uint32_t *a) { (void)a; dev->in_scene = 0; return DD_OK; }
static uint32_t dev_SetRenderTarget(const uint32_t *a) { dev->target = dd_surface_of(a[1]); return DD_OK; }
static uint32_t dev_GetRenderTarget(const uint32_t *a) { WR32(a[1], dev->target ? dev->target->obj[0] : 0); return DD_OK; }
static uint32_t dev_SetRenderState(const uint32_t *a) { if (a[1] < 256) dev->rs[a[1]] = a[2]; return DD_OK; }
static uint32_t dev_GetRenderState(const uint32_t *a) { WR32(a[2], a[1] < 256 ? dev->rs[a[1]] : 0); return DD_OK; }
static uint32_t dev_SetLightState(const uint32_t *a) { (void)a; return DD_OK; }
static uint32_t dev_SetTransform(const uint32_t *a) { (void)a; return DD_OK; }
static uint32_t dev_GetStats(const uint32_t *a) { memset(MEM + a[1] + 4, 0, 16); return DD_OK; }

// ---------------------------------------------------------------- rasterizer
struct vtx { float x, y, z, w, r, g, b, a, u, v; };

static void read_vtx(uint32_t p, struct vtx *o) {
    float f[8]; uint32_t raw[8];
    for (int k = 0; k < 8; k++) raw[k] = RD32(p + 4u * (uint32_t)k);
    memcpy(f, raw, sizeof f);
    o->x = f[0]; o->y = f[1]; o->z = f[2]; o->w = f[3] != 0.0f ? f[3] : 1.0f;
    uint32_t c = raw[4];
    o->a = (float)(c >> 24) / 255.0f; o->r = (float)((c >> 16) & 255) / 255.0f;
    o->g = (float)((c >> 8) & 255) / 255.0f; o->b = (float)(c & 255) / 255.0f;
    o->u = f[6]; o->v = f[7];
}

// texel as ARGB floats; returns 0 when the texel is colour-keyed out
// raw texel -> 0xRRGGBB for the fast path: one table per 16-bit format (built once), or the
// texture's palette for 8-bit textures (rebuilt per draw: the game changes palettes often)
static uint32_t *texel_lut(struct dd_surface *t) {
    static uint32_t pal[256];
    if (t->fmt.pal8) {
        const uint32_t *e = dd_palette_entries(t->palette);
        for (int k = 0; k < 256; k++) {
            uint32_t p = e ? e[k] : (uint32_t)k * 0x010101u;   // PALETTEENTRY r, g, b
            pal[k] = ((p & 255u) << 16) | (p & 0xff00u) | ((p >> 16) & 255u);
        }
        return pal;
    }
    static struct { uint32_t r, g, b, a; uint32_t *lut; } cache[4];
    for (int k = 0; k < 4; k++)
        if (cache[k].lut && cache[k].r == t->fmt.r && cache[k].g == t->fmt.g && cache[k].b == t->fmt.b && cache[k].a == t->fmt.a)
            return cache[k].lut;
    int slot = 0;
    while (slot < 3 && cache[slot].lut) slot++;
    uint32_t *lut = cache[slot].lut ? cache[slot].lut : malloc(65536 * sizeof *lut);
    uint32_t m[3] = { t->fmt.r, t->fmt.g, t->fmt.b };
    uint32_t am = t->fmt.a;
    for (uint32_t v = 0; v < 65536; v++) {
        uint32_t out = 0xff000000u;                      // alpha in the top byte (opaque without an alpha mask)
        if (am) {
            int sh = __builtin_ctz(am);
            uint32_t mx = am >> sh, c = (v & am) >> sh;
            out = ((c * 255u + mx / 2) / mx) << 24;
        }
        for (int k = 0; k < 3; k++) {
            if (!m[k]) continue;
            int sh = __builtin_ctz(m[k]);
            uint32_t mx = m[k] >> sh, c = (v & m[k]) >> sh;
            out |= ((c * 255u + mx / 2) / mx) << (16 - 8 * k);
        }
        lut[v] = out;
    }
    cache[slot].r = t->fmt.r; cache[slot].g = t->fmt.g; cache[slot].b = t->fmt.b; cache[slot].a = t->fmt.a; cache[slot].lut = lut;
    return lut;
}

static int sample(struct dd_surface *t, float u, float v, float *o) {
    int wrap = 1;
    int32_t x = (int32_t)floorf(u * (float)t->w), y = (int32_t)floorf(v * (float)t->h);
    if (wrap) { x %= (int32_t)t->w; if (x < 0) x += (int32_t)t->w; y %= (int32_t)t->h; if (y < 0) y += (int32_t)t->h; }
    uint32_t px;
    uint32_t row = t->pixels + (uint32_t)y * t->pitch;
    if (t->fmt.bpp == 8) px = RD8(row + (uint32_t)x);
    else if (t->fmt.bpp == 16) px = RD16(row + 2u * (uint32_t)x);
    else px = RD32(row + 4u * (uint32_t)x);
    if (dev->rs[41] && t->ck_src_set && px >= t->ck_src_lo && px <= t->ck_src_hi) return 0;
    if (t->fmt.pal8) {
        const uint32_t *pal = dd_palette_entries(t->palette);
        uint32_t e = pal ? pal[px & 255] : px * 0x010101u;     // PALETTEENTRY: r, g, b, flags
        o[1] = (float)(e & 255) / 255.0f; o[2] = (float)((e >> 8) & 255) / 255.0f; o[3] = (float)((e >> 16) & 255) / 255.0f;
        o[0] = 1.0f;
        return 1;
    }
    if (t->fmt.bpp == 16) {                              // through the format's lookup table
        uint32_t c = texel_lut(t)[px & 0xffffu];
        o[0] = (float)(c >> 24) * (1.0f / 255.0f); o[1] = (float)((c >> 16) & 255) * (1.0f / 255.0f);
        o[2] = (float)((c >> 8) & 255) * (1.0f / 255.0f); o[3] = (float)(c & 255) * (1.0f / 255.0f);
        return 1;
    }
    const struct pixfmt *f = &t->fmt;
    uint32_t m[4] = { f->a, f->r, f->g, f->b };
    for (int k = 0; k < 4; k++) {
        if (!m[k]) { o[k] = 1.0f; continue; }
        int sh = __builtin_ctz(m[k]);
        o[k] = (float)((px & m[k]) >> sh) / (float)(m[k] >> sh);
    }
    return 1;
}

static float blend_factor(uint32_t mode, const float *src, const float *dst, int ch) {
    switch (mode) {
    case 1: return 0.0f;
    case 2: return 1.0f;
    case 3: return src[ch];
    case 4: return 1.0f - src[ch];
    case 5: return src[0];
    case 6: return 1.0f - src[0];
    case 7: return dst[0];
    case 8: return 1.0f - dst[0];
    case 9: return dst[ch];
    case 10: return 1.0f - dst[ch];
    case 11: { float f = src[0] < 1.0f - dst[0] ? src[0] : 1.0f - dst[0]; return ch ? f : 1.0f; }
    default: return 1.0f;
    }
}

static int zpass(uint32_t func, uint16_t z, uint16_t cur) {
    switch (func) {
    case 1: return 0; case 2: return z < cur; case 3: return z == cur; case 4: return z <= cur;
    case 5: return z > cur; case 6: return z != cur; case 7: return z >= cur; default: return 1;
    }
}

// Where a draw lands: the screen's high-resolution 32-bit buffer (see ddraw.c), or any other
// surface at its own size in guest memory (16-bit RGB565).
struct fb { int w, h, s; uint16_t *p16; uint32_t *p32; size_t pitch; uint16_t *z; size_t zpitch; };
static int target_fb(struct fb *f) {
    struct dd_surface *t = dev->target;
    if (!t) return 0;
    if (t->hi) {
        f->s = t->hs; f->w = (int)t->w * t->hs; f->h = (int)t->h * t->hs;
        f->p32 = t->hi; f->p16 = 0; f->pitch = (size_t)f->w;
        f->z = t->hiz; f->zpitch = (size_t)f->w;
    } else {
        f->s = 1; f->w = (int)t->w; f->h = (int)t->h;
        f->p16 = (uint16_t *)(MEM + t->pixels); f->p32 = 0; f->pitch = t->pitch / 2;
        f->z = t->zbuf ? (uint16_t *)(MEM + t->zbuf->pixels) : 0;
        f->zpitch = t->zbuf ? t->zbuf->pitch / 2 : 0;
    }
    return 1;
}

// Everything one triangle needs while its rows are drawn (read-only, shared by the worker threads).
struct tri {
    struct fb F;
    const struct vtx *v0, *v1, *v2;
    int32_t x0, x1;
    int64_t ex[3], ey[3], fx[3], fy[3], bias[3], stepx[3];
    float inv, wq[3], uq[3], vq[3];
    int affine, gouraud, zen, zwrite, ablend, atest;
    uint32_t sb, db, tb, afunc, zfunc;
    float aref;
    struct dd_surface *tex;
    // fast path
    const uint32_t *lut;
    int ck, pal8;
    uint32_t ck_lo, ck_hi;
    float dudx, dvdx;
};

static void row_edges(const struct tri *t, int32_t y, int64_t *E) {
    int64_t PY = (int64_t)y * 16 + 8, PX = (int64_t)t->x0 * 16 + 8;
    for (int k = 0; k < 3; k++) E[k] = (t->fx[k] - t->ex[k]) * (PY - t->ey[k]) - (t->fy[k] - t->ey[k]) * (PX - t->ex[k]);
}

// flat textured, white, no blend / alpha test / depth: texels straight from a lookup table
static void rows_fast(const struct tri *t, int32_t ya, int32_t yb) {
    const struct dd_surface *tex = t->tex;
    int32_t tw = (int32_t)tex->w, th = (int32_t)tex->h;
    for (int32_t y = ya; y < yb; y++) {
        int64_t E[3];
        row_edges(t, y, E);
        float u = ((float)E[0] * t->uq[0] + (float)E[1] * t->uq[1] + (float)E[2] * t->uq[2]) * t->inv;
        float v = ((float)E[0] * t->vq[0] + (float)E[1] * t->vq[1] + (float)E[2] * t->vq[2]) * t->inv;
        uint16_t *row16 = t->F.p16 ? t->F.p16 + (size_t)y * t->F.pitch : 0;
        uint32_t *row32 = t->F.p32 ? t->F.p32 + (size_t)y * t->F.pitch : 0;
        for (int32_t x = t->x0; x < t->x1; x++, E[0] += t->stepx[0], E[1] += t->stepx[1], E[2] += t->stepx[2], u += t->dudx, v += t->dvdx) {
            if (E[0] + t->bias[0] < 0 || E[1] + t->bias[1] < 0 || E[2] + t->bias[2] < 0) continue;
            int32_t tx = (int32_t)floorf(u * (float)tw) % tw, ty = (int32_t)floorf(v * (float)th) % th;
            if (tx < 0) tx += tw;
            if (ty < 0) ty += th;
            uint32_t trow = tex->pixels + (uint32_t)ty * tex->pitch;
            uint32_t raw = t->pal8 ? RD8(trow + (uint32_t)tx) : RD16(trow + 2u * (uint32_t)tx);
            if (t->ck && raw >= t->ck_lo && raw <= t->ck_hi) continue;
            uint32_t c = t->lut[raw];
            if (row32) row32[x] = c;
            else row16[x] = (uint16_t)(((c >> 8) & 0xf800u) | ((c >> 5) & 0x07e0u) | ((c >> 3) & 0x1fu));
        }
    }
}

// everything else: Gouraud colour, perspective, texture blend modes, alpha test, blending, depth
static void rows_general(const struct tri *t, int32_t ya, int32_t yb) {
    const struct vtx *v0 = t->v0, *v1 = t->v1, *v2 = t->v2;
    struct dd_surface *tex = t->tex;
    for (int32_t y = ya; y < yb; y++) {
        int64_t E[3];
        row_edges(t, y, E);
        uint16_t *row16 = t->F.p16 ? t->F.p16 + (size_t)y * t->F.pitch : 0;
        uint32_t *row32 = t->F.p32 ? t->F.p32 + (size_t)y * t->F.pitch : 0;
        uint16_t *zrow = t->F.z ? t->F.z + (size_t)y * t->F.zpitch : 0;
        for (int32_t x = t->x0; x < t->x1; x++, E[0] += t->stepx[0], E[1] += t->stepx[1], E[2] += t->stepx[2]) {
            if (E[0] + t->bias[0] < 0 || E[1] + t->bias[1] < 0 || E[2] + t->bias[2] < 0) continue;
            float e0 = (float)E[0] * t->inv, e1 = (float)E[1] * t->inv, e2 = (float)E[2] * t->inv;
            uint16_t zi = 0;
            if (t->zen || t->zwrite) {
                float z = e0 * v0->z + e1 * v1->z + e2 * v2->z;
                zi = (uint16_t)(z <= 0 ? 0 : z >= 1 ? 65535 : (int)(z * 65535.0f));
                if (t->zen && !zpass(t->zfunc, zi, zrow[x])) continue;
            }
            float c[4];   // a r g b
            if (t->gouraud) {
                c[0] = e0 * v0->a + e1 * v1->a + e2 * v2->a; c[1] = e0 * v0->r + e1 * v1->r + e2 * v2->r;
                c[2] = e0 * v0->g + e1 * v1->g + e2 * v2->g; c[3] = e0 * v0->b + e1 * v1->b + e2 * v2->b;
            } else { c[0] = v0->a; c[1] = v0->r; c[2] = v0->g; c[3] = v0->b; }
            if (tex) {
                float u, vv;
                if (t->affine) { u = e0 * t->uq[0] + e1 * t->uq[1] + e2 * t->uq[2]; vv = e0 * t->vq[0] + e1 * t->vq[1] + e2 * t->vq[2]; }
                else {
                    float w = e0 * t->wq[0] + e1 * t->wq[1] + e2 * t->wq[2];
                    u = (e0 * t->uq[0] + e1 * t->uq[1] + e2 * t->uq[2]) / w; vv = (e0 * t->vq[0] + e1 * t->vq[1] + e2 * t->vq[2]) / w;
                }
                float tx[4];
                if (!sample(tex, u, vv, tx)) continue;
                switch (t->tb) {
                case 1: case 7: c[0] = tx[0]; c[1] = tx[1]; c[2] = tx[2]; c[3] = tx[3]; break;        // DECAL / COPY
                case 3: c[1] = c[1] * (1 - tx[0]) + tx[1] * tx[0]; c[2] = c[2] * (1 - tx[0]) + tx[2] * tx[0];
                        c[3] = c[3] * (1 - tx[0]) + tx[3] * tx[0]; break;                            // DECALALPHA
                case 8: c[1] += tx[1]; c[2] += tx[2]; c[3] += tx[3]; break;                           // ADD
                case 4: c[0] *= tx[0]; c[1] *= tx[1]; c[2] *= tx[2]; c[3] *= tx[3]; break;          // MODULATEALPHA
                default:                                                                             // MODULATE
                    if (tex->fmt.a) c[0] = tx[0];
                    c[1] *= tx[1]; c[2] *= tx[2]; c[3] *= tx[3]; break;
                }
            }
            if (t->atest) {
                int pass = 1;
                switch (t->afunc) { case 1: pass = 0; break; case 2: pass = c[0] < t->aref; break; case 3: pass = c[0] == t->aref; break;
                case 4: pass = c[0] <= t->aref; break; case 5: pass = c[0] > t->aref; break; case 6: pass = c[0] != t->aref; break;
                case 7: pass = c[0] >= t->aref; break; default: break; }
                if (!pass) continue;
            }
            if (t->ablend) {
                float dst[4];
                dst[0] = 1.0f;
                if (row32) {
                    uint32_t d = row32[x];
                    dst[1] = (float)((d >> 16) & 255) / 255.0f; dst[2] = (float)((d >> 8) & 255) / 255.0f; dst[3] = (float)(d & 255) / 255.0f;
                } else {
                    uint16_t d = row16[x];
                    dst[1] = (float)(d >> 11) / 31.0f; dst[2] = (float)((d >> 5) & 63) / 63.0f; dst[3] = (float)(d & 31) / 31.0f;
                }
                float out[4];
                for (int k = 1; k < 4; k++) out[k] = c[k] * blend_factor(t->sb, c, dst, k) + dst[k] * blend_factor(t->db, c, dst, k);
                c[1] = out[1]; c[2] = out[2]; c[3] = out[3];
            }
            if (row32) row32[x] = to888(c[1], c[2], c[3]);
            else row16[x] = to565(c[1], c[2], c[3]);
            if (t->zwrite) zrow[x] = zi;
        }
    }
}

// ---------------------------------------------------------------- worker threads
// A triangle's rows are split across worker threads; each triangle finishes before the next one
// starts, so drawing order is unchanged. Triangles come microseconds apart while a frame is drawn,
// so idle workers spin briefly for the next job before going to sleep (a semaphore wake-up costs
// more than a typical triangle). Between frames they sleep.
#define MAX_WORKERS 6
static int nworkers = -1;
static SDL_Thread *workers[MAX_WORKERS];
static SDL_Semaphore *wake[MAX_WORKERS];
static SDL_AtomicInt sleeping[MAX_WORKERS];
static SDL_AtomicInt job_gen, job_left;
static struct { const struct tri *t; int fast; int32_t y0, y1, parts; } job;

static void job_part(int k) {
    int32_t h = job.y1 - job.y0;
    int32_t ya = job.y0 + (int32_t)((int64_t)h * k / job.parts);
    int32_t yb = job.y0 + (int32_t)((int64_t)h * (k + 1) / job.parts);
    if (job.fast) rows_fast(job.t, ya, yb); else rows_general(job.t, ya, yb);
}

static int worker_main(void *arg) {
    int k = (int)(intptr_t)arg;
    int last = 0;
    for (;;) {
        int spins = 0, g;
        while ((g = SDL_GetAtomicInt(&job_gen)) == last) {
            if (++spins < 20000) { SDL_CPUPauseInstruction(); continue; }
            // go to sleep; re-check after announcing it so a job posted meanwhile is not missed
            SDL_SetAtomicInt(&sleeping[k], 1);
            if (SDL_GetAtomicInt(&job_gen) != last) {
                if (!SDL_CompareAndSwapAtomicInt(&sleeping[k], 1, 0)) SDL_WaitSemaphore(wake[k]);   // a wake-up is on its way
                break;
            }
            SDL_WaitSemaphore(wake[k]);
            spins = 0;
        }
        last = SDL_GetAtomicInt(&job_gen);
        job_part(k + 1);                    // part 0 is the calling thread's
        SDL_AddAtomicInt(&job_left, -1);
    }
    return 0;
}

static void start_workers(void) {
    int cores = SDL_GetNumLogicalCPUCores();
    nworkers = cores > 1 ? cores - 1 : 0;
    if (nworkers > MAX_WORKERS) nworkers = MAX_WORKERS;
    if (getenv("P1_RASTER_THREADS")) nworkers = atoi(getenv("P1_RASTER_THREADS"));
    if (nworkers < 0) nworkers = 0;
    if (nworkers > MAX_WORKERS) nworkers = MAX_WORKERS;
    for (int k = 0; k < nworkers; k++) {
        wake[k] = SDL_CreateSemaphore(0);
        workers[k] = SDL_CreateThread(worker_main, "raster", (void *)(intptr_t)k);
        if (!workers[k]) { nworkers = k; break; }
    }
}

uint64_t raster_stat[4];   // P1_FPS diagnostics: [fast px, general px, fast tris, general tris]
static void run_rows(const struct tri *t, int fast, int32_t y0, int32_t y1) {
    if (nworkers < 0) start_workers();
    int64_t pixels = (int64_t)(y1 - y0) * (t->x1 - t->x0);
    raster_stat[fast ? 0 : 1] += (uint64_t)pixels;
    raster_stat[fast ? 2 : 3]++;
    int parts = nworkers + 1;
    if (pixels < 2048 || parts < 2 || y1 - y0 < parts) {
        if (fast) rows_fast(t, y0, y1); else rows_general(t, y0, y1);
        return;
    }
    job.t = t; job.fast = fast; job.y0 = y0; job.y1 = y1; job.parts = parts;
    SDL_SetAtomicInt(&job_left, nworkers);
    SDL_AddAtomicInt(&job_gen, 1);                  // publishes the job (atomics are full barriers)
    for (int k = 0; k < nworkers; k++)
        if (SDL_CompareAndSwapAtomicInt(&sleeping[k], 1, 0)) SDL_SignalSemaphore(wake[k]);
    job_part(0);
    while (SDL_GetAtomicInt(&job_left) > 0) SDL_CPUPauseInstruction();
}

static void raster_tri_body(const struct vtx *v0, const struct vtx *v1, const struct vtx *v2);
uint64_t raster_ticks;    // P1_FPS diagnostics: performance-counter ticks spent rasterizing
static void raster_tri(const struct vtx *v0, const struct vtx *v1, const struct vtx *v2) {
    uint64_t t0 = SDL_GetPerformanceCounter();
    raster_tri_body(v0, v1, v2);
    raster_ticks += SDL_GetPerformanceCounter() - t0;
}
static void raster_tri_body(const struct vtx *v0, const struct vtx *v1, const struct vtx *v2) {
    struct tri T;
    struct tri *t = &T;
    if (!target_fb(&t->F)) return;
    float area = (v1->x - v0->x) * (v2->y - v0->y) - (v2->x - v0->x) * (v1->y - v0->y);
    if (area == 0.0f) return;
    uint32_t cull = dev->rs[22];
    // screen y points down, so a positive area is clockwise as seen on screen
    if (cull == 2 && area > 0) return;      // D3DCULL_CW
    if (cull == 3 && area < 0) return;      // D3DCULL_CCW
    const int S = t->F.s;
    struct viewport *vp = dev->vp;
    int32_t cx0 = vp ? vp->x * S : 0, cy0 = vp ? vp->y * S : 0;
    int32_t cx1 = vp ? (vp->x + vp->w) * S : t->F.w, cy1 = vp ? (vp->y + vp->h) * S : t->F.h;
    if (cx1 > t->F.w) cx1 = t->F.w;
    if (cy1 > t->F.h) cy1 = t->F.h;
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    // Exact coverage: vertices snapped to 1/16 pixel (of the target resolution), integer edge
    // functions stepped per pixel, and the top-left fill rule, so a pixel on an edge shared by two
    // triangles belongs to exactly one of them. Pixels are sampled at their centres (x + 0.5).
    if (area < 0) { const struct vtx *tmp = v1; v1 = v2; v2 = tmp; }
    t->v0 = v0; t->v1 = v1; t->v2 = v2;
    const float k16 = 16.0f * (float)S;
    int64_t X0 = (int64_t)lrintf(v0->x * k16), Y0 = (int64_t)lrintf(v0->y * k16);
    int64_t X1 = (int64_t)lrintf(v1->x * k16), Y1 = (int64_t)lrintf(v1->y * k16);
    int64_t X2 = (int64_t)lrintf(v2->x * k16), Y2 = (int64_t)lrintf(v2->y * k16);
    int64_t A = (X1 - X0) * (Y2 - Y0) - (X2 - X0) * (Y1 - Y0);
    if (A <= 0) return;
    // edge k is opposite vertex k: E0 on v1->v2, E1 on v2->v0, E2 on v0->v1
    int64_t ex[3] = { X1, X2, X0 }, ey[3] = { Y1, Y2, Y0 }, fx[3] = { X2, X0, X1 }, fy[3] = { Y2, Y0, Y1 };
    for (int k = 0; k < 3; k++) {
        t->ex[k] = ex[k]; t->ey[k] = ey[k]; t->fx[k] = fx[k]; t->fy[k] = fy[k];
        int64_t dx = fx[k] - ex[k], dy = fy[k] - ey[k];
        t->bias[k] = ((dy == 0 && dx > 0) || dy < 0) ? 0 : -1;
        t->stepx[k] = -dy * 16;
    }
    int64_t minX = X0 < X1 ? (X0 < X2 ? X0 : X2) : (X1 < X2 ? X1 : X2);
    int64_t maxX = X0 > X1 ? (X0 > X2 ? X0 : X2) : (X1 > X2 ? X1 : X2);
    int64_t minY = Y0 < Y1 ? (Y0 < Y2 ? Y0 : Y2) : (Y1 < Y2 ? Y1 : Y2);
    int64_t maxY = Y0 > Y1 ? (Y0 > Y2 ? Y0 : Y2) : (Y1 > Y2 ? Y1 : Y2);
    int32_t x0 = (int32_t)(minX >> 4), x1 = (int32_t)(maxX >> 4) + 1;
    int32_t y0 = (int32_t)(minY >> 4), y1 = (int32_t)(maxY >> 4) + 1;
    if (x0 < cx0) x0 = cx0;
    if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    if (x0 >= x1 || y0 >= y1) return;
    t->x0 = x0; t->x1 = x1;

    t->tex = dev->rs[1] && dev->rs[1] < ntextures ? textures[dev->rs[1]] : 0;
    t->gouraud = dev->rs[9] != 1;          // D3DSHADE_FLAT = 1
    t->zen = dev->rs[7] && t->F.z;
    t->zwrite = dev->rs[14] && t->F.z;
    t->ablend = dev->rs[27];
    t->sb = dev->rs[19]; t->db = dev->rs[20]; t->tb = dev->rs[21];
    if (t->sb == 12) { t->sb = 5; t->db = 6; } else if (t->sb == 13) { t->sb = 6; t->db = 5; }
    t->atest = dev->rs[15];
    t->aref = (float)(dev->rs[24] & 255) / 255.0f;
    t->afunc = dev->rs[25]; t->zfunc = dev->rs[23];

    t->inv = 1.0f / (float)A;
    // perspective-correct attributes: interpolate attr*w and w (w = rhw); flat 2D quads (equal w)
    // skip the per-pixel divide
    const struct vtx *V[3] = { v0, v1, v2 };
    for (int k = 0; k < 3; k++) { t->wq[k] = V[k]->w; t->uq[k] = V[k]->u * t->wq[k]; t->vq[k] = V[k]->v * t->wq[k]; }
    t->affine = t->wq[0] == t->wq[1] && t->wq[1] == t->wq[2];
    if (t->affine) for (int k = 0; k < 3; k++) { t->uq[k] = V[k]->u; t->vq[k] = V[k]->v; }

    // the fast path covers what the game draws most: sprites, backgrounds, text boxes
    int white = 1;
    for (int k = 0; k < 3; k++) if (V[k]->r < 0.999f || V[k]->g < 0.999f || V[k]->b < 0.999f) white = 0;
    struct dd_surface *tex = t->tex;
    if (tex && t->affine && white && !t->ablend && !t->atest && !t->zen && !t->zwrite &&
        (t->tb == 1 || t->tb == 2 || t->tb == 4 || t->tb == 7) && (tex->fmt.pal8 || tex->fmt.bpp == 16)) {
        t->lut = texel_lut(tex);
        t->ck = dev->rs[41] && tex->ck_src_set;
        t->ck_lo = tex->ck_src_lo; t->ck_hi = tex->ck_src_hi;
        t->pal8 = tex->fmt.pal8;
        t->dudx = t->dvdx = 0;
        for (int k = 0; k < 3; k++) { t->dudx += (float)t->stepx[k] * t->inv * t->uq[k]; t->dvdx += (float)t->stepx[k] * t->inv * t->vq[k]; }
        run_rows(t, 1, y0, y1);
        return;
    }
    run_rows(t, 0, y0, y1);
}

static void draw(uint32_t prim, struct vtx *v, uint32_t n, const uint16_t *idx, uint32_t nidx) {
    uint32_t count = idx ? nidx : n;
#define V(k) (&v[idx ? idx[k] : (k)])
    switch (prim) {
    case 4: for (uint32_t k = 0; k + 2 < count; k += 3) raster_tri(V(k), V(k + 1), V(k + 2)); break;
    case 5: for (uint32_t k = 0; k + 2 < count; k++) { if (k & 1) raster_tri(V(k + 1), V(k), V(k + 2)); else raster_tri(V(k), V(k + 1), V(k + 2)); } break;
    case 6: for (uint32_t k = 1; k + 1 < count; k++) raster_tri(V(0), V(k), V(k + 1)); break;
    default: break;   // points and lines are not drawn
    }
#undef V
}

static struct vtx vbuf[65536];
static uint32_t dev_DrawPrimitive(const uint32_t *a) {
    uint32_t prim = a[1], vt = a[2], p = a[3], n = a[4];
    if (vt != 3) rt_fatal("DrawPrimitive: vertex type %u (only D3DVT_TLVERTEX is supported)", vt);
    if (n > 65536) n = 65536;
    for (uint32_t k = 0; k < n; k++) read_vtx(p + 32u * k, &vbuf[k]);
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("P1_VTX") ? atoi(getenv("P1_VTX")) : 0;
    if (dbg && frame_counter >= 200 && (!getenv("P1_VTX_X") || (vbuf[0].x >= atof(getenv("P1_VTX_X")) - 40 && vbuf[0].x <= atof(getenv("P1_VTX_X")) + 40 && vbuf[0].y < 80))) {
        dbg--;
        struct dd_surface *tx = dev->rs[1] && dev->rs[1] < ntextures ? textures[dev->rs[1]] : 0;
        uint32_t nz = 0;
        if (tx) for (uint32_t k = 0; k < tx->pitch * tx->h; k++) nz += RD8(tx->pixels + k) != 0;
        fprintf(stderr, "draw prim %u n %u tex %u (%ux%u bpp %u, %u nonzero bytes) blend %u ab %u ck %u z %u\n", prim, n, dev->rs[1],
                tx ? tx->w : 0, tx ? tx->h : 0, tx ? tx->fmt.bpp : 0, nz, dev->rs[21], dev->rs[27], dev->rs[41], dev->rs[7]);
        for (uint32_t k = 0; k < n && k < 4; k++)
            fprintf(stderr, "  v%u: %.1f %.1f z %.3f w %.3f c %08x uv %.3f %.3f\n", k, vbuf[k].x, vbuf[k].y, vbuf[k].z, vbuf[k].w, RD32(p + 32u * k + 16), vbuf[k].u, vbuf[k].v);
    }
    if (getenv("P1_PICK") && frame_counter == 850) {     // log draws covering a screen point
        float px = 475, py = 50, x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
        for (uint32_t k = 0; k < n; k++) { x0 = fminf(x0, vbuf[k].x); x1 = fmaxf(x1, vbuf[k].x); y0 = fminf(y0, vbuf[k].y); y1 = fmaxf(y1, vbuf[k].y); }
        if (px >= x0 && px <= x1 && py >= y0 && py <= y1) {
            fprintf(stderr, "pick: prim %u n %u tex %u blend %u ab %u sb %u db %u ck %u at %.0f,%.0f-%.0f,%.0f\n", prim, n, dev->rs[1], dev->rs[21], dev->rs[27], dev->rs[19], dev->rs[20], dev->rs[41], x0, y0, x1, y1);
            for (uint32_t k = 0; k < n; k++) fprintf(stderr, "   %.1f %.1f c %08x uv %.4f %.4f\n", vbuf[k].x, vbuf[k].y, RD32(p + 32u * k + 16), vbuf[k].u, vbuf[k].v);
        }
    }
    static int dumped;
    if (getenv("P1_TEXDUMP") && !dumped && frame_counter >= 800 && dev->rs[1] == (uint32_t)atoi(getenv("P1_TEXDUMP"))) {
        dumped = 1;
        struct dd_surface *tx = textures[dev->rs[1]];
        FILE *f = fopen("build/tex.bin", "wb");
        const uint32_t *pal = dd_palette_entries(tx->palette);
        uint32_t hdr[4] = { tx->w, tx->h, tx->pitch, tx->fmt.bpp };
        fwrite(hdr, 4, 4, f);
        uint32_t zero[256] = { 0 };
        fwrite(pal ? pal : zero, 4, 256, f);
        fwrite(MEM + tx->pixels, 1, tx->pitch * tx->h, f);
        fclose(f);
        fprintf(stderr, "dumped texture %u (ck %d %x-%x)\n", dev->rs[1], tx->ck_src_set, tx->ck_src_lo, tx->ck_src_hi);
    }
    draw(prim, vbuf, n, 0, 0);
    return DD_OK;
}
static uint16_t ibuf[65536 * 3];
static uint32_t dev_DrawIndexedPrimitive(const uint32_t *a) {
    uint32_t prim = a[1], vt = a[2], p = a[3], n = a[4], ip = a[5], ni = a[6];
    if (vt != 3) rt_fatal("DrawIndexedPrimitive: vertex type %u", vt);
    if (n > 65536) n = 65536;
    if (ni > 65536 * 3) ni = 65536 * 3;
    for (uint32_t k = 0; k < n; k++) read_vtx(p + 32u * k, &vbuf[k]);
    for (uint32_t k = 0; k < ni; k++) { ibuf[k] = RD16(ip + 2u * k); if (ibuf[k] >= n) ibuf[k] = 0; }
    draw(prim, vbuf, n, ibuf, ni);
    return DD_OK;
}

// Begin / Vertex / End immediate mode
static uint32_t imm_prim, imm_n;
static uint32_t dev_Begin(const uint32_t *a) { imm_prim = a[1]; imm_n = 0; if (a[2] != 3) rt_fatal("Begin: vertex type %u", a[2]); return DD_OK; }
static uint32_t dev_Vertex(const uint32_t *a) { if (imm_n < 65536) read_vtx(a[1], &vbuf[imm_n++]); return DD_OK; }
static uint32_t dev_End(const uint32_t *a) { (void)a; draw(imm_prim, vbuf, imm_n, 0, 0); return DD_OK; }

static const struct com_method DEV2_IMPL[] = {
    { "AddRef", com_addref, 1 }, { "Release", com_release, 1 }, { "GetCaps", dev_GetCaps, 3 },
    { "GetStats", dev_GetStats, 2 }, { "AddViewport", dev_AddViewport, 2 },
    { "EnumTextureFormats", dev_EnumTextureFormats, 3 }, { "BeginScene", dev_BeginScene, 1 },
    { "EndScene", dev_EndScene, 1 }, { "SetCurrentViewport", dev_SetCurrentViewport, 2 },
    { "SetRenderTarget", dev_SetRenderTarget, 3 }, { "GetRenderTarget", dev_GetRenderTarget, 2 },
    { "Begin", dev_Begin, 4 }, { "Vertex", dev_Vertex, 2 }, { "End", dev_End, 2 },
    { "GetRenderState", dev_GetRenderState, 3 }, { "SetRenderState", dev_SetRenderState, 3 },
    { "SetLightState", dev_SetLightState, 3 }, { "SetTransform", dev_SetTransform, 3 },
    { "DrawPrimitive", dev_DrawPrimitive, 6 }, { "DrawIndexedPrimitive", dev_DrawIndexedPrimitive, 8 },
    { 0 } };

uint32_t d3d_create(void) {
    if (!vt_d3d2) {
        vt_d3d2 = com_vtable("IDirect3D2", D3D2_METHODS, N(D3D2_METHODS), D3D2_IMPL);
        vt_dev2 = com_vtable("IDirect3DDevice2", DEV2_METHODS, N(DEV2_METHODS), DEV2_IMPL);
        vt_vp2 = com_vtable("IDirect3DViewport2", VP2_METHODS, N(VP2_METHODS), VP2_IMPL);
        vt_mat2 = com_vtable("IDirect3DMaterial2", MAT2_METHODS, N(MAT2_METHODS), MAT2_IMPL);
        vt_tex2 = com_vtable("IDirect3DTexture2", TEX2_METHODS, N(TEX2_METHODS), TEX2_IMPL);
    }
    return com_new(vt_d3d2, 0);
}
