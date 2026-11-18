// Runtime core: guest memory, CPU state, dispatch of indirect branches, string ops, x87 helpers.
#include "rt.h"
#include "paths.h"
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

uint32_t R[8], DF, FS_BASE;
uint32_t fk = K_LOGIC, fsz = 32, fa, fb, fr, fc, fraw;
double FPR[8];
uint32_t FPU_TOP, FPU_C0, FPU_C1, FPU_C2, FPU_C3, FPU_CW = 0x027f;
uint8_t *MEM;
volatile int rt_switch_pending;
int rt_trace;

// ---------------------------------------------------------------- dispatch
// Direct-mapped table: one host function pointer per byte of the guest code range.
static guest_fn_t *code_map;

static void build_code_map(void) {
    code_map = calloc(TEXT_HI - TEXT_LO, sizeof(guest_fn_t));
    for (int k = 0; k < GUEST_NFUNCS; k++) code_map[GUEST_FUNCS[k].addr - TEXT_LO] = GUEST_FUNCS[k].fn;
}

guest_fn_t rt_lookup(uint32_t a) {
    if (a >= TEXT_LO && a < TEXT_HI) return code_map[a - TEXT_LO];
    return 0;
}

void rt_call(uint32_t target, uint32_t ret) {
    PUSH32(ret);
    if (target >= MAGIC_BASE) { host_invoke(target); return; }
    guest_fn_t f = rt_lookup(target);
    if (!f) rt_fatal("call to unknown address %08x (return %08x): add it to tools/extra_entries.txt", target, ret);
    f();
}

void rt_jump(uint32_t target) {
    if (target >= MAGIC_BASE) { host_invoke(target); return; }
    guest_fn_t f = rt_lookup(target);
    if (!f) rt_fatal("jump to unknown address %08x: add it to tools/extra_entries.txt", target);
    f();
}

// ---------------------------------------------------------------- errors
_Noreturn void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FATAL: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n  eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n",
            EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI);
    fprintf(stderr, "  stack:");
    for (int k = 0; k < 12; k++) fprintf(stderr, " %08x", RD32(ESP + 4 * k));
    fprintf(stderr, "\n");
    va_end(ap);
    fflush(stderr);
    exit(70);
}

void rt_unimpl(uint32_t addr, const char *what) { rt_fatal("untranslated instruction at %08x: %s", addr, what); }
void rt_divide_error(void) { rt_fatal("divide error"); }

// ---------------------------------------------------------------- string instructions
static inline uint32_t rdn(uint32_t a, int sz) { return sz == 8 ? RD8(a) : sz == 16 ? RD16(a) : RD32(a); }
static inline void wrn(uint32_t a, uint32_t v, int sz) { if (sz == 8) WR8(a, v); else if (sz == 16) WR16(a, v); else WR32(a, v); }
static inline uint32_t acc(int sz) { return EAX & szmask(sz); }
static inline void set_acc(uint32_t v, int sz) { EAX = (EAX & ~szmask(sz)) | (v & szmask(sz)); }

void str_movs(int sz, int mode) {
    int d = (DF ? -1 : 1) * sz / 8;
    if (mode && d > 0 && ECX) {                       // fast path for forward rep movs
        uint32_t n = ECX * (uint32_t)(sz / 8);
        if (ESI + n <= MEM_SIZE && EDI + n <= MEM_SIZE && (ESI >= EDI + n || EDI >= ESI + n)) {
            memcpy(MEM + EDI, MEM + ESI, n); ESI += n; EDI += n; ECX = 0; return;
        }
    }
    if (!mode) { wrn(EDI, rdn(ESI, sz), sz); ESI += d; EDI += d; return; }
    while (ECX) { wrn(EDI, rdn(ESI, sz), sz); ESI += d; EDI += d; ECX--; }
}
void str_stos(int sz, int mode) {
    int d = (DF ? -1 : 1) * sz / 8;
    if (mode && d > 0 && sz == 8 && ECX && EDI + ECX <= MEM_SIZE) { memset(MEM + EDI, (uint8_t)EAX, ECX); EDI += ECX; ECX = 0; return; }
    if (!mode) { wrn(EDI, acc(sz), sz); EDI += d; return; }
    while (ECX) { wrn(EDI, acc(sz), sz); EDI += d; ECX--; }
}
void str_lods(int sz, int mode) {
    int d = (DF ? -1 : 1) * sz / 8;
    if (!mode) { set_acc(rdn(ESI, sz), sz); ESI += d; return; }
    while (ECX) { set_acc(rdn(ESI, sz), sz); ESI += d; ECX--; }
}
void str_scas(int sz, int mode) {
    int d = (DF ? -1 : 1) * sz / 8;
    if (!mode) { op_sub(acc(sz), rdn(EDI, sz), sz); EDI += d; return; }
    while (ECX) {
        op_sub(acc(sz), rdn(EDI, sz), sz); EDI += d; ECX--;
        if (mode == 1 ? !cc_z() : cc_z()) break;
    }
}
void str_cmps(int sz, int mode) {
    int d = (DF ? -1 : 1) * sz / 8;
    if (!mode) { op_sub(rdn(ESI, sz), rdn(EDI, sz), sz); ESI += d; EDI += d; return; }
    while (ECX) {
        op_sub(rdn(ESI, sz), rdn(EDI, sz), sz); ESI += d; EDI += d; ECX--;
        if (mode == 1 ? !cc_z() : cc_z()) break;
    }
}

// ---------------------------------------------------------------- x87 helpers
double fpu_round(double v) {
    switch ((FPU_CW >> 10) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}
void fpu_init(void) { FPU_TOP = 0; FPU_CW = 0x037f; FPU_C0 = FPU_C1 = FPU_C2 = FPU_C3 = 0; }
void fpu_xam(void) {
    double v = ST(0);
    FPU_C1 = signbit(v) ? 1 : 0;
    if (isnan(v)) { FPU_C3 = 0; FPU_C2 = 0; FPU_C0 = 1; }
    else if (isinf(v)) { FPU_C3 = 0; FPU_C2 = 1; FPU_C0 = 1; }
    else if (v == 0) { FPU_C3 = 1; FPU_C2 = 0; FPU_C0 = 0; }
    else { FPU_C3 = 0; FPU_C2 = 1; FPU_C0 = 0; }
}
double ld_f80(uint32_t a) {
    uint64_t m = RD64(a); uint16_t se = RD16(a + 8);
    int e = se & 0x7fff;
    if (!e && !m) return (se & 0x8000) ? -0.0 : 0.0;
    double v = ldexp((double)m, e - 16383 - 63);
    return (se & 0x8000) ? -v : v;
}
void st_f80(uint32_t a, double v) {
    uint16_t se = signbit(v) ? 0x8000 : 0;
    if (v == 0) { WR64(a, 0); WR16(a + 8, se); return; }
    int e; double f = frexp(fabs(v), &e);
    uint64_t m = (uint64_t)ldexp(f, 64);
    WR64(a, m); WR16(a + 8, se | (uint16_t)(e - 1 + 16383));
}

// ---------------------------------------------------------------- startup
int main(int argc, char **argv) {
    rt_trace = getenv("P1_TRACE") ? atoi(getenv("P1_TRACE")) : 0;
    if (argc == 3 && !strcmp(argv[1], "--install")) {          // install from a disc image, no window
        paths_init();
        char err[300];
        printf("installing %s into %s ...\n", argv[2], path_data());
        if (install_from_disc(argv[2], NULL, NULL, err, sizeof err)) { fprintf(stderr, "install failed: %s\n", err); return 1; }
        printf("installed\n");
        return 0;
    }
    host_launcher();      // settings + launcher window (before the game is loaded)
    MEM = host_alloc_guest(MEM_SIZE);
    if (!MEM) rt_fatal("cannot reserve %u MB of guest memory", MEM_SIZE >> 20);
    const char *exe = path_exe();
    load_image(exe);
    english_boot();       // Language = English: data/en overlay, script tables, dialogue engine
    build_code_map();
    win32_init(argc, argv);
    fpu_init();
    if (rt_trace) fprintf(stderr, "entry %08x\n", GUEST_ENTRY);
    // the process entry point never returns (the CRT calls ExitProcess)
    rt_call(GUEST_ENTRY, MAGIC_EXIT);
    rt_fatal("guest entry point returned");
}

#ifdef P1_WATCHREAD
uint32_t watch_lo = 0xffffffffu, watch_len;
static struct { uint32_t fn, n, lo, hi; } watch_hits[256];
static int watch_n;
static void watch_dump(void) {
    for (int k = 0; k < watch_n; k++)
        fprintf(stderr, "watch: %08x read %u times, %08x-%08x\n", watch_hits[k].fn, watch_hits[k].n, watch_hits[k].lo, watch_hits[k].hi);
}
void watch_read(uint32_t a) {
    uint32_t fn = rt_fn_of[rt_tidx];
    for (int k = 0; k < watch_n; k++)
        if (watch_hits[k].fn == fn) {
            watch_hits[k].n++;
            if (a < watch_hits[k].lo) watch_hits[k].lo = a;
            if (a > watch_hits[k].hi) watch_hits[k].hi = a;
            return;
        }
    if (watch_n < 256) { watch_hits[watch_n].fn = fn; watch_hits[watch_n].n = 1; watch_hits[watch_n].lo = watch_hits[watch_n].hi = a; watch_n++; }
}
__attribute__((constructor)) static void watch_init(void) {
    const char *lo = getenv("P1_WATCH_LO"), *len = getenv("P1_WATCH_LEN");
    if (lo && len) { watch_lo = (uint32_t)strtoul(lo, 0, 16); watch_len = (uint32_t)strtoul(len, 0, 16); atexit(watch_dump); }
}
#endif
