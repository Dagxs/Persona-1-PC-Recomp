// KERNEL32: what the MSVC C runtime and the game call. Files are in files.c, threads in thread.c.
#include "win32.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <SDL3/SDL.h>

// ---------------------------------------------------------------- handles
#define MAXH 1024
static struct { int kind; void *obj; } handles[MAXH];
uint32_t handle_new(int kind, void *obj) {
    for (int k = 1; k < MAXH; k++)
        if (!handles[k].kind) { handles[k].kind = kind; handles[k].obj = obj; return 0x100u + 4u * (uint32_t)k; }
    rt_fatal("out of handles");
}
void *handle_get(uint32_t h, int kind) {
    uint32_t k = (h - 0x100u) / 4u;
    if (h < 0x100u || k >= MAXH || handles[k].kind != kind) return 0;
    return handles[k].obj;
}
int handle_kind(uint32_t h) {
    uint32_t k = (h - 0x100u) / 4u;
    if (h < 0x100u || k >= MAXH) return H_FREE;
    return handles[k].kind;
}
void handle_close(uint32_t h) {
    uint32_t k = (h - 0x100u) / 4u;
    if (h >= 0x100u && k < MAXH) { handles[k].kind = H_FREE; handles[k].obj = 0; }
}

// ---------------------------------------------------------------- version, process
DEF(GetVersion) { UNUSED_ARGS; return 0xc0000a04u; }           // Windows 98 (4.10, Win9x bit)
DEF(GetVersionExA) {
    uint32_t p = a[0];
    WR32(p + 4, 4); WR32(p + 8, 10); WR32(p + 12, 0x040a0222u); WR32(p + 16, 1);  // VER_PLATFORM_WIN32_WINDOWS
    gstrcpy(p + 20, "", 128);
    return 1;
}
DEF(GetCommandLineA) { UNUSED_ARGS; return g_cmdline; }
DEF(GetStartupInfoA) { memset(MEM + a[0], 0, 68); WR32(a[0], 68); return 0; }
DEF(GetModuleHandleA) { return a[0] ? 0 : IMAGE_BASE; }
DEF(GetModuleFileNameA) { gstrcpy(a[1], gstr(g_module_path), a[2]); return (uint32_t)strlen(gstr(a[1])); }
DEF(GetCurrentProcess) { UNUSED_ARGS; return 0xffffffffu; }
DEF(SetPriorityClass) { UNUSED_ARGS; return 1; }
DEF(SetErrorMode) { UNUSED_ARGS; return 0; }
DEF(ExitProcess) { if (rt_trace) fprintf(stderr, "ExitProcess(%u)\n", a[0]); exit((int)a[0]); }
DEF(TerminateProcess) { exit((int)a[1]); }
DEF(UnhandledExceptionFilter) { UNUSED_ARGS; return 0; }
DEF(RtlUnwind) { UNUSED_ARGS; rt_fatal("RtlUnwind: structured exception unwinding is not supported"); }
DEF(GetLastError) { UNUSED_ARGS; return last_error; }
DEF(SetLastError) { last_error = a[0]; return 0; }

// ---------------------------------------------------------------- heap and virtual memory
DEF(HeapCreate) { UNUSED_ARGS; return 0x00ee0000u; }
DEF(HeapDestroy) { UNUSED_ARGS; return 1; }
DEF(HeapAlloc) { return guest_heap_alloc(a[2], (a[1] & 8u) != 0); }       // HEAP_ZERO_MEMORY
DEF(HeapFree) { guest_heap_free(a[2]); return 1; }
DEF(HeapSize) { return guest_heap_size(a[2]); }
DEF(HeapReAlloc) {
    uint32_t old = guest_heap_size(a[2]);
    if (a[1] & 0x10u && a[3] > old) return 0;                                // HEAP_REALLOC_IN_PLACE_ONLY
    uint32_t p = guest_heap_realloc(a[2], a[3]);
    if ((a[1] & 8u) && a[3] > old) memset(MEM + p + old, 0, guest_heap_size(p) - old);
    return p;
}
// Reservations are carved from the heap region; commit/decommit only zero the pages.
DEF(VirtualAlloc) {
    uint32_t addr = a[0], size = a[1], type = a[2];
    if (addr && !(type & 0x2000u)) {                                         // commit inside a reservation
        if (type & 0x1000u) memset(MEM + (addr & ~0xfffu), 0, ((addr + size + 0xfffu) & ~0xfffu) - (addr & ~0xfffu));
        return addr;
    }
    return guest_vm_reserve(size);
}
DEF(VirtualFree) {
    if (a[2] & 0x4000u) memset(MEM + (a[0] & ~0xfffu), 0, a[1]);            // MEM_DECOMMIT
    return 1;
}

// ---------------------------------------------------------------- environment
DEF(GetEnvironmentStrings) { UNUSED_ARGS; uint32_t p = hostobj_alloc(4); return p; }   // empty block "\0\0"
DEF(GetEnvironmentStringsW) { UNUSED_ARGS; uint32_t p = hostobj_alloc(4); return p; }
DEF(FreeEnvironmentStringsA) { UNUSED_ARGS; return 1; }
DEF(FreeEnvironmentStringsW) { UNUSED_ARGS; return 1; }
DEF(GetEnvironmentVariableA) { UNUSED_ARGS; last_error = 203; return 0; }  // ERROR_ENVVAR_NOT_FOUND
DEF(SetEnvironmentVariableA) { UNUSED_ARGS; return 1; }

// ---------------------------------------------------------------- code pages and strings
DEF(GetACP) { UNUSED_ARGS; return 932; }
DEF(GetOEMCP) { UNUSED_ARGS; return 932; }
DEF(GetCPInfo) {
    uint32_t p = a[1];
    memset(MEM + p, 0, 20);
    WR32(p, 2);                       // MaxCharSize
    WR8(p + 4, '?');
    if (a[0] == 932 || a[0] == 0 || a[0] == 1) {  // Shift-JIS lead-byte ranges
        WR8(p + 6, 0x81); WR8(p + 7, 0x9f); WR8(p + 8, 0xe0); WR8(p + 9, 0xfc);
    }
    return 1;
}

static int sjis_lead(uint8_t c) { return (c >= 0x81 && c <= 0x9f) || (c >= 0xe0 && c <= 0xfc); }

// Narrow <-> wide conversion: the CRT only uses it for locale tables, so map bytes 1:1 and keep
// Shift-JIS pairs as one code unit (lead << 8 | trail). Round-trips exactly.
DEF(MultiByteToWideChar) {
    uint32_t src = a[2], dst = a[4];
    int32_t n = (int32_t)a[3], cap = (int32_t)a[5];
    if (n < 0) n = (int32_t)strlen(gstr(src)) + 1;
    int32_t out = 0;
    for (int32_t k = 0; k < n; k++) {
        uint32_t c = RD8(src + (uint32_t)k);
        if (sjis_lead((uint8_t)c) && k + 1 < n) { c = (c << 8) | RD8(src + (uint32_t)++k); }
        if (cap) { if (out >= cap) { last_error = 122; return 0; } WR16(dst + 2u * (uint32_t)out, c); }
        out++;
    }
    return (uint32_t)out;
}
DEF(WideCharToMultiByte) {
    uint32_t src = a[2], dst = a[4];
    int32_t n = (int32_t)a[3], cap = (int32_t)a[5];
    if (n < 0) { n = 0; while (RD16(src + 2u * (uint32_t)n)) n++; n++; }
    int32_t out = 0;
    for (int32_t k = 0; k < n; k++) {
        uint32_t c = RD16(src + 2u * (uint32_t)k);
        int w = c > 0xff ? 2 : 1;
        if (cap) {
            if (out + w > cap) { last_error = 122; return 0; }
            if (w == 2) { WR8(dst + (uint32_t)out, c >> 8); WR8(dst + (uint32_t)out + 1, c); }
            else WR8(dst + (uint32_t)out, c);
        }
        out += w;
    }
    return (uint32_t)out;
}
// GetStringType: classify single-byte characters (the CRT builds its ctype table with it)
static uint16_t ctype1(uint32_t c) {
    uint16_t t = 0;
    if (c >= 'A' && c <= 'Z') t |= 0x001 | 0x100;
    if (c >= 'a' && c <= 'z') t |= 0x002 | 0x100;
    if (c >= '0' && c <= '9') t |= 0x004;
    if (c == ' ' || (c >= 9 && c <= 13)) t |= 0x008;
    if ((c >= 0x21 && c <= 0x2f) || (c >= 0x3a && c <= 0x40) || (c >= 0x5b && c <= 0x60) || (c >= 0x7b && c <= 0x7e)) t |= 0x010;
    if (c < 0x20 || c == 0x7f) t |= 0x020;
    if (c == ' ' || c == 9) t |= 0x040;
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) t |= 0x080;
    if (c >= 0xa1 && c <= 0xdf) t |= 0x100;  // half-width katakana count as alpha
    return t;
}
DEF(GetStringTypeA) {
    uint32_t src = a[2], dst = a[4]; int32_t n = (int32_t)a[3];
    if (n < 0) n = (int32_t)strlen(gstr(src)) + 1;
    for (int32_t k = 0; k < n; k++) WR16(dst + 2u * (uint32_t)k, ctype1(RD8(src + (uint32_t)k)));
    return 1;
}
DEF(GetStringTypeW) {
    uint32_t src = a[1], dst = a[3]; int32_t n = (int32_t)a[2];
    if (n < 0) { n = 0; while (RD16(src + 2u * (uint32_t)n)) n++; n++; }
    for (int32_t k = 0; k < n; k++) WR16(dst + 2u * (uint32_t)k, ctype1(RD16(src + 2u * (uint32_t)k)));
    return 1;
}
static uint32_t fold(uint32_t c, uint32_t flags) {
    if ((flags & 0x100) && c >= 'A' && c <= 'Z') return c + 32;      // LCMAP_LOWERCASE
    if ((flags & 0x200) && c >= 'a' && c <= 'z') return c - 32;      // LCMAP_UPPERCASE
    return c;
}
DEF(LCMapStringA) {
    uint32_t src = a[2], dst = a[4]; int32_t n = (int32_t)a[3], cap = (int32_t)a[5];
    if (n < 0) n = (int32_t)strlen(gstr(src)) + 1;
    if (!cap) return (uint32_t)n;
    if (cap < n) { last_error = 122; return 0; }
    for (int32_t k = 0; k < n; k++) WR8(dst + (uint32_t)k, fold(RD8(src + (uint32_t)k), a[1]));
    return (uint32_t)n;
}
DEF(LCMapStringW) {
    uint32_t src = a[2], dst = a[4]; int32_t n = (int32_t)a[3], cap = (int32_t)a[5];
    if (n < 0) { n = 0; while (RD16(src + 2u * (uint32_t)n)) n++; n++; }
    if (!cap) return (uint32_t)n;
    if (cap < n) { last_error = 122; return 0; }
    for (int32_t k = 0; k < n; k++) WR16(dst + 2u * (uint32_t)k, fold(RD16(src + 2u * (uint32_t)k), a[1]));
    return (uint32_t)n;
}
static uint32_t compare(uint32_t s1, int32_t n1, uint32_t s2, int32_t n2, int wide, uint32_t flags) {
    int sz = wide ? 2 : 1;
    if (n1 < 0) { n1 = 0; while (wide ? RD16(s1 + 2u * (uint32_t)n1) : RD8(s1 + (uint32_t)n1)) n1++; }
    if (n2 < 0) { n2 = 0; while (wide ? RD16(s2 + 2u * (uint32_t)n2) : RD8(s2 + (uint32_t)n2)) n2++; }
    for (int32_t k = 0; k < n1 && k < n2; k++) {
        uint32_t x = wide ? RD16(s1 + (uint32_t)(k * sz)) : RD8(s1 + (uint32_t)k);
        uint32_t y = wide ? RD16(s2 + (uint32_t)(k * sz)) : RD8(s2 + (uint32_t)k);
        if (flags & 1) { x = fold(x, 0x100); y = fold(y, 0x100); }   // NORM_IGNORECASE
        if (x != y) return x < y ? 1 : 3;
    }
    return n1 < n2 ? 1 : n1 > n2 ? 3 : 2;
}
DEF(CompareStringA) { return compare(a[2], (int32_t)a[3], a[4], (int32_t)a[5], 0, a[1]); }
DEF(CompareStringW) { return compare(a[2], (int32_t)a[3], a[4], (int32_t)a[5], 1, a[1]); }

// ---------------------------------------------------------------- time
static void put_systemtime(uint32_t p, struct tm *t, int ms) {
    WR16(p, t->tm_year + 1900); WR16(p + 2, t->tm_mon + 1); WR16(p + 4, t->tm_wday); WR16(p + 6, t->tm_mday);
    WR16(p + 8, t->tm_hour); WR16(p + 10, t->tm_min); WR16(p + 12, t->tm_sec); WR16(p + 14, ms);
}
DEF(GetLocalTime) { time_t now = time(0); struct tm t = *localtime(&now); put_systemtime(a[0], &t, 0); return 0; }
DEF(GetSystemTime) { time_t now = time(0); struct tm t = *gmtime(&now); put_systemtime(a[0], &t, 0); return 0; }
DEF(GetTimeZoneInformation) { memset(MEM + a[0], 0, 172); WR32(a[0], (uint32_t)-540); return 0; }   // JST, no DST
void thread_sleep(uint32_t ms);
DEF(Sleep) { thread_sleep(a[0]); return 0; }

// ---------------------------------------------------------------- critical sections, interlocked, TLS
// Guest threads are serialised by the runtime, so a critical section only needs a recursion count.
DEF(InitializeCriticalSection) { memset(MEM + a[0], 0, 24); return 0; }
DEF(DeleteCriticalSection) { UNUSED_ARGS; return 0; }
DEF(EnterCriticalSection) { thread_cs_enter(a[0]); return 0; }
DEF(LeaveCriticalSection) { thread_cs_leave(a[0]); return 0; }
DEF(InterlockedIncrement) { uint32_t v = RD32(a[0]) + 1; WR32(a[0], v); return v; }
DEF(InterlockedDecrement) { uint32_t v = RD32(a[0]) - 1; WR32(a[0], v); return v; }
static uint32_t tls_next;
DEF(TlsAlloc) { UNUSED_ARGS; return tls_next < 64 ? tls_next++ : 0xffffffffu; }
DEF(TlsGetValue) { last_error = 0; return RD32(thread_teb() + 0xe10u + 4u * a[0]); }
DEF(TlsSetValue) { WR32(thread_teb() + 0xe10u + 4u * a[0], a[1]); return 1; }
DEF(GetCurrentThreadId) { UNUSED_ARGS; return thread_current_id(); }

// ---------------------------------------------------------------- modules
// LoadLibrary/GetProcAddress hand out host implementations (DSOUND, DINPUT...) by name.
DEF(LoadLibraryA) {
    const char *n = gstr(a[0]);
    if (rt_trace) fprintf(stderr, "LoadLibraryA(\"%s\")\n", n);
    char base[64]; size_t k = 0;
    const char *s = strrchr(n, '\\'); s = s ? s + 1 : n;
    for (; s[k] && s[k] != '.' && k < 60; k++) base[k] = s[k];
    base[k] = 0;
    strcat(base, ".DLL");
    for (char *c = base; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
    return handle_new(H_MODULE, strdup(base));
}
DEF(FreeLibrary) { if (handle_get(a[0], H_MODULE)) handle_close(a[0]); return 1; }
DEF(GetProcAddress) {
    const char *dll = handle_get(a[0], H_MODULE);
    if (!dll) dll = "KERNEL32.DLL";
    if (a[1] < 0x10000u) rt_fatal("GetProcAddress by ordinal %u from %s", a[1], dll);
    const char *name = gstr(a[1]);
    if (rt_trace) fprintf(stderr, "GetProcAddress(%s, \"%s\")\n", dll, name);
    if (!find_impl(dll, name)) { last_error = 127; return 0; }
    return resolve_import(dll, name);
}

const struct impl KERNEL32_IMPL[] = {
#define K(name, n) API("KERNEL32.DLL", name, n)
    K(GetVersion, 0), K(GetVersionExA, 1), K(GetCommandLineA, 0), K(GetStartupInfoA, 1),
    K(GetModuleHandleA, 1), K(GetModuleFileNameA, 3), K(GetCurrentProcess, 0), K(SetPriorityClass, 2),
    K(SetErrorMode, 1), K(ExitProcess, 1), K(TerminateProcess, 2), K(UnhandledExceptionFilter, 1),
    K(RtlUnwind, 4), K(GetLastError, 0), K(SetLastError, 1),
    K(HeapCreate, 3), K(HeapDestroy, 1), K(HeapAlloc, 3), K(HeapFree, 3), K(HeapSize, 3), K(HeapReAlloc, 4),
    K(VirtualAlloc, 4), K(VirtualFree, 3),
    K(GetEnvironmentStrings, 0), K(GetEnvironmentStringsW, 0), K(FreeEnvironmentStringsA, 1),
    K(FreeEnvironmentStringsW, 1), K(GetEnvironmentVariableA, 3), K(SetEnvironmentVariableA, 2),
    K(GetACP, 0), K(GetOEMCP, 0), K(GetCPInfo, 2), K(MultiByteToWideChar, 6), K(WideCharToMultiByte, 8),
    K(GetStringTypeA, 5), K(GetStringTypeW, 4), K(LCMapStringA, 6), K(LCMapStringW, 6),
    K(CompareStringA, 6), K(CompareStringW, 6),
    K(GetLocalTime, 1), K(GetSystemTime, 1), K(GetTimeZoneInformation, 1), K(Sleep, 1),
    K(InitializeCriticalSection, 1), K(DeleteCriticalSection, 1), K(EnterCriticalSection, 1),
    K(LeaveCriticalSection, 1), K(InterlockedIncrement, 1), K(InterlockedDecrement, 1),
    K(TlsAlloc, 0), K(TlsGetValue, 1), K(TlsSetValue, 2), K(GetCurrentThreadId, 0),
    K(LoadLibraryA, 1), K(FreeLibrary, 1), K(GetProcAddress, 2),
#undef K
    { 0 }
};
