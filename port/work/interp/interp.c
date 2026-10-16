// x86-32 user-mode interpreter working directly on the recompiler's Cpu (see interp.h / README.md).
// Layout: (1) decoder -> cached Ins records, (2) integer / x87 / SSE executors, (3) the run loop.
// Flag, x87 and SSE helper semantics are the lifter's own (rt.h / rt_vec.c), so interpreted and recompiled code agree bit for bit.
#include "interp.h"
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>

_Static_assert(offsetof(Cpu, ecx) == 4 && offsetof(Cpu, edi) == 28 && offsetof(Cpu, teb) == 32, "Cpu register order");
#define R(i) (((uint32_t *)c)[i])

#ifndef INTERP_CACHE_BITS
#define INTERP_CACHE_BITS 16
#endif
#define CACHE_N (1u << INTERP_CACHE_BITS)
#ifdef INTERP_ENTRY_ACCOUNTING
#define ENTRY_RESET c->last_eflags = 0;
#define IND_MARK(t) ind_target = (t);
#define INTERP_CALL_BUDGET 0      // test mode: the thread budget is charged at function entries exactly like lifted code (host supplies interp_is_entry), not per call
#else
#define INTERP_CALL_BUDGET 1
#define ENTRY_RESET
#define IND_MARK(t)
#endif
static inline uint32_t cidx(uint32_t pc) { return (pc + (pc >> 16) * 40503u) & (CACHE_N - 1); }     // direct mapped; the page number is mixed in so code in different 64K pages (game / msvcr90) does not alias

// ---- decoded instruction ------------------------------------------------------------------------------------------------------------
enum { IF_MEM = 1, IF_FS = 2, IF_REP = 4, IF_REPNE = 8, IF_LOCK = 16, IF_O16 = 32, IF_CL = 64, IF_ENTRY = 128 };
typedef struct Ins {
    uint32_t pc;                      // cache tag
    uint16_t op;                      // raw opcode (0x00-0xff, 0x100 | 0F-map opcode) or one of the OP_* classes below
    uint8_t len, sz, fl, reg, rm, base, idx, sc, pfx, k;   // sz: operand bytes; reg/rm: modrm fields; base/idx 8 = none; pfx: 0 none, 1 66, 2 F3, 3 F2; k: sub-op
    int32_t disp;
    uint32_t imm, imm2;               // immediates; branch instructions keep the absolute target in imm
} Ins;
enum { OP_EXIT = 0x300, OP_BAD, OP_ALU_MR, OP_ALU_RM, OP_ALU_AI, OP_ALU_MI, OP_SHIFT, OP_INCDEC_R, OP_INCDEC_M, OP_GRP3, OP_CALL_M, OP_JMP_M, OP_PUSH_M, OP_JCC, OP_SETCC, OP_CMOV, OP_FPU };
// operand-shape flags per opcode (decoder tables)
enum { T_M = 1, T_IB = 2, T_IZ = 4, T_IW = 8, T_R8 = 16, T_RZ = 32, T_MO = 64 };
static uint8_t T1[256], T2[256];
static int tables_ready;
static void tables_init(void) {
    int b;
    for (b = 0; b < 0x40; b++) { int k = b & 7; T1[b] = k < 4 ? T_M : k == 4 ? T_IB : k == 5 ? T_IZ : 0; }
    T1[0x62] = T1[0x63] = T_M; T1[0x68] = T_IZ; T1[0x69] = T_M | T_IZ; T1[0x6a] = T_IB; T1[0x6b] = T_M | T_IB;
    for (b = 0x70; b < 0x80; b++) T1[b] = T_R8;
    T1[0x80] = T1[0x82] = T1[0x83] = T_M | T_IB; T1[0x81] = T_M | T_IZ;
    for (b = 0x84; b < 0x90; b++) T1[b] = T_M;
    T1[0x9a] = T_IZ | T_IW;
    for (b = 0xa0; b < 0xa4; b++) T1[b] = T_MO;
    T1[0xa8] = T_IB; T1[0xa9] = T_IZ;
    for (b = 0xb0; b < 0xb8; b++) T1[b] = T_IB;
    for (b = 0xb8; b < 0xc0; b++) T1[b] = T_IZ;
    T1[0xc0] = T1[0xc1] = T_M | T_IB; T1[0xc2] = T_IW; T1[0xc4] = T1[0xc5] = T_M; T1[0xc6] = T_M | T_IB; T1[0xc7] = T_M | T_IZ; T1[0xc8] = T_IW; T1[0xca] = T_IW; T1[0xcd] = T_IB;
    for (b = 0xd0; b < 0xe0; b++) T1[b] = T_M;
    T1[0xd4] = T1[0xd5] = T_IB;
    T1[0xd6] = T1[0xd7] = 0;
    for (b = 0xe0; b < 0xe4; b++) T1[b] = T_R8;
    for (b = 0xe4; b < 0xe8; b++) T1[b] = T_IB;
    T1[0xe8] = T1[0xe9] = T_RZ; T1[0xea] = T_IZ | T_IW; T1[0xeb] = T_R8;
    T1[0xf6] = T1[0xf7] = T1[0xfe] = T1[0xff] = T_M;
    for (b = 0; b < 256; b++) T2[b] = T_M;
    static const uint8_t nom[] = {0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0e, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x37, 0x77, 0xa0, 0xa1, 0xa2, 0xa8, 0xa9, 0xaa};
    for (unsigned i = 0; i < sizeof nom; i++) T2[nom[i]] = 0;
    for (b = 0x80; b < 0x90; b++) T2[b] = T_RZ;
    for (b = 0xc8; b < 0xd0; b++) T2[b] = 0;
    static const uint8_t ib2[] = {0x70, 0x71, 0x72, 0x73, 0xa4, 0xac, 0xba, 0xc2, 0xc4, 0xc5, 0xc6, 0x0f};
    for (unsigned i = 0; i < sizeof ib2; i++) T2[ib2[i]] |= T_IB;
    tables_ready = 1;
}
static int byte_op1(int b) {
    if (b < 0x40) return !(b & 1) && (b & 7) < 6;
    if (b >= 0xb0 && b < 0xb8) return 1;
    switch (b) { case 0x80: case 0x82: case 0x84: case 0x86: case 0x88: case 0x8a: case 0xa0: case 0xa2: case 0xa4: case 0xa6: case 0xa8: case 0xaa: case 0xac: case 0xae:
        case 0xc0: case 0xc6: case 0xd0: case 0xd2: case 0xf6: case 0xfe: return 1; }
    return 0;
}

static void do_fsave(Cpu *c, uint32_t a, int save);
static void vmap_init(void);
static InterpIsNative host_native;
uint32_t interp_stub_floor = 0x7f000000u;
int32_t interp_slice = 1 << 20;
uint64_t interp_icount;
InterpIntHook interp_int_hook;
#ifdef INTERP_ENTRY_ACCOUNTING
int (*interp_is_entry)(uint32_t pc);
int interp_charge_trace;
unsigned (*interp_entry_internal)(uint32_t fn, uint32_t pc);
#endif
void (*interp_call_hook)(uint32_t target, uint32_t ret_addr);
static Ins *cache;

static void decode(uint32_t pc, Ins *in) {
    memset(in, 0, sizeof *in); in->pc = pc; in->base = in->idx = 8;
    if (pc >= interp_stub_floor || (host_native && host_native(pc))) { in->op = OP_EXIT; in->len = 1; return; }
#ifdef INTERP_ENTRY_ACCOUNTING
    int is_entry = interp_is_entry && interp_is_entry(pc);
#endif       // never read code bytes of host-owned addresses
    const uint8_t *s = G + pc, *p = s;
    int o16 = 0, rep = 0, repne = 0, fs = 0, lock = 0, bad = 0;
    for (;;) {
        switch (*p) {
        case 0x66: o16 = 1; break;
        case 0x67: bad = 1; break;                       // 16-bit addressing is never used by 32-bit user code
        case 0xf2: repne = 1; rep = 0; break;
        case 0xf3: rep = 1; repne = 0; break;
        case 0xf0: lock = 1; break;
        case 0x64: fs = 1; break;
        case 0x65: bad = 1; break;                       // gs
        case 0x26: case 0x2e: case 0x36: case 0x3e: break;
        default: goto pfx_done;
        }
        if (++p - s > 14) { bad = 1; goto pfx_done; }
    }
pfx_done:;
    int b = *p++, map2 = 0, tab;
    if (b == 0x0f) { b = *p++; map2 = 1; tab = T2[b]; in->op = 0x100 | b; if (b == 0x38 || b == 0x3a) bad = 1; }
    else { tab = T1[b]; in->op = b; }
    int osz = o16 ? 2 : 4;
    in->sz = osz;
    if (!map2 && byte_op1(b)) in->sz = 1;
    in->fl = (o16 ? IF_O16 : 0) | (rep ? IF_REP : 0) | (repne ? IF_REPNE : 0) | (lock ? IF_LOCK : 0);
    in->pfx = rep ? 2 : repne ? 3 : o16 ? 1 : 0;
    if (tab & T_M) {
        int m = *p++, mod = m >> 6; in->reg = (m >> 3) & 7; in->rm = m & 7;
        if (mod != 3) {
            in->fl |= IF_MEM | (fs ? IF_FS : 0);
            if (in->rm == 4) {
                int sib = *p++; in->sc = sib >> 6; int ix = (sib >> 3) & 7; in->idx = ix == 4 ? 8 : ix; int bs = sib & 7;
                if (bs == 5 && mod == 0) { in->disp = (int32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); p += 4; } else in->base = bs;
            } else if (in->rm == 5 && mod == 0) { in->disp = (int32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); p += 4; }
            else in->base = in->rm;
            if (mod == 1) { in->disp = (int8_t)*p; p += 1; } else if (mod == 2) { in->disp = (int32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); p += 4; }
        }
    } else if (tab & T_MO) { in->fl |= IF_MEM | (fs ? IF_FS : 0); in->disp = (int32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); p += 4; }
    if (!map2 && (b == 0xf6 || b == 0xf7) && in->reg < 2) tab |= b == 0xf6 ? T_IB : T_IZ;
    if (tab & T_IB) { in->imm = (uint32_t)(int32_t)(int8_t)*p; p += 1; }
    if (tab & T_IZ) { if (o16) { in->imm = p[0] | p[1] << 8; p += 2; } else { in->imm = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; p += 4; } }
    if (tab & T_IW) { in->imm = p[0] | p[1] << 8; p += 2; if (b == 0xc8) { in->imm2 = *p++; } }
    if ((tab & T_IW) && (tab & T_IZ)) bad = 1;                                   // far pointers
    if (tab & T_R8) { int32_t d = (int8_t)*p; p += 1; in->imm = pc + (uint32_t)(p - s) + (uint32_t)d; }
    if (tab & T_RZ) { int32_t d = (int32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); p += 4; in->imm = pc + (uint32_t)(p - s) + (uint32_t)d; if (o16) bad = 1; }
    in->len = (uint8_t)(p - s);
    // classify
    int op = in->op;
    if (!map2) {
        if (b < 0x40 && (b & 7) < 6) { in->k = (b >> 3) & 7; in->op = (b & 7) < 2 ? OP_ALU_MR : (b & 7) < 4 ? OP_ALU_RM : OP_ALU_AI; if ((b & 7) == 5 || (b & 7) == 3 || (b & 7) == 1) in->sz = osz; }
        else if (b == 0x80 || b == 0x81 || b == 0x83 || b == 0x82) { in->op = OP_ALU_MI; in->k = in->reg; in->sz = (b & 1) ? osz : 1; if (b == 0x82) bad = 1; }
        else if (b >= 0x40 && b < 0x50) { in->op = OP_INCDEC_R; in->k = (b >> 3) & 1; in->rm = b & 7; }
        else if (b >= 0x70 && b < 0x80) { in->op = OP_JCC; in->k = b & 15; }
        else if (b == 0xc0 || b == 0xc1 || (b >= 0xd0 && b <= 0xd3)) {
            in->op = OP_SHIFT; in->k = in->reg; in->sz = (b & 1) ? osz : 1;
            if (b < 0xd0) { in->imm &= 0xff; } else if (b < 0xd2) in->imm = 1; else in->fl |= IF_CL;
        }
        else if (b == 0xf6 || b == 0xf7) { in->op = OP_GRP3; in->k = in->reg; }
        else if (b == 0xfe || b == 0xff) {
            switch (in->reg) {
            case 0: case 1: in->op = OP_INCDEC_M; in->k = in->reg; break;
            case 2: in->op = OP_CALL_M; in->sz = 4; break;
            case 4: in->op = OP_JMP_M; in->sz = 4; break;
            case 6: in->op = OP_PUSH_M; break;
            default: bad = 1;
            }
            if (b == 0xfe && in->reg > 1) bad = 1;
        }
        else if (b >= 0xd8 && b <= 0xdf) { in->op = OP_FPU; in->k = b - 0xd8; }
        else if (b == 0x90 && rep) in->k = 1;                                    // pause
        else if (b == 0xa4 || b == 0xa5 || b == 0xa6 || b == 0xa7 || (b >= 0xaa && b <= 0xaf)) {}
        else if (b == 0xc6 || b == 0xc7) { if (in->reg != 0) bad = 1; }
        else if (b == 0x8f && in->reg != 0) bad = 1;
        else if (b == 0xc2 || b == 0xc3) { if (o16) bad = 1; }
    } else {
        if (b >= 0x80 && b < 0x90) { in->op = OP_JCC; in->k = b & 15; }
        else if (b >= 0x90 && b < 0xa0) { in->op = OP_SETCC; in->k = b & 15; in->sz = 1; }
        else if (b >= 0x40 && b < 0x50) { in->op = OP_CMOV; in->k = b & 15; }
        else if (b == 0xb6 || b == 0xbe) in->k = 1;                              // source size 1
        else if (b == 0xb7 || b == 0xbf) in->k = 2;
        else if (b == 0xb0 || b == 0xc0) in->sz = 1;
        else if (b >= 0xc8 && b < 0xd0) in->rm = b & 7;
    }
    (void)op;
    if (bad) in->op = OP_BAD;
#ifdef INTERP_ENTRY_ACCOUNTING
    if (is_entry) in->fl |= IF_ENTRY;
#endif
}

void interp_init(InterpIsNative is_native) {
    host_native = is_native;
    if (!tables_ready) { tables_init(); vmap_init(); }
    if (!cache) cache = (Ins *)malloc(sizeof(Ins) * CACHE_N);
    interp_flush();
}
void interp_flush(void) { if (cache) for (uint32_t i = 0; i < CACHE_N; i++) cache[i].pc = 1; }   // pc 1 can never be a tag of a hit unless fetched (then op decodes normally)
void interp_flush_range(uint32_t lo, uint32_t hi) {
    if (!cache) return;
    if (hi - lo > CACHE_N) { interp_flush(); return; }
    for (uint32_t a = lo - 14; a != hi; a++) { Ins *s = &cache[cidx(a)]; if (s->pc == a) s->pc = 1; }
}

// ---- operand access -----------------------------------------------------------------------------------------------------------------
static inline uint32_t ea_raw(const Cpu *c, const Ins *in) {
    uint32_t a = (uint32_t)in->disp;
    if (in->base < 8) a += R(in->base);
    if (in->idx < 8) a += R(in->idx) << in->sc;
    return a;
}
static inline uint32_t ea(const Cpu *c, const Ins *in) { uint32_t a = ea_raw(c, in); if (in->fl & IF_FS) a += c->teb; return a; }
static inline uint32_t r8get(const Cpu *c, unsigned r) { return r < 4 ? (R(r) & 0xffu) : ((R(r - 4) >> 8) & 0xffu); }
static inline void r8set(Cpu *c, unsigned r, uint32_t v) { if (r < 4) R(r) = (R(r) & ~0xffu) | (v & 0xffu); else R(r - 4) = (R(r - 4) & ~0xff00u) | ((v & 0xffu) << 8); }
static inline uint32_t rget(const Cpu *c, unsigned r, int sz) { return sz == 4 ? R(r) : sz == 2 ? (R(r) & 0xffffu) : r8get(c, r); }
static inline void rset(Cpu *c, unsigned r, int sz, uint32_t v) { if (sz == 4) R(r) = v; else if (sz == 2) R(r) = (R(r) & 0xffff0000u) | (v & 0xffffu); else r8set(c, r, v); }
static inline uint32_t mget(uint32_t a, int sz) { return sz == 4 ? M32(a) : sz == 2 ? M16(a) : M8(a); }
static inline void mset(uint32_t a, int sz, uint32_t v) { if (sz == 4) M32(a) = v; else if (sz == 2) M16(a) = (uint16_t)v; else M8(a) = (uint8_t)v; }
#define EA0 ((in->fl & IF_MEM) ? ea(c, in) : 0u)
static inline uint32_t rm_rd(const Cpu *c, const Ins *in, uint32_t a, int sz) { return (in->fl & IF_MEM) ? mget(a, sz) : rget(c, in->rm, sz); }
static inline void rm_wr(Cpu *c, const Ins *in, uint32_t a, int sz, uint32_t v) { if (in->fl & IF_MEM) mset(a, sz, v); else rset(c, in->rm, sz, v); }
static inline uint32_t sx(uint32_t v, int sz) { return sz == 1 ? (uint32_t)(int32_t)(int8_t)v : sz == 2 ? (uint32_t)(int32_t)(int16_t)v : v; }
static inline uint32_t zx(uint32_t v, int sz) { return sz == 1 ? (v & 0xffu) : sz == 2 ? (v & 0xffffu) : v; }
static inline void push32(Cpu *c, uint32_t v) { c->esp -= 4; M32(c->esp) = v; }
static inline uint32_t pop32(Cpu *c) { uint32_t v = M32(c->esp); c->esp += 4; return v; }
static inline int cond(const Fl *f, int cc) {
    int r;
    switch (cc >> 1) { case 0: r = f->of; break; case 1: r = f->cf; break; case 2: r = f->zf; break; case 3: r = f->cf | f->zf; break;
                       case 4: r = f->sf; break; case 5: r = f->pf; break; case 6: r = f->sf != f->of; break; default: r = f->zf | (f->sf != f->of); }
    return r ^ (cc & 1);
}
static inline uint32_t alu(Cpu *c, int k, uint32_t a, uint32_t b, int bits) {
    Fl *f = &c->fl;
    switch (k) {
    case 0: return fl_add(f, a, b, 0, bits);
    case 1: return fl_logic(f, a | b, bits);
    case 2: return fl_add(f, a, b, f->cf, bits);
    case 3: return fl_sub(f, a, b, f->cf, bits);
    case 4: return fl_logic(f, a & b, bits);
    case 5: case 7: return fl_sub(f, a, b, 0, bits);
    default: return fl_logic(f, a ^ b, bits);
    }
}
static void __attribute__((noreturn)) fault(Cpu *c, const Ins *in, const char *what) { c->pc = in->pc; rt_unsupported(c, in->pc, what); }

// ---- integer groups -----------------------------------------------------------------------------------------------------------------
static void do_shift(Cpu *c, const Ins *in) {
    int sz = in->sz, bits = sz * 8; uint32_t a = EA0, v = rm_rd(c, in, a, sz), n = (in->fl & IF_CL) ? (c->ecx & 0xff) : in->imm, r; Fl *f = &c->fl;
    switch (in->k) {
    case 0: r = fl_rol(f, v, n, bits); break; case 1: r = fl_ror(f, v, n, bits); break; case 2: r = fl_rcl(f, v, n, bits); break; case 3: r = fl_rcr(f, v, n, bits); break;
    case 5: r = fl_shr(f, v, n, bits); break; case 7: r = fl_sar(f, v, n, bits); break; default: r = fl_shl(f, v, n, bits);
    }
    rm_wr(c, in, a, sz, r);
}
static void do_grp3(Cpu *c, const Ins *in) {
    int sz = in->sz, bits = sz * 8; uint32_t a = EA0, v = rm_rd(c, in, a, sz); Fl *f = &c->fl;
    switch (in->k) {
    case 0: case 1: fl_logic(f, v & in->imm, bits); break;
    case 2: rm_wr(c, in, a, sz, ~v); break;
    case 3: rm_wr(c, in, a, sz, fl_neg(f, v, bits)); break;
    case 4:
        if (sz == 4) { uint64_t p = (uint64_t)c->eax * v; c->eax = (uint32_t)p; c->edx = (uint32_t)(p >> 32); f->cf = f->of = c->edx != 0; }
        else if (sz == 2) { uint32_t p = (uint16_t)c->eax * v; c->eax = (c->eax & 0xffff0000u) | (p & 0xffff); c->edx = (c->edx & 0xffff0000u) | (p >> 16); f->cf = f->of = (p >> 16) != 0; }
        else { uint32_t p = (uint8_t)c->eax * v; c->eax = (c->eax & 0xffff0000u) | (p & 0xffff); f->cf = f->of = (p >> 8) != 0; }
        break;
    case 5:
        if (sz == 4) { int64_t p = (int64_t)(int32_t)c->eax * (int32_t)v; c->eax = (uint32_t)p; c->edx = (uint32_t)(p >> 32); f->cf = f->of = p != (int64_t)(int32_t)p; }
        else if (sz == 2) { int32_t p = (int32_t)(int16_t)c->eax * (int16_t)v; c->eax = (c->eax & 0xffff0000u) | (p & 0xffff); c->edx = (c->edx & 0xffff0000u) | (((uint32_t)p >> 16) & 0xffff); f->cf = f->of = p != (int16_t)p; }
        else { int32_t p = (int32_t)(int8_t)c->eax * (int8_t)v; c->eax = (c->eax & 0xffff0000u) | (p & 0xffff); f->cf = f->of = p != (int8_t)p; }
        break;
    case 6:
        if (!v) fault(c, in, "divide error");
        if (sz == 4) { uint64_t n = ((uint64_t)c->edx << 32) | c->eax, q = n / v; if (q >> 32) fault(c, in, "divide overflow"); c->eax = (uint32_t)q; c->edx = (uint32_t)(n % v); }
        else if (sz == 2) { uint32_t n = ((c->edx & 0xffff) << 16) | (c->eax & 0xffff), q = n / v; if (q >> 16) fault(c, in, "divide overflow"); c->eax = (c->eax & 0xffff0000u) | q; c->edx = (c->edx & 0xffff0000u) | (n % v); }
        else { uint32_t n = c->eax & 0xffff, q = n / v; if (q >> 8) fault(c, in, "divide overflow"); c->eax = (c->eax & 0xffff0000u) | ((n % v) << 8) | q; }
        break;
    default:
        if (!v) fault(c, in, "divide error");
        if (sz == 4) { int32_t d = (int32_t)v; int64_t n = (int64_t)(((uint64_t)c->edx << 32) | c->eax); if (d == -1 && n == INT64_MIN) fault(c, in, "divide overflow"); int64_t q = n / d; if (q != (int32_t)q) fault(c, in, "divide overflow"); c->eax = (uint32_t)(int32_t)q; c->edx = (uint32_t)(int32_t)(n % d); }
        else if (sz == 2) { int32_t d = (int16_t)v, n = (int32_t)(((c->edx & 0xffff) << 16) | (c->eax & 0xffff)); int32_t q = n / d; if (q != (int16_t)q) fault(c, in, "divide overflow"); c->eax = (c->eax & 0xffff0000u) | (uint16_t)q; c->edx = (c->edx & 0xffff0000u) | (uint16_t)(n % d); }
        else { int32_t d = (int8_t)v, n = (int16_t)c->eax; int32_t q = n / d; if (q != (int8_t)q) fault(c, in, "divide overflow"); c->eax = (c->eax & 0xffff0000u) | (((uint32_t)(n % d) & 0xff) << 8) | (uint8_t)q; }
    }
}
static void do_imul3(Cpu *c, const Ins *in, uint32_t a_, uint32_t b_) {           // reg <- a * b (2/3 operand forms)
    int sz = in->sz; Fl *f = &c->fl;
    if (sz == 2) { int32_t p = (int32_t)(int16_t)a_ * (int16_t)b_; rset(c, in->reg, 2, (uint32_t)p); f->cf = f->of = p != (int16_t)p; }
    else { int64_t p = (int64_t)(int32_t)a_ * (int32_t)b_; R(in->reg) = (uint32_t)p; f->cf = f->of = p != (int64_t)(int32_t)p; }
}
static void do_bt(Cpu *c, const Ins *in, int kind, int imm_form) {
    int sz = in->sz, bits = sz * 8; uint32_t off = imm_form ? (in->imm & 0xff) : sx(rget(c, in->reg, sz), sz), a = 0, v, bit;
    if (in->fl & IF_MEM) {
        a = ea(c, in);
        if (!imm_form) a += (uint32_t)(((int32_t)off >> (sz == 4 ? 5 : 4)) * sz);
        v = mget(a, sz);
    } else v = rget(c, in->rm, sz);
    bit = off & (bits - 1); c->fl.cf = (v >> bit) & 1;
    if (kind == 0) return;
    v = kind == 1 ? (v | (1u << bit)) : kind == 2 ? (v & ~(1u << bit)) : (v ^ (1u << bit));
    if (in->fl & IF_MEM) mset(a, sz, v); else rset(c, in->rm, sz, v);
}
static void do_shxd(Cpu *c, const Ins *in, int right, int use_cl) {
    int sz = in->sz, bits = sz * 8; uint32_t a = EA0, d = rm_rd(c, in, a, sz), s = rget(c, in->reg, sz), n = (use_cl ? c->ecx : in->imm) & 31, r; Fl *f = &c->fl;
    if (!n) return;
    if (sz == 4) {
        if (right) { r = (d >> n) | (s << (32 - n)); f->cf = (d >> (n - 1)) & 1; } else { r = (d << n) | (s >> (32 - n)); f->cf = (d >> (32 - n)) & 1; }
    } else {
        uint64_t t = right ? (((uint64_t)s << 16) | d) : (((uint64_t)d << 16) | s);
        if (n > 16) { t = right ? (((uint64_t)s << 32) | ((uint64_t)s << 16) | d) : (((uint64_t)d << 32) | ((uint64_t)d << 16) | s); }
        if (right) { r = (uint32_t)(t >> n) & 0xffff; f->cf = (t >> (n - 1)) & 1; } else { uint64_t u = t << n; r = (uint32_t)(u >> 16) & 0xffff; f->cf = (uint8_t)((t >> (32 - n)) & 1); if (n > 16) f->cf = (uint8_t)((d >> ((16 - n) & 15)) & 1); }
    }
    f->of = ((r ^ d) >> (bits - 1)) & 1; fl_zsp(f, r, bits); rm_wr(c, in, a, sz, r);
}
static void do_string(Cpu *c, const Ins *in) {
    int sz = in->sz, bits = sz * 8, step = c->df ? -sz : sz, opc = in->op & 0xfe; Fl *f = &c->fl;
    int rep = in->fl & (IF_REP | IF_REPNE);
    if (!rep) {
        switch (opc) {
        case 0xa4: mset(c->edi, sz, mget(c->esi, sz)); c->esi += step; c->edi += step; break;
        case 0xa6: fl_sub(f, mget(c->esi, sz), mget(c->edi, sz), 0, bits); c->esi += step; c->edi += step; break;
        case 0xaa: mset(c->edi, sz, c->eax); c->edi += step; break;
        case 0xac: rset(c, 0, sz, mget(c->esi, sz)); c->esi += step; break;
        default: fl_sub(f, zx(c->eax, sz), mget(c->edi, sz), 0, bits); c->edi += step; break;
        }
        return;
    }
    uint32_t n = c->ecx;
    if (!n) return;
    switch (opc) {
    case 0xa4: {
        if (!c->df && (c->edi + (uint64_t)n * sz <= c->esi || c->esi + (uint64_t)n * sz <= c->edi)) { memcpy(G + c->edi, G + c->esi, (size_t)n * sz); c->esi += n * sz; c->edi += n * sz; c->ecx = 0; return; }
        while (n) { mset(c->edi, sz, mget(c->esi, sz)); c->esi += step; c->edi += step; n--; }
        break; }
    case 0xaa: {
        uint32_t v = c->eax;
        if (sz == 1 && !c->df) { memset(G + c->edi, (int)(v & 0xff), n); c->edi += n; n = 0; break; }
        while (n) { mset(c->edi, sz, v); c->edi += step; n--; }
        break; }
    case 0xac: while (n) { rset(c, 0, sz, mget(c->esi, sz)); c->esi += step; n--; } break;
    case 0xa6: {
        int repe = (in->fl & IF_REPNE) == 0;
        while (n) { fl_sub(f, mget(c->esi, sz), mget(c->edi, sz), 0, bits); c->esi += step; c->edi += step; n--; if (repe ? !f->zf : f->zf) break; }
        break; }
    default: {
        int repe = (in->fl & IF_REPNE) == 0;
        while (n) { fl_sub(f, zx(c->eax, sz), mget(c->edi, sz), 0, bits); c->edi += step; n--; if (repe ? !f->zf : f->zf) break; }
        break; }
    }
    c->ecx = n;
}

// ---- x87 (double model, like the lifter) -----------------------------------------------------------------------------------------------
static inline double f_arith(int k, double a, double b) {
    double r;
    switch (k) { case 0: r = a + b; break; case 1: r = a * b; break; case 4: r = a - b; break; case 5: r = b - a; break; case 6: r = a / b; break; default: r = b / a; }
    if (__builtin_expect(r != r, 0) && a == a && b == b) { uint64_t u = 0xfff8000000000000ull; memcpy(&r, &u, 8); }          // x87 "real indefinite" (negative QNaN), like the SSE paths
    return r;
}
// fsin/fcos/fptan/fsincos with |x| >= 2^63 (or infinity): C2 is set and the stack is left alone (the lifted code computes the function anyway)
static inline int x87_oor(Cpu *c) { double v = ST(0); if (v == v && !(fabs(v) < 9223372036854775808.0)) { c->fsw |= 0x400; return 1; } return 0; }
static void do_fpu(Cpu *c, const Ins *in) {
    int fop = in->k, reg = in->reg, i = in->rm;
    if (in->fl & IF_MEM) {
        uint32_t a = ea(c, in);
        switch (fop) {
        case 0: case 4: case 2: case 6: {                                              // arithmetic with m32 float / m64 float / m32 int / m16 int
            double v = fop == 0 ? (double)MF32(a) : fop == 4 ? MF64(a) : fop == 2 ? (double)(int32_t)M32(a) : (double)(int16_t)M16(a);
            if (reg == 2 || reg == 3) { x87_com(c, ST(0), v); if (reg == 3) fpop(c); }
            else ST(0) = f_arith(reg, ST(0), v);
            return; }
        case 1:
            switch (reg) {
            case 0: { double v = (double)MF32(a); fpush(c, v); return; }
            case 2: MF32(a) = (float)ST(0); return;
            case 3: MF32(a) = (float)ST(0); fpop(c); return;
            case 4: c->fcw = M16(a); c->fsw = M16(a + 4) & ~0x3800; c->top = (M16(a + 4) >> 11) & 7; return;       // fldenv
            case 5: c->fcw = M16(a); return;
            case 6: M32(a) = c->fcw; M32(a + 4) = x87_sw(c); M32(a + 8) = 0xffff; M32(a + 12) = 0; M32(a + 16) = 0; M32(a + 20) = 0; M32(a + 24) = 0; return;   // fnstenv
            case 7: M16(a) = c->fcw; return;
            default: break;
            }
            break;
        case 3:
            switch (reg) {
            case 0: { double v = (double)(int32_t)M32(a); fpush(c, v); return; }
            case 1: M32(a) = (uint32_t)x87_toint_t(ST(0), 32); fpop(c); return;
            case 2: M32(a) = (uint32_t)x87_toint(c, ST(0), 32); return;
            case 3: M32(a) = (uint32_t)x87_toint(c, ST(0), 32); fpop(c); return;
            case 5: { double v = ld80(a); fpush(c, v); return; }
            case 7: st80(a, ST(0)); fpop(c); return;
            default: break;
            }
            break;
        case 5:
            switch (reg) {
            case 0: { double v = MF64(a); fpush(c, v); return; }
            case 1: { int64_t q = x87_toint_t(ST(0), 64); M64(a) = (uint64_t)q; fpop(c); return; }
            case 2: MF64(a) = ST(0); return;
            case 3: MF64(a) = ST(0); fpop(c); return;
            case 4: do_fsave(c, a, 0); return;
            case 6: do_fsave(c, a, 1); return;
            case 7: M16(a) = x87_sw(c); return;
            default: break;
            }
            break;
        case 7:
            switch (reg) {
            case 0: { double v = (double)(int16_t)M16(a); fpush(c, v); return; }
            case 1: M16(a) = (uint16_t)x87_toint_t(ST(0), 16); fpop(c); return;
            case 2: M16(a) = (uint16_t)x87_toint(c, ST(0), 16); return;
            case 3: M16(a) = (uint16_t)x87_toint(c, ST(0), 16); fpop(c); return;
            case 5: { double v = (double)(int64_t)M64(a); fpush(c, v); return; }
            case 7: M64(a) = (uint64_t)x87_toint(c, ST(0), 64); fpop(c); return;
            default: break;
            }
            break;
        }
        fault(c, in, "x87 memory form");
    }
    switch (fop) {
    case 0: if (reg == 2 || reg == 3) { x87_com(c, ST(0), ST(i)); if (reg == 3) fpop(c); } else ST(0) = f_arith(reg, ST(0), ST(i)); return;
    case 4: { if (reg == 2 || reg == 3) fault(c, in, "x87 DC D0"); int k = reg >= 4 ? reg ^ 1 : reg; ST(i) = f_arith(k, ST(i), ST(0)); return; }                 // sub/subr and div/divr swap their meaning
    case 6: if (reg == 3) { if (i != 1) fault(c, in, "x87 DE D9"); x87_com(c, ST(0), ST(1)); fpop(c); fpop(c); return; }
            if (reg == 2) fault(c, in, "x87 DE D0");
            { int k = reg >= 4 ? reg ^ 1 : reg; ST(i) = f_arith(k, ST(i), ST(0)); fpop(c); } return;
    case 1:
        switch (reg) {
        case 0: { double v = ST(i); fpush(c, v); return; }
        case 1: { double t = ST(0); ST(0) = ST(i); ST(i) = t; return; }
        case 2: return;
        case 4:
            switch (i) {
            case 0: ST(0) = -ST(0); return;
            case 1: ST(0) = fabs(ST(0)); return;
            case 4: x87_com(c, ST(0), 0.0); return;
            case 5: { double v = ST(0); c->fsw &= ~0x4700; if (signbit(v)) c->fsw |= 0x200; if (v != v) c->fsw |= 0x100; else if (isinf(v)) c->fsw |= 0x500; else if (v == 0) c->fsw |= 0x4000; else c->fsw |= 0x400; return; }
            }
            break;
        case 5:
            switch (i) {
            case 0: fpush(c, 1.0); return; case 1: fpush(c, 3.3219280948873623479); return; case 2: fpush(c, 1.4426950408889634074); return;
            case 3: fpush(c, 3.14159265358979323846); return; case 4: fpush(c, 0.30102999566398119521); return; case 5: fpush(c, 0.69314718055994530942); return;
            case 6: fpush(c, 0.0); return;
            }
            break;
        case 6:
            switch (i) {
            case 0: ST(0) = exp2(ST(0)) - 1.0; return;
            case 1: ST(1) = ST(1) * log2(ST(0)); fpop(c); return;
            case 2: if (x87_oor(c)) return; ST(0) = tan(ST(0)); fpush(c, 1.0); c->fsw &= ~0x400; return;
            case 3: ST(1) = atan2(ST(1), ST(0)); fpop(c); return;
            case 4: { double v = ST(0); int e = ilogb(v); if (v == 0) { ST(0) = -INFINITY; fpush(c, v); } else if (isinf(v)) { ST(0) = INFINITY; fpush(c, v); } else if (v != v) { fpush(c, v); } else { ST(0) = (double)e; fpush(c, scalbn(v, -e)); } return; }
            case 5: ST(0) = remainder(ST(0), ST(1)); c->fsw &= ~0x400; return;
            case 6: c->top = (c->top - 1) & 7; return;
            case 7: c->top = (c->top + 1) & 7; return;
            }
            break;
        case 7:
            switch (i) {
            case 0: ST(0) = fmod(ST(0), ST(1)); c->fsw &= ~0x400; return;
            case 1: ST(1) = ST(1) * log2(ST(0) + 1.0); fpop(c); return;
            case 2: { double v = ST(0), r = sqrt(v); if (r != r && v == v) { uint64_t u = 0xfff8000000000000ull; memcpy(&r, &u, 8); } ST(0) = r; return; }
            case 3: { if (x87_oor(c)) return; double v = ST(0); ST(0) = sin(v); fpush(c, cos(v)); c->fsw &= ~0x400; return; }
            case 4: ST(0) = x87_rint(c, ST(0)); return;
            case 5: ST(0) = ldexp(ST(0), (int)trunc(ST(1))); return;
            case 6: if (x87_oor(c)) return; ST(0) = sin(ST(0)); c->fsw &= ~0x400; return;
            case 7: if (x87_oor(c)) return; ST(0) = cos(ST(0)); c->fsw &= ~0x400; return;
            }
            break;
        }
        break;
    case 2:
        switch (reg) {
        case 0: case 1: case 2: case 3: {
            static const uint8_t cc[4] = {2 /*b*/, 4 /*e*/, 6 /*be*/, 0xff /*u*/};
            int t = cc[reg], take = t == 0xff ? c->fl.pf : cond(&c->fl, t);
            if (take) ST(0) = ST(i); return; }
        case 5: if (i == 1) { x87_com(c, ST(0), ST(1)); fpop(c); fpop(c); return; } break;
        }
        break;
    case 3:
        switch (reg) {
        case 0: case 1: case 2: case 3: {
            static const uint8_t cc[4] = {3 /*nb*/, 5 /*ne*/, 7 /*nbe*/, 0xff};
            int t = cc[reg], take = t == 0xff ? !c->fl.pf : cond(&c->fl, t);
            if (take) ST(0) = ST(i); return; }
        case 4: if (i == 2) { c->fsw &= 0x7f00; return; } if (i == 3) { c->top = 0; c->fcw = 0x37f; c->fsw = 0; return; } if (i == 0 || i == 1 || i == 4) return; break;
        case 5: sse_comis(&c->fl, ST(0), ST(i)); return;
        case 6: sse_comis(&c->fl, ST(0), ST(i)); return;
        }
        break;
    case 5:
        switch (reg) {
        case 0: return;                                                            // ffree
        case 2: ST(i) = ST(0); return;
        case 3: ST(i) = ST(0); fpop(c); return;
        case 4: x87_com(c, ST(0), ST(i)); return;
        case 5: x87_com(c, ST(0), ST(i)); fpop(c); return;
        }
        break;
    case 7:
        switch (reg) {
        case 0: fpop(c); return;                                                   // ffreep
        case 4: if (i == 0) { c->eax = (c->eax & 0xffff0000u) | x87_sw(c); return; } break;
        case 5: sse_comis(&c->fl, ST(0), ST(i)); fpop(c); return;
        case 6: sse_comis(&c->fl, ST(0), ST(i)); fpop(c); return;
        }
        break;
    }
    fault(c, in, "x87 form");
}

// ---- SSE / MMX ---------------------------------------------------------------------------------------------------------------------------
static int8_t VMAP[256], VSHMAP[256];
static void vmap_init(void) {
    memset(VMAP, -1, sizeof VMAP); memset(VSHMAP, -1, sizeof VSHMAP);
    static const struct { uint8_t o; int8_t v; } B[] = {
        {0x60, V_PUNPCKLBW}, {0x61, V_PUNPCKLWD}, {0x62, V_PUNPCKLDQ}, {0x63, V_PACKSSWB}, {0x64, V_PCMPGTB}, {0x65, V_PCMPGTW}, {0x66, V_PCMPGTD}, {0x67, V_PACKUSWB},
        {0x68, V_PUNPCKHBW}, {0x69, V_PUNPCKHWD}, {0x6a, V_PUNPCKHDQ}, {0x6b, V_PACKSSDW}, {0x74, V_PCMPEQB}, {0x75, V_PCMPEQW}, {0x76, V_PCMPEQD},
        {0xd4, V_PADDQ}, {0xd5, V_PMULLW}, {0xd8, V_PSUBUSB}, {0xd9, V_PSUBUSW}, {0xda, V_PMINUB}, {0xdb, V_PAND}, {0xdc, V_PADDUSB}, {0xdd, V_PADDUSW}, {0xde, V_PMAXUB}, {0xdf, V_PANDN},
        {0xe0, V_PAVGB}, {0xe3, V_PAVGW}, {0xe4, V_PMULHUW}, {0xe5, V_PMULHW}, {0xe8, V_PSUBSB}, {0xe9, V_PSUBSW}, {0xea, V_PMINSW}, {0xeb, V_POR}, {0xec, V_PADDSB}, {0xed, V_PADDSW},
        {0xee, V_PMAXSW}, {0xef, V_PXOR}, {0xf5, V_PMADDWD}, {0xf6, V_PSADBW}, {0xf8, V_PSUBB}, {0xf9, V_PSUBW}, {0xfa, V_PSUBD}, {0xfb, V_PSUBQ}, {0xfc, V_PADDB}, {0xfd, V_PADDW}, {0xfe, V_PADDD}};
    for (unsigned i = 0; i < sizeof B / sizeof B[0]; i++) VMAP[B[i].o] = B[i].v;
    static const struct { uint8_t o; int8_t v; } S[] = {{0xd1, V_PSRLW}, {0xd2, V_PSRLD}, {0xd3, V_PSRLQ}, {0xe1, V_PSRAW}, {0xe2, V_PSRAD}, {0xf1, V_PSLLW}, {0xf2, V_PSLLD}, {0xf3, V_PSLLQ}};
    for (unsigned i = 0; i < sizeof S / sizeof S[0]; i++) VSHMAP[S[i].o] = S[i].v;
}
static inline uint8_t *vreg(Cpu *c, int x, int r) { return x ? c->xmm[r] : (uint8_t *)&c->mm[r]; }
static inline const uint8_t *vsrc(Cpu *c, const Ins *in, int x) { return (in->fl & IF_MEM) ? G + ea(c, in) : vreg(c, x, in->rm); }
#define XS(k) (((float *)s)[k])
#define XSD(k) (((double *)s)[k])
#define XSU(k) (((uint32_t *)s)[k])
#define XSQ(k) (((uint64_t *)s)[k])
static inline int pred_f(int p, float a, float b) { switch (p) { case 0: return a == b; case 1: return a < b; case 2: return a <= b; case 3: return a != a || b != b; case 4: return !(a == b); case 5: return !(a < b); case 6: return !(a <= b); default: return a == a && b == b; } }
static inline int pred_d(int p, double a, double b) { switch (p) { case 0: return a == b; case 1: return a < b; case 2: return a <= b; case 3: return a != a || b != b; case 4: return !(a == b); case 5: return !(a < b); case 6: return !(a <= b); default: return a == a && b == b; } }

// x86 returns the negative "default QNaN" for an invalid operation (inf-inf, 0*inf, 0/0, sqrt(-1)); arm64 / wasm hosts produce the positive one.  Fix it up so the interpreter is
// deterministic and bit-exact with hardware (a NaN operand is propagated by the host exactly like x86: first operand, quieted).
static inline float nanf_fix(float r, int has_nan_in) { if (__builtin_expect(r != r, 0) && !has_nan_in) { uint32_t u = 0xffc00000u; memcpy(&r, &u, 4); } return r; }
static inline double nand_fix(double r, int has_nan_in) { if (__builtin_expect(r != r, 0) && !has_nan_in) { uint64_t u = 0xfff8000000000000ull; memcpy(&r, &u, 8); } return r; }
static inline float farith(int b, float a, float v) {
    float r = b == 0x58 ? a + v : b == 0x59 ? a * v : b == 0x5c ? a - v : b == 0x5e ? a / v : b == 0x5d ? sse_min(a, v) : sse_max(a, v);
    return b >= 0x5d && b != 0x5e ? r : nanf_fix(r, a != a || v != v);
}
static inline double darith(int b, double a, double v) {
    double r = b == 0x58 ? a + v : b == 0x59 ? a * v : b == 0x5c ? a - v : b == 0x5e ? a / v : b == 0x5d ? sse_mind(a, v) : sse_maxd(a, v);
    return b >= 0x5d && b != 0x5e ? r : nand_fix(r, a != a || v != v);
}
static void do_sse(Cpu *c, const Ins *in) {
    int b = in->op & 0xff, p = in->pfx, d = in->reg, x = p == 1;          // x: 66 prefix selects xmm for the integer instructions
    uint8_t *D = c->xmm[d]; const uint8_t *s;
    uint32_t a;
    switch (b) {
    case 0x10: s = vsrc(c, in, 1);
        if (p < 2) { uint8_t t[16]; memcpy(t, s, 16); memcpy(D, t, 16); }
        else if (p == 2) { if (in->fl & IF_MEM) { uint32_t v = XSU(0); XQ(d, 0) = 0; XQ(d, 1) = 0; XU(d, 0) = v; } else XU(d, 0) = XSU(0); }
        else { if (in->fl & IF_MEM) { uint64_t v = XSQ(0); XQ(d, 0) = v; XQ(d, 1) = 0; } else XQ(d, 0) = XSQ(0); }
        return;
    case 0x11: {
        uint8_t *dst = (in->fl & IF_MEM) ? G + ea(c, in) : c->xmm[in->rm];
        if (p < 2) { uint8_t t[16]; memcpy(t, D, 16); memcpy(dst, t, 16); } else if (p == 2) memcpy(dst, D, 4); else memcpy(dst, D, 8);
        return; }
    case 0x12: s = vsrc(c, in, 1);
        if (p == 2) { uint32_t t[4] = {XSU(0), XSU(0), XSU(2), XSU(2)}; memcpy(D, t, 16); }
        else if (p == 3) { uint64_t v = XSQ(0); XQ(d, 0) = v; XQ(d, 1) = v; }
        else XQ(d, 0) = (in->fl & IF_MEM) ? XSQ(0) : XSQ(1);
        return;
    case 0x13: memcpy(G + ea(c, in), D, 8); return;
    case 0x14: s = vsrc(c, in, 1);
        if (p == 1) { XQ(d, 1) = XSQ(0); } else { uint32_t a0 = XU(d, 0), a1 = XU(d, 1), b0 = XSU(0), b1 = XSU(1); XU(d, 0) = a0; XU(d, 1) = b0; XU(d, 2) = a1; XU(d, 3) = b1; }
        return;
    case 0x15: s = vsrc(c, in, 1);
        if (p == 1) { uint64_t a1 = XQ(d, 1), b1 = XSQ(1); XQ(d, 0) = a1; XQ(d, 1) = b1; } else { uint32_t a2 = XU(d, 2), a3 = XU(d, 3), b2 = XSU(2), b3 = XSU(3); XU(d, 0) = a2; XU(d, 1) = b2; XU(d, 2) = a3; XU(d, 3) = b3; }
        return;
    case 0x16: s = vsrc(c, in, 1);
        if (p == 2) { uint32_t t[4] = {XSU(1), XSU(1), XSU(3), XSU(3)}; memcpy(D, t, 16); }
        else XQ(d, 1) = (in->fl & IF_MEM) ? XSQ(0) : XSQ(0);
        return;
    case 0x17: memcpy(G + ea(c, in), D + 8, 8); return;
    case 0x28: s = vsrc(c, in, 1); { uint8_t t[16]; memcpy(t, s, 16); memcpy(D, t, 16); } return;
    case 0x29: case 0x2b: {
        uint8_t *dst = (in->fl & IF_MEM) ? G + ea(c, in) : c->xmm[in->rm]; uint8_t t[16]; memcpy(t, D, 16); memcpy(dst, t, 16); return; }
    case 0x2a:
        if (p == 2) { XF(d, 0) = (float)(int32_t)rm_rd(c, in, EA0, 4); return; }
        if (p == 3) { XD(d, 0) = (double)(int32_t)rm_rd(c, in, EA0, 4); return; }
        s = vsrc(c, in, 0);
        { int32_t i0 = (int32_t)(uint32_t)XSQ(0), i1 = (int32_t)(XSQ(0) >> 32);
          if (p == 0) { XF(d, 0) = (float)i0; XF(d, 1) = (float)i1; } else { XD(d, 0) = i0; XD(d, 1) = i1; } }
        return;
    case 0x2c: case 0x2d: {
        int32_t (*cv)(double) = b == 0x2c ? sse_cvtt : sse_cvt;
        if (p >= 2) {
            s = vsrc(c, in, 1); double v; if (p == 2) { float f; memcpy(&f, s, 4); v = f; } else memcpy(&v, s, 8);
            R(in->reg) = (uint32_t)cv(v); return; }
        s = vsrc(c, in, 1);
        { int32_t r0, r1; if (p == 0) { float f0, f1; memcpy(&f0, s, 4); memcpy(&f1, s + 4, 4); r0 = cv(f0); r1 = cv(f1); } else { double d0, d1; memcpy(&d0, s, 8); memcpy(&d1, s + 8, 8); r0 = cv(d0); r1 = cv(d1); }
          c->mm[in->reg] = (uint32_t)r0 | ((uint64_t)(uint32_t)r1 << 32); }
        return; }
    case 0x2e: case 0x2f:
        s = vsrc(c, in, 1);
        if (p == 0) { float v; memcpy(&v, s, 4); sse_comis(&c->fl, XF(d, 0), v); } else { double v; memcpy(&v, s, 8); sse_comis(&c->fl, XD(d, 0), v); }
        return;
    case 0x50: { const uint8_t *r = c->xmm[in->rm]; uint32_t m;
        if (p == 0) m = (((uint32_t *)r)[0] >> 31) | ((((uint32_t *)r)[1] >> 31) << 1) | ((((uint32_t *)r)[2] >> 31) << 2) | ((((uint32_t *)r)[3] >> 31) << 3);
        else m = (uint32_t)((((uint64_t *)r)[0] >> 63) | ((((uint64_t *)r)[1] >> 63) << 1));
        R(in->reg) = m; return; }
    case 0x51: case 0x52: case 0x53:
        s = vsrc(c, in, 1);
        if (p == 0) { float v[4]; memcpy(v, s, 16); for (int k = 0; k < 4; k++) XF(d, k) = nanf_fix(b == 0x51 ? sqrtf(v[k]) : b == 0x52 ? 1.0f / sqrtf(v[k]) : 1.0f / v[k], v[k] != v[k]); }
        else if (p == 1) { double v[2]; memcpy(v, s, 16); XD(d, 0) = nand_fix(sqrt(v[0]), v[0] != v[0]); XD(d, 1) = nand_fix(sqrt(v[1]), v[1] != v[1]); }
        else if (p == 2) { float v; memcpy(&v, s, 4); XF(d, 0) = nanf_fix(b == 0x51 ? sqrtf(v) : b == 0x52 ? 1.0f / sqrtf(v) : 1.0f / v, v != v); }
        else { double v; memcpy(&v, s, 8); XD(d, 0) = nand_fix(sqrt(v), v != v); }
        return;
    case 0x54: case 0x55: case 0x56: case 0x57: {
        s = vsrc(c, in, 1); uint64_t q0 = XSQ(0), q1 = XSQ(1);
        if (b == 0x54) { XQ(d, 0) &= q0; XQ(d, 1) &= q1; } else if (b == 0x55) { XQ(d, 0) = ~XQ(d, 0) & q0; XQ(d, 1) = ~XQ(d, 1) & q1; }
        else if (b == 0x56) { XQ(d, 0) |= q0; XQ(d, 1) |= q1; } else { XQ(d, 0) ^= q0; XQ(d, 1) ^= q1; }
        return; }
    case 0x58: case 0x59: case 0x5c: case 0x5d: case 0x5e: case 0x5f: {
        s = vsrc(c, in, 1);
        if (p == 0) { float v[4]; memcpy(v, s, 16); for (int k = 0; k < 4; k++) { float a_ = XF(d, k);
            XF(d, k) = farith(b, a_, v[k]); } }
        else if (p == 1) { double v[2]; memcpy(v, s, 16); for (int k = 0; k < 2; k++) { double a_ = XD(d, k);
            XD(d, k) = darith(b, a_, v[k]); } }
        else if (p == 2) { float v; memcpy(&v, s, 4); float a_ = XF(d, 0);
            XF(d, 0) = farith(b, a_, v); }
        else { double v; memcpy(&v, s, 8); double a_ = XD(d, 0);
            XD(d, 0) = darith(b, a_, v); }
        return; }
    case 0x5a:
        s = vsrc(c, in, 1);
        if (p == 0) { float a0, a1; memcpy(&a0, s, 4); memcpy(&a1, s + 4, 4); XD(d, 0) = a0; XD(d, 1) = a1; }
        else if (p == 1) { double a0, a1; memcpy(&a0, s, 8); memcpy(&a1, s + 8, 8); XF(d, 0) = (float)a0; XF(d, 1) = (float)a1; XU(d, 2) = 0; XU(d, 3) = 0; }
        else if (p == 2) { float a0; memcpy(&a0, s, 4); XD(d, 0) = a0; }
        else { double a0; memcpy(&a0, s, 8); XF(d, 0) = (float)a0; }
        return;
    case 0x5b:
        s = vsrc(c, in, 1);
        if (p == 0) { int32_t v[4]; memcpy(v, s, 16); for (int k = 0; k < 4; k++) XF(d, k) = (float)v[k]; }
        else { float v[4]; memcpy(v, s, 16); for (int k = 0; k < 4; k++) XU(d, k) = (uint32_t)(p == 1 ? sse_cvt(v[k]) : sse_cvtt(v[k])); }
        return;
    case 0x6c: s = vsrc(c, in, 1); XQ(d, 1) = XSQ(0); return;
    case 0x6d: s = vsrc(c, in, 1); { uint64_t a1 = XQ(d, 1), b1 = XSQ(1); XQ(d, 0) = a1; XQ(d, 1) = b1; } return;
    case 0x6e: { uint32_t v = rm_rd(c, in, EA0, 4); if (x) { XU(d, 0) = v; XU(d, 1) = 0; XQ(d, 1) = 0; } else c->mm[d] = v; return; }
    case 0x6f: s = vsrc(c, in, x || p == 2);
        if (p == 0) c->mm[d] = XSQ(0); else { uint8_t t[16]; memcpy(t, s, 16); memcpy(D, t, 16); }
        return;
    case 0x70: {
        s = vsrc(c, in, p != 0); int imm = in->imm & 0xff;
        if (p == 0) { uint16_t t[4]; memcpy(t, s, 8); uint16_t *o = (uint16_t *)&c->mm[d]; for (int k = 0; k < 4; k++) o[k] = t[(imm >> (2 * k)) & 3]; }
        else if (p == 1) { uint32_t t[4]; memcpy(t, s, 16); for (int k = 0; k < 4; k++) XU(d, k) = t[(imm >> (2 * k)) & 3]; }
        else { uint16_t t[8]; memcpy(t, s, 16); uint16_t *o = (uint16_t *)D; int off = p == 2 ? 4 : 0; if (p == 2) { memcpy(o, t, 8); } else memcpy(o + 4, t + 4, 8);
               for (int k = 0; k < 4; k++) o[off + k] = t[off + ((imm >> (2 * k)) & 3)]; }
        return; }
    case 0x71: case 0x72: case 0x73: {
        int r = in->reg, rr = in->rm; uint8_t *dd = vreg(c, x, rr); int n = x ? 16 : 8; uint64_t cnt = in->imm & 0xff;
        int op = -1;
        if (b == 0x71) op = r == 2 ? V_PSRLW : r == 4 ? V_PSRAW : r == 6 ? V_PSLLW : -1;
        else if (b == 0x72) op = r == 2 ? V_PSRLD : r == 4 ? V_PSRAD : r == 6 ? V_PSLLD : -1;
        else { op = r == 2 ? V_PSRLQ : r == 6 ? V_PSLLQ : -1;
               if (x && (r == 3 || r == 7)) { unsigned k = cnt > 16 ? 16 : (unsigned)cnt; uint8_t t[16]; memset(t, 0, 16);
                   if (r == 7) memcpy(t + k, dd, 16 - k); else memcpy(t, dd + k, 16 - k); memcpy(dd, t, 16); return; } }
        if (op < 0 || (in->fl & IF_MEM)) fault(c, in, "SSE shift group");
        v_shift(op, dd, cnt, n); return; }
    case 0x77: return;
    case 0x7e:
        if (p == 2) { s = vsrc(c, in, 1); uint64_t v = XSQ(0); XQ(d, 0) = v; XQ(d, 1) = 0; return; }
        { uint32_t v = x ? XU(d, 0) : (uint32_t)c->mm[d]; rm_wr(c, in, EA0, 4, v); return; }
    case 0x7f:
        if (p == 0) { if (in->fl & IF_MEM) M64(ea(c, in)) = c->mm[d]; else c->mm[in->rm] = c->mm[d]; return; }
        { uint8_t *dst = (in->fl & IF_MEM) ? G + ea(c, in) : c->xmm[in->rm]; uint8_t t[16]; memcpy(t, D, 16); memcpy(dst, t, 16); return; }
    case 0xc2: {
        s = vsrc(c, in, 1); int pr = in->imm & 7;
        if (p == 0) { float v[4]; memcpy(v, s, 16); for (int k = 0; k < 4; k++) XU(d, k) = pred_f(pr, XF(d, k), v[k]) ? 0xffffffffu : 0; }
        else if (p == 1) { double v[2]; memcpy(v, s, 16); for (int k = 0; k < 2; k++) XQ(d, k) = pred_d(pr, XD(d, k), v[k]) ? ~0ull : 0; }
        else if (p == 2) { float v; memcpy(&v, s, 4); XU(d, 0) = pred_f(pr, XF(d, 0), v) ? 0xffffffffu : 0; }
        else { double v; memcpy(&v, s, 8); XQ(d, 0) = pred_d(pr, XD(d, 0), v) ? ~0ull : 0; }
        return; }
    case 0xc3: M32(ea(c, in)) = R(in->reg); return;
    case 0xc4: { uint32_t v = (in->fl & IF_MEM) ? M16(ea(c, in)) : (R(in->rm) & 0xffff); uint8_t *dd = vreg(c, x, d); int lane = in->imm & (x ? 7 : 3); ((uint16_t *)dd)[lane] = (uint16_t)v; return; }
    case 0xc5: { const uint8_t *r = vreg(c, x, in->rm); R(in->reg) = ((const uint16_t *)r)[in->imm & (x ? 7 : 3)]; return; }
    case 0xc6:
        s = vsrc(c, in, 1);
        if (p == 0) { float a_[4], b_[4]; memcpy(a_, D, 16); memcpy(b_, s, 16); int imm = in->imm; XF(d, 0) = a_[imm & 3]; XF(d, 1) = a_[(imm >> 2) & 3]; XF(d, 2) = b_[(imm >> 4) & 3]; XF(d, 3) = b_[(imm >> 6) & 3]; }
        else { double a0 = XD(d, in->imm & 1), b0 = XSD((in->imm >> 1) & 1); XD(d, 0) = a0; XD(d, 1) = b0; }
        return;
    case 0xd6:
        if (p == 1) { if (in->fl & IF_MEM) M64(ea(c, in)) = XQ(d, 0); else { XQ(in->rm, 0) = XQ(d, 0); XQ(in->rm, 1) = 0; } return; }
        if (p == 2) { XQ(d, 0) = c->mm[in->rm]; XQ(d, 1) = 0; return; }
        if (p == 3) { c->mm[d] = XQ(in->rm, 0); return; }
        break;
    case 0xd7: { const uint8_t *r = vreg(c, x, in->rm); R(in->reg) = v_movmskb(r, x ? 16 : 8); return; }
    case 0xe6:
        s = vsrc(c, in, 1);
        if (p == 2) { int32_t a0 = (int32_t)XSU(0), a1 = (int32_t)XSU(1); XD(d, 0) = a0; XD(d, 1) = a1; }
        else { double v[2]; memcpy(v, s, 16); int32_t (*cv)(double) = p == 1 ? sse_cvtt : sse_cvt; XU(d, 0) = (uint32_t)cv(v[0]); XU(d, 1) = (uint32_t)cv(v[1]); XQ(d, 1) = 0; }
        return;
    case 0xe7: if (x) { uint8_t t[16]; memcpy(t, D, 16); memcpy(G + ea(c, in), t, 16); } else M64(ea(c, in)) = c->mm[d]; return;
    case 0xf0: memcpy(D, G + ea(c, in), 16); return;
    case 0xf4: {
        s = vsrc(c, in, x);
        if (x) { uint64_t p0 = (uint64_t)XU(d, 0) * XSU(0), p1 = (uint64_t)XU(d, 2) * XSU(2); XQ(d, 0) = p0; XQ(d, 1) = p1; }
        else c->mm[d] = (uint64_t)(uint32_t)c->mm[d] * XSU(0);
        return; }
    case 0xf7: {
        const uint8_t *m = vreg(c, x, in->rm); int n = x ? 16 : 8; uint8_t *src = vreg(c, x, d); uint32_t a2 = c->edi; for (int k = 0; k < n; k++) if (m[k] & 0x80) M8(a2 + k) = src[k]; return; }
    default: break;
    }
    // 0F 60-FE integer vector operations
    if (VMAP[b] >= 0 || VSHMAP[b] >= 0) {
        if (!(p == 0 || p == 1)) fault(c, in, "SSE integer prefix");
        s = vsrc(c, in, x); uint8_t *dd = vreg(c, x, d); int n = x ? 16 : 8;
        if (VMAP[b] >= 0) { if (!x && b == 0xd4 && 0) {} v_binop(VMAP[b], dd, s, n); }
        else { uint64_t cnt; memcpy(&cnt, s, 8); v_shift(VSHMAP[b], dd, cnt, n); }
        return;
    }
    (void)a;
    fault(c, in, "SSE opcode");
}

// ---- 0F map and miscellaneous one-byte opcodes ---------------------------------------------------------------------------------------------
static const uint16_t SEGVAL[6] = {0x2b, 0x23, 0x2b, 0x2b, 0x53, 0x2b};          // es cs ss ds fs gs as seen by a Windows XP+ user process

static void do_fxsave(Cpu *c, uint32_t a, int save) {
    if (save) {
        memset(G + a, 0, 512); M16(a) = c->fcw; M16(a + 2) = x87_sw(c); M8(a + 4) = 0xff; M32(a + 24) = c->mxcsr; M32(a + 28) = 0xffff;
        for (int i = 0; i < 8; i++) st80(a + 32 + 16 * i, ST(i));
        for (int i = 0; i < 8; i++) memcpy(G + a + 160 + 16 * i, c->xmm[i], 16);
    } else {
        c->fcw = M16(a); c->fsw = M16(a + 2) & ~0x3800; c->top = (M16(a + 2) >> 11) & 7; c->mxcsr = M32(a + 24);
        for (int i = 0; i < 8; i++) ST(i) = ld80(a + 32 + 16 * i);
        for (int i = 0; i < 8; i++) memcpy(c->xmm[i], G + a + 160 + 16 * i, 16);
    }
}
static void do_fsave(Cpu *c, uint32_t a, int save) {                              // 32-bit protected-mode layout: 28-byte environment + 8 x 10 bytes
    if (save) {
        M32(a) = c->fcw; M32(a + 4) = x87_sw(c); M32(a + 8) = 0; M32(a + 12) = 0; M32(a + 16) = 0; M32(a + 20) = 0; M32(a + 24) = 0;
        for (int i = 0; i < 8; i++) st80(a + 28 + 10 * i, ST(i));
        c->top = 0; c->fcw = 0x37f; c->fsw = 0;
    } else {
        c->fcw = M16(a); c->fsw = M16(a + 4) & ~0x3800; c->top = (M16(a + 4) >> 11) & 7;
        for (int i = 0; i < 8; i++) ST(i) = ld80(a + 28 + 10 * i);
    }
}

int interp_run_n(Cpu *c, int32_t budget) {
    if (!cache) interp_init(NULL);
    uint32_t pc = c->pc; int32_t left = budget; Fl *f = &c->fl;
#ifdef INTERP_ENTRY_ACCOUNTING
    uint32_t ind_target = 0;
#endif
    for (;;) {
        Ins *in = &cache[cidx(pc)];
        if (__builtin_expect(in->pc != pc, 0)) decode(pc, in);
        if (__builtin_expect(in->op == OP_EXIT, 0)) {
#ifdef INTERP_ENTRY_ACCOUNTING
            c->last_eflags = 0;
#endif
            c->pc = pc; interp_icount += (uint64_t)(budget - left); return INTERP_NATIVE; }
#ifdef INTERP_ENTRY_ACCOUNTING
        if (__builtin_expect(in->fl & IF_ENTRY, 0)) {
          // c->last_eflags (unused otherwise) = entry of the lifted function we are "in".  A direct jump to an address inside the function's own body is a goto there, an
          // indirect jmp is a goto only for addresses that have a label (jump-table / branch targets); everything else is a tail call through rt_dispatch = a new entry.
          int ind = pc == ind_target; ind_target = 0;
          unsigned kind = interp_entry_internal && c->last_eflags ? interp_entry_internal(c->last_eflags, pc) : 0;       // bit0: pc is inside the body, bit1: it has a label
          if (ind ? (kind & 2) : (pc == c->last_eflags || (kind & 1))) goto no_charge;
            if (__builtin_expect(interp_charge_trace > 0, 0)) { interp_charge_trace--; fprintf(stderr, "[charge] %08x (from fn %08x) budget=%d\n", pc, c->last_eflags, c->budget); }
            c->last_eflags = pc;
            if (--c->budget < 0) { c->last_eflags = 0; c->pc = pc; interp_icount += (uint64_t)(budget - left); return INTERP_PREEMPT; }       // same charge as a lifted function's entry check
        }
        no_charge:;
#endif
        uint32_t npc = pc + in->len, a, v, t;
        int sz = in->sz;
        switch (in->op) {
        // ---- ALU ----
        case OP_ALU_MR: { a = EA0; uint32_t d = rm_rd(c, in, a, sz); uint32_t r = alu(c, in->k, d, rget(c, in->reg, sz), sz * 8); if (in->k != 7) rm_wr(c, in, a, sz, r); break; }
        case OP_ALU_RM: { uint32_t s = rm_rd(c, in, EA0, sz); uint32_t r = alu(c, in->k, rget(c, in->reg, sz), s, sz * 8); if (in->k != 7) rset(c, in->reg, sz, r); break; }
        case OP_ALU_AI: { uint32_t r = alu(c, in->k, rget(c, 0, sz), in->imm, sz * 8); if (in->k != 7) rset(c, 0, sz, r); break; }
        case OP_ALU_MI: { a = EA0; uint32_t d = rm_rd(c, in, a, sz); uint32_t r = alu(c, in->k, d, in->imm, sz * 8); if (in->k != 7) rm_wr(c, in, a, sz, r); break; }
        case OP_SHIFT: do_shift(c, in); break;
        case OP_INCDEC_R: { v = rget(c, in->rm, sz); rset(c, in->rm, sz, in->k ? fl_dec(f, v, sz * 8) : fl_inc(f, v, sz * 8)); break; }
        case OP_INCDEC_M: { a = EA0; v = rm_rd(c, in, a, sz); rm_wr(c, in, a, sz, in->k ? fl_dec(f, v, sz * 8) : fl_inc(f, v, sz * 8)); break; }
        case OP_GRP3: do_grp3(c, in); break;
        case OP_JCC: if (cond(f, in->k)) npc = in->imm; break;
        case OP_SETCC: rm_wr(c, in, EA0, 1, cond(f, in->k)); break;
        case OP_CMOV: v = rm_rd(c, in, EA0, sz); if (cond(f, in->k)) rset(c, in->reg, sz, v); break;
        case OP_CALL_M: { ENTRY_RESET t = rm_rd(c, in, EA0, 4); push32(c, npc); if (__builtin_expect(interp_call_hook != NULL, 0)) interp_call_hook(t, npc); npc = t; if (INTERP_CALL_BUDGET && __builtin_expect(--c->budget < 0, 0)) goto preempt; break; }
        case OP_JMP_M: npc = rm_rd(c, in, EA0, 4); IND_MARK(npc) break;
        case OP_PUSH_M: { v = rm_rd(c, in, EA0, sz); if (sz == 2) { c->esp -= 2; M16(c->esp) = (uint16_t)v; } else push32(c, v); break; }
        case OP_FPU: do_fpu(c, in); break;
        case OP_BAD: case OP_EXIT: fault(c, in, "unsupported opcode");
        // ---- one-byte opcodes ----
        case 0x50 ... 0x57: v = R(in->op & 7); if (sz == 2) { c->esp -= 2; M16(c->esp) = (uint16_t)v; } else push32(c, v); break;
        case 0x58 ... 0x5f: if (sz == 2) { v = M16(c->esp); c->esp += 2; rset(c, in->op & 7, 2, v); } else { v = pop32(c); R(in->op & 7) = v; } break;
        case 0x60: { t = c->esp; c->esp -= 32; uint32_t e = c->esp; M32(e + 28) = c->eax; M32(e + 24) = c->ecx; M32(e + 20) = c->edx; M32(e + 16) = c->ebx; M32(e + 12) = t; M32(e + 8) = c->ebp; M32(e + 4) = c->esi; M32(e) = c->edi; break; }
        case 0x61: { uint32_t e = c->esp; c->edi = M32(e); c->esi = M32(e + 4); c->ebp = M32(e + 8); c->ebx = M32(e + 16); c->edx = M32(e + 20); c->ecx = M32(e + 24); c->eax = M32(e + 28); c->esp = e + 32; break; }
        case 0x68: case 0x6a: if (sz == 2) { c->esp -= 2; M16(c->esp) = (uint16_t)in->imm; } else push32(c, in->imm); break;
        case 0x69: case 0x6b: do_imul3(c, in, rm_rd(c, in, EA0, sz), in->imm); break;
        case 0x84: case 0x85: fl_logic(f, rm_rd(c, in, EA0, sz) & rget(c, in->reg, sz), sz * 8); break;
        case 0x86: case 0x87: { a = EA0; uint32_t x1 = rm_rd(c, in, a, sz), x2 = rget(c, in->reg, sz); rm_wr(c, in, a, sz, x2); rset(c, in->reg, sz, x1); break; }
        case 0x88: case 0x89: rm_wr(c, in, EA0, sz, rget(c, in->reg, sz)); break;
        case 0x8a: case 0x8b: rset(c, in->reg, sz, rm_rd(c, in, EA0, sz)); break;
        case 0x8c: rm_wr(c, in, EA0, (in->fl & IF_MEM) ? 2 : sz, in->reg < 6 ? SEGVAL[in->reg] : 0); break;
        case 0x8d: if (!(in->fl & IF_MEM)) fault(c, in, "lea reg"); rset(c, in->reg, sz, ea_raw(c, in)); break;
        case 0x8e: break;                                                     // segment loads are ignored (flat model)
        case 0x8f: if (sz == 2) { v = M16(c->esp); c->esp += 2; } else v = pop32(c); rm_wr(c, in, EA0, sz, v); break;
        case 0x90: if (in->k) rt_spin(c); break;
        case 0x91 ... 0x97: { t = rget(c, in->op & 7, sz); v = rget(c, 0, sz); rset(c, 0, sz, t); rset(c, in->op & 7, sz, v); break; }
        case 0x98: if (sz == 2) rset(c, 0, 2, sx(c->eax, 1)); else c->eax = sx(c->eax, 2); break;
        case 0x99: if (sz == 2) rset(c, 2, 2, (uint32_t)((int16_t)c->eax >> 15)); else c->edx = (uint32_t)((int32_t)c->eax >> 31); break;
        case 0x9b: break;
        case 0x9c: push32(c, fl_eflags(f, c->df)); break;
        case 0x9d: v = pop32(c); fl_load(f, v, &c->df); break;
        case 0x9e: { uint8_t ah = (c->eax >> 8) & 0xff; f->cf = ah & 1; f->pf = (ah >> 2) & 1; f->af = (ah >> 4) & 1; f->zf = (ah >> 6) & 1; f->sf = (ah >> 7) & 1; break; }
        case 0x9f: c->eax = (c->eax & 0xffff00ffu) | ((fl_eflags(f, 0) & 0xd7u) << 8); break;
        case 0xa0: case 0xa1: rset(c, 0, sz, mget(ea(c, in), sz)); break;
        case 0xa2: case 0xa3: mset(ea(c, in), sz, c->eax); break;
        case 0xa4: case 0xa5: case 0xa6: case 0xa7: case 0xaa: case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf: do_string(c, in); break;
        case 0xa8: case 0xa9: fl_logic(f, rget(c, 0, sz) & in->imm, sz * 8); break;
        case 0xb0 ... 0xb7: r8set(c, in->op & 7, in->imm); break;
        case 0xb8 ... 0xbf: rset(c, in->op & 7, sz, in->imm); break;
        case 0xc2: ENTRY_RESET v = pop32(c); c->esp += in->imm; npc = v; if (0) {} break;
        case 0xc3: ENTRY_RESET npc = pop32(c); break;
        case 0xc6: case 0xc7: rm_wr(c, in, EA0, sz, in->imm); break;
        case 0xc8: if (in->imm2) fault(c, in, "enter nesting"); push32(c, c->ebp); c->ebp = c->esp; c->esp -= in->imm; break;
        case 0xc9: c->esp = c->ebp; c->ebp = pop32(c); break;
        case 0xcc: if (interp_int_hook && interp_int_hook(c, 3)) break; fault(c, in, "int3");
        case 0xcd: if (interp_int_hook && interp_int_hook(c, (int)(in->imm & 0xff))) break; fault(c, in, "int");
        case 0xd7: c->eax = (c->eax & 0xffffff00u) | M8(c->ebx + (c->eax & 0xff)); break;
        case 0xe0: case 0xe1: case 0xe2: c->ecx--; if (c->ecx != 0 && (in->op == 0xe2 || (in->op == 0xe1 ? f->zf : !f->zf))) npc = in->imm; break;
        case 0xe3: if (c->ecx == 0) npc = in->imm; break;
        case 0xe8: ENTRY_RESET push32(c, npc); if (__builtin_expect(interp_call_hook != NULL, 0)) interp_call_hook(in->imm, npc); npc = in->imm; if (INTERP_CALL_BUDGET && __builtin_expect(--c->budget < 0, 0)) goto preempt; break;
        case 0xe9: case 0xeb: npc = in->imm; break;
        case 0xf5: f->cf = !f->cf; break;
        case 0xf8: f->cf = 0; break;
        case 0xf9: f->cf = 1; break;
        case 0xfc: c->df = 0; break;
        case 0xfd: c->df = 1; break;
        case 0x06: case 0x0e: case 0x16: case 0x1e: push32(c, SEGVAL[in->op >> 3]); break;
        case 0x07: case 0x17: case 0x1f: c->esp += 4; break;
        // ---- 0F map ----
        case 0x105: case 0x10b: fault(c, in, "syscall/ud2");
        case 0x10d: case 0x118: case 0x119: case 0x11a: case 0x11b: case 0x11c: case 0x11d: case 0x11e: case 0x11f: case 0x10e: break;      // prefetch / hint nops / femms
        case 0x131: c->pc = pc; rt_rdtsc(c); break;
        case 0x1a2: c->pc = pc; rt_cpuid(c); break;
        case 0x1a3: do_bt(c, in, 0, 0); break; case 0x1ab: do_bt(c, in, 1, 0); break; case 0x1b3: do_bt(c, in, 2, 0); break; case 0x1bb: do_bt(c, in, 3, 0); break;
        case 0x1ba: if (in->reg < 4) fault(c, in, "0FBA group"); do_bt(c, in, in->reg - 4, 1); break;
        case 0x1a4: do_shxd(c, in, 0, 0); break; case 0x1a5: do_shxd(c, in, 0, 1); break; case 0x1ac: do_shxd(c, in, 1, 0); break; case 0x1ad: do_shxd(c, in, 1, 1); break;
        case 0x1af: do_imul3(c, in, rget(c, in->reg, sz), rm_rd(c, in, EA0, sz)); break;
        case 0x1b0: case 0x1b1: {
            a = EA0; uint32_t d = rm_rd(c, in, a, sz), acc = rget(c, 0, sz); fl_sub(f, acc, d, 0, sz * 8);
            if (f->zf) rm_wr(c, in, a, sz, rget(c, in->reg, sz)); else { rt_spin(c); rset(c, 0, sz, d); }
            break; }
        case 0x1b6: case 0x1b7: rset(c, in->reg, sz, zx(rm_rd(c, in, EA0, in->k), in->k)); break;
        case 0x1be: case 0x1bf: rset(c, in->reg, sz, sx(rm_rd(c, in, EA0, in->k), in->k)); break;
        case 0x1bc: case 0x1bd: {
            v = zx(rm_rd(c, in, EA0, sz), sz); f->zf = v == 0;
            if (v) rset(c, in->reg, sz, in->op == 0x1bc ? (uint32_t)__builtin_ctz(v) : (uint32_t)(31 - __builtin_clz(v)));
            break; }
        case 0x1c0: case 0x1c1: { a = EA0; uint32_t d = rm_rd(c, in, a, sz), s = rget(c, in->reg, sz); uint32_t r = fl_add(f, d, s, 0, sz * 8); rset(c, in->reg, sz, d); rm_wr(c, in, a, sz, r); break; }
        case 0x1c7: {
            if (!(in->fl & IF_MEM) || in->reg != 1) fault(c, in, "0FC7");
            a = ea(c, in); uint64_t m = M64(a), e = ((uint64_t)c->edx << 32) | c->eax;
            if (m == e) { M64(a) = ((uint64_t)c->ecx << 32) | c->ebx; f->zf = 1; } else { c->eax = (uint32_t)m; c->edx = (uint32_t)(m >> 32); f->zf = 0; rt_spin(c); }
            break; }
        case 0x1c8 ... 0x1cf: R(in->rm) = __builtin_bswap32(R(in->rm)); break;
        case 0x1ae:
            if (in->fl & IF_MEM) {
                a = ea(c, in);
                switch (in->reg) { case 0: do_fxsave(c, a, 1); break; case 1: do_fxsave(c, a, 0); break; case 2: c->mxcsr = M32(a); break; case 3: M32(a) = c->mxcsr; break; case 7: break; default: fault(c, in, "0FAE"); }
            } else if (in->reg < 5) fault(c, in, "0FAE reg");
            break;
        case 0x1a0: case 0x1a8: push32(c, SEGVAL[in->op == 0x1a0 ? 4 : 5]); break;
        case 0x1a1: case 0x1a9: c->esp += 4; break;
        default:
            if (in->op >= 0x110 && in->op < 0x200) { c->pc = pc; do_sse(c, in); break; }
            fault(c, in, "unsupported opcode");
        }
        if (INTERP_CALL_BUDGET && __builtin_expect(npc <= pc, 0) && --c->budget < 0) goto preempt;      // a backward branch is a loop iteration: lifted code charges each block entry, so spin-waits (no calls) must yield too
        pc = npc;
        if (__builtin_expect(--left <= 0, 0)) { c->pc = pc; interp_icount += (uint64_t)budget; return INTERP_BUDGET; }
        continue;
    preempt:                                    // the thread's time slice (c->budget, decremented per call like the lifted code does per function entry) ran out
        c->pc = npc; interp_icount += (uint64_t)(budget - left) + 1; return INTERP_PREEMPT;
    }
}
void interp_run(Cpu *c) { interp_run_n(c, interp_slice); }

#ifdef INTERP_TEST_API
// Test hook (tools/declen.py): decode one instruction from a plain buffer; returns its length, *op = decoded class (OP_BAD / OP_EXIT mean "would trap").
int interp_decode_probe(const uint8_t *code, int *op) {
    static Ins tmp; uint8_t *saveG = G; InterpIsNative saveN = host_native;
    if (!tables_ready) { tables_init(); vmap_init(); }
    host_native = NULL; G = (uint8_t *)code - 0x10000000u; decode(0x10000000u, &tmp); G = saveG; host_native = saveN;
    *op = tmp.op; return tmp.len;
}
#endif
