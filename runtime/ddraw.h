// DirectDraw surfaces shared between ddraw.c and the Direct3D rasterizer (d3d.c).
#pragma once
#include <stdint.h>

#define DDSCAPS_BACKBUFFER     0x00000004u
#define DDSCAPS_COMPLEX        0x00000008u
#define DDSCAPS_FLIP           0x00000010u
#define DDSCAPS_FRONTBUFFER    0x00000020u
#define DDSCAPS_PRIMARYSURFACE 0x00000200u
#define DDSCAPS_SYSTEMMEMORY   0x00000800u
#define DDSCAPS_TEXTURE        0x00001000u
#define DDSCAPS_3DDEVICE       0x00002000u
#define DDSCAPS_VIDEOMEMORY    0x00004000u
#define DDSCAPS_ZBUFFER        0x00020000u

struct pixfmt { uint32_t bpp, r, g, b, a, pal8; };
extern const struct pixfmt PF_565;

struct dd_surface {
    uint32_t w, h, pitch, caps;
    uint32_t pixels;                 // guest address
    struct pixfmt fmt;
    uint32_t obj[3];                 // IDirectDrawSurface, 2, 3
    uint32_t tex_obj;                // IDirect3DTexture2, created on demand
    uint32_t tex_handle;
    struct dd_surface *back, *zbuf;
    int ck_src_set;
    uint32_t ck_src_lo, ck_src_hi;
    void *palette;
};

struct dd_surface *dd_surface_of(uint32_t obj);
extern struct dd_surface *dd_primary;
const uint32_t *dd_palette_entries(void *pal);
void dd_present(struct dd_surface *s);
extern uint32_t frame_counter;

uint32_t d3d_create(void);
uint32_t d3d_texture_of(struct dd_surface *s);
void dump_frame_if_requested(const uint32_t *px, int w, int h);
