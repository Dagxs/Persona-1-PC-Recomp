// Guest CPU model shared by the generated code (gen/*.c) and the runtime.
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

// ---------------------------------------------------------------- registers
extern uint32_t R[8];                 // eax ecx edx ebx esp ebp esi edi
#define EAX R[0]
#define ECX R[1]
#define EDX R[2]
#define EBX R[3]
#define ESP R[4]
#define EBP R[5]
#define ESI R[6]
#define EDI R[7]
extern uint32_t DF;                   // direction flag
extern uint32_t FS_BASE;              // guest address of the current thread's TEB

// ---------------------------------------------------------------- memory
// One flat guest address space; the PE image sits at 0x400000.
extern uint8_t *MEM;
#define MEM_SIZE (512u << 20)
#define MEM_MASK (MEM_SIZE - 1u)
#ifdef P1_WATCHREAD
// debug builds: report which guest function reads a watched address range (P1_WATCH_LO/LEN)
void watch_read(uint32_t a);
extern uint32_t watch_lo, watch_len;
#define WATCHED(a) do { if ((a) - watch_lo < watch_len) watch_read(a); } while (0)
#else
#define WATCHED(a) ((void)0)
#endif
static inline uint8_t RD8(uint32_t a) { WATCHED(a); return MEM[a & MEM_MASK]; }
static inline uint16_t RD16(uint32_t a) { WATCHED(a); uint16_t v; memcpy(&v, MEM + (a & MEM_MASK), 2); return v; }
static inline uint32_t RD32(uint32_t a) { WATCHED(a); uint32_t v; memcpy(&v, MEM + (a & MEM_MASK), 4); return v; }
static inline uint64_t RD64(uint32_t a) { uint64_t v; memcpy(&v, MEM + (a & MEM_MASK), 8); return v; }
static inline void WR8(uint32_t a, uint32_t v) { MEM[a & MEM_MASK] = (uint8_t)v; }
static inline void WR16(uint32_t a, uint32_t v) { uint16_t t = (uint16_t)v; memcpy(MEM + (a & MEM_MASK), &t, 2); }
static inline void WR32(uint32_t a, uint32_t v) { memcpy(MEM + (a & MEM_MASK), &v, 4); }
static inline void WR64(uint32_t a, uint64_t v) { memcpy(MEM + (a & MEM_MASK), &v, 8); }
static inline void PUSH32(uint32_t v) { ESP -= 4; WR32(ESP, v); }
static inline void PUSH16(uint32_t v) { ESP -= 2; WR16(ESP, v); }
static inline uint32_t POP32(void) { uint32_t v = RD32(ESP); ESP += 4; return v; }
static inline uint32_t POP16(void) { uint32_t v = RD16(ESP); ESP += 2; return v; }

// ---------------------------------------------------------------- lazy flags
// The last flag-setting operation is recorded; each condition is derived on demand.
enum { K_ADD, K_ADC, K_SUB, K_SBB, K_LOGIC, K_INC, K_DEC, K_NEG, K_SHIFT, K_MUL, K_RAW };
extern uint32_t fk, fsz, fa, fb, fr, fc;   // kind, size, operands, result, carry (in or out)
extern uint32_t fraw;                      // explicit EFLAGS for K_RAW
#define F_CF 0x001u
#define F_PF 0x004u
#define F_ZF 0x040u
#define F_SF 0x080u
#define F_OF 0x800u
static inline uint32_t szmask(int sz) { return sz == 32 ? 0xffffffffu : (1u << sz) - 1u; }
static inline uint32_t szsign(int sz) { return 1u << (sz - 1); }
static inline int32_t sext(uint32_t v, int sz) { return sz == 8 ? (int8_t)v : sz == 16 ? (int16_t)v : (int32_t)v; }
#define SETF(k, s, x, y, r) (fk = (k), fsz = (s), fa = (x), fb = (y), fr = (r))

static inline int cc_cf(void) {
    uint32_t m = szmask(fsz);
    switch (fk) {
    case K_ADD: return (fr & m) < (fa & m);
    case K_ADC: return fc ? (fr & m) <= (fa & m) : (fr & m) < (fa & m);
    case K_SUB: return (fa & m) < (fb & m);
    case K_SBB: return (uint64_t)(fa & m) < (uint64_t)(fb & m) + fc;
    case K_LOGIC: return 0;
    case K_NEG: return (fa & m) != 0;
    case K_RAW: return (fraw & F_CF) != 0;
    default: return fc != 0;     // INC/DEC keep CF in fc; shifts and mul store theirs
    }
}
static inline int cc_z(void) { return fk == K_RAW ? (fraw & F_ZF) != 0 : (fr & szmask(fsz)) == 0; }
static inline int cc_s(void) { return fk == K_RAW ? (fraw & F_SF) != 0 : (fr & szsign(fsz)) != 0; }
static inline int cc_p(void) { return fk == K_RAW ? (fraw & F_PF) != 0 : !__builtin_parity(fr & 0xffu); }
static inline int cc_o(void) {
    uint32_t s = szsign(fsz), m = szmask(fsz);
    switch (fk) {
    case K_ADD: case K_ADC: return (((fa ^ fr) & (fb ^ fr)) & s) != 0;
    case K_SUB: case K_SBB: return (((fa ^ fb) & (fa ^ fr)) & s) != 0;
    case K_INC: return (fr & m) == s;
    case K_DEC: return (fr & m) == s - 1;
    case K_NEG: return (fa & m) == s;
    case K_LOGIC: return 0;
    case K_RAW: return (fraw & F_OF) != 0;
    default: return fb != 0;     // shifts and mul store OF in fb
    }
}
static inline int cc_b(void) { if (fk == K_SUB) return (fa & szmask(fsz)) < (fb & szmask(fsz)); return cc_cf(); }
static inline int cc_b_raw(void) { return cc_cf(); }
static inline int cc_be(void) { if (fk == K_SUB) return (fa & szmask(fsz)) <= (fb & szmask(fsz)); return cc_cf() || cc_z(); }
static inline int cc_l(void) { if (fk == K_SUB) return sext(fa, fsz) < sext(fb, fsz); return cc_s() != cc_o(); }
static inline int cc_le(void) { if (fk == K_SUB) return sext(fa, fsz) <= sext(fb, fsz); return cc_z() || cc_s() != cc_o(); }

static inline uint32_t eflags_get(void) {
    return 0x202u | (cc_cf() ? F_CF : 0) | (cc_p() ? F_PF : 0) | (cc_z() ? F_ZF : 0) |
           (cc_s() ? F_SF : 0) | (cc_o() ? F_OF : 0) | (DF ? 0x400u : 0);
}
static inline void eflags_set(uint32_t f) { fk = K_RAW; fraw = f; DF = (f >> 10) & 1u; }
static inline void flags_cf(int c) { uint32_t f = eflags_get(); eflags_set(c ? (f | F_CF) : (f & ~F_CF)); }

// ---------------------------------------------------------------- ALU
static inline uint32_t op_add(uint32_t a, uint32_t b, int sz) { uint32_t r = (a + b) & szmask(sz); SETF(K_ADD, sz, a, b, r); return r; }
static inline uint32_t op_adc(uint32_t a, uint32_t b, int sz) { uint32_t c = cc_cf(); uint32_t r = (a + b + c) & szmask(sz); SETF(K_ADC, sz, a, b, r); fc = c; return r; }
static inline uint32_t op_sub(uint32_t a, uint32_t b, int sz) { uint32_t r = (a - b) & szmask(sz); SETF(K_SUB, sz, a, b, r); return r; }
static inline uint32_t op_sbb(uint32_t a, uint32_t b, int sz) { uint32_t c = cc_cf(); uint32_t r = (a - b - c) & szmask(sz); SETF(K_SBB, sz, a, b, r); fc = c; return r; }
static inline uint32_t op_and(uint32_t a, uint32_t b, int sz) { uint32_t r = a & b & szmask(sz); SETF(K_LOGIC, sz, a, b, r); return r; }
static inline uint32_t op_or(uint32_t a, uint32_t b, int sz) { uint32_t r = (a | b) & szmask(sz); SETF(K_LOGIC, sz, a, b, r); return r; }
static inline uint32_t op_xor(uint32_t a, uint32_t b, int sz) { uint32_t r = (a ^ b) & szmask(sz); SETF(K_LOGIC, sz, a, b, r); return r; }
static inline uint32_t op_inc(uint32_t a, int sz) { uint32_t c = cc_cf(); uint32_t r = (a + 1) & szmask(sz); SETF(K_INC, sz, a, 1, r); fc = c; return r; }
static inline uint32_t op_dec(uint32_t a, int sz) { uint32_t c = cc_cf(); uint32_t r = (a - 1) & szmask(sz); SETF(K_DEC, sz, a, 1, r); fc = c; return r; }
static inline uint32_t op_neg(uint32_t a, int sz) { uint32_t r = (0u - a) & szmask(sz); SETF(K_NEG, sz, a, 0, r); return r; }
static inline uint32_t op_not(uint32_t a, int sz) { return ~a & szmask(sz); }   // no flags

// Shifts: a count of 0 leaves every flag untouched.
static inline uint32_t op_shl(uint32_t a, uint32_t n, int sz) {
    if (!n) return a;
    uint32_t m = szmask(sz);
    uint32_t r = n >= 32 ? 0 : (a << n) & m;
    uint32_t c = n > (uint32_t)sz ? 0 : (a >> (sz - n)) & 1u;
    SETF(K_SHIFT, sz, a, ((r & szsign(sz)) != 0) ^ c, r); fc = c; return r;
}
static inline uint32_t op_shr(uint32_t a, uint32_t n, int sz) {
    if (!n) return a;
    a &= szmask(sz);
    uint32_t r = n >= 32 ? 0 : a >> n;
    uint32_t c = (a >> (n - 1)) & 1u;
    SETF(K_SHIFT, sz, a, (a & szsign(sz)) != 0, r); fc = c; return r;
}
static inline uint32_t op_sar(uint32_t a, uint32_t n, int sz) {
    if (!n) return a;
    int32_t s = sext(a, sz);
    uint32_t r = (uint32_t)(s >> (n > 31 ? 31 : n)) & szmask(sz);
    uint32_t c = (uint32_t)(s >> (n - 1 > 31 ? 31 : n - 1)) & 1u;
    SETF(K_SHIFT, sz, a, 0, r); fc = c; return r;
}
// Rotates change only CF and OF; ZF/SF/PF keep their previous values.
static inline void rot_flags(uint32_t cf, uint32_t of) {
    uint32_t f = eflags_get() & ~(F_CF | F_OF);
    eflags_set(f | (cf ? F_CF : 0) | (of ? F_OF : 0));
}
static inline uint32_t op_rol(uint32_t a, uint32_t n, int sz) {
    if (!n) return a;
    a &= szmask(sz); n %= (uint32_t)sz;
    uint32_t r = n ? ((a << n) | (a >> (sz - n))) & szmask(sz) : a;
    rot_flags(r & 1u, ((r >> (sz - 1)) ^ r) & 1u); return r;
}
static inline uint32_t op_ror(uint32_t a, uint32_t n, int sz) {
    if (!n) return a;
    a &= szmask(sz); n %= (uint32_t)sz;
    uint32_t r = n ? ((a >> n) | (a << (sz - n))) & szmask(sz) : a;
    rot_flags((r >> (sz - 1)) & 1u, ((r >> (sz - 1)) ^ (r >> (sz - 2))) & 1u); return r;
}
static inline uint32_t op_rcl(uint32_t a, uint32_t n, int sz) {
    uint32_t c = cc_cf(); n %= (uint32_t)(sz + 1);
    if (!n) return a;
    uint64_t v = ((uint64_t)c << sz) | (a & szmask(sz));
    for (uint32_t k = 0; k < n; k++) v = ((v << 1) | (v >> sz)) & ((1ull << (sz + 1)) - 1);
    uint32_t r = (uint32_t)v & szmask(sz); c = (uint32_t)(v >> sz) & 1u;
    rot_flags(c, ((r >> (sz - 1)) & 1u) ^ c); return r;
}
static inline uint32_t op_rcr(uint32_t a, uint32_t n, int sz) {
    uint32_t c = cc_cf(); n %= (uint32_t)(sz + 1);
    if (!n) return a;
    uint64_t v = ((uint64_t)c << sz) | (a & szmask(sz));
    for (uint32_t k = 0; k < n; k++) v = ((v >> 1) | ((v & 1u) << sz)) & ((1ull << (sz + 1)) - 1);
    uint32_t r = (uint32_t)v & szmask(sz); c = (uint32_t)(v >> sz) & 1u;
    rot_flags(c, ((r >> (sz - 1)) ^ (r >> (sz - 2))) & 1u); return r;
}
static inline uint32_t op_shld(uint32_t d, uint32_t s, uint32_t n, int sz) {
    if (!n) return d;
    uint64_t v = ((uint64_t)d << 32) | s;
    uint32_t r = (uint32_t)((v << n) >> 32);
    SETF(K_SHIFT, sz, d, ((r ^ d) >> 31) & 1u, r); fc = (d >> (32 - n)) & 1u; return r;
}
static inline uint32_t op_shrd(uint32_t d, uint32_t s, uint32_t n, int sz) {
    if (!n) return d;
    uint64_t v = ((uint64_t)s << 32) | d;
    uint32_t r = (uint32_t)(v >> n);
    SETF(K_SHIFT, sz, d, ((r ^ d) >> 31) & 1u, r); fc = (d >> (n - 1)) & 1u; return r;
}
static inline uint32_t op_imul(uint32_t a, uint32_t b, int sz) {
    int64_t p = (int64_t)sext(a, sz) * (int64_t)sext(b, sz);
    uint32_t r = (uint32_t)p & szmask(sz);
    uint32_t o = p != (int64_t)sext(r, sz);
    SETF(K_MUL, sz, a, o, r); fc = o; return r;
}
static inline void op_imul1(uint32_t b, int sz) {
    if (sz == 32) { int64_t p = (int64_t)(int32_t)EAX * (int32_t)b; EAX = (uint32_t)p; EDX = (uint32_t)(p >> 32);
        uint32_t o = p != (int64_t)(int32_t)EAX; SETF(K_MUL, 32, 0, o, EAX); fc = o; return; }
    if (sz == 16) { int32_t p = (int16_t)EAX * (int16_t)b; EAX = (EAX & 0xffff0000u) | (uint16_t)p; EDX = (EDX & 0xffff0000u) | (uint16_t)(p >> 16);
        uint32_t o = p != (int16_t)p; SETF(K_MUL, 16, 0, o, (uint32_t)p); fc = o; return; }
    { int32_t p = (int8_t)EAX * (int8_t)b; EAX = (EAX & 0xffff0000u) | (uint16_t)p;
        uint32_t o = p != (int8_t)p; SETF(K_MUL, 8, 0, o, (uint32_t)p); fc = o; }
}
static inline void op_mul1(uint32_t b, int sz) {
    if (sz == 32) { uint64_t p = (uint64_t)EAX * b; EAX = (uint32_t)p; EDX = (uint32_t)(p >> 32);
        uint32_t o = EDX != 0; SETF(K_MUL, 32, 0, o, EAX); fc = o; return; }
    if (sz == 16) { uint32_t p = (uint16_t)EAX * (uint16_t)b; EAX = (EAX & 0xffff0000u) | (uint16_t)p; EDX = (EDX & 0xffff0000u) | (p >> 16);
        uint32_t o = (p >> 16) != 0; SETF(K_MUL, 16, 0, o, p); fc = o; return; }
    { uint32_t p = (uint8_t)EAX * (uint8_t)b; EAX = (EAX & 0xffff0000u) | (uint16_t)p;
        uint32_t o = (p >> 8) != 0; SETF(K_MUL, 8, 0, o, p); fc = o; }
}
void rt_divide_error(void);
static inline void op_div1(uint32_t b, int sz) {
    if (sz == 32) { uint64_t n = ((uint64_t)EDX << 32) | EAX; if (!b || n / b > 0xffffffffu) rt_divide_error();
        EAX = (uint32_t)(n / b); EDX = (uint32_t)(n % b); return; }
    if (sz == 16) { uint32_t n = ((EDX & 0xffffu) << 16) | (EAX & 0xffffu); b &= 0xffffu; if (!b || n / b > 0xffffu) rt_divide_error();
        EAX = (EAX & 0xffff0000u) | (n / b); EDX = (EDX & 0xffff0000u) | (n % b); return; }
    { uint32_t n = EAX & 0xffffu; b &= 0xffu; if (!b || n / b > 0xffu) rt_divide_error();
        EAX = (EAX & 0xffff0000u) | ((n % b) << 8) | (n / b); }
}
static inline void op_idiv1(uint32_t b, int sz) {
    if (sz == 32) { int64_t n = (int64_t)(((uint64_t)EDX << 32) | EAX); int32_t d = (int32_t)b;
        if (!d) rt_divide_error(); int64_t q = n / d; if (q != (int32_t)q) rt_divide_error();
        EAX = (uint32_t)q; EDX = (uint32_t)(n % d); return; }
    if (sz == 16) { int32_t n = (int32_t)(((EDX & 0xffffu) << 16) | (EAX & 0xffffu)); int16_t d = (int16_t)b;
        if (!d) rt_divide_error(); int32_t q = n / d; if (q != (int16_t)q) rt_divide_error();
        EAX = (EAX & 0xffff0000u) | (uint16_t)q; EDX = (EDX & 0xffff0000u) | (uint16_t)(n % d); return; }
    { int16_t n = (int16_t)EAX; int8_t d = (int8_t)b; if (!d) rt_divide_error(); int16_t q = n / d; if (q != (int8_t)q) rt_divide_error();
        EAX = (EAX & 0xffff0000u) | ((uint32_t)(uint8_t)(n % d) << 8) | (uint8_t)q; }
}

// ---------------------------------------------------------------- string instructions
// mode: 0 = single, 1 = rep / repe, 2 = repne
void str_movs(int sz, int mode);
void str_stos(int sz, int mode);
void str_lods(int sz, int mode);
void str_scas(int sz, int mode);
void str_cmps(int sz, int mode);

// ---------------------------------------------------------------- x87
extern double FPR[8];
extern uint32_t FPU_TOP, FPU_C0, FPU_C1, FPU_C2, FPU_C3, FPU_CW;
#define ST(n) FPR[(FPU_TOP + (n)) & 7u]
static inline void fpu_push(double v) { FPU_TOP = (FPU_TOP - 1u) & 7u; FPR[FPU_TOP] = v; }
static inline void fpu_pop(void) { FPU_TOP = (FPU_TOP + 1u) & 7u; }
static inline uint32_t fpu_sw(void) {
    return (FPU_C0 << 8) | (FPU_C1 << 9) | (FPU_C2 << 10) | (FPU_TOP << 11) | (FPU_C3 << 14);
}
static inline void fpu_compare(double a, double b) {
    if (isnan(a) || isnan(b)) { FPU_C0 = FPU_C2 = FPU_C3 = 1; return; }
    FPU_C2 = 0; FPU_C0 = a < b; FPU_C3 = a == b;
}
double fpu_round(double v);           // honours the rounding control in FPU_CW
void fpu_xam(void);
void fpu_init(void);
static inline double ld_f32(uint32_t a) { float f; uint32_t v = RD32(a); memcpy(&f, &v, 4); return f; }
static inline double ld_f64(uint32_t a) { double d; uint64_t v = RD64(a); memcpy(&d, &v, 8); return d; }
double ld_f80(uint32_t a);
static inline double ld_i16(uint32_t a) { return (int16_t)RD16(a); }
static inline double ld_i32(uint32_t a) { return (int32_t)RD32(a); }
static inline double ld_i64(uint32_t a) { return (double)(int64_t)RD64(a); }
static inline void st_f32(uint32_t a, double v) { float f = (float)v; uint32_t t; memcpy(&t, &f, 4); WR32(a, t); }
static inline void st_f64(uint32_t a, double v) { uint64_t t; memcpy(&t, &v, 8); WR64(a, t); }
void st_f80(uint32_t a, double v);
static inline void st_i16(uint32_t a, double v) { double r = fpu_round(v); WR16(a, (r >= -32768.0 && r <= 32767.0) ? (uint32_t)(int32_t)r : 0x8000u); }
static inline void st_i32(uint32_t a, double v) { double r = fpu_round(v); WR32(a, (r >= -2147483648.0 && r <= 2147483647.0) ? (uint32_t)(int32_t)r : 0x80000000u); }
static inline void st_i64(uint32_t a, double v) { double r = fpu_round(v); WR64(a, (r >= -9.2233720368547758e18 && r < 9.2233720368547758e18) ? (uint64_t)(int64_t)r : 0x8000000000000000ull); }

// ---------------------------------------------------------------- control transfer
typedef void (*guest_fn_t)(void);
struct guest_fn { uint32_t addr; guest_fn_t fn; };
struct guest_import { uint32_t slot; const char *dll; const char *name; };
extern const struct guest_fn GUEST_FUNCS[];
extern const int GUEST_NFUNCS;
extern const uint32_t GUEST_ENTRY;
extern const struct guest_import GUEST_IMPORTS[];
extern const int GUEST_NIMPORTS;

// where each guest thread is (for the stall watchdog, P1_WATCHDOG)
extern volatile uint32_t rt_fn_of[64];
extern _Thread_local int rt_tidx;
#define FN_ENTER(a) (rt_fn_of[rt_tidx] = (a))

// English dialogue engine (runtime/vwf.c), called from the patch points in tools/recomp.py.
// While vwf_active is 0 the patched code runs the original Japanese paths.
enum { VWF_ADV, VWF_B, VWF_C };   // the three copies of the MsgStep interpreter
extern int vwf_active, vwf_two_byte;
extern int vwf_on[3];   // per interpreter: English engine in use (off: the original code runs)
void vwf_emit(int interp, int name_path);
// the dungeon field script's message box (runtime/vwf_field.c)
extern int vwf_field_on, vwf_field_two;
void vwf_field_put(void);
extern const uint32_t VWF_PS_GLYPH[4];
int vwf_button_action(uint32_t code, int two_byte);   // a PlayStation button glyph: 0-3, else -1
uint32_t dng_spill(uint32_t p);
int map_title(void);   // long dungeon map titles (vwf_field.c)
// the battle text system (runtime/vwf_btl.c)
extern int vwf_btl_on;
extern int vwf_menu_on;
void sf_record(void);
int sf_sort(void);
void sf_insert(int copy);
void name_save_compact(void);
int name_complete(void);
void sf_name_left(void);
void btl_name_copy(int lead);
uint32_t btl_name_slot(uint32_t slot);
uint32_t msg_text(uint32_t p);
extern int english_on;
void vwf_btl_glyph(void);
void vwf_btl_str16(void);
uint32_t btl_text(uint32_t p);   // a battle string -> its English (or itself)

// direct call to a recompiled function: push the guest return address, run it
#define CALL(f, ret) do { PUSH32(ret); f(); } while (0)
void rt_call(uint32_t target, uint32_t ret);   // indirect call (code or magic host function)
void rt_jump(uint32_t target);                 // indirect jump / tail call
_Noreturn void rt_fatal(const char *fmt, ...);
void rt_unimpl(uint32_t addr, const char *what);
extern volatile int rt_switch_pending;
void rt_yield(void);
// backward branches: let another guest thread run if one is waiting
// Like an OS scheduler, a thread keeps the CPU for a time slice before a waiting thread gets it;
// switching at every back-edge exposes races the game never hits on Windows.
void rt_preempt(void);
#define POLL() do { if (rt_switch_pending) rt_preempt(); } while (0)
#define POLLAT(a) do { rt_fn_of[rt_tidx] = (a); if (rt_switch_pending) rt_preempt(); } while (0)
