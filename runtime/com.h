// Host-implemented COM objects.
#pragma once
#include "win32.h"

struct com_method { const char *name; api_fn fn; int nargs; };   // nargs includes `this`
uint32_t com_vtable(const char *iface, const char *const *names, int n, const struct com_method *impl);
uint32_t com_new(uint32_t vtable, void *host);
void com_set_host(uint32_t obj, void *host);
void *com_host(uint32_t obj);
uint32_t com_missing(const uint32_t *a);
uint32_t com_addref(const uint32_t *a);
uint32_t com_release(const uint32_t *a);
int guid_eq(uint32_t p, const char *s);
void guid_str(uint32_t p, char *out);
uint32_t dshow_create(uint32_t iid, uint32_t out);
int dshow_is_multimedia_stream(uint32_t clsid);
uint32_t movie_create(uint32_t iid, uint32_t out);

#define CLSID_FilterGraph   "e436ebb3-524f-11ce-9f53-0020af0ba770"
#define IID_IDirectDraw     "6c14db80-a733-11ce-a521-0020af0be560"
#define IID_IDirectDraw2    "b3a6f3e0-2b43-11cf-a2de-00aa00b93356"
#define IID_IDirectDrawSurface  "6c14db81-a733-11ce-a521-0020af0be560"
#define IID_IDirectDrawSurface2 "57805885-6eec-11cf-9441-a82303c10e27"
#define IID_IDirectDrawSurface3 "da044e00-69b2-11d0-a1d5-00aa00b8dfbb"
#define IID_IDirect3D       "3bba0080-2421-11cf-a31a-00aa00b93356"
#define IID_IDirect3D2      "6aae1ec1-662a-11d0-889d-00aa00bbb76a"
#define IID_IDirect3DHALDevice "84e63de0-46aa-11cf-816f-0000c020156e"
#define IID_IDirect3DRGBDevice "a4665c60-2673-11cf-a31a-00aa00b93356"
#define IID_IDirect3DTexture2  "93281502-8cf8-11d0-89ab-00a0c9054129"
#define IID_IDirect3DTexture   "2cdcd9e0-25a0-11cf-a31a-00aa00b93356"

#define DD_OK 0
#define E_NOINTERFACE 0x80004002u
#define DDERR_UNSUPPORTED 0x80004001u
#define DDERR_GENERIC 0x80004005u
