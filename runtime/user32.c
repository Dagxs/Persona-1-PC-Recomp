// USER32 / GDI32: one top-level window (an SDL window), its window procedure, a small
// message queue fed from SDL events, the keyboard state for GetAsyncKeyState, wsprintfA.
#include "win32.h"
#include "host.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_PAINT 0x000f
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_ACTIVATEAPP 0x001c
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104

// ---------------------------------------------------------------- calling guest code from the host
uint32_t guest_call(uint32_t fn, int n, const uint32_t *args) {
    for (int k = n - 1; k >= 0; k--) PUSH32(args[k]);
    rt_call(fn, MAGIC_RET);      // the callee's `ret` returns to here (stdcall pops the args)
    return EAX;
}

// ---------------------------------------------------------------- window classes and windows
struct wclass { char name[64]; uint32_t wndproc; };
static struct wclass classes[16];
static int nclasses;

struct gwindow { uint32_t hwnd, wndproc; char cls[64]; int alive; };
static struct gwindow windows[8];
static int nwindows;
uint32_t main_hwnd;

static struct gwindow *win_of(uint32_t h) {
    for (int k = 0; k < nwindows; k++) if (windows[k].hwnd == h && windows[k].alive) return &windows[k];
    return 0;
}
static uint32_t send(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    struct gwindow *w = win_of(hwnd);
    if (!w) return 0;
    uint32_t args[4] = { hwnd, msg, wp, lp };
    return guest_call(w->wndproc, 4, args);
}

DEF(RegisterClassA) {
    if (nclasses == 16) rt_fatal("too many window classes");
    struct wclass *c = &classes[nclasses++];
    c->wndproc = RD32(a[0] + 4);
    snprintf(c->name, sizeof c->name, "%s", gstr(RD32(a[0] + 36)));
    return 0xc000u + (uint32_t)nclasses;
}
DEF(FindWindowA) {
    const char *cls = a[0] ? gstr(a[0]) : 0;
    for (int k = 0; k < nwindows; k++)
        if (windows[k].alive && (!cls || !strcmp(cls, windows[k].cls))) return windows[k].hwnd;
    return 0;
}
DEF(CreateWindowExA) {
    const char *cls = gstr(a[1]);
    struct wclass *c = 0;
    for (int k = 0; k < nclasses; k++) if (!strcmp(classes[k].name, cls)) c = &classes[k];
    if (!c) rt_fatal("CreateWindowExA: unknown class %s", cls);
    struct gwindow *w = &windows[nwindows++];
    w->hwnd = 0x10010u + 0x10u * (uint32_t)nwindows;
    w->wndproc = c->wndproc;
    w->alive = 1;
    snprintf(w->cls, sizeof w->cls, "%s", cls);
    if (!main_hwnd) main_hwnd = w->hwnd;
    host_window_create(a[2] ? gstr(a[2]) : "Persona");
    // WM_CREATE with a CREATESTRUCT
    uint32_t cs = hostobj_alloc(48);
    WR32(cs + 0, a[11]); WR32(cs + 4, IMAGE_BASE); WR32(cs + 20, a[7]); WR32(cs + 16, a[6]);
    WR32(cs + 28, a[4]); WR32(cs + 24, a[5]); WR32(cs + 32, a[3]); WR32(cs + 36, a[2]); WR32(cs + 40, a[1]);
    send(w->hwnd, WM_CREATE, 0, cs);
    return w->hwnd;
}
DEF(DestroyWindow) {
    struct gwindow *w = win_of(a[0]);
    if (!w) return 0;
    send(a[0], WM_DESTROY, 0, 0);
    w->alive = 0;
    return 1;
}
DEF(UpdateWindow) { send(a[0], WM_PAINT, 0, 0); return 1; }
DEF(DefWindowProcA) {
    if (a[1] == WM_CLOSE) DestroyWindow_host(a[0]);
    return 0;
}
void DestroyWindow_host(uint32_t hwnd) {
    struct gwindow *w = win_of(hwnd);
    if (!w) return;
    send(hwnd, WM_DESTROY, 0, 0);
    w->alive = 0;
}
DEF(BeginPaint) { memset(MEM + a[1], 0, 64); WR32(a[1], 0x20001u); return 0x20001u; }
DEF(EndPaint) { UNUSED_ARGS; return 1; }
DEF(LoadIconA) { UNUSED_ARGS; return 0x30001u; }
DEF(SetCursor) { UNUSED_ARGS; return 0; }
DEF(SetCursorPos) { UNUSED_ARGS; return 1; }
DEF(GetSystemMetrics) {
    switch (a[0]) { case 0: return 640; case 1: return 480; default: return 0; }
}
DEF(MessageBoxA) {
    const char *text = gstr(a[1]), *cap = gstr(a[2]);
    fprintf(stderr, "MessageBox [%s] %s\n", cap ? cap : "", text ? text : "");
    host_message_box(cap, text);
    return 1;   // IDOK
}

// ---------------------------------------------------------------- message queue
struct msg { uint32_t hwnd, message, wp, lp; };
static struct msg queue[256];
static int qhead, qtail, quit_posted;
static uint32_t quit_code;
static void post(uint32_t hwnd, uint32_t m, uint32_t wp, uint32_t lp) {
    int nx = (qtail + 1) & 255;
    if (nx == qhead) return;
    queue[qtail] = (struct msg){ hwnd, m, wp, lp };
    qtail = nx;
}
void user32_post_key(int vk, int down) { post(main_hwnd, down ? WM_KEYDOWN : WM_KEYUP, (uint32_t)vk, down ? 1u : 0xc0000001u); }
void user32_post_close(void) { post(main_hwnd, WM_CLOSE, 0, 0); }

DEF(PostQuitMessage) { quit_posted = 1; quit_code = a[0]; return 0; }
DEF(PeekMessageA) {
    host_pump_events();
    rt_yield();
    uint32_t m = a[0];
    if (qhead == qtail) {
        if (quit_posted) {
            if (a[4] & 1) quit_posted = 0;
            memset(MEM + m, 0, 28); WR32(m + 4, WM_QUIT); WR32(m + 8, quit_code);
            return 1;
        }
        return 0;
    }
    struct msg q = queue[qhead];
    if (a[4] & 1) qhead = (qhead + 1) & 255;      // PM_REMOVE
    memset(MEM + m, 0, 28);
    WR32(m, q.hwnd); WR32(m + 4, q.message); WR32(m + 8, q.wp); WR32(m + 12, q.lp);
    WR32(m + 16, (uint32_t)SDL_GetTicks());
    return 1;
}
DEF(TranslateMessage) { UNUSED_ARGS; return 0; }
DEF(DispatchMessageA) {
    uint32_t m = a[0];
    return send(RD32(m), RD32(m + 4), RD32(m + 8), RD32(m + 12));
}
DEF(GetAsyncKeyState) { return host_key_down((int)a[0]) ? 0x8000u : 0; }

// ---------------------------------------------------------------- wsprintfA (cdecl, varargs)
DEF(wsprintfA) {
    uint32_t out = a[0];
    const char *f = gstr(a[1]);
    uint32_t ap = ESP + 12;          // first variadic argument (after return address, buf, fmt)
    char buf[1100]; int n = 0;
    while (*f && n < 1024) {
        if (*f != '%') { buf[n++] = *f++; continue; }
        f++;
        if (*f == '%') { buf[n++] = '%'; f++; continue; }
        char spec[16]; int k = 0;
        spec[k++] = '%';
        while (*f && strchr("-+ #0", *f) && k < 8) spec[k++] = *f++;
        while (*f >= '0' && *f <= '9' && k < 12) spec[k++] = *f++;
        if (*f == 'l' || *f == 'h') f++;
        char c = *f++;
        uint32_t v = RD32(ap); ap += 4;
        if (c == 's') { spec[k++] = 's'; spec[k] = 0; n += snprintf(buf + n, sizeof buf - n, spec, v ? gstr(v) : "(null)"); }
        else if (c == 'c') { buf[n++] = (char)v; }
        else if (c == 'd' || c == 'i') { spec[k++] = 'd'; spec[k] = 0; n += snprintf(buf + n, sizeof buf - n, spec, (int32_t)v); }
        else if (c == 'u' || c == 'x' || c == 'X') { spec[k++] = c; spec[k] = 0; n += snprintf(buf + n, sizeof buf - n, spec, v); }
        else rt_fatal("wsprintfA: unsupported conversion %%%c", c);
    }
    buf[n] = 0;
    gstrcpy(out, buf, 1025);
    return (uint32_t)n;
}

// ---------------------------------------------------------------- GDI (text on the window: debug/errors only)
DEF(GetStockObject) { return 0x40000u + a[0]; }
DEF(SetBkMode) { UNUSED_ARGS; return 1; }
DEF(SetTextColor) { UNUSED_ARGS; return 0; }
DEF(TextOutA) {
    char s[256]; uint32_t n = a[4] < 255 ? a[4] : 255;
    memcpy(s, MEM + a[3], n); s[n] = 0;
    if (rt_trace) fprintf(stderr, "TextOutA(%d,%d): %s\n", (int)a[1], (int)a[2], s);
    return 1;
}

const struct impl USER32_IMPL[] = {
#define K(name, n) API("USER32.DLL", name, n)
    K(RegisterClassA, 1), K(FindWindowA, 2), K(CreateWindowExA, 12), K(DestroyWindow, 1), K(UpdateWindow, 1),
    K(DefWindowProcA, 4), K(BeginPaint, 2), K(EndPaint, 2), K(LoadIconA, 2), K(SetCursor, 1),
    K(SetCursorPos, 2), K(GetSystemMetrics, 1), K(MessageBoxA, 4), K(PostQuitMessage, 1),
    K(PeekMessageA, 5), K(TranslateMessage, 1), K(DispatchMessageA, 1), K(GetAsyncKeyState, 1),
    K(wsprintfA, -1),
#undef K
    { 0 }
};
const struct impl GDI32_IMPL[] = {
#define K(name, n) API("GDI32.DLL", name, n)
    K(GetStockObject, 1), K(SetBkMode, 2), K(SetTextColor, 2), K(TextOutA, 5),
#undef K
    { 0 }
};
