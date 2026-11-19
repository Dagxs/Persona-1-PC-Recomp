// ADVAPI32 (the game's settings), WINMM (timer, joystick), OLE32 (COM), QUARTZ.
#include "win32.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- registry
// HKLM\SOFTWARE\ASCII Corp.\Persona, normally written by the disc's CONFIG.EXE. Defaults below
// are what CONFIG.EXE writes on a fresh install: numpad D-pad, S/Z/W/A face buttons,
// numpad 7/1/9/3 shoulders, numpad 0 select, numpad . start, volumes 100.
static const uint8_t DEVCONFIG[216] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x01,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x02,0x01,0x00,
    0xc1,0x07,0xd0,0x95,0x7c,0x72,0x46,0x30,0x38,0x00,0x00,0x00,0xf3,0xac,0x3f,0x00,0xb3,0x61,0x73,0x0c,
    0xff,0x00,0x00,0x00,0xff,0x1f,0x00,0x00,0xff,0x1f,0x00,0x00,0xff,0x00,0x00,0x00,0x8a,0x52,0x0c,0x00,
    0x4d,0xec,0x01,0x00,0x3f,0x07,0x03,0x07,0xcf,0x00,0x00,0x00,0x3f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x38,0x00,0x00,0x00,0x70,0x00,0x00,0x00,0xd1,0x01,0x00,0x00,0xff,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x8a,0xa2,0x0c,0x00,0x0b,0x00,0x00,0x00,
    0x1f,0x00,0x00,0x00,0x42,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x04,0x00,0x00,0x00,
    0x38,0x00,0x00,0x00,0x70,0x00,0x00,0x00,0x51,0x00,0x00,0x00,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x45,0xa1,0x00,0x00,0x0b,0x00,0x00,0x00,0x05,0x00,0x00,0x00,
    0x42,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x04,0x00,0x00,0x00,
};
static const uint8_t KEYS[14] = { 104, 98, 100, 102, 83, 90, 87, 65, 103, 97, 105, 99, 96, 110 };

#define HKEY_PERSONA 0x7e570001u
DEF(RegOpenKeyExA) {
    const char *k = gstr(a[1]);
    if (a[0] == 0x80000002u && k && !strcasecmp_ascii(k, "SOFTWARE\\ASCII Corp.\\Persona")) {
        WR32(a[4], HKEY_PERSONA);
        return 0;
    }
    return 2;     // ERROR_FILE_NOT_FOUND
}
DEF(RegCloseKey) { UNUSED_ARGS; return 0; }
static uint32_t put_value(uint32_t type_p, uint32_t data_p, uint32_t size_p, const void *v, uint32_t n) {
    if (type_p) WR32(type_p, 3);                     // REG_BINARY
    if (!size_p) return 0;
    uint32_t cap = RD32(size_p);
    WR32(size_p, n);
    if (!data_p) return 0;
    if (cap < n) return 234;                         // ERROR_MORE_DATA
    memcpy(MEM + data_p, v, n);
    return 0;
}
DEF(RegQueryValueExA) {
    if (a[0] != HKEY_PERSONA) return 6;
    const char *name = gstr(a[1]);
    uint8_t v[16] = { 0 };
    if (!strncmp(name, "Key_", 4)) {
        int k = atoi(name + 4);
        if (k < 1 || k > 14) return 2;
        v[0] = KEYS[k - 1];
        return put_value(a[3], a[4], a[5], v, 2);
    }
    if (!strcmp(name, "DevConfig")) return put_value(a[3], a[4], a[5], DEVCONFIG, sizeof DEVCONFIG);
    if (!strcmp(name, "GUID")) return put_value(a[3], a[4], a[5], v, 16);
    if (!strcmp(name, "AutoSelect")) { v[0] = 1; return put_value(a[3], a[4], a[5], v, 1); }
    if (!strcmp(name, "SEVol") || !strcmp(name, "BGMVol")) { v[0] = 100; return put_value(a[3], a[4], a[5], v, 1); }
    if (!strcmp(name, "SEVolRange")) { v[0] = 0xe8; v[1] = 3; return put_value(a[3], a[4], a[5], v, 2); }
    if (!strcmp(name, "Version")) { v[0] = 10; return put_value(a[3], a[4], a[5], v, 1); }
    return 2;
}

// ---------------------------------------------------------------- WINMM
// P1_TURBO=n (tests): a virtual clock that moves n ms on every call, so headless runs reach a
// scene sooner and scripted keys (P1_KEYS) land on the same frames every run
DEF(timeGetTime) {
    UNUSED_ARGS;
    static int turbo = -1;
    static uint32_t clock;
    if (turbo < 0) turbo = getenv("P1_TURBO") ? atoi(getenv("P1_TURBO")) : 0;
    if (turbo > 0) return clock += (uint32_t)turbo;
    return (uint32_t)SDL_GetTicks();
}
DEF(joyGetNumDevs) { UNUSED_ARGS; return 0; }
DEF(joyGetDevCapsA) { UNUSED_ARGS; return 165; }    // JOYERR_UNPLUGGED
DEF(joyGetPosEx) { UNUSED_ARGS; return 167; }

// ---------------------------------------------------------------- OLE32 / QUARTZ
DEF(CoInitialize) { UNUSED_ARGS; return 0; }
DEF(CoUninitialize) { UNUSED_ARGS; return 0; }
uint32_t com_create_instance(uint32_t clsid, uint32_t iid, uint32_t out);
DEF(CoCreateInstance) { return com_create_instance(a[0], a[3], a[4]); }
DEF(AMGetErrorTextA) { if (a[2]) gstrcpy(a[1], "DirectShow error", a[2]); return 1; }

const struct impl ADVAPI32_IMPL[] = {
    API("ADVAPI32.DLL", RegOpenKeyExA, 5), API("ADVAPI32.DLL", RegCloseKey, 1), API("ADVAPI32.DLL", RegQueryValueExA, 6),
    { 0 }
};
const struct impl WINMM_IMPL[] = {
    API("WINMM.DLL", timeGetTime, 0), API("WINMM.DLL", joyGetNumDevs, 0), API("WINMM.DLL", joyGetDevCapsA, 3),
    API("WINMM.DLL", joyGetPosEx, 2),
    { 0 }
};
const struct impl OLE32_IMPL[] = {
    API("OLE32.DLL", CoInitialize, 1), API("OLE32.DLL", CoUninitialize, 0), API("OLE32.DLL", CoCreateInstance, 5),
    { 0 }
};
const struct impl QUARTZ_IMPL[] = { API("QUARTZ.DLL", AMGetErrorTextA, 3), { 0 } };
