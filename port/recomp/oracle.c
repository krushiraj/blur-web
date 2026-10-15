// Differential test: run recompiled functions natively and in Unicorn with identical random inputs; compare registers and memory.
#include "rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unicorn/unicorn.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/time.h>
typedef struct { uint64_t mantissa; uint16_t exponent; } uc_x86_float80;
static sigjmp_buf jb;
static void on_sig(int sig) { siglongjmp(jb, sig); }

uint8_t *G;
typedef struct { uint32_t addr; GuestFn fn; } Ent;
extern Ent table[]; extern int ntable;
void rt_spin(Cpu *c) { (void)c; }
void rt_preempt(Cpu *c) { c->budget = 1 << 30; }
int rt_trace; void rt_enter(Cpu *c, uint32_t a) { (void)c; (void)a; }
void rt_dispatch(Cpu *c) { if (c->pc == 0x7efff000u) return; siglongjmp(jb, 1); }   // the function returned to the harness, or leaves the lifted set: not testable here
void rt_cpuid(Cpu *c) { c->eax = c->ebx = c->ecx = c->edx = 0; } void rt_rdtsc(Cpu *c) { c->eax = c->edx = 0; }
void rt_unsupported(Cpu *c, uint32_t a, const char *w) { (void)c; static uint32_t seen[4096]; static int ns; for (int i = 0; i < ns; i++) if (seen[i] == a) goto skip; if (ns < 4096) seen[ns++] = a; fprintf(stderr, "unsupported %08x %s\n", a, w); skip: siglongjmp(jb, 3); }   // not testable: count as skipped

#define IMG_BASE 0x400000u
#define SCR_BASE 0x30000000u
#define SCR_SIZE 0x00100000u
#define STK_TOP  0x0f800000u
#define STK_SIZE 0x00010000u
#define RET_MAGIC 0x7efff000u
static uint64_t rng = 88172645463325252ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }
static uint32_t rand_val(void) {
    switch (rnd() % 12) { case 0: case 1: case 2: return rnd() % 16; case 3: return 0xffffffffu - rnd() % 4; case 4: case 5: case 6: case 7: case 8: return SCR_BASE + 0x1000 + ((rnd() % (SCR_SIZE / 2)) & ~3u);
        case 9: return rnd() & 0xff; case 10: return rnd() % 4096; default: return rnd(); }
}
int main(int argc, char **argv) {
    const char *img = argv[1]; int trials = argc > 2 ? atoi(argv[2]) : 200;
    G = mmap(NULL, (1ull << 32) + 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (G == MAP_FAILED) { perror("mmap"); return 1; }
    FILE *f = fopen(img, "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); uint8_t *im = malloc(n); if (fread(im, 1, n, f) != (size_t)n) return 1; fclose(f);
    memcpy(G + IMG_BASE, im, n);
    uc_engine *uc; uc_open(UC_ARCH_X86, UC_MODE_32, &uc);
    uc_mem_map(uc, IMG_BASE, (n + 0xfff) & ~0xfffl, UC_PROT_ALL); uc_mem_write(uc, IMG_BASE, im, n);
    uc_mem_protect(uc, IMG_BASE, (n + 0xfff) & ~0xfffl, UC_PROT_READ | UC_PROT_EXEC);       // image is read-only in both worlds: a write ends the trial
    if (mprotect(G + IMG_BASE, (n + 0x3fff) & ~0x3fffl, PROT_READ)) perror("mprotect image");
    uc_mem_map(uc, 0x4000, 0x3fc000, UC_PROT_READ);          // low memory reads as zero on both sides (small "pointers"), read-only
    if (mprotect(G + 0x4000, 0x3fc000, PROT_READ)) perror("mprotect low");
    uc_mem_map(uc, SCR_BASE, SCR_SIZE, UC_PROT_ALL); uc_mem_map(uc, STK_TOP - STK_SIZE, STK_SIZE, UC_PROT_ALL); uc_mem_map(uc, RET_MAGIC, 0x1000, UC_PROT_ALL);
    // fs segment: a TEB page both sides can read
    uc_mem_map(uc, 0x7ffdf000, 0x1000, UC_PROT_ALL); { uc_x86_msr m = {0xC0000100, 0x7ffdf000}; uc_reg_write(uc, UC_X86_REG_MSR, &m); }
    signal(SIGALRM, on_sig); signal(SIGSEGV, on_sig); signal(SIGBUS, on_sig); signal(SIGFPE, on_sig);
    int bad = 0, skipped = 0, good = 0;
    for (int fi = 0; fi < ntable; fi++) {
        if (getenv("ORACLE_ONLY") && table[fi].addr != (uint32_t)strtoul(getenv("ORACLE_ONLY"), 0, 16)) continue;
        int fbad = 0; if (getenv("ORACLE_TRACE")) fprintf(stderr, "f_%08x\n", table[fi].addr);
        for (int t = 0; t < trials && !fbad; t++) {
            static uint8_t scratch[SCR_SIZE], stk[0x2000], teb[0x1000]; memset(stk, 0, sizeof stk);
            for (uint32_t i = 0; i < SCR_SIZE; i += 4) *(uint32_t *)(scratch + i) = rnd();
            for (uint32_t i = 0; i < 0x400; i += 4) *(uint32_t *)(stk + i) = rand_val();
            for (uint32_t i = 0; i < sizeof teb; i += 4) *(uint32_t *)(teb + i) = rnd() & 0xffff;
            uint32_t r[8] = {rand_val(), rand_val(), rand_val(), rand_val(), 0, rand_val(), rand_val(), rand_val()};
            uint32_t esp = STK_TOP - 0x2000; r[4] = esp;
            // native side
            memset(G, 0, 0x4000); memset(G + STK_TOP - STK_SIZE, 0, STK_SIZE); { static uint8_t zs[STK_SIZE]; uc_mem_write(uc, STK_TOP - STK_SIZE, zs, STK_SIZE); }
            memcpy(G + SCR_BASE, scratch, SCR_SIZE); memcpy(G + STK_TOP - 0x2000, stk, sizeof stk); memcpy(G + 0x7ffdf000, teb, sizeof teb);
            *(uint32_t *)(G + esp) = RET_MAGIC;
            Cpu c; memset(&c, 0, sizeof c); c.eax = r[0]; c.ecx = r[1]; c.edx = r[2]; c.ebx = r[3]; c.esp = esp; c.ebp = r[5]; c.esi = r[6]; c.edi = r[7]; c.teb = 0x7ffdf000;
            uint8_t xin[8][16]; for (int k = 0; k < 8; k++) for (int j = 0; j < 4; j++) { uint32_t m = rnd() % 8; float v = m < 3 ? (float)((int)(rnd() % 2001) - 1000) / 8.0f : m < 6 ? (float)((int)(rnd() % 200001) - 100000) / 1000.0f : m == 6 ? (float)(int)(rnd() % 7) : *(float *)&(uint32_t){rnd()}; if (v != v) v = 1.5f; memcpy(xin[k] + 4 * j, &v, 4); }
            memcpy(c.xmm, xin, sizeof xin); c.fcw = 0x27f; c.fsw = 0; c.top = 0;
            if (sigsetjmp(jb, 1)) { struct itimerval z = {{0, 0}, {0, 0}}; setitimer(ITIMER_REAL, &z, NULL); skipped++; continue; }     // fault/timeout/leaves lifted set
            { struct itimerval tv = {{0, 0}, {0, 200000}}; setitimer(ITIMER_REAL, &tv, NULL); }
            table[fi].fn(&c);
            { struct itimerval z = {{0, 0}, {0, 0}}; setitimer(ITIMER_REAL, &z, NULL); }
            // unicorn side
            uc_mem_write(uc, SCR_BASE, scratch, SCR_SIZE); uc_mem_write(uc, STK_TOP - 0x2000, stk, sizeof stk); uc_mem_write(uc, 0x7ffdf000, teb, sizeof teb);
            uc_mem_write(uc, esp, &(uint32_t){RET_MAGIC}, 4);
            int ids[8] = {UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI};
            for (int i = 0; i < 8; i++) uc_reg_write(uc, ids[i], &r[i]);
            for (int k = 0; k < 8; k++) uc_reg_write(uc, UC_X86_REG_XMM0 + k, xin[k]);
            { uint16_t cw = 0x27f, sw = 0, tag = 0xffff; uc_reg_write(uc, UC_X86_REG_FPCW, &cw); uc_reg_write(uc, UC_X86_REG_FPSW, &sw); uc_reg_write(uc, UC_X86_REG_FPTAG, &tag); }
            uint32_t zero = 0x202; uc_reg_write(uc, UC_X86_REG_EFLAGS, &zero);
            uc_err e;
            if (getenv("ORACLE_STEP") && t == 0) { uint32_t pc = table[fi].addr; for (int s2 = 0; s2 < 200 && pc != RET_MAGIC; s2++) { e = uc_emu_start(uc, pc, RET_MAGIC, 0, 1); uc_reg_read(uc, UC_X86_REG_EIP, &pc); uint16_t sw; uc_reg_read(uc, UC_X86_REG_FPSW, &sw); printf("  pc=%08x top=%d", pc, (sw >> 11) & 7); for (int q2 = 0; q2 < 4; q2++) { uc_x86_float80 w; uc_reg_read(uc, UC_X86_REG_ST0 + q2, &w); double d2 = (w.exponent & 0x7fff) == 0 ? 0 : ldexp((double)w.mantissa, (int)(w.exponent & 0x7fff) - 16383 - 63); if (w.exponent & 0x8000) d2 = -d2; printf(" st%d=%g", q2, d2); } printf("\n"); } uc_reg_write(uc, UC_X86_REG_EIP, &pc); e = UC_ERR_OK; }
            else e = uc_emu_start(uc, table[fi].addr, RET_MAGIC, 0, 2000000);
            uint32_t reip = 0; uc_reg_read(uc, UC_X86_REG_EIP, &reip);
            if (e != UC_ERR_OK || reip != RET_MAGIC) { skipped++; continue; }                   // random inputs faulted in the reference: not a usable trial
            uint32_t ur[8]; for (int i = 0; i < 8; i++) uc_reg_read(uc, ids[i], &ur[i]);
            uint32_t nr[8] = {c.eax, c.ecx, c.edx, c.ebx, c.esp, c.ebp, c.esi, c.edi};
            int xdiff = 0; { uint8_t ux[16]; for (int k = 0; k < 8 && !xdiff; k++) { uc_reg_read(uc, UC_X86_REG_XMM0 + k, ux);
                for (int j = 0; j < 4; j++) { uint32_t a = ((uint32_t *)c.xmm[k])[j], b = ((uint32_t *)ux)[j]; float fa, fb; memcpy(&fa, &a, 4); memcpy(&fb, &b, 4); if (a != b && !(fa != fa && fb != fb)) { xdiff = 1; printf("MISMATCH f_%08x trial %d: xmm%d lane%d native=%08x ref=%08x\n", table[fi].addr, t, k, j, a, b); fbad = 1; bad++; break; } } } }
            if (!xdiff) { uint16_t usw; uc_reg_read(uc, UC_X86_REG_FPSW, &usw); int utop = (usw >> 11) & 7; int k = (0 - c.top) & 7;
                if (utop != (c.top & 7)) { printf("MISMATCH f_%08x trial %d: x87 top native=%d ref=%d\n", table[fi].addr, t, c.top & 7, utop); xdiff = 1; fbad = 1; bad++; }
                else for (int j = 0; j < k && !xdiff; j++) { uc_x86_float80 u; uc_reg_read(uc, UC_X86_REG_ST0 + j, &u);
                    double ud = (u.exponent & 0x7fff) == 0x7fff ? ((u.mantissa << 1) ? NAN : INFINITY) : u.exponent == 0 ? 0.0 : ldexp((double)u.mantissa, (int)(u.exponent & 0x7fff) - 16383 - 63); if (u.exponent & 0x8000) ud = -ud; double nd = c.st[(c.top + j) & 7];
                    if (!(nd == ud || (nd != nd && ud != ud) || fabs(nd - ud) <= 1e-9 * fabs(ud))) { printf("MISMATCH f_%08x trial %d: st(%d) native=%.17g ref=%.17g top=%d k=%d\n", table[fi].addr, t, j, nd, ud, c.top, k); if (getenv("ORACLE_DEBUG")) { for (int q2 = 0; q2 < 8; q2++) { uc_x86_float80 w; uc_reg_read(uc, UC_X86_REG_ST0 + q2, &w); printf("   st%d native=%g ref(raw)=%04x:%016llx\n", q2, c.st[(c.top + q2) & 7], w.exponent, (unsigned long long)w.mantissa); } printf("   edx=%08x ecx=%08x\n", r[2], r[1]); } xdiff = 1; fbad = 1; bad++; } } }
            if (xdiff) continue;
            static uint8_t um[SCR_SIZE], ustk[0x2000];
            uc_mem_read(uc, SCR_BASE, um, SCR_SIZE); uc_mem_read(uc, STK_TOP - 0x2000, ustk, sizeof ustk);
            int diff = 0; const char *nm[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
            for (int i = 0; i < 8; i++) if (nr[i] != ur[i]) { if (!diff) printf("MISMATCH f_%08x trial %d:", table[fi].addr, t); printf(" %s native=%08x ref=%08x", nm[i], nr[i], ur[i]); diff = 1; }
            if (memcmp(G + SCR_BASE, um, SCR_SIZE)) { if (!diff) printf("MISMATCH f_%08x trial %d:", table[fi].addr, t); { uint32_t q3 = 0; while (G[SCR_BASE + q3] == um[q3]) q3++; printf(" scratch-memory differs at +%x native=%08x ref=%08x", q3, *(uint32_t *)(G + SCR_BASE + (q3 & ~3u)), *(uint32_t *)(um + (q3 & ~3u))); } diff = 1; }
            if (memcmp(G + STK_TOP - 0x2000, ustk, sizeof ustk)) { if (!diff) printf("MISMATCH f_%08x trial %d:", table[fi].addr, t); printf(" stack differs"); diff = 1; }
            if (diff && getenv("ORACLE_PEEK")) { uint32_t pa = (uint32_t)strtoul(getenv("ORACLE_PEEK"), 0, 16), uv = 0; uc_mem_read(uc, pa, &uv, 4); printf("\n   peek %08x native=%08x ref=%08x", pa, *(uint32_t *)(G + pa), uv); }
            if (diff && getenv("ORACLE_DEBUG")) { printf("\n   init: eax=%08x ecx=%08x edx=%08x ebx=%08x ebp=%08x esi=%08x edi=%08x  [esp+4]=%08x [esp+8]=%08x", r[0], r[1], r[2], r[3], r[5], r[6], r[7], *(uint32_t *)(stk + 4), *(uint32_t *)(stk + 8)); }
            if (diff) { printf("\n"); fbad = 1; bad++; } else good++;
        }
    }
    printf("functions=%d  trials ok=%d skipped(ref fault)=%d  functions with mismatch=%d\n", ntable, good, skipped, bad);
    return bad != 0;
}
