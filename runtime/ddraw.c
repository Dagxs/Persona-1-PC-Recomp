// DirectDraw / Direct3D (DirectX 5 interfaces) on the host. Interfaces are declared in full so
// every call the game makes is named; methods are implemented as the game needs them.
#include "com.h"
#include <stdlib.h>
#include <string.h>

static const char *const DD_METHODS[] = {
    "QueryInterface", "AddRef", "Release", "Compact", "CreateClipper", "CreatePalette", "CreateSurface",
    "DuplicateSurface", "EnumDisplayModes", "EnumSurfaces", "FlipToGDISurface", "GetCaps", "GetDisplayMode",
    "GetFourCCCodes", "GetGDISurface", "GetMonitorFrequency", "GetScanLine", "GetVerticalBlankStatus",
    "Initialize", "RestoreDisplayMode", "SetCooperativeLevel", "SetDisplayMode", "WaitForVerticalBlank",
    "GetAvailableVidMem" /* IDirectDraw2 */ };
static const char *const DDS_METHODS[] = {
    "QueryInterface", "AddRef", "Release", "AddAttachedSurface", "AddOverlayDirtyRect", "Blt", "BltBatch",
    "BltFast", "DeleteAttachedSurface", "EnumAttachedSurfaces", "EnumOverlayZOrders", "Flip",
    "GetAttachedSurface", "GetBltStatus", "GetCaps", "GetClipper", "GetColorKey", "GetDC", "GetFlipStatus",
    "GetOverlayPosition", "GetPalette", "GetPixelFormat", "GetSurfaceDesc", "Initialize", "IsLost", "Lock",
    "ReleaseDC", "Restore", "SetClipper", "SetColorKey", "SetOverlayPosition", "SetPalette", "Unlock",
    "UpdateOverlay", "UpdateOverlayDisplay", "UpdateOverlayZOrder",
    "GetDDInterface", "PageLock", "PageUnlock" /* IDirectDrawSurface2 */, "SetSurfaceDesc" /* 3 */ };
static const char *const D3D2_METHODS[] = {
    "QueryInterface", "AddRef", "Release", "EnumDevices", "CreateLight", "CreateMaterial", "CreateViewport",
    "FindDevice", "CreateDevice" };
static const char *const D3DDEV2_METHODS[] = {
    "QueryInterface", "AddRef", "Release", "GetCaps", "SwapTextureHandles", "GetStats", "AddViewport",
    "DeleteViewport", "NextViewport", "EnumTextureFormats", "BeginScene", "EndScene", "GetDirect3D",
    "SetCurrentViewport", "GetCurrentViewport", "SetRenderTarget", "GetRenderTarget", "Begin", "BeginIndexed",
    "Vertex", "Index", "End", "GetRenderState", "SetRenderState", "GetLightState", "SetLightState",
    "SetTransform", "GetTransform", "MultiplyTransform", "DrawPrimitive", "DrawIndexedPrimitive",
    "SetClipStatus", "GetClipStatus" };
#define N(x) ((int)(sizeof x / sizeof x[0]))

static uint32_t vt_dd, vt_dd2, vt_d3d2;

struct dd { uint32_t obj_dd, obj_dd2, obj_d3d2; uint32_t hwnd, coop; };

static uint32_t dd_QueryInterface(const uint32_t *a) {
    struct dd *d = com_host(a[0]);
    char g[40]; guid_str(a[1], g);
    uint32_t r = 0;
    if (guid_eq(a[1], IID_IDirectDraw)) r = d->obj_dd;
    else if (guid_eq(a[1], IID_IDirectDraw2)) r = d->obj_dd2;
    else if (guid_eq(a[1], IID_IDirect3D2)) r = d->obj_d3d2;
    if (rt_trace) fprintf(stderr, "IDirectDraw::QueryInterface(%s) -> %08x\n", g, r);
    WR32(a[2], r);
    if (!r) return E_NOINTERFACE;
    WR32(r + 8, RD32(r + 8) + 1);
    return DD_OK;
}
static uint32_t dd_SetCooperativeLevel(const uint32_t *a) {
    struct dd *d = com_host(a[0]);
    d->hwnd = a[1]; d->coop = a[2];
    return DD_OK;
}

static const struct com_method DD_IMPL[] = {
    { "QueryInterface", dd_QueryInterface, 3 }, { "AddRef", com_addref, 1 }, { "Release", com_release, 1 },
    { "SetCooperativeLevel", dd_SetCooperativeLevel, 3 },
    { 0 } };

static void make_vtables(void) {
    if (vt_dd) return;
    vt_dd = com_vtable("IDirectDraw", DD_METHODS, 23, DD_IMPL);
    vt_dd2 = com_vtable("IDirectDraw2", DD_METHODS, N(DD_METHODS), DD_IMPL);
    vt_d3d2 = com_vtable("IDirect3D2", D3D2_METHODS, N(D3D2_METHODS), 0);
    (void)D3DDEV2_METHODS; (void)DDS_METHODS;
}

DEF(DirectDrawCreate) {
    make_vtables();
    struct dd *d = calloc(1, sizeof *d);
    d->obj_dd = com_new(vt_dd, d);
    d->obj_dd2 = com_new(vt_dd2, d);
    d->obj_d3d2 = com_new(vt_d3d2, d);
    if (a[0] && rt_trace) { char g[40]; guid_str(a[0], g); fprintf(stderr, "DirectDrawCreate(device %s)\n", g); }
    WR32(a[1], d->obj_dd);
    return DD_OK;
}

const struct impl DDRAW_IMPL[] = { API("DDRAW.DLL", DirectDrawCreate, 3), { 0 } };
