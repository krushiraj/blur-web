// Differential tests of the x86 interpreter against Unicorn.
//   difftest single <image.bin> <trials> [seed] [samples.bin]  one-instruction trials (random encodings, or real instructions from samples.bin)
//   difftest funcs  <image.bin> <addrs.txt> <trials> [seed]    whole-function trials (call trees run to the return magic)
// Mismatches are printed with the instruction bytes; a summary by category is printed at the end.
#include "interp.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <unicorn/unicorn.h>

typedef struct { uint64_t mantissa; uint16_t exponent; } uc_x86_float80;
uint8_t *G;
static sigjmp_buf jb;
static const char *uns_what; static uint32_t uns_addr;
static void on_sig(int sig) { siglongjmp(jb, 100 + sig); }
void rt_spin(Cpu *c) { (void)c; }
void rt_preempt(Cpu *c) { c->budget = 1 << 30; }
int rt_trace; void rt_enter(Cpu *c, uint32_t a) { (void)c; (void)a; }
#ifdef MIXED
// Mixed mode: a subset of the corpus is run as lifted C (mixed/hot.c from the real lifter), everything else in the interpreter, handing over at function boundaries
// exactly like the production dispatcher would.
extern const struct { uint32_t a; GuestFn f; } hot_table[]; extern const unsigned hot_n;
static GuestFn hot_lookup(uint32_t pc) { for (unsigned i = 0; i < hot_n; i++) if (hot_table[i].a == pc) return hot_table[i].f; return NULL; }
static int hot_calls, cold_runs;
void rt_dispatch(Cpu *c) {
    for (;;) {
        uint32_t pc = c->pc; if (pc == 0x7efff000u) return;
        GuestFn f = hot_lookup(pc); if (f) { hot_calls++; MUSTTAIL return f(c); }
        cold_runs++; int r = interp_run_n(c, 1000000); if (r == INTERP_PREEMPT) c->budget = 1 << 30;
    }
}
#else
void rt_dispatch(Cpu *c) { (void)c; }
#endif
void rt_cpuid(Cpu *c) { c->eax = c->ebx = c->ecx = c->edx = 0; } void rt_rdtsc(Cpu *c) { c->eax = c->edx = 0; }
void rt_unsupported(Cpu *c, uint32_t a, const char *w) { (void)c; uns_what = w; uns_addr = a; siglongjmp(jb, 3); }

#define IMG_BASE 0x400000u
#define CODE_ADDR 0x10000000u
#define SCR_BASE 0x30000000u
#define SCR_SIZE 0x10000u
#define STK_TOP  0x0f800000u
#define STK_SIZE 0x4000u
#define STK_CMP  0x2000u
#define TEB_ADDR 0x7ffdf000u
#define RET_MAGIC 0x7efff000u
static int skip_pfx(const uint8_t *b) { int i = 0; while (b[i] == 0x66 || b[i] == 0xf2 || b[i] == 0xf3 || b[i] == 0x64 || b[i] == 0xf0 || b[i] == 0x2e || b[i] == 0x3e || b[i] == 0x26 || b[i] == 0x36 || b[i] == 0x65 || b[i] == 0x67) i++; return i; }
static uint64_t rng = 88172645463325252ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }
static uint32_t rand_val(void) {
    switch (rnd() % 14) { case 0: case 1: case 2: return rnd() % 16; case 3: return 0xffffffffu - rnd() % 4; case 4: case 5: case 6: case 7: case 8: case 9: return SCR_BASE + 0x2000 + (rnd() % (SCR_SIZE / 2));
        case 10: return rnd() & 0xff; case 11: return rnd() % 4096; case 12: return 0x80000000u + (rnd() % 3) - 1; default: return rnd(); }
}
static double rand_dbl(void) {
    switch (rnd() % 12) {
    case 0: return 0.0; case 1: return -0.0; case 2: return (double)((int)(rnd() % 2001) - 1000);
    case 3: case 4: case 5: return (double)((int)(rnd() % 200001) - 100000) / 1000.0;
    case 6: return (double)((int)(rnd() % 20) - 10) + 0.5;
    case 7: return ldexp((double)(int)rnd(), (int)(rnd() % 80) - 40);
    case 8: return (rnd() & 1) ? INFINITY : -INFINITY;
    case 9: return 1.0 / (1 + rnd() % 7);
    default: return (double)(int)rnd() / 65536.0;
    }
}
static float rand_flt(void) {
    switch (rnd() % 10) {
    case 0: return 0.0f; case 1: return -0.0f; case 2: case 3: case 4: return (float)((int)(rnd() % 2001) - 1000) / 8.0f;
    case 5: case 6: return (float)((int)(rnd() % 200001) - 100000) / 1000.0f; case 7: return (float)(int)(rnd() % 7);
    case 8: { uint32_t b = rnd(); float f; memcpy(&f, &b, 4); return f != f ? 1.5f : f; }
    default: return (rnd() & 1) ? INFINITY : 3e9f;
    }
}

typedef struct { uint32_t r[8]; uint32_t fl; uint8_t xmm[8][16]; uint64_t mm[8]; double st[8]; int top; uint16_t fcw; int use_mm;
    uint8_t scr[SCR_SIZE], stk[STK_SIZE], teb[0x1000], code[32]; int codelen; } TS;
typedef struct { uint32_t r[8], eip, fl; uint8_t xmm[8][16]; uint64_t mm[8]; double st[8]; int top; uint16_t fsw;
    uint8_t scr[SCR_SIZE], stk[STK_CMP], teb[0x1000]; } RS;

static uc_engine *uc;
static void d2f80(double d, uc_x86_float80 *o) {
    uint16_t sign = signbit(d) ? 0x8000 : 0; d = fabs(d);
    if (d == 0) { o->mantissa = 0; o->exponent = sign; return; }
    if (d != d) { o->mantissa = 0xc000000000000000ull; o->exponent = 0x7fff | sign; return; }
    if (isinf(d)) { o->mantissa = 0x8000000000000000ull; o->exponent = 0x7fff | sign; return; }
    int e; double fr = frexp(d, &e); o->mantissa = (uint64_t)ldexp(fr, 64); o->exponent = (uint16_t)((e - 1 + 16383) | sign);
}
static double f802d(const uc_x86_float80 *u) {
    int e = u->exponent & 0x7fff; double r;
    if (e == 0x7fff) r = (u->mantissa << 1) ? NAN : INFINITY; else if (e == 0 && u->mantissa == 0) r = 0.0; else r = ldexp((double)u->mantissa, e - 16383 - 63);
    return (u->exponent & 0x8000) ? -r : r;
}
static const int REGID[8] = {UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI};

static struct { uint32_t base; long n; } imgs[2]; static int nimgs;
static void uc_build(void) {
    uc_open(UC_ARCH_X86, UC_MODE_32, &uc);
    for (int k = 0; k < nimgs; k++) {
        long sz = (imgs[k].n + 0xfff) & ~0xfffl; uc_mem_map(uc, imgs[k].base, sz, UC_PROT_READ | UC_PROT_EXEC);
        uc_mem_write(uc, imgs[k].base, G + imgs[k].base, imgs[k].n);
    }
    uc_mem_map(uc, CODE_ADDR, 0x2000, UC_PROT_ALL);
    uc_mem_map(uc, SCR_BASE, SCR_SIZE, UC_PROT_ALL);
    uc_mem_map(uc, STK_TOP - STK_SIZE, STK_SIZE, UC_PROT_ALL);
    uc_mem_map(uc, RET_MAGIC, 0x1000, UC_PROT_ALL);
    uc_mem_map(uc, TEB_ADDR, 0x1000, UC_PROT_ALL);
    uc_x86_msr m = {0xC0000100, TEB_ADDR}; uc_reg_write(uc, UC_X86_REG_MSR, &m);
}
static void world_init(const char *img, const char *img2, uint32_t base2) {
    G = mmap(NULL, (1ull << 32) + 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (G == MAP_FAILED) { perror("mmap"); exit(1); }
    for (int k = 0; k < 2; k++) {
        const char *fn = k ? img2 : img; uint32_t base = k ? base2 : IMG_BASE; if (!fn) continue;
        FILE *f = fopen(fn, "rb"); if (!f) { perror(fn); exit(1); }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); if (fread(G + base, 1, n, f) != (size_t)n) exit(1); fclose(f);
        mprotect(G + base, (n + 0x3fff) & ~0x3fffl, PROT_READ); imgs[nimgs].base = base; imgs[nimgs++].n = n;
    }
    uc_build();
    signal(SIGSEGV, on_sig); signal(SIGBUS, on_sig); signal(SIGFPE, on_sig); signal(SIGALRM, on_sig);
}
static void uc_rebuild(void) { uc_close(uc); uc_build(); }
// Unicorn can be left unusable by some guest events (halt, exception state): check that a nop still executes, otherwise rebuild the engine.
static void uc_probe(void) {
    // mov eax, fs:[0x18] ; push eax ; pop ebx ; mov ecx, [SCR_BASE+0x100] ; mov edx, [esp-4] ; nop   -> exercises FS base, SS, DS, flat memory and the stack
    static const uint8_t prog[] = {0x64, 0xa1, 0x18, 0, 0, 0, 0x50, 0x5b, 0x8b, 0x0d, 0x00, 0x01, 0x00, 0x30, 0x8b, 0x54, 0x24, 0xfc, 0x90};
    uint32_t fl = 0x202, esp = STK_TOP - 0x100, v1 = 0x11223344, v2 = 0x55667788, eip = 0, eax = 0, ebx = 0, ecx = 0;
    uc_mem_write(uc, CODE_ADDR, prog, sizeof prog); uc_ctl_remove_cache(uc, CODE_ADDR, CODE_ADDR + 0x1000); uc_reg_write(uc, UC_X86_REG_EFLAGS, &fl); uc_reg_write(uc, UC_X86_REG_ESP, &esp);
    uc_mem_write(uc, TEB_ADDR + 0x18, &v1, 4); uc_mem_write(uc, SCR_BASE + 0x100, &v2, 4);
    uc_err e = uc_emu_start(uc, CODE_ADDR, CODE_ADDR + sizeof prog, 0, 0);
    uc_reg_read(uc, UC_X86_REG_EIP, &eip); uc_reg_read(uc, UC_X86_REG_EAX, &eax); uc_reg_read(uc, UC_X86_REG_EBX, &ebx); uc_reg_read(uc, UC_X86_REG_ECX, &ecx);
    if (e != UC_ERR_OK || eip != CODE_ADDR + sizeof prog || eax != v1 || ebx != v1 || ecx != v2) { static int n; if (n++ < 3) fprintf(stderr, "[unicorn rebuilt after probe failure]\n"); uc_rebuild(); }
}
#define POOL_SCR (4u << 20)
#define POOL_STK (1u << 20)
static uint8_t *pool_scr, *pool_stk;
static void pools_init(void) {
    pool_scr = malloc(POOL_SCR + SCR_SIZE); pool_stk = malloc(POOL_STK + STK_SIZE);
    for (uint32_t i = 0; i < POOL_SCR + SCR_SIZE; i += 8) { uint64_t v = ((uint64_t)rnd() << 32) | rnd(); if (rnd() % 4 == 0) { float a = rand_flt(), b = rand_flt(); memcpy(&v, &a, 4); memcpy((char *)&v + 4, &b, 4); } memcpy(pool_scr + i, &v, 8); }
    for (uint32_t i = 0; i < POOL_STK + STK_SIZE; i += 4) { uint32_t v = rand_val(); memcpy(pool_stk + i, &v, 4); }
}
static void ts_random(TS *t, int mmx_mode) {
    if (!pool_scr) pools_init();
    memcpy(t->scr, pool_scr + ((rnd() % (POOL_SCR / 8)) * 8), SCR_SIZE);
    memcpy(t->stk, pool_stk + ((rnd() % (POOL_STK / 4)) * 4), STK_SIZE);
    for (uint32_t i = 0; i < 0x1000; i += 4) { uint32_t v = rnd() & 0xffff; memcpy(t->teb + i, &v, 4); }
    for (int i = 0; i < 8; i++) t->r[i] = rand_val();
    t->r[4] = STK_TOP + (rnd() % 8 == 0 ? (rnd() % 4) : 0) - 0x800; if (rnd() % 8) t->r[4] &= ~3u;
    t->fl = 0x202 | ((rnd() & 1) ? 1 : 0) | ((rnd() & 1) << 2) | ((rnd() & 1) << 4) | ((rnd() & 1) << 6) | ((rnd() & 1) << 7) | ((rnd() & 1) << 11) | (rnd() % 8 == 0 ? 0x400 : 0);
    for (int i = 0; i < 8; i++) for (int j = 0; j < 4; j++) { float v = rand_flt(); if (rnd() % 7 == 0) { uint32_t b = rnd(); memcpy(t->xmm[i] + 4 * j, &b, 4); } else memcpy(t->xmm[i] + 4 * j, &v, 4); }
    if (rnd() % 5 == 0) for (int i = 0; i < 8; i++) for (int j = 0; j < 2; j++) { double v = rand_dbl(); memcpy(t->xmm[i] + 8 * j, &v, 8); }
    for (int i = 0; i < 8; i++) t->mm[i] = ((uint64_t)rnd() << 32) | rnd();
    t->top = 4; t->use_mm = mmx_mode;
    for (int i = 0; i < 8; i++) t->st[i] = rand_dbl();
    t->fcw = 0x27f | ((rnd() % 4 == 0 ? rnd() % 4 : 0) << 10);
}
static void ts_apply_native(const TS *t, Cpu *c) {
    memcpy(G + SCR_BASE, t->scr, SCR_SIZE); memcpy(G + STK_TOP - STK_SIZE, t->stk, STK_SIZE); memcpy(G + TEB_ADDR, t->teb, 0x1000); memset(G + CODE_ADDR, 0, 0x1000); memset(G + RET_MAGIC, 0, 0x1000); memcpy(G + CODE_ADDR, t->code, 32);
    memset(c, 0, sizeof *c); memcpy(&c->eax, t->r, 32); c->teb = TEB_ADDR; c->pc = CODE_ADDR; c->budget = 1 << 30;
    c->fl.cf = t->fl & 1; c->fl.pf = (t->fl >> 2) & 1; c->fl.af = (t->fl >> 4) & 1; c->fl.zf = (t->fl >> 6) & 1; c->fl.sf = (t->fl >> 7) & 1; c->fl.of = (t->fl >> 11) & 1; c->df = (t->fl >> 10) & 1;
    memcpy(c->xmm, t->xmm, sizeof t->xmm); memcpy(c->mm, t->mm, sizeof t->mm); memcpy(c->st, t->st, sizeof t->st); c->top = t->top; c->fcw = t->fcw; c->fsw = 0; c->mxcsr = 0x1f80;
}
static void ts_apply_uc(const TS *t) {
    uc_mem_write(uc, SCR_BASE, t->scr, SCR_SIZE); uc_mem_write(uc, STK_TOP - STK_SIZE, t->stk, STK_SIZE); uc_mem_write(uc, TEB_ADDR, t->teb, 0x1000);
    { static const uint8_t zp[0x1000]; uc_mem_write(uc, CODE_ADDR, zp, 0x1000); uc_mem_write(uc, RET_MAGIC, zp, 0x1000); } uc_mem_write(uc, CODE_ADDR, t->code, 32); uc_ctl_remove_cache(uc, CODE_ADDR, CODE_ADDR + 0x1000);
    for (int i = 0; i < 8; i++) uc_reg_write(uc, REGID[i], &t->r[i]);
    for (int i = 0; i < 8; i++) uc_reg_write(uc, UC_X86_REG_XMM0 + i, t->xmm[i]);
    uint32_t fl = t->fl; uc_reg_write(uc, UC_X86_REG_EFLAGS, &fl); { uint32_t mx = 0x1f80; uc_reg_write(uc, UC_X86_REG_MXCSR, &mx); }
    uint16_t cw = t->fcw, sw = (uint16_t)(t->top << 11), tag = 0xffff;
    if (t->use_mm) { uint16_t sw0 = 0, tag0 = 0; uc_reg_write(uc, UC_X86_REG_FPCW, &cw); uc_reg_write(uc, UC_X86_REG_FPSW, &sw0); uc_reg_write(uc, UC_X86_REG_FPTAG, &tag0);
        for (int i = 0; i < 8; i++) { uc_x86_float80 w = {t->mm[i], 0xffff}; uc_reg_write(uc, UC_X86_REG_ST0 + i, &w); } }
    else {
        uc_reg_write(uc, UC_X86_REG_FPCW, &cw); uc_reg_write(uc, UC_X86_REG_FPSW, &sw);
        tag = 0; uc_reg_write(uc, UC_X86_REG_FPTAG, &tag);
        for (int i = 0; i < 8; i++) { uc_x86_float80 w; d2f80(t->st[(t->top + i) & 7], &w); uc_reg_write(uc, UC_X86_REG_ST0 + i, &w); }
    }
}
static void rs_from_native(RS *r, const Cpu *c) {
    memcpy(r->r, &c->eax, 32); r->eip = c->pc; r->fl = fl_eflags(&c->fl, c->df);
    memcpy(r->xmm, c->xmm, sizeof r->xmm); memcpy(r->mm, c->mm, sizeof r->mm); memcpy(r->st, c->st, sizeof r->st); r->top = c->top; r->fsw = c->fsw;
    memcpy(r->scr, G + SCR_BASE, SCR_SIZE); memcpy(r->stk, G + STK_TOP - STK_CMP, STK_CMP); memcpy(r->teb, G + TEB_ADDR, 0x1000);
}
static void rs_from_uc(RS *r, int use_mm) {
    for (int i = 0; i < 8; i++) uc_reg_read(uc, REGID[i], &r->r[i]);
    uc_reg_read(uc, UC_X86_REG_EIP, &r->eip); uc_reg_read(uc, UC_X86_REG_EFLAGS, &r->fl);
    for (int i = 0; i < 8; i++) uc_reg_read(uc, UC_X86_REG_XMM0 + i, r->xmm[i]);
    uint16_t sw = 0; uc_reg_read(uc, UC_X86_REG_FPSW, &sw); r->top = (sw >> 11) & 7; r->fsw = sw;
    if (use_mm) for (int i = 0; i < 8; i++) { uc_x86_float80 w; uc_reg_read(uc, UC_X86_REG_ST0 + ((i - r->top) & 7), &w); r->mm[i] = w.mantissa; }
    else for (int i = 0; i < 8; i++) { uc_x86_float80 w; uc_reg_read(uc, UC_X86_REG_ST0 + i, &w); r->st[(r->top + i) & 7] = f802d(&w); }
    uc_mem_read(uc, SCR_BASE, r->scr, SCR_SIZE); uc_mem_read(uc, STK_TOP - STK_CMP, r->stk, STK_CMP); uc_mem_read(uc, TEB_ADDR, r->teb, 0x1000);
}

// ---- categories ---------------------------------------------------------------------------------------------------------------------
typedef struct { char key[48]; int n; char first[400]; } Cat;
static Cat cats[4096]; static int ncat;
static int note_defer, npend; static struct { char key[120]; char detail[400]; } pend[32];
static void note(const char *key, const char *detail) {
    if (note_defer) { if (npend < 32) { snprintf(pend[npend].key, sizeof pend[npend].key, "%s", key); snprintf(pend[npend++].detail, sizeof pend[0].detail, "%s", detail); } return; }
    for (int i = 0; i < ncat; i++) if (!strcmp(cats[i].key, key)) { cats[i].n++; return; }
    if (ncat < 4096) { snprintf(cats[ncat].key, sizeof cats[ncat].key, "%s", key); cats[ncat].n = 1; snprintf(cats[ncat].first, sizeof cats[ncat].first, "%s", detail); ncat++; }
}
static void hexs(char *o, const uint8_t *b, int n) { for (int i = 0; i < n; i++) sprintf(o + 2 * i, "%02x", b[i]); o[2 * n] = 0; }
// opcode key: prefixes stripped; "0f xx" for the two-byte map; "/n" group field appended for group opcodes
static void opkey(const uint8_t *b, char *k) {
    int i = 0, pf = 0; while (b[i] == 0x66 || b[i] == 0xf2 || b[i] == 0xf3 || b[i] == 0x64 || b[i] == 0xf0 || b[i] == 0x2e || b[i] == 0x3e || b[i] == 0x26 || b[i] == 0x36 || b[i] == 0x65 || b[i] == 0x67) { if (b[i] == 0x66) pf |= 1; if (b[i] == 0xf3) pf |= 2; if (b[i] == 0xf2) pf |= 4; i++; }
    const char *ps = (pf & 2) ? "f3:" : (pf & 4) ? "f2:" : (pf & 1) ? "66:" : "";
    int op = b[i], two = 0; if (op == 0x0f) { two = 1; op = b[i + 1]; }
    int modrm = b[i + 1 + two]; int grp = (modrm >> 3) & 7, mod = modrm >> 6;
    int isgrp = !two && (op == 0x80 || op == 0x81 || op == 0x83 || op == 0xc0 || op == 0xc1 || (op >= 0xd0 && op <= 0xd3) || op == 0xf6 || op == 0xf7 || op == 0xfe || op == 0xff || op == 0xc6 || op == 0xc7 || op == 0x8f);
    int fpu = !two && op >= 0xd8 && op <= 0xdf;
    if (fpu) sprintf(k, "%s%02x/%d%s%s", ps, op, grp, mod == 3 ? "r" : "m", mod == 3 ? (char[3]){"0123456789abcdef"[(modrm >> 4) & 15], "0123456789abcdef"[modrm & 15], 0} : "");
    else if (isgrp || (two && (op == 0xba || op == 0x71 || op == 0x72 || op == 0x73 || op == 0xae || op == 0xc7 || op == 0x18))) sprintf(k, "%s%s%02x/%d", ps, two ? "0f " : "", op, grp);
    else sprintf(k, "%s%s%02x", ps, two ? "0f " : "", op);
}

// ---- comparison ---------------------------------------------------------------------------------------------------------------------
static int fp_close(double a, double b) { if (a != a && b != b) return 1; if (a == b) return 1; if (isinf(a) || isinf(b) || a != a || b != b) return 0; return fabs(a - b) <= 1e-9 * fabs(b) + 1e-300; }
// flags that are architecturally defined after this instruction (others are skipped in the comparison)
static uint32_t flag_mask(const TS *t) {
    const uint8_t *b = t->code; int i = 0; uint32_t all = 0x8d5 | 0x400;
    while (b[i] == 0x66 || b[i] == 0xf2 || b[i] == 0xf3 || b[i] == 0x64 || b[i] == 0xf0 || b[i] == 0x2e || b[i] == 0x3e || b[i] == 0x26 || b[i] == 0x36) i++;
    int op = b[i], two = 0; if (op == 0x0f) { two = 1; op = b[i + 1]; }
    int modrm = b[i + 1 + two], grp = (modrm >> 3) & 7, mod = modrm >> 6, rm = modrm & 7;
    // instruction length of the modrm form (for immediate lookup)
    int mlen = 1; if (mod != 3) { if (rm == 4) mlen++; if (mod == 1) mlen += 1; else if (mod == 2) mlen += 4; else if (mod == 0 && (rm == 5 || (rm == 4 && (b[i + 2 + two] & 7) == 5))) mlen += 4; }
    if (!two) {
        if ((op == 0xdb || op == 0xdf) && mod == 3 && grp >= 5) return 0x45;
        if (op == 0xc0 || op == 0xc1 || (op >= 0xd0 && op <= 0xd3)) {
            uint32_t n = (op == 0xd0 || op == 0xd1) ? 1 : (op >= 0xd2) ? (t->r[1] & 0xff) : b[i + 1 + mlen]; n &= 31; int bits = (op & 1) ? ((b[0] == 0x66) ? 16 : 32) : 8;
            if (!n) return all;
            uint32_t m = all; if (grp >= 4) m &= ~0x10u; if (n != 1) m &= ~0x800u;
            if (grp < 4 && (int)n >= bits && bits < 32) m &= ~0x801u;      // rotate by >= width: partially undefined on some CPUs
            if (grp >= 4 && (int)n > bits) m &= ~0x1u;
            return m; }
        if (op == 0xf6 || op == 0xf7) { if (grp == 4 || grp == 5) return 0x801; if (grp >= 6) return 0; }
        if (op == 0x69 || op == 0x6b) return 0x801;
        if (op == 0x9d) return 0;
    } else {
        if (op == 0xaf) return 0x801;
        if (op == 0xa3 || op == 0xab || op == 0xb3 || op == 0xbb) return 0x1;
        if (op == 0xba) return 0x1;
        if (op == 0xbc || op == 0xbd) return 0x40;
        if (op == 0xa4 || op == 0xa5 || op == 0xac || op == 0xad) { uint32_t n = (op & 1) ? (t->r[1] & 31) : (b[i + 2 + mlen] & 31); uint32_t m = all & ~0x10u; if (n != 1) m &= ~0x800u; if (n == 0) return all; if ((b[0] == 0x66) && n > 16) return 0; return m; }
    }
    return all;
}
static int is_fp_cmp(const uint8_t *code) {      // NaN payloads / sign are not compared for these (lifted code uses host arithmetic too)
    int i = 0; while (code[i] == 0x66 || code[i] == 0xf2 || code[i] == 0xf3 || code[i] == 0x64 || code[i] == 0xf0) i++;
    if (code[i] != 0x0f) return 0; int op = code[i + 1];
    return (op >= 0x10 && op <= 0x17) || (op >= 0x28 && op <= 0x2f) || (op >= 0x50 && op <= 0x5f) || op == 0xc2 || op == 0xc6 || op == 0xe6;
}
static void report(const TS *t, const char *what, const char *detail) {
    if (getenv("DT_DUMP")) { fprintf(stderr, "MISMATCH %s: %s\n", what, detail); for (int k = 0; k < 8; k++) { fprintf(stderr, "  xmm%d:", k); for (int j = 0; j < 4; j++) { uint32_t v; memcpy(&v, t->xmm[k] + 4 * j, 4); fprintf(stderr, " %08x", v); } fprintf(stderr, "\n"); }
        fprintf(stderr, "  st:"); for (int k = 0; k < 8; k++) fprintf(stderr, " %.17g", t->st[(t->top + k) & 7]); fprintf(stderr, "\n  fcw=%04x\n", t->fcw);
        for (int k = 0; k < 8; k++) if (t->r[k] >= SCR_BASE && t->r[k] < SCR_BASE + SCR_SIZE - 8) { fprintf(stderr, "  [r%d]=", k); for (int q = 0; q < 8; q++) fprintf(stderr, "%02x", t->scr[t->r[k] - SCR_BASE + q]); fprintf(stderr, "\n"); }
        fprintf(stderr, "  regs:"); for (int k = 0; k < 8; k++) fprintf(stderr, " %08x", t->r[k]); fprintf(stderr, "\n"); }
    char key[48], bytes[80], k2[96]; opkey(t->code, key); hexs(bytes, t->code, 12);
    snprintf(k2, sizeof k2, "%s %s", key, what);
    char d[400]; snprintf(d, sizeof d, "%s : %s", bytes, detail); note(k2, d);
}
static int compare(const TS *t, RS *n, RS *u, int use_mm, uint32_t exp_eip) {
    char d[300]; int bad = 0;
    if (n->eip != exp_eip && u->eip == exp_eip) { snprintf(d, sizeof d, "eip native=%08x ref=%08x", n->eip, u->eip); report(t, "eip", d); bad = 1; }
    else if (n->eip != u->eip) { snprintf(d, sizeof d, "eip native=%08x ref=%08x", n->eip, u->eip); report(t, "eip", d); bad = 1; }
    static const char *nm[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
    for (int i = 0; i < 8; i++) if (n->r[i] != u->r[i]) { snprintf(d, sizeof d, "%s native=%08x ref=%08x (init %08x)", nm[i], n->r[i], u->r[i], t->r[i]); report(t, nm[i], d); bad = 1; }
    uint32_t fm = flag_mask(t);
    if ((n->fl ^ u->fl) & fm) { snprintf(d, sizeof d, "eflags native=%03x ref=%03x diff=%03x mask=%03x (init %03x)", n->fl, u->fl, (n->fl ^ u->fl) & fm, fm, t->fl); report(t, "flags", d); bad = 1; }
    int fpc = is_fp_cmp(t->code);
    for (int k = 0; k < 8; k++) for (int j = 0; j < 4; j++) {
        uint32_t a, b; memcpy(&a, n->xmm[k] + 4 * j, 4); memcpy(&b, u->xmm[k] + 4 * j, 4);
        if (a != b) { float fa, fb; memcpy(&fa, &a, 4); memcpy(&fb, &b, 4); if (fpc && fa != fa && fb != fb) continue;
            if (fpc) { double da, db; uint64_t qa, qb; memcpy(&qa, n->xmm[k] + 8 * (j / 2), 8); memcpy(&qb, u->xmm[k] + 8 * (j / 2), 8); memcpy(&da, &qa, 8); memcpy(&db, &qb, 8); if (da != da && db != db) continue; }
            snprintf(d, sizeof d, "xmm%d lane%d native=%08x ref=%08x", k, j, a, b); report(t, "xmm", d); bad = 1; k = 8; break; }
    }
    if (use_mm) { for (int k = 0; k < 8; k++) if (n->mm[k] != u->mm[k]) { snprintf(d, sizeof d, "mm%d native=%016llx ref=%016llx", k, (unsigned long long)n->mm[k], (unsigned long long)u->mm[k]); report(t, "mm", d); bad = 1; break; } }
    else {
        if (n->top != u->top) { snprintf(d, sizeof d, "top native=%d ref=%d", n->top, u->top); report(t, "x87top", d); bad = 1; }
        else { int kk = 8;
            for (int j = 0; j < kk && j < 8; j++) { double a = n->st[(n->top + j) & 7], b = u->st[(u->top + j) & 7]; if (!fp_close(a, b)) { snprintf(d, sizeof d, "st(%d) native=%.17g ref=%.17g top=%d", j, a, b, n->top); report(t, "x87val", d); bad = 1; break; } }
            int opx = t->code[skip_pfx(t->code)], op2 = t->code[skip_pfx(t->code) + 1]; int fswskip = opx == 0xd9 && (op2 == 0xf5 || op2 == 0xf8 || op2 == 0xf1 || op2 == 0xf9 || op2 == 0xfa);
            if (!fswskip && ((n->fsw ^ u->fsw) & 0x4700)) { snprintf(d, sizeof d, "fsw native=%04x ref=%04x", n->fsw & 0x4700, u->fsw & 0x4700); report(t, "x87sw", d); bad = 1; } }
    }
    { int i0 = skip_pfx(t->code); if (t->code[i0] == 0x0f && t->code[i0 + 1] == 0xae && ((t->code[i0 + 2] >> 3) & 7) == 0 && (t->code[i0 + 2] >> 6) == 0 && (t->code[i0 + 2] & 7) < 4) {      // fxsave: reserved / pointer fields are not modelled
        uint32_t base = t->r[t->code[i0 + 2] & 7] - SCR_BASE; if (base + 512 <= SCR_SIZE) { for (int z = 0; z < 8; z++) { memset(n->scr + base + 32 + 16 * z + 10, 0, 6); memset(u->scr + base + 32 + 16 * z + 10, 0, 6); }
            memset(n->scr + base + 6, 0, 18); memset(u->scr + base + 6, 0, 18); memset(n->scr + base + 288, 0, 224); memset(u->scr + base + 288, 0, 224); } } }
    if (memcmp(n->scr, u->scr, SCR_SIZE)) { uint32_t q = 0; while (n->scr[q] == u->scr[q]) q++; snprintf(d, sizeof d, "scratch differs +%x native=%02x ref=%02x", q, n->scr[q], u->scr[q]);
        if (getenv("DT_DUMP")) { int cnt = 0; fprintf(stderr, "  diff offsets (abs scr):"); for (uint32_t z = 0; z < SCR_SIZE && cnt < 40; z++) if (n->scr[z] != u->scr[z]) { fprintf(stderr, " %x:%02x/%02x", z, n->scr[z], u->scr[z]); cnt++; } fprintf(stderr, "\n"); }
        report(t, "mem", d); bad = 1; }
    if (memcmp(n->stk, u->stk, STK_CMP)) { uint32_t q = 0; while (n->stk[q] == u->stk[q]) q++; snprintf(d, sizeof d, "stack differs +%x native=%02x ref=%02x", q, n->stk[q], u->stk[q]); report(t, "stack", d); bad = 1; }
    if (memcmp(n->teb, u->teb, 0x1000)) { report(t, "teb", "teb page differs"); bad = 1; }
    return bad;
}

// ---- encoding generators --------------------------------------------------------------------------------------------------------------
static int gen_fuzz(uint8_t *b) {
    int n = 0, r = rnd() % 100;
    if (r < 10) b[n++] = 0x66; else if (r < 18) b[n++] = 0xf3; else if (r < 26) b[n++] = 0xf2; else if (r < 31) b[n++] = 0x64; else if (r < 33) b[n++] = 0xf0;
    if (rnd() % 40 == 0) b[n++] = 0x66;
    int two = rnd() % 100 < 45; int op = rnd() & 0xff;
    int lk = b[0] == 0xf0 && n > 0;
    if (lk) { static const uint8_t L1[] = {0x00,0x01,0x08,0x09,0x10,0x11,0x18,0x19,0x20,0x21,0x28,0x29,0x30,0x31,0x86,0x87}, L2[] = {0xb0,0xb1,0xc0,0xc1,0xab,0xb3,0xbb}; two = rnd() % 3 == 0; op = two ? L2[rnd() % 7] : L1[rnd() % 16]; }
    if (two) { b[n++] = 0x0f; b[n++] = op; } else b[n++] = op;
    int mod = rnd() % 100; mod = mod < 25 ? 3 : mod < 55 ? 0 : mod < 85 ? 1 : 2; if (lk && mod == 3) mod = 1;
    int rm = rnd() % 8, reg = rnd() % 8;
    b[n++] = (mod << 6) | (reg << 3) | rm;
    if (mod != 3) {
        int base = rm;
        if (rm == 4) { int sc = rnd() % 4, ix = rnd() % 8, bs = rnd() % 8; if (mod == 0 && bs == 5) bs = rnd() % 5; b[n++] = (sc << 6) | (ix << 3) | bs; base = bs; if (mod == 0 && bs == 5) base = 5; }
        if (mod == 1) b[n++] = (int8_t)((int)(rnd() % 80) - 16);
        else if (mod == 2 || (mod == 0 && rm == 5) || (mod == 0 && rm == 4 && (b[n - 1] & 7) == 5)) { uint32_t d = (mod == 0) ? SCR_BASE + 0x2000 + rnd() % 0x4000 : (uint32_t)((int)(rnd() % 0x2000) - 0x100); memcpy(b + n, &d, 4); n += 4; }
        (void)base;
    }
    for (int i = 0; i < 10; i++) b[n + i] = rnd();
    return n + 10;
}
static uint8_t *samples; static long nsamples, samples_n; static long *sample_off;
static long forced_sample = -1;
static int gen_sample(uint8_t *b) {
    long k = forced_sample >= 0 ? forced_sample : (long)(rnd() % nsamples); uint8_t *p = samples + sample_off[k]; int len = p[0];
    memcpy(b, p + 1, len); for (int i = len; i < 20; i++) b[i] = rnd(); return len;
}

// Out-of-scope for user-mode guest code (privileged, segment-register loading, BCD adjust, port I/O, far control transfer ...): not fuzzed, listed in README "known gaps".
static int denied_code(const uint8_t *b) {
    int i = 0; while (b[i] == 0x66 || b[i] == 0xf2 || b[i] == 0xf3 || b[i] == 0x64 || b[i] == 0xf0 || b[i] == 0x2e || b[i] == 0x3e || b[i] == 0x26 || b[i] == 0x36 || b[i] == 0x65 || b[i] == 0x67) { if (b[i] == 0x65 || b[i] == 0x67) return 1; i++; }
    int o = b[i], o2 = b[i + 1];
    int has66 = 0; for (int q = 0; q < i; q++) if (b[q] == 0x66) has66 = 1;
    int modrm = b[i + (o == 0x0f ? 2 : 1)], mod = modrm >> 6, grp = (modrm >> 3) & 7;
    if (has66) {              // 16-bit control flow / stack forms are never used by 32-bit user code (and EIP truncation differs per CPU)
        if ((o >= 0x70 && o <= 0x7f) || (o >= 0xe0 && o <= 0xe3) || o == 0xe8 || o == 0xe9 || o == 0xeb || o == 0xc2 || o == 0xc3 || o == 0x60 || o == 0x61 || o == 0x9c || o == 0x9d || o == 0xc9 || o == 0xcf || o == 0x90 && 0) return 1;
        if (o == 0xff && (grp == 2 || grp == 4)) return 1;
        if (o == 0x0f && ((o2 >= 0x80 && o2 <= 0x8f) || (o2 >= 0xc8 && o2 <= 0xcf))) return 1;
        if (o >= 0x50 && o <= 0x5f && 0) return 1;
    }
    if (o == 0xc2 && b[i + 2] >= 0x80) return 1;             // QEMU sign-extends the imm16 of `ret imm16`
    if ((o == 0x8f && grp != 0) || ((o == 0xc6 || o == 0xc7) && grp != 0)) return 1;
    if (o == 0x0f && mod != 3 && (o2 == 0x50 || o2 == 0xd7 || o2 == 0xc5 || o2 == 0x71 || o2 == 0x72 || o2 == 0x73 || o2 == 0xf7 || o2 == 0x12 && 0)) return 1;   // register-only encodings that QEMU executes anyway
    if (o == 0x0f && (o2 == 0xd0 || o2 == 0x7c || o2 == 0x7d) ) return 1;                // SSE3
    { int nseg = 0; for (int q = 0; q < i; q++) if (b[q] == 0x64 || b[q] == 0x2e || b[q] == 0x3e || b[q] == 0x26 || b[q] == 0x36) nseg++; if (nseg > 1) return 1; }
    { int has_f = 0, has_fs = 0; for (int q = 0; q < i; q++) { if (b[q] == 0xf2 || b[q] == 0xf3) has_f = b[q]; if (b[q] == 0x64) has_fs = 1; }
      if (has_fs && (o == 0xd7 || (o >= 0xa4 && o <= 0xa7) || (o >= 0xaa && o <= 0xaf))) return 1;      // segment override on string/xlat sources is not modelled
      if (o == 0x8d && mod == 3) return 1;
      if (has_f == 0xf2 || has_f == 0xf3) { int c2 = 0; for (int q = 0; q < i; q++) if (b[q] == 0xf2 || b[q] == 0xf3) c2 |= b[q] == 0xf2 ? 1 : 2; if (c2 == 3) return 1; }
      if (o == 0xf0 || 0) {}
      { int lk = 0; for (int q = 0; q < i; q++) if (b[q] == 0xf0) lk = 1; if (lk && (o == 0xf6 || o == 0xf7)) return 1; }
      if (o >= 0xd8 && o <= 0xdf) {
          int rm = modrm & 7, mem = mod != 3;
          if (mem) { if ((o == 0xd9 && (grp == 1 || grp == 4 || grp == 6)) || (o == 0xdb && (grp == 4 || grp == 6)) || (o == 0xdd && (grp == 1 + 0 && 0)) || (o == 0xdd && (grp == 4 || grp == 6)) || (o == 0xdf && (grp == 4 || grp == 6))) return 1; }
          else switch (o) {
              case 0xd9: if (grp == 2 && rm != 0) return 1; if (grp == 3) return 1; if (grp == 4 && (rm == 2 || rm == 3 || rm >= 6)) return 1; if (grp == 5 && rm == 7) return 1; break;
              case 0xda: if (grp == 4 || grp == 6 || grp == 7) return 1; if (grp == 5 && rm != 1) return 1; break;
              case 0xdb: if (grp == 4 && rm != 2 && rm != 3) return 1; if (grp == 7) return 1; break;
              case 0xdc: if (grp == 2 || grp == 3) return 1; break;
              case 0xdd: if (grp == 1 || grp == 6 || grp == 7) return 1; break;
              case 0xde: if (grp == 2) return 1; if (grp == 3 && rm != 1) return 1; break;
              case 0xdf: if (grp >= 1 && grp <= 3) return 1; if (grp == 4 && rm != 0) return 1; if (grp == 7) return 1; break;
          }
      }
      if (o == 0x0f) {
          if (has_f && !(o2 == 0x10 || o2 == 0x11 || o2 == 0x12 || o2 == 0x16 || o2 == 0x2a || o2 == 0x2c || o2 == 0x2d || o2 == 0x51 || o2 == 0x58 || o2 == 0x59 || o2 == 0x5a || o2 == 0x5c || o2 == 0x5d || o2 == 0x5e || o2 == 0x5f
                         || o2 == 0x5b || o2 == 0x52 || o2 == 0x53 || o2 == 0x6f || o2 == 0x70 || o2 == 0x7e || o2 == 0x7f || o2 == 0xc2 || o2 == 0xd6 || o2 == 0xe6 || (o2 >= 0x80 && o2 <= 0x8f) || (o2 >= 0x90 && o2 <= 0x9f) || o2 == 0xaf
                         || (o2 >= 0x40 && o2 <= 0x4f) || o2 == 0xa3 || o2 == 0xab || o2 == 0xb3 || o2 == 0xbb || o2 == 0xb6 || o2 == 0xb7 || o2 == 0xbe || o2 == 0xbf || o2 == 0xa4 || o2 == 0xa5 || o2 == 0xac || o2 == 0xad || o2 == 0xb0 || o2 == 0xb1 || o2 == 0xc0 || o2 == 0xc1 || o2 == 0xba || o2 == 0xc7 || o2 == 0xc8)) return 1;
          if (has_f == 0xf3 && (o2 == 0xbc || o2 == 0xbd || o2 == 0xb8)) return 1;
          if (has_f && (o2 == 0xbc || o2 == 0xbd)) return 1;
          if (has_f == 0xf2 && (o2 == 0x7e || o2 == 0x6f || o2 == 0x5b || o2 == 0x52 || o2 == 0x53 || o2 == 0x10 && 0)) return 1;
          if (has_f == 0xf3 && (o2 == 0x70 && 0)) return 1;
          if (has_f && (o2 == 0x16 || o2 == 0x12)) return 1;                                   // movshdup/movsldup/movddup (SSE3)
          if (has_f && o2 >= 0x80 && o2 <= 0xbf && o2 != 0xaf && o2 != 0xb6 && o2 != 0xb7 && o2 != 0xbe && o2 != 0xbf) return 1;
          if ((has_f == 0xf2 || has_f == 0xf3) && ((o2 >= 0x40 && o2 <= 0x4f) || o2 == 0xaf)) return 1;
          if (o2 == 0xd6 && mod != 3 && has_f) return 1;
          if (o2 == 0xd6 && !has66 && !has_f) return 1;
          if (o2 == 0xc7 && grp != 1) return 1;
          if (o2 == 0x2b && has_f) return 1;
          if (o2 == 0xf0 && has_f != 0xf2) return 1;
          if (o2 == 0xf0) return 1;
          if (o2 == 0xba && grp < 4) return 1;
          if (o2 == 0xae && ((mod != 3 && (grp >= 4 || grp <= 1)) || (mod == 3 && grp < 5))) return 1;       // fxsave/fxrstor: dedicated runs (DT_ALLOW=1 DT_CODE=0fae00), see README
          if ((o2 == 0x71 || o2 == 0x72 || o2 == 0x73) && (grp == 0 || grp == 1 || grp == 5 || (o2 == 0x71 && grp != 2 && grp != 4 && grp != 6) || (o2 == 0x72 && grp != 2 && grp != 4 && grp != 6) || (o2 == 0x73 && grp != 2 && grp != 3 && grp != 6 && grp != 7))) return 1;
          if (has66 && has_f) return 1;
          if (has_fs && o2 == 0xf7) return 1;
      } }
    if (o != 0x0f) {
        switch (o) { case 0x06: case 0x07: case 0x0e: case 0x16: case 0x17: case 0x1e: case 0x1f: case 0x27: case 0x2f: case 0x37: case 0x3f: case 0x62: case 0x63: case 0x6c: case 0x6d: case 0x6e: case 0x6f:
            case 0x82: case 0x8e: case 0x9a: case 0xc4: case 0xc5: case 0xc8: case 0xca: case 0xcb: case 0xcc: case 0xcd: case 0xce: case 0xcf: case 0xd4: case 0xd5: case 0xd6:
            case 0xe4: case 0xe5: case 0xe6: case 0xe7: case 0xea: case 0xec: case 0xed: case 0xee: case 0xef: case 0xf1: case 0xf4: case 0xfa: case 0xfb: return 1; }
        if (o == 0x8c && ((b[i + 1] >> 3) & 7) >= 6) return 1;
        if ((o == 0xfe && ((b[i + 1] >> 3) & 7) > 1) || (o == 0xff && (((b[i + 1] >> 3) & 7) == 3 || ((b[i + 1] >> 3) & 7) == 5 || ((b[i + 1] >> 3) & 7) == 7))) return 1;
        if (o == 0x9b) return 1;
        if (o == 0x9d) return 1;                                             // popfd with TF/NT/IOPL/RF bits from random stack data aborts Unicorn; popfd is covered by unit_test.c
        if (o == 0x8c) return 1;                                             // mov r/m, sreg: selector values differ by environment
        return 0;
    }
    if (o2 < 0x10 && o2 != 0x0d && o2 != 0x18) return 1;
    if ((o2 >= 0x20 && o2 <= 0x27) || (o2 >= 0x30 && o2 <= 0x3f) || o2 == 0xa0 || o2 == 0xa1 || o2 == 0xa2 || o2 == 0xa8 || o2 == 0xa9 || o2 == 0xaa || o2 == 0xb2 || o2 == 0xb4 || o2 == 0xb5 || o2 == 0xb8 || o2 == 0xb9 || o2 == 0xff || (o2 >= 0x78 && o2 <= 0x7d) || (o2 >= 0xa6 && o2 <= 0xa7) || o2 == 0xbc + 0x100) return 1;
    if (o2 == 0x0f || o2 == 0x0e || o2 == 0x77 && 0) return 1;
    return 0;
}
static uint8_t ring[8][16]; static int ringn, consec_skip;
// per-opcode count of successful trials (key = prefix class + opcode [+ group field]), printed with DT_COV_FILE=<path>
typedef struct { char key[40]; int n; } Cov; static Cov cov[8192]; static int ncov;
static void cov_hit(const uint8_t *code) { char k[48]; opkey(code, k); for (int i = 0; i < ncov; i++) if (!strcmp(cov[i].key, k)) { cov[i].n++; return; } if (ncov < 8192) { snprintf(cov[ncov].key, sizeof cov[ncov].key, "%s", k); cov[ncov++].n = 1; } }
static int trials_denied;
static int trials_ok, trials_skip_ref, trials_skip_native, trials_bad;
static int one_trial(TS *t, int sample_mode, int mmx_hint) {
    static Cpu c; static RS nres, ures;
    int len;
    static int fixed = -1; static uint8_t fixed_code[32];
    if (fixed < 0) { const char *e = getenv("DT_CODE"); fixed = 0; if (e) { for (; e[0] && e[1]; e += 2) { unsigned v; sscanf(e, "%2x", &v); fixed_code[fixed++] = v; } } }
    if (fixed > 0) { memcpy(t->code, fixed_code, fixed); for (int q = fixed; q < 20; q++) t->code[q] = rnd(); len = fixed; }
    else len = sample_mode ? gen_sample(t->code) : gen_fuzz(t->code);
    memset(t->code + len + 4 > 32 ? t->code + 31 : t->code + 31, 0, 1);
    if (!getenv("DT_ALLOW") && denied_code(t->code)) { trials_denied++; return 0; }
    ts_random(t, mmx_hint); interp_flush_range(CODE_ADDR, CODE_ADDR + 32);
    { int i0 = skip_pfx(t->code); if (t->code[i0] == 0x0f && t->code[i0 + 1] == 0xae && (t->code[i0 + 2] >> 6) != 3) {          // fxsave/fxrstor/ldmxcsr/stmxcsr: 16-byte aligned operand (otherwise #GP, which leaves Unicorn unusable)
          int rm = t->code[i0 + 2] & 7; if (rm != 4 && ((t->code[i0 + 2] >> 6) == 0) && rm != 5) { t->r[rm] &= ~15u; if (t->r[rm] < SCR_BASE + 0x100 || t->r[rm] > SCR_BASE + SCR_SIZE - 0x400) t->r[rm] = SCR_BASE + 0x1000 + 16 * (rnd() % 0x400); } } }
    { int i = 0; int h66 = 0; while (t->code[i] == 0x66 || t->code[i] == 0xf2 || t->code[i] == 0xf3 || t->code[i] == 0x64 || t->code[i] == 0xf0 || t->code[i] == 0x2e || t->code[i] == 0x3e || t->code[i] == 0x26 || t->code[i] == 0x36) { if (t->code[i] == 0x66) h66 = 1; i++; }
      int o = t->code[i], o2 = t->code[i + 1];
      if (o >= 0xd8 && o <= 0xdf) { int mod = t->code[i + 1] >> 6, grp = (t->code[i + 1] >> 3) & 7; int rcop = (mod != 3 && ((o == 0xdb && (grp == 2 || grp == 3)) || (o == 0xdf && (grp == 2 || grp == 3 || grp == 7)) || (o == 0xdd && grp == 1) || (o == 0xdf && grp == 1) || (o == 0xdb && grp == 1))) || (o == 0xd9 && mod == 3 && o2 == 0xfc) || (o == 0xd9 && mod == 3 && o2 == 0xf8); if (!rcop) t->fcw = 0x27f; }
      if (o >= 0xd8 && o <= 0xdf) {      // reference-implementation (QEMU softfloat) divergences outside the architected domain: not tested
          double s0 = t->st[t->top & 7], s1 = t->st[(t->top + 1) & 7]; int mod = t->code[i + 1] >> 6, grp = (t->code[i + 1] >> 3) & 7;
          if (o == 0xd9 && mod == 3) {
              if (o2 == 0xf1 && !(s0 > 0 && s0 < 1e300)) return 0;
              if (o2 == 0xf4 && !(fabs(s0) > 1e-300 && fabs(s0) < 1e300)) return 0;
              if (o2 == 0xf9 && !(s0 > -0.29 && s0 < 0.29)) return 0;
              if ((o2 == 0xf2 || o2 == 0xfb || o2 == 0xfe || o2 == 0xff) && !(fabs(s0) < 9e18) && !(fabs(s0) >= 9.3e18 && fabs(s0) < 1e300)) return 0;   // beyond 2^63: C2 set, operand unchanged (checked); NaN / near the limit: skipped
              if (o2 == 0xf0 && !(fabs(s0) <= 1.0)) return 0;
              if (o2 == 0xfd && !(fabs(s1) < 1e4 && fabs(s0) < 1e300)) return 0;
              if ((o2 == 0xf5 || o2 == 0xf8) && !(fabs(s0) < 1e300 && fabs(s1) < 1e300 && s1 != 0 && fabs(s0 / s1) < 1000)) return 0;     // QEMU computes fprem/fprem1 with a floating-point quotient: exact only for small quotients (hardware, the lifter and the interpreter: exact)
          }
          if (mod != 3 && grp == 1 && (o == 0xdb || o == 0xdd || o == 0xdf)) { double lim = o == 0xdb ? 2147483648.0 : o == 0xdd ? 9223372036854775808.0 : 32768.0; if (!(fabs(trunc(s0)) < lim)) return 0; }
      }
      if (o == 0x0f && (o2 == 0xa3 || o2 == 0xab || o2 == 0xb3 || o2 == 0xbb) && (t->code[i + 2] >> 6) != 3) {       // bit string far outside the operand: the effective address wraps; QEMU's locked variants disagree there
          int32_t off = (int32_t)t->r[(t->code[i + 2] >> 3) & 7]; if (h66) off = (int16_t)off; if (off > (1 << 24) || off < -(1 << 24)) return 0; }
      if (h66 && o == 0x0f && (o2 == 0xa4 || o2 == 0xa5 || o2 == 0xac || o2 == 0xad)) { int n; if (o2 & 1) n = t->r[1] & 31; else { int m = t->code[i + 2], mod = m >> 6, rm = m & 7, ml = 1; if (mod != 3) { if (rm == 4) ml++; if (mod == 1) ml += 1; else if (mod == 2) ml += 4; else if (mod == 0 && (rm == 5 || (rm == 4 && (t->code[i + 3] & 7) == 5))) ml += 4; } n = t->code[i + 2 + ml] & 31; } if (n > 16) return 0; } }
    // MMX register instructions (0F 60-7F, D0-FF without 66) use mm regs; x87 ones use the stack: choose by opcode
    { int i = skip_pfx(t->code);
      int mm = t->code[i] == 0x0f && ((t->code[i + 1] >= 0x60 && t->code[i + 1] <= 0x7f) || t->code[i + 1] >= 0xd0 || t->code[i + 1] == 0x2a || t->code[i + 1] == 0x2d || t->code[i + 1] == 0x2c || t->code[i + 1] == 0xc4 || t->code[i + 1] == 0xc5 || t->code[i + 1] == 0xd7 || t->code[i + 1] == 0xe7)
               && !(t->code[i + 1] >= 0xd0 && 0);
      t->use_mm = mm; }
    int rep = 0; { int i = 0; while (t->code[i] == 0x66 || t->code[i] == 0xf2 || t->code[i] == 0xf3 || t->code[i] == 0x64 || t->code[i] == 0xf0 || t->code[i] == 0x2e || t->code[i] == 0x3e || t->code[i] == 0x26 || t->code[i] == 0x36) { if (t->code[i] == 0xf2 || t->code[i] == 0xf3) rep = 1; i++; } int o = t->code[i]; if (!((o >= 0xa4 && o <= 0xa7) || (o >= 0xaa && o <= 0xaf))) rep = 0; }
    if (rep) t->r[1] = rnd() % 40;
    memcpy(ring[ringn++ & 7], t->code, 16);
    if (getenv("DT_VERBOSE")) { char hb[80]; hexs(hb, t->code, 16); fprintf(stderr, "T %s\n", hb); }
    // native side first (the length of rep-string instructions is needed to stop the reference)
    ts_apply_native(t, &c);
    int jr = sigsetjmp(jb, 1);
    if (jr) {
        char key[48], k2[120], d[200]; opkey(t->code, key); hexs(d, t->code, 12);
        // only an issue when the reference can execute it
        ts_apply_uc(t); uc_err e2 = uc_emu_start(uc, CODE_ADDR, 0xdead0000u, 0, 1);
        { struct itimerval z = {{0, 0}, {0, 0}}; setitimer(ITIMER_REAL, &z, NULL); }
        { uint32_t ee = 0; uc_reg_read(uc, UC_X86_REG_EIP, &ee); if (e2 == UC_ERR_OK && ee == CODE_ADDR) { uc_rebuild(); e2 = UC_ERR_EXCEPTION; } }
        if (e2 != UC_ERR_OK) { trials_skip_ref++; interp_flush(); return 0; }
        if (jr == 3) snprintf(k2, sizeof k2, "%s UNIMPL(%s)", key, uns_what); else snprintf(k2, sizeof k2, "%s HOSTFAULT(%d)", key, jr);
        if (getenv("DT_DUMP")) { uint32_t ee = 0; uc_reg_read(uc, UC_X86_REG_EIP, &ee); fprintf(stderr, "e2=%d eip=%08x uns=%s at %08x\n", e2, ee, uns_what, uns_addr); report(t, "unimpl", d); }
        note(k2, d); trials_bad++; interp_flush(); return 1;
    }
    { struct itimerval tv = {{0, 0}, {0, 200000}}; setitimer(ITIMER_REAL, &tv, NULL); }
    if (rep) { int32_t guard = 200; while (guard-- > 0) { interp_run_n(&c, 1); if (c.pc != CODE_ADDR) break; } }
    else interp_run_n(&c, 1);
    { struct itimerval z = {{0, 0}, {0, 0}}; setitimer(ITIMER_REAL, &z, NULL); }
    rs_from_native(&nres, &c);
    if (getenv("DT_VERBOSE")) fprintf(stderr, " native done eip=%08x\n", nres.eip);
    ts_apply_uc(t);
    uc_err e;
    if (rep) { if (nres.eip <= CODE_ADDR || nres.eip > CODE_ADDR + 16) { trials_skip_ref++; return 0; } e = uc_emu_start(uc, CODE_ADDR, nres.eip, 100000, 0); }
    else e = uc_emu_start(uc, CODE_ADDR, 0xdead0000u, 0, 1);
    if (e != UC_ERR_OK) { trials_skip_ref++; if (++consec_skip == 200) { if (getenv("DT_VERBOSE")) { fprintf(stderr, "Unicorn stuck? last code:\n"); for (int q = 8; q >= 1; q--) { char hb[40]; hexs(hb, ring[(ringn - q) & 7], 12); fprintf(stderr, "  %s\n", hb); } } uc_rebuild(); consec_skip = 0; } return 0; }
    consec_skip = 0;
    if (getenv("DT_VERBOSE")) { uint32_t ee = 0; uc_reg_read(uc, UC_X86_REG_EIP, &ee); fprintf(stderr, " uc done e=%d eip=%08x\n", e, ee); }
    rs_from_uc(&ures, t->use_mm);
    if (!t->use_mm && (ures.fsw & 0x41)) { trials_skip_ref++; return 0; }          // x87 stack over/underflow in the reference: operands were not valid
    if (ures.eip == CODE_ADDR && nres.eip != CODE_ADDR) {            // Unicorn did not execute anything: a previous instruction left it halted
        static int shown; if (shown++ < 5) { fprintf(stderr, "Unicorn did not run; previous code:\n"); for (int q = 4; q >= 1; q--) { char hb[40]; hexs(hb, ring[(ringn - q) & 7], 12); fprintf(stderr, "  %s\n", hb); } }
        uc_rebuild(); trials_skip_ref++; return 0; }
    int bad = compare(t, &nres, &ures, t->use_mm, CODE_ADDR + len);
    if (bad) trials_bad++; else { trials_ok++; cov_hit(t->code); }
    return bad;
}

static int cmp_cat(const void *a, const void *b) { return ((const Cat *)b)->n - ((const Cat *)a)->n; }
static void summary(void) {
    qsort(cats, ncat, sizeof(Cat), cmp_cat);
    printf("ok=%d bad=%d skipped(ref fault)=%d  categories=%d  opcode-keys-with-ok-trials=%d\n", trials_ok, trials_bad, trials_skip_ref, ncat, ncov);
    if (getenv("DT_COV_FILE")) { FILE *cf = fopen(getenv("DT_COV_FILE"), "w"); for (int i = 0; i < ncov; i++) fprintf(cf, "%s %d\n", cov[i].key, cov[i].n); fclose(cf); }
    for (int i = 0; i < ncat; i++) printf("  %6d  %-34s %s\n", cats[i].n, cats[i].key, cats[i].first);
}

// ---- function corpus ----------------------------------------------------------------------------------------------------------------
// QEMU (the Unicorn reference) copies the payload of a signalling NaN through fld/fstp m32 unchanged; real x87 hardware (and the host conversion used by
// the lifter and the interpreter) set the quiet bit.  A difference that is exactly "quiet bit set in native, clear in reference" inside a float NaN is that quirk.
static int trials_snan;
static int snan_only_diff(const uint8_t *n, const uint8_t *u, uint32_t len) {
    for (uint32_t z = 0; z < len; z++) if (n[z] != u[z]) {
        if ((n[z] ^ u[z]) != 0x40 || !(n[z] & 0x40)) return 0;
        int ok = 0; for (int al = 0; al < 4 && !ok; al++) { int base = (int)z - 2 + 0; int w = base - 0; (void)al; int b2 = z, b3 = z + 1; if (b3 < len && (u[b3] & 0x7f) == 0x7f && (u[b2] & 0x80)) ok = 1; }
        if (!ok) return 0; }
    return 1;
}
// Default-NaN sign: x86 produces the negative QNaN for invalid operations, the host (arm64, wasm) the positive one.  Same for the lifted code. Exempt a
// difference that is exactly the sign bit of a float/double NaN.
static int trials_skip_data, trials_nansign, trials_snan_taint; static double t_interp;
static int nan_sign_only_diff(const uint8_t *n, const uint8_t *u, uint32_t len) {       // every differing byte lies inside a float or double that is a NaN on both sides
    for (uint32_t z = 0; z < len; z++) if (n[z] != u[z]) {
        int ok = 0;
        for (int k = 0; k < 4 && !ok; k++) { int64_t s = (int64_t)z - k; if (s < 0 || s + 4 > len) continue; uint32_t a, b; memcpy(&a, n + s, 4); memcpy(&b, u + s, 4); ok = ((a & 0x7fffffff) > 0x7f800000) && ((b & 0x7fffffff) > 0x7f800000); }
        for (int k = 0; k < 8 && !ok; k++) { int64_t s = (int64_t)z - k; if (s < 0 || s + 8 > len) continue; uint64_t a, b; memcpy(&a, n + s, 8); memcpy(&b, u + s, 8); ok = ((a & 0x7fffffffffffffffull) > 0x7ff0000000000000ull) && ((b & 0x7fffffffffffffffull) > 0x7ff0000000000000ull); }
        if (!ok) return 0; }
    return 1;
}
#ifdef MIXED
static int is_native_cb(uint32_t pc) { return pc == RET_MAGIC || hot_lookup(pc) != NULL; }
#else
static int is_native_cb(uint32_t pc) { return pc == RET_MAGIC; }
#endif
// Debug aid (DT_LOCKSTEP=1): replay a mismatching function trial one instruction at a time on both sides and report the first divergence.
static int lockstep(uint32_t fn, TS *t, int quiet) {
    static Cpu c; ts_apply_uc(t); { uint16_t sw = 0, tag = 0xffff; uc_reg_write(uc, UC_X86_REG_FPSW, &sw); uc_reg_write(uc, UC_X86_REG_FPTAG, &tag); }
    ts_apply_native(t, &c); c.pc = fn; memcpy(G + STK_TOP - STK_SIZE, t->stk, STK_SIZE); c.budget = 1 << 30; interp_flush();
    uint32_t upc = fn;
    for (int step = 0; step < 3000000; step++) {
        uint32_t pc0 = c.pc; uint8_t ib[16]; memcpy(ib, G + pc0, 16);
        if (pc0 == RET_MAGIC) { if (!quiet) fprintf(stderr, "  lockstep: reached the return with no divergence\n"); return 0; }
        uc_err e = uc_emu_start(uc, upc, 0xdead0000u, 0, 1); uc_reg_read(uc, UC_X86_REG_EIP, &upc);
        if (e != UC_ERR_OK) { if (!quiet) fprintf(stderr, "  lockstep: reference faulted at step %d pc %08x\n", step, pc0); return 0; }
        int jr = sigsetjmp(jb, 1); if (jr) { if (!quiet) fprintf(stderr, "  lockstep: interpreter trap '%s' at %08x\n", uns_what ? uns_what : "?", pc0); return 1; }
        interp_run_n(&c, 1);
        RS n, u; rs_from_native(&n, &c); rs_from_uc(&u, 0);
        uint32_t uefl = u.fl & 0x8c5, nefl = n.fl & 0x8c5;
        int bad = (c.pc != upc) || memcmp(n.r, u.r, 32) || nefl != uefl;
        if (!bad && memcmp(n.scr, u.scr, SCR_SIZE)) { uint32_t q = 0; while (n.scr[q] == u.scr[q]) q++;
            if (n.scr[q] == (u.scr[q] | 0x40) && q + 1 < SCR_SIZE && (u.scr[q + 1] & 0x7f) == 0x7f && (u.scr[q] & 0x80) && !memcmp(n.scr + q + 1, u.scr + q + 1, 8)) return 2;          // float: quiet bit set by the host, not by QEMU
            if (n.scr[q] == (u.scr[q] | 0x08) && q + 1 < SCR_SIZE && (u.scr[q + 1] & 0x7f) == 0x7f && (u.scr[q] & 0xf0) == 0xf0 && !memcmp(n.scr + q + 1, u.scr + q + 1, 8)) return 2;   // double
            if (quiet) return 1;
            char hb[40]; hexs(hb, ib, 12); fprintf(stderr, "  lockstep: memory divergence after step %d, instruction at %08x: %s : scratch+%x native=%02x ref=%02x\n", step, pc0, hb, q, n.scr[q], u.scr[q]); for (int i = 0; i < 8; i++) fprintf(stderr, "    pre-state reg%d=%08x\n", i, ((uint32_t *)&c)[i]); for (int k = 0; k < 2; k++) { fprintf(stderr, "    xmm%d:", k); for (int j = 0; j < 4; j++) { uint32_t a; memcpy(&a, n.xmm[k] + 4 * j, 4); fprintf(stderr, " %08x", a); } fprintf(stderr, "\n"); } return 1; }
        for (int k = 0; k < 8 && !bad; k++) for (int j = 0; j < 4; j++) { uint32_t a, b; memcpy(&a, n.xmm[k] + 4 * j, 4); memcpy(&b, u.xmm[k] + 4 * j, 4); if (a != b && !(((a & 0x7fffffff) > 0x7f800000) && ((b & 0x7fffffff) > 0x7f800000))) { bad = 1; break; } }
        if (bad) {
            if (quiet) return 1;
            char hb[40]; hexs(hb, ib, 12); fprintf(stderr, "  lockstep: first divergence after step %d, instruction at %08x: %s\n", step, pc0, hb);
            fprintf(stderr, "    pc native=%08x ref=%08x  eflags native=%03x ref=%03x\n", c.pc, upc, nefl, uefl);
            static const char *nm[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
            for (int i = 0; i < 8; i++) if (n.r[i] != u.r[i]) fprintf(stderr, "    %s native=%08x ref=%08x\n", nm[i], n.r[i], u.r[i]);
            for (int k = 0; k < 8; k++) if (memcmp(n.xmm[k], u.xmm[k], 16)) { fprintf(stderr, "    xmm%d native=", k); for (int j = 0; j < 4; j++) { uint32_t a; memcpy(&a, n.xmm[k] + 4 * j, 4); fprintf(stderr, "%08x ", a); } fprintf(stderr, " ref="); for (int j = 0; j < 4; j++) { uint32_t b; memcpy(&b, u.xmm[k] + 4 * j, 4); fprintf(stderr, "%08x ", b); } fprintf(stderr, "\n"); }
            return 1; }
    }
    return 0;
}
static int func_trial(uint32_t fn, TS *t) {
    static Cpu c; static RS nres, ures;
    ts_random(t, 0); t->fcw = 0x27f; t->r[4] = STK_TOP - 0x2000 + 0x400;
    // Random bit patterns would make float NaNs/infinities common; their sign and payload follow the host (see README), and unaligned guest code that reinterprets such bytes
    // turns that into numeric differences.  Keep the function-level inputs free of float NaN/Inf (invalid operations can still produce them; those are compared tolerantly).
    for (uint32_t z = 0; z + 3 < SCR_SIZE; z++) if ((t->scr[z + 3] & 0x7f) == 0x7f && (t->scr[z + 2] & 0x80)) t->scr[z + 2] &= 0x7f;       // no float with an all-ones exponent at any alignment
    for (int k = 0; k < 8; k++) for (int j = 0; j < 4; j++) { uint32_t w; memcpy(&w, t->xmm[k] + 4 * j, 4); if ((w & 0x7f800000u) == 0x7f800000u) { w ^= 0x40000000u; memcpy(t->xmm[k] + 4 * j, &w, 4); } } t->r[4] &= ~3u; t->fl = 0x202;
    memcpy(t->stk + (STK_SIZE - 0x2000 + 0x400) - 0 + 0, &(uint32_t){RET_MAGIC}, 0);   // return address written below
    uint32_t esp = t->r[4]; uint32_t off = esp - (STK_TOP - STK_SIZE); memcpy(t->stk + off, &(uint32_t){RET_MAGIC}, 4);
    t->top = 0; for (int i = 0; i < 8; i++) t->st[i] = 0;
    // unicorn
    ts_apply_uc(t); { uint16_t sw = 0, tag = 0xffff; uc_reg_write(uc, UC_X86_REG_FPSW, &sw); uc_reg_write(uc, UC_X86_REG_FPTAG, &tag); }
    uc_err e = uc_emu_start(uc, fn, RET_MAGIC, 0, 2000000);
    uint32_t ueip = 0; uc_reg_read(uc, UC_X86_REG_EIP, &ueip);
    if (e != UC_ERR_OK || ueip != RET_MAGIC) { trials_skip_ref++; uc_probe(); return 0; }
    rs_from_uc(&ures, 1);
    ts_apply_native(t, &c); c.pc = fn; memcpy(G + STK_TOP - STK_SIZE, t->stk, STK_SIZE);
    note_defer = 0;
    int jr = sigsetjmp(jb, 1);
    if (jr == 3 && ((uns_addr >= SCR_BASE && uns_addr < SCR_BASE + SCR_SIZE) || (uns_addr >= STK_TOP - STK_SIZE && uns_addr < STK_TOP + 0x1000))) { trials_skip_data++; interp_flush(); return 0; }       // the random state sent the function into data; a trap there is expected
    if (jr) { char k2[100], d[200]; snprintf(k2, sizeof k2, "f_%08x %s", fn, jr == 3 ? uns_what : "HOSTFAULT"); snprintf(d, sizeof d, "uns at %08x sig=%d", uns_addr, jr); note(k2, d); trials_bad++; interp_flush(); return 1; }
    { struct itimerval tv = {{0, 0}, {5, 0}}; setitimer(ITIMER_REAL, &tv, NULL); }
    if (getenv("DT_NOCACHE")) interp_flush();                       // throughput including instruction decoding (worst case: code executed once)
    int r = INTERP_BUDGET; int32_t total = 0; struct timespec ts0, ts1; clock_gettime(CLOCK_MONOTONIC, &ts0);
#ifdef MIXED
    c.budget = 1 << 30; rt_dispatch(&c); r = INTERP_NATIVE;
#else
    while (r == INTERP_BUDGET && total < 4000000) { r = interp_run_n(&c, 100000); total += 100000; }
#endif
    clock_gettime(CLOCK_MONOTONIC, &ts1); t_interp += (ts1.tv_sec - ts0.tv_sec) + (ts1.tv_nsec - ts0.tv_nsec) * 1e-9;
    { struct itimerval z = {{0, 0}, {0, 0}}; setitimer(ITIMER_REAL, &z, NULL); }
    if (r != INTERP_NATIVE) { trials_skip_native++; return 0; }
    rs_from_native(&nres, &c);
    char d[300]; int bad = 0; char k2[100]; note_defer = 1; npend = 0;
    static const char *nm[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
    snprintf(k2, sizeof k2, "f_%08x", fn);
    for (int i = 0; i < 8; i++) if (nres.r[i] != ures.r[i]) { snprintf(d, sizeof d, "%s native=%08x ref=%08x", nm[i], nres.r[i], ures.r[i]); char kk[120]; snprintf(kk, sizeof kk, "%s %s", k2, nm[i]); note(kk, d); bad = 1; }
    uint32_t fm = 0x8d5; if ((nres.fl ^ ures.fl) & fm & 0) { bad = 1; }
    if (memcmp(nres.stk, ures.stk, STK_CMP) && nan_sign_only_diff(nres.stk, ures.stk, STK_CMP)) memcpy(nres.stk, ures.stk, STK_CMP);
    if (memcmp(nres.scr, ures.scr, SCR_SIZE) && nan_sign_only_diff(nres.scr, ures.scr, SCR_SIZE)) { trials_nansign++; memcpy(nres.scr, ures.scr, SCR_SIZE); }
    if (memcmp(nres.scr, ures.scr, SCR_SIZE) && snan_only_diff(nres.scr, ures.scr, SCR_SIZE)) { static int shown; trials_snan++; (void)shown; memcpy(nres.scr, ures.scr, SCR_SIZE); }
    if (memcmp(nres.scr, ures.scr, SCR_SIZE)) { uint32_t q = 0; while (nres.scr[q] == ures.scr[q]) q++; snprintf(d, sizeof d, "scratch differs +%x", q);
        if (getenv("DT_DUMP")) { int cnt = 0; fprintf(stderr, "func %08x mismatch; args:", fn); for (int z = 0; z < 6; z++) { uint32_t v; memcpy(&v, t->stk + off + 4 * z, 4); fprintf(stderr, " %08x", v); } fprintf(stderr, " regs:"); for (int z = 0; z < 8; z++) fprintf(stderr, " %08x", t->r[z]); fprintf(stderr, "\n  diffs:"); for (uint32_t z = 0; z < SCR_SIZE && cnt < 16; z++) if (nres.scr[z] != ures.scr[z]) { fprintf(stderr, " %x:%02x/%02x(init %02x)", z, nres.scr[z], ures.scr[z], t->scr[z]); cnt++; } fprintf(stderr, "\n"); } char kk[120]; snprintf(kk, sizeof kk, "%s mem", k2); note(kk, d); bad = 1; }
    if (memcmp(nres.stk, ures.stk, STK_CMP)) { snprintf(d, sizeof d, "stack differs");
        if (getenv("DT_DUMP")) { int cnt = 0; fprintf(stderr, "func %08x stack mismatch; args:", fn); for (int z = 0; z < 6; z++) { uint32_t v; memcpy(&v, t->stk + off + 4 * z, 4); fprintf(stderr, " %08x", v); } fprintf(stderr, "\n  diffs (offset from stack top-0x2000):"); for (uint32_t z = 0; z < STK_CMP && cnt < 16; z++) if (nres.stk[z] != ures.stk[z]) { fprintf(stderr, " %x:%02x/%02x", z, nres.stk[z], ures.stk[z]); cnt++; } fprintf(stderr, "\n"); } char kk[120]; snprintf(kk, sizeof kk, "%s stack", k2); note(kk, d); bad = 1; }
    for (int k = 0; k < 8 && !bad; k++) for (int j = 0; j < 4; j++) { uint32_t a, b; memcpy(&a, nres.xmm[k] + 4 * j, 4); memcpy(&b, ures.xmm[k] + 4 * j, 4); float fa, fb; memcpy(&fa, &a, 4); memcpy(&fb, &b, 4);
        if (a != b && !(fa != fa && fb != fb)) { double da, db; memcpy(&da, nres.xmm[k] + 8 * (j / 2), 8); memcpy(&db, ures.xmm[k] + 8 * (j / 2), 8); if (da != da && db != db) continue; snprintf(d, sizeof d, "xmm%d lane%d native=%08x ref=%08x", k, j, a, b); char kk[120]; snprintf(kk, sizeof kk, "%s xmm", k2); note(kk, d); bad = 1; break; } }
    if (!bad && nres.top == ures.top) { int kk = (0 - nres.top) & 7; for (int j = 0; j < kk; j++) { double a = nres.st[(nres.top + j) & 7], b; uc_x86_float80 w; uc_reg_read(uc, UC_X86_REG_ST0 + j, &w); b = f802d(&w); if (!fp_close(a, b)) { snprintf(d, sizeof d, "st(%d) native=%.17g ref=%.17g", j, a, b); char k3[120]; snprintf(k3, sizeof k3, "%s x87val", k2); note(k3, d); bad = 1; break; } } }
    else if (!bad) { snprintf(d, sizeof d, "top native=%d ref=%d", nres.top, ures.top); char k3[120]; snprintf(k3, sizeof k3, "%s x87top", k2); note(k3, d); bad = 1; }
    if (bad) {      // a mismatch whose first divergence is a signalling-NaN quiet bit is the QEMU quirk (see README), not an interpreter bug
        int cls = lockstep(fn, t, 1);
        note_defer = 0;
        if (cls == 2) { trials_snan_taint++; bad = 0; }
        else { for (int q = 0; q < npend; q++) note(pend[q].key, pend[q].detail); if (getenv("DT_LOCKSTEP")) { static int shown; if (shown++ < 3) { fprintf(stderr, "LOCKSTEP for f_%08x\n", fn); lockstep(fn, t, 0); } } }
    }
    note_defer = 0;
    if (bad) trials_bad++; else trials_ok++;
    return bad;
}

// ---- instruction streams -----------------------------------------------------------------------------------------------------------
// difftest stream <image> <addrs.txt> <trials> [seed] [steps] [image2 base2]: from every start address run up to `steps` instructions one at a time in both worlds
// (random registers / memory), comparing registers, defined flags, xmm and the touched memory after every instruction.  Used for code that is not a clean function
// (protector handlers reached by ret-dispatch, jump-table targets ...).  Stops a trial when either side faults or the code leaves the image.
static uint32_t stream_starts[4096]; static int nstarts;
static long stream_steps_ok, stream_trials_ok, stream_trials_bad, stream_trials_skip;
static int stream_trial(uint32_t start, TS *t, int steps) {
    static Cpu c; static RS n, u;
    ts_random(t, 0); t->fcw = 0x27f; t->r[4] = STK_TOP - 0x2000 + 0x400; t->r[4] &= ~3u; t->fl = 0x202; t->top = 4;
    for (uint32_t z = 0; z + 3 < SCR_SIZE; z++) if ((t->scr[z + 3] & 0x7f) == 0x7f && (t->scr[z + 2] & 0x80)) t->scr[z + 2] &= 0x7f;
    uint32_t esp = t->r[4], off = esp - (STK_TOP - STK_SIZE); uint32_t magic = RET_MAGIC; memcpy(t->stk + off, &magic, 4);
    if (nstarts) for (int i = 1; i < 96; i++) { uint32_t w = rnd() % 10 < 7 ? stream_starts[rnd() % nstarts] : rand_val(); memcpy(t->stk + off + 4 * i, &w, 4); }       // ret-dispatch chains need code addresses on the stack
    ts_apply_uc(t); ts_apply_native(t, &c); c.pc = start; memcpy(G + STK_TOP - STK_SIZE, t->stk, STK_SIZE); c.budget = 1 << 30; interp_flush();
    uint32_t upc = start; int done = 0;
    for (int step = 0; step < steps; step++) {
        uint32_t pc0 = c.pc; if (pc0 == RET_MAGIC || pc0 != upc) break;
        uint8_t ib[16]; memcpy(ib, G + pc0, 16);
        uc_err e = uc_emu_start(uc, upc, 0xdead0000u, 0, 1); uc_reg_read(uc, UC_X86_REG_EIP, &upc);
        if (e != UC_ERR_OK) { static int shown; if (getenv("DT_STREAM_DEBUG") && shown++ < 30) { char hb[40]; hexs(hb, ib, 12); fprintf(stderr, "[stream %08x] step %d at %08x %s: reference error %d\n", start, step, pc0, hb, e); } uc_probe(); break; }
        uint32_t uefl0 = 0; (void)uefl0;
        TS ft; memset(&ft, 0, sizeof ft); memcpy(ft.code, ib, 16); memcpy(ft.r, &c.eax, 32);           // flag mask needs the instruction bytes and the pre-state registers
        { int skip = denied_code(ft.code); if (skip && !getenv("DT_STREAM_ALL")) { /* the reference executes it but it is out of scope: stop here */ break; } }
        int jr = sigsetjmp(jb, 1);
        if (jr) { char hb[40]; hexs(hb, ib, 12); char k2[100], d[200]; snprintf(k2, sizeof k2, "stream %08x %s", start, jr == 3 ? uns_what : "HOSTFAULT"); snprintf(d, sizeof d, "at %08x step %d: %s", pc0, step, hb); note(k2, d); stream_trials_bad++; interp_flush(); return 1; }
        interp_run_n(&c, 1);
        rs_from_native(&n, &c); rs_from_uc(&u, 0);
        uint32_t fm = flag_mask(&ft); int bad = c.pc != upc || memcmp(n.r, u.r, 32) || ((n.fl ^ u.fl) & fm);
        for (int k = 0; k < 8 && !bad; k++) for (int j = 0; j < 4; j++) { uint32_t a, b; memcpy(&a, n.xmm[k] + 4 * j, 4); memcpy(&b, u.xmm[k] + 4 * j, 4); if (a != b && !(((a & 0x7fffffff) > 0x7f800000) && ((b & 0x7fffffff) > 0x7f800000))) { bad = 1; break; } }
        if (!bad && (memcmp(n.scr, u.scr, SCR_SIZE) && !nan_sign_only_diff(n.scr, u.scr, SCR_SIZE))) bad = 2;
        if (!bad && memcmp(n.stk, u.stk, STK_CMP)) bad = 3;
        if (bad) {
            char hb[40], d[300], k2[100]; hexs(hb, ib, 12); snprintf(k2, sizeof k2, "stream %08x", start);
            snprintf(d, sizeof d, "step %d insn at %08x %s : pc native=%08x ref=%08x, mismatch kind %d", step, pc0, hb, c.pc, upc, bad); note(k2, d); stream_trials_bad++; return 1; }
        { uint32_t keep = ~fm & u.fl;      // flags the architecture leaves undefined here: continue with the reference's values so that they cannot cause false alarms later
          uint32_t cur = fl_eflags(&c.fl, c.df); cur = (cur & fm) | keep | 0x202; fl_load(&c.fl, cur, &c.df); }
        stream_steps_ok++; done++;
    }
    if (done) stream_trials_ok++; else stream_trials_skip++;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: difftest single|funcs <image> ...\n"); return 2; }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!strcmp(argv[1], "single")) {
        long trials = atol(argv[3]); if (argc > 4) rng ^= (uint64_t)atoll(argv[4]) * 0x9e3779b97f4a7c15ull; for (int i = 0; i < 10; i++) rnd();
        const char *sf = argc > 5 ? argv[5] : NULL;
        world_init(argv[2], NULL, 0); interp_init(NULL);
        if (sf) { FILE *f = fopen(sf, "rb"); fseek(f, 0, SEEK_END); samples_n = ftell(f); fseek(f, 0, SEEK_SET); samples = malloc(samples_n); if (fread(samples, 1, samples_n, f) != (size_t)samples_n) return 1; fclose(f);
            sample_off = malloc(sizeof(long) * (samples_n / 2 + 1)); for (long o = 0; o < samples_n; o += 1 + samples[o]) sample_off[nsamples++] = o; }
        static TS t; long att = 0; struct timeval t0; gettimeofday(&t0, NULL);
        if (getenv("DT_SAMPLE_EACH")) { int per = atoi(getenv("DT_SAMPLE_EACH")); FILE *of = fopen(getenv("DT_FORMS_OUT"), "w");        // per-sample outcome table for tools/forms_report.py
            for (long k = 0; k < nsamples; k++) { forced_sample = k; int ok0 = trials_ok, bad0 = trials_bad, den0 = trials_denied, sk0 = trials_skip_ref; for (int n = 0; n < per; n++) one_trial(&t, 1, 0);
                uint8_t *p = samples + sample_off[k]; char hb[64]; hexs(hb, p + 1, p[0]); fprintf(of, "%s %d %d %d %d\n", hb, trials_ok - ok0, trials_bad - bad0, trials_denied - den0, trials_skip_ref - sk0); }
            fclose(of); return 0; }
        while (trials_ok + trials_bad < trials && att < trials * 50) { att++; one_trial(&t, sf != NULL, 0); }
        struct timeval t1; gettimeofday(&t1, NULL); fprintf(stderr, "attempts=%ld time=%.1fs\n", att, (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6);
        summary(); return trials_bad != 0;
    }
    if (!strcmp(argv[1], "stream")) {
        FILE *lf = fopen(argv[3], "r"); int trials = atoi(argv[4]); if (argc > 5) rng ^= (uint64_t)atoll(argv[5]) * 0x9e3779b97f4a7c15ull; for (int i = 0; i < 10; i++) rnd();
        int steps = argc > 6 ? atoi(argv[6]) : 2000;
        world_init(argv[2], argc > 7 ? argv[7] : NULL, 0x78520000u); interp_init(is_native_cb);
        uint32_t a; static TS t; int na = 0;
        while (lf && fscanf(lf, "%x", &a) == 1 && nstarts < 4096) stream_starts[nstarts++] = a;
        for (int q = 0; q < nstarts; q++) { a = stream_starts[q]; na++; for (int k = 0; k < trials; k++) stream_trial(a, &t, steps); }
        printf("stream: %d start addresses, %ld trials with progress, %ld trials without, %ld mismatching trials, %ld instructions compared\n", na, stream_trials_ok, stream_trials_skip, stream_trials_bad, stream_steps_ok);
        summary(); return stream_trials_bad != 0;
    }
    if (!strcmp(argv[1], "funcs")) {
        FILE *lf = fopen(argv[3], "r"); int trials = atoi(argv[4]); if (argc > 5) rng ^= (uint64_t)atoll(argv[5]) * 0x9e3779b97f4a7c15ull; for (int i = 0; i < 10; i++) rnd();
        world_init(argv[2], argc > 6 ? argv[6] : NULL, 0x78520000u); interp_init(is_native_cb);
        uint32_t fn; static TS t; int nf = 0;
        while (lf && fscanf(lf, "%x", &fn) == 1) { nf++; int ok0 = trials_ok, sk0 = trials_skip_ref; for (int k = 0; k < trials; k++) func_trial(fn, &t); if (getenv("DT_VERBOSE")) fprintf(stderr, "f_%08x ok=%d skipref=%d\n", fn, trials_ok - ok0, trials_skip_ref - sk0); }
        printf("functions=%d trials ok=%d (incl. %d tainted by the QEMU sNaN-quieting quirk, %d with a NaN payload/sign difference) bad=%d skipped(ref fault/timeout)=%d skipped(interp budget)=%d skipped(traps in executed data)=%d\n", nf, trials_ok, trials_snan + trials_snan_taint, trials_nansign, trials_bad, trials_skip_ref, trials_skip_native, trials_skip_data);
#ifdef MIXED
        printf("mixed mode: %d lifted-function entries, %d interpreter runs\n", hot_calls, cold_runs);
#endif
        printf("interpreter-only time %.3fs for %llu guest instructions = %.1f Minsn/s (real game functions, random inputs)\n", t_interp, (unsigned long long)interp_icount, interp_icount / t_interp / 1e6);
        summary(); return trials_bad != 0;
    }
    return 2;
}
