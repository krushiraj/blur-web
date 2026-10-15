// Runtime for statically recompiled x86-32 code. Generated functions keep registers and flags in C locals (so the C compiler
// can drop dead flag computations) and spill them to the Cpu struct only at calls and returns.
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

#define OPTNONE __attribute__((optnone, noinline))
typedef struct Cpu {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t teb;                    // guest address of the thread's TEB (FS base)
    uint32_t df;                     // direction flag (0/1), preserved across calls
    int rn; uint32_t rs[65536];     // shadow stack of the return addresses the active C frames expect
    uint32_t lastret;                // return address the most recent `ret` (or API return) really popped
    double st[8]; int top;           // x87 register stack (double precision; 80-bit extended is not reproduced)
    uint32_t mxcsr;                  // SSE control/status (rounding mode is not modelled)
    uint16_t fcw, fsw;               // x87 control / status words
    uint8_t xmm[8][16];              // SSE registers
    uint32_t last_eflags;            // eflags image for pushfd/popfd round trips
} Cpu;

extern uint8_t *G;                   // guest memory base: guest address A lives at G + A
#define M8(a)   (*(uint8_t  *)(G + (uint32_t)(a)))
#define M16(a)  (*(uint16_t *)(G + (uint32_t)(a)))
#define M32(a)  (*(uint32_t *)(G + (uint32_t)(a)))
#define M64(a)  (*(uint64_t *)(G + (uint32_t)(a)))
#define MF32(a) (*(float    *)(G + (uint32_t)(a)))
#define MF64(a) (*(double   *)(G + (uint32_t)(a)))

typedef struct { uint8_t cf, zf, sf, of, pf, af; } Fl;

// ---- dispatch for calls the lifter could not resolve statically -------------------------------------------------------------------
typedef void (*GuestFn)(Cpu *c);
extern int rt_trace;                                     // debugging: log every function entry
void rt_enter(Cpu *c, uint32_t addr);
extern void rt_mismatch(Cpu *c, uint32_t a, int found);
static inline int rt_expected(Cpu *c, uint32_t a) { int f = 0; for (int i = c->rn - 1; i >= 0; i--) if (c->rs[i] == a) { f = 1; break; } if (__builtin_expect(rt_trace, 0)) rt_mismatch(c, a, f); return f; }
void rt_violation(Cpu *c, uint32_t callee, uint32_t be, uint32_t bb, uint32_t bs, uint32_t bd, uint32_t bp);
// direct call; in trace mode, check that the callee preserved the callee-saved registers and the stack balance
#define CALLF(fn, t) do { if (__builtin_expect(rt_trace, 0)) { uint32_t _b = c->ebx, _s = c->esi, _d = c->edi, _p = c->ebp, _e = c->esp; fn(c); \
    if (c->ebx != _b || c->esi != _s || c->edi != _d || c->ebp != _p || ((c->esp - _e) & 3)) rt_violation(c, (t), _e, _b, _s, _d, _p); } else fn(c); } while (0)
void rt_call(Cpu *c, uint32_t target);                  // indirect call (function table, then host API stubs)
void rt_unsupported(Cpu *c, uint32_t addr, const char *what) __attribute__((noreturn));

// ---- flag helpers (size in bits: 8/16/32) -----------------------------------------------------------------------------------------
static inline uint32_t fl_mask(int bits) { return bits == 32 ? 0xffffffffu : (1u << bits) - 1u; }
static inline uint8_t fl_par(uint32_t v) { v &= 0xff; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return (~v) & 1; }
static inline void fl_zsp(Fl *f, uint32_t res, int bits) { res &= fl_mask(bits); f->zf = res == 0; f->sf = (res >> (bits - 1)) & 1; f->pf = fl_par(res); }
static inline uint32_t fl_add(Fl *f, uint32_t a, uint32_t b, uint32_t cin, int bits) {
    uint32_t m = fl_mask(bits); a &= m; b &= m; uint64_t wide = (uint64_t)a + b + cin; uint32_t r = (uint32_t)wide & m;
    f->cf = (wide >> bits) & 1; f->of = (((a ^ r) & (b ^ r)) >> (bits - 1)) & 1; f->af = ((a ^ b ^ r) >> 4) & 1; fl_zsp(f, r, bits); return r;
}
static inline uint32_t fl_sub(Fl *f, uint32_t a, uint32_t b, uint32_t cin, int bits) {
    uint32_t m = fl_mask(bits); a &= m; b &= m; uint32_t r = (a - b - cin) & m;
    f->cf = (uint64_t)a < (uint64_t)b + cin; f->of = (((a ^ b) & (a ^ r)) >> (bits - 1)) & 1; f->af = ((a ^ b ^ r) >> 4) & 1; fl_zsp(f, r, bits); return r;
}
static inline uint32_t fl_logic(Fl *f, uint32_t r, int bits) { r &= fl_mask(bits); f->cf = 0; f->of = 0; f->af = 0; fl_zsp(f, r, bits); return r; }
static inline uint32_t fl_inc(Fl *f, uint32_t a, int bits) { uint8_t cf = f->cf; uint32_t r = fl_add(f, a, 1, 0, bits); f->cf = cf; return r; }
static inline uint32_t fl_dec(Fl *f, uint32_t a, int bits) { uint8_t cf = f->cf; uint32_t r = fl_sub(f, a, 1, 0, bits); f->cf = cf; return r; }
static inline uint32_t fl_neg(Fl *f, uint32_t a, int bits) { uint32_t r = fl_sub(f, 0, a, 0, bits); f->cf = (a & fl_mask(bits)) != 0; return r; }
static inline uint32_t fl_shl(Fl *f, uint32_t a, uint32_t n, int bits) {
    n &= 31; uint32_t m = fl_mask(bits); a &= m; if (!n) return a;
    uint64_t w = (uint64_t)a << n; uint32_t r = (uint32_t)w & m; f->cf = n <= (uint32_t)bits ? (uint8_t)((w >> bits) & 1) : 0; f->of = (uint8_t)(((r >> (bits - 1)) & 1) ^ f->cf); f->af = 0; fl_zsp(f, r, bits); return r;
}
static inline uint32_t fl_shr(Fl *f, uint32_t a, uint32_t n, int bits) {
    n &= 31; uint32_t m = fl_mask(bits); a &= m; if (!n) return a;
    uint32_t r = n >= 32 ? 0 : a >> n; f->cf = (uint8_t)((n - 1 < 32 ? (a >> (n - 1)) : 0) & 1); f->of = (uint8_t)((a >> (bits - 1)) & 1); f->af = 0; fl_zsp(f, r, bits); return r;
}
static inline uint32_t fl_sar(Fl *f, uint32_t a, uint32_t n, int bits) {
    n &= 31; uint32_t m = fl_mask(bits); a &= m; if (!n) return a;
    int32_t s = bits == 32 ? (int32_t)a : bits == 16 ? (int16_t)a : (int8_t)a; uint32_t r = (uint32_t)(s >> (n > 31 ? 31 : n)) & m;
    f->cf = (uint8_t)(((uint32_t)s >> (n - 1 > 31 ? 31 : n - 1)) & 1); f->of = 0; f->af = 0; fl_zsp(f, r, bits); return r;
}
static inline uint32_t fl_rol(Fl *f, uint32_t a, uint32_t n, int bits) {
    uint32_t m = fl_mask(bits); a &= m; uint32_t k = n & (bits == 32 ? 31 : 31); if (!k) return a; k %= bits; uint32_t r = k ? ((a << k) | (a >> (bits - k))) & m : a;
    f->cf = r & 1; f->of = (uint8_t)(((r >> (bits - 1)) & 1) ^ f->cf); return r;
}
static inline uint32_t fl_ror(Fl *f, uint32_t a, uint32_t n, int bits) {
    uint32_t m = fl_mask(bits); a &= m; uint32_t k = n & 31; if (!k) return a; k %= bits; uint32_t r = k ? ((a >> k) | (a << (bits - k))) & m : a;
    f->cf = (uint8_t)((r >> (bits - 1)) & 1); f->of = (uint8_t)(((r >> (bits - 1)) ^ (r >> (bits - 2))) & 1); return r;
}
static inline uint32_t fl_eflags(const Fl *f, uint32_t df) { return 2u | f->cf | (f->pf << 2) | (f->af << 4) | (f->zf << 6) | (f->sf << 7) | (df << 10) | (f->of << 11) | 0x200u; }
static inline void fl_load(Fl *f, uint32_t e, uint32_t *df) { f->cf = e & 1; f->pf = (e >> 2) & 1; f->af = (e >> 4) & 1; f->zf = (e >> 6) & 1; f->sf = (e >> 7) & 1; f->of = (e >> 11) & 1; *df = (e >> 10) & 1; }

static inline uint32_t fl_rcl(Fl *f, uint32_t a, uint32_t n, int bits) {
    n &= 31; if (bits < 32) n %= (bits + 1); if (!n) return a & fl_mask(bits); uint64_t v = (a & fl_mask(bits)) | ((uint64_t)f->cf << bits);
    uint64_t w = bits + 1; uint64_t r = ((v << n) | (v >> (w - n))) & ((1ull << w) - 1); f->cf = (r >> bits) & 1; uint32_t res = (uint32_t)r & fl_mask(bits); f->of = (((res >> (bits - 1)) & 1) ^ f->cf); return res;
}
static inline uint32_t fl_rcr(Fl *f, uint32_t a, uint32_t n, int bits) {
    n &= 31; if (bits < 32) n %= (bits + 1); if (!n) return a & fl_mask(bits); uint64_t v = (a & fl_mask(bits)) | ((uint64_t)f->cf << bits);
    uint64_t w = bits + 1; f->of = (((a >> (bits - 1)) & 1) ^ f->cf); uint64_t r = ((v >> n) | (v << (w - n))) & ((1ull << w) - 1); f->cf = (r >> bits) & 1; return (uint32_t)r & fl_mask(bits);
}
// ---- SSE helpers --------------------------------------------------------------------------------------------------------------------
#define XF(i, k) (((float *)c->xmm[i])[k])
#define XD(i, k) (((double *)c->xmm[i])[k])
#define XU(i, k) (((uint32_t *)c->xmm[i])[k])
#define XQ(i, k) (((uint64_t *)c->xmm[i])[k])
static inline void sse_comis(Fl *f, double a, double b) {          // comiss/ucomiss/comisd: ZF,PF,CF; OF=SF=AF=0
    f->of = f->sf = f->af = 0;
    if (a != a || b != b) { f->zf = f->pf = f->cf = 1; } else if (a > b) { f->zf = f->pf = f->cf = 0; } else if (a < b) { f->zf = f->pf = 0; f->cf = 1; } else { f->zf = 1; f->pf = f->cf = 0; }
}
static inline int32_t sse_cvtt(double v) { return (v != v || v >= 2147483648.0 || v < -2147483648.0) ? (int32_t)0x80000000 : (int32_t)v; }
static inline int32_t sse_cvt(double v) { double r = nearbyint(v); return (r != r || r >= 2147483648.0 || r < -2147483648.0) ? (int32_t)0x80000000 : (int32_t)r; }
static inline float sse_min(float a, float b) { return a < b ? a : b; }
static inline float sse_max(float a, float b) { return a > b ? a : b; }
static inline double sse_mind(double a, double b) { return a < b ? a : b; }
static inline double sse_maxd(double a, double b) { return a > b ? a : b; }

// ---- x87 helpers --------------------------------------------------------------------------------------------------------------------
#define ST(i) (c->st[(c->top + (i)) & 7])
static inline void fpush(Cpu *c, double v) { c->top = (c->top - 1) & 7; c->st[c->top] = v; }
static inline double fpop(Cpu *c) { double v = c->st[c->top]; c->top = (c->top + 1) & 7; return v; }
static inline double x87_rint(Cpu *c, double v) {                    // honours the rounding-control field of the control word
    switch ((c->fcw >> 10) & 3) { case 0: return nearbyint(v); case 1: return floor(v); case 2: return ceil(v); default: return trunc(v); }
}
static inline int64_t x87_toint(Cpu *c, double v, int bits) {
    double r = x87_rint(c, v); double lim = bits == 16 ? 32768.0 : bits == 32 ? 2147483648.0 : 9223372036854775808.0;
    if (r != r || r >= lim || r < -lim) return bits == 16 ? (int64_t)(int16_t)0x8000 : bits == 32 ? (int64_t)(int32_t)0x80000000 : (int64_t)0x8000000000000000ull;
    return (int64_t)r;
}
static inline void x87_com(Cpu *c, double a, double b) {             // sets C0 (bit 8), C2 (bit 10), C3 (bit 14)
    c->fsw &= ~0x4500; if (a != a || b != b) c->fsw |= 0x4500; else if (a < b) c->fsw |= 0x0100; else if (a == b) c->fsw |= 0x4000;
}
static inline uint16_t x87_sw(Cpu *c) { return (uint16_t)((c->fsw & ~0x3800) | ((c->top & 7) << 11)); }
static inline double ld80(uint32_t a) {                              // 80-bit extended in guest memory -> double
    uint64_t m = M64(a); uint16_t se = M16(a + 8); int e = se & 0x7fff; double sign = (se & 0x8000) ? -1.0 : 1.0;
    if (e == 0 && m == 0) return sign * 0.0; if (e == 0x7fff) return (m << 1) ? NAN : sign * INFINITY;
    return sign * ldexp((double)m, e - 16383 - 63);
}
static inline void st80(uint32_t a, double v) {                      // double -> 80-bit extended
    uint16_t sign = signbit(v) ? 0x8000 : 0; v = fabs(v); uint64_t m = 0; int e = 0;
    if (v == 0) { M64(a) = 0; M16(a + 8) = sign; return; }
    if (v != v) { M64(a) = 0xc000000000000000ull; M16(a + 8) = 0x7fff; return; }
    if (isinf(v)) { M64(a) = 0x8000000000000000ull; M16(a + 8) = sign | 0x7fff; return; }
    int ex; double fr = frexp(v, &ex); m = (uint64_t)ldexp(fr, 64); e = ex - 1 + 16383; M64(a) = m; M16(a + 8) = sign | (uint16_t)e;
}
static inline int64_t x87_toint_t(double v, int bits) {              // fisttp: truncating conversion
    double r = trunc(v); double lim = bits == 16 ? 32768.0 : bits == 32 ? 2147483648.0 : 9223372036854775808.0;
    if (r != r || r >= lim || r < -lim) return bits == 16 ? (int64_t)(int16_t)0x8000 : bits == 32 ? (int64_t)(int32_t)0x80000000 : (int64_t)0x8000000000000000ull;
    return (int64_t)r;
}
