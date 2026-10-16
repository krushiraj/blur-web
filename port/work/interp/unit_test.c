// Hand-checked unit tests for interpreter behaviour that the Unicorn differential tests cannot cover (host interface, hooks, instructions whose reference
// behaviour is environment specific).  Build/run: make unit
#include "interp.h"
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>

uint8_t *G;
static int n_spin, n_preempt, n_cpuid, n_rdtsc; static sigjmp_buf jb; static const char *uns;
void rt_spin(Cpu *c) { (void)c; n_spin++; } void rt_preempt(Cpu *c) { n_preempt++; c->budget = 1000; }
void rt_cpuid(Cpu *c) { n_cpuid++; c->eax = 0x1234; c->ebx = 1; c->ecx = 2; c->edx = 3; } void rt_rdtsc(Cpu *c) { n_rdtsc++; c->eax = 0xaabbccdd; c->edx = 0x11223344; }
void rt_unsupported(Cpu *c, uint32_t a, const char *w) { (void)c; (void)a; uns = w; siglongjmp(jb, 1); }
static int fails, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define NATIVE 0x7e000000u
static int is_native(uint32_t pc) { return pc >= NATIVE && pc < NATIVE + 0x100000; }
static Cpu cpu;
#define CODE 0x1000u
static void prog(const uint8_t *b, int n) { memset(G + CODE, 0xf4, 256); memcpy(G + CODE, b, n); interp_flush(); }
static void reset(void) { memset(&cpu, 0, sizeof cpu); cpu.pc = CODE; cpu.esp = 0x80000; cpu.fcw = 0x27f; cpu.mxcsr = 0x1f80; cpu.budget = 1 << 30; cpu.teb = 0x90000; }
static uint32_t seen[4], nseen; static void hook_call(uint32_t t, uint32_t r) { (void)r; if (nseen < 4) seen[nseen++] = t; }
static int run(void) { return interp_run_n(&cpu, 10000); }
static int hook_fn(Cpu *c, int vec) { (void)c; return vec == 3 || vec == 0x2d; }

int main(void) {
    G = calloc(1, 4u << 20);
    interp_init(is_native);

    // 1. mov eax,5 ; jmp NATIVE+0x10  -> returns INTERP_NATIVE with pc at the native address
    { uint32_t rel = NATIVE + 0x10 - (CODE + 5 + 5); uint8_t p[] = {0xb8, 5, 0, 0, 0, 0xe9, rel, rel >> 8, rel >> 16, rel >> 24}; prog(p, sizeof p); reset();
      int r = run(); CHECK(r == INTERP_NATIVE); CHECK(cpu.pc == NATIVE + 0x10); CHECK(cpu.eax == 5); }
    // 2. call/ret: call +5 ; (ret target) mov ebx,7 ; jmp native ... callee: mov eax,9 ; ret
    { uint8_t p[] = {0xe8, 6, 0, 0, 0,  0xbb, 7, 0, 0, 0,  0xcc /*unused*/, 0xb8, 9, 0, 0, 0, 0xc3};
      // layout: 1000 call 100b ; 1005 mov ebx,7 ; 100a int3(never) ; 100b mov eax,9 ; 1010 ret
      prog(p, sizeof p); reset(); cpu.esp = 0x80000; M32(cpu.esp) = 0; /* ret target after final ret = 0 */
      // after callee returns to 1005 we execute mov ebx,7 then int3 (no hook) -> unsupported
      if (!sigsetjmp(jb, 1)) run(); CHECK(cpu.eax == 9); CHECK(cpu.ebx == 7); CHECK(uns && !strcmp(uns, "int3")); CHECK(cpu.esp == 0x80000 - 0); }
    // 3. ret imm16 pops the extra bytes
    { uint8_t p[] = {0xc2, 8, 0}; prog(p, sizeof p); reset(); cpu.esp = 0x80000; M32(cpu.esp) = NATIVE; run(); CHECK(cpu.pc == NATIVE); CHECK(cpu.esp == 0x80000 + 4 + 8); }
    // 4. cpuid / rdtsc go through the host hooks
    { uint8_t p[] = {0x0f, 0xa2, 0x0f, 0x31, 0xe9, 0, 0, 0, 0}; uint32_t rel = NATIVE - (CODE + 9); memcpy(p + 5, &rel, 4); prog(p, sizeof p); reset(); run();
      CHECK(n_cpuid == 1 && n_rdtsc == 1); CHECK(cpu.eax == 0xaabbccdd && cpu.edx == 0x11223344 && cpu.ebx == 1 && cpu.ecx == 2); }
    // 5. int3 hook
    { static int hook_calls; interp_int_hook = NULL; (void)hook_calls;
      uint8_t p[] = {0xcc, 0xb8, 1, 0, 0, 0, 0xcd, 0x2d, 0xe9, 0, 0, 0, 0}; uint32_t rel = NATIVE - (CODE + 13); memcpy(p + 9, &rel, 4); prog(p, sizeof p); reset();
      interp_int_hook = hook_fn; int r = run(); CHECK(r == INTERP_NATIVE); CHECK(cpu.eax == 1); interp_int_hook = NULL; }
    // 6. fs: access goes through c->teb
    { uint8_t p[] = {0x64, 0xa1, 0x18, 0, 0, 0, 0x64, 0x8b, 0x1d, 0x30, 0, 0, 0, 0xe9, 0, 0, 0, 0}; uint32_t rel = NATIVE - (CODE + 18); memcpy(p + 14, &rel, 4); prog(p, sizeof p); reset();
      M32(0x90000 + 0x18) = 0xdeadbeef; M32(0x90000 + 0x30) = 0x12345678; run(); CHECK(cpu.eax == 0xdeadbeef && cpu.ebx == 0x12345678); }
    // 7. pushfd/popfd round trip and sahf/lahf
    { uint8_t p[] = {0xf9 /*stc*/, 0xfd /*std*/, 0x9c, 0xf8, 0xfc, 0x9d /*popfd*/, 0x9f /*lahf*/, 0xe9, 0, 0, 0, 0}; uint32_t rel = NATIVE - (CODE + 12); memcpy(p + 8, &rel, 4); prog(p, sizeof p); reset(); run();
      CHECK(cpu.fl.cf == 1 && cpu.df == 1); CHECK(((cpu.eax >> 8) & 1) == 1); CHECK(cpu.esp == 0x80000); }
    // 8. 16-bit push/pop and push seg
    { uint8_t p[] = {0x66, 0xb8, 0x34, 0x12, 0x66, 0x50, 0x66, 0x5b, 0x0e, 0x58, 0xe9, 0, 0, 0, 0}; uint32_t rel = NATIVE - (CODE + 15); memcpy(p + 11, &rel, 4); prog(p, sizeof p); reset(); cpu.ebx = 0xffff0000; run();
      CHECK(cpu.ebx == 0xffff1234); CHECK(cpu.eax == 0x23); }
    // 9. rep movsd forward and backward (df), overlapping forward copy replicates like the hardware loop
    { uint8_t p[] = {0xf3, 0xa5, 0xe9, 0, 0, 0, 0}; uint32_t rel = NATIVE - (CODE + 7); memcpy(p + 3, &rel, 4); prog(p, sizeof p); reset();
      for (int i = 0; i < 16; i++) M32(0x2000 + 4 * i) = 0x100 + i; cpu.esi = 0x2000; cpu.edi = 0x2004; cpu.ecx = 8; run();
      CHECK(M32(0x2004) == 0x100 && M32(0x2008) == 0x100 && M32(0x2020) == 0x100); CHECK(cpu.ecx == 0 && cpu.esi == 0x2020 && cpu.edi == 0x2024);
      for (int i = 0; i < 16; i++) M32(0x3000 + 4 * i) = 0x200 + i; reset(); prog(p, sizeof p); cpu.df = 1; cpu.esi = 0x303c; cpu.edi = 0x3040; cpu.ecx = 4; run();
      CHECK(M32(0x3040) == 0x20f && M32(0x303c) == 0x20e && M32(0x3034) == 0x20c); CHECK(cpu.esi == 0x302c); }
    // 10. repne scasb finds a byte
    { uint8_t p[] = {0xf2, 0xae, 0xe9, 0, 0, 0, 0}; uint32_t rel = NATIVE - (CODE + 7); memcpy(p + 3, &rel, 4); prog(p, sizeof p); reset(); memcpy(G + 0x2000, "hello, world", 13); cpu.edi = 0x2000; cpu.eax = ','; cpu.ecx = 100; run();
      CHECK(cpu.edi == 0x2006); CHECK(cpu.ecx == 100 - 6); CHECK(cpu.fl.zf == 1); }
    // 11. decode cache: self-modifying code needs interp_flush_range
    { uint8_t p[] = {0xb8, 1, 0, 0, 0, 0xe9, 0, 0, 0, 0}; uint32_t rel = NATIVE - (CODE + 10); memcpy(p + 6, &rel, 4); prog(p, sizeof p); reset(); run(); CHECK(cpu.eax == 1);
      M32(CODE + 1) = 2; reset(); run(); CHECK(cpu.eax == 1); /* stale until flushed */ interp_flush_range(CODE, CODE + 5); reset(); run(); CHECK(cpu.eax == 2); }
    // 12. budget: a tight loop returns INTERP_BUDGET with a valid pc, then continues
    { uint8_t p[] = {0x40, 0xeb, 0xfd}; prog(p, sizeof p); reset(); int r = interp_run_n(&cpu, 100); CHECK(r == INTERP_BUDGET); CHECK(cpu.eax == 50 && (cpu.pc == CODE || cpu.pc == CODE + 1)); }
    // 13. call counts down c->budget; when it goes negative the run returns INTERP_PREEMPT (pc at the call target) so the host can yield like a lifted function entry does
    { uint8_t p[] = {0xe8, 0, 0, 0, 0, 0xe9, 0, 0, 0, 0}; uint32_t rel = NATIVE - (CODE + 10); memcpy(p + 6, &rel, 4); prog(p, sizeof p); reset(); cpu.budget = 0; int r = run(); CHECK(r == INTERP_PREEMPT); CHECK(cpu.pc == CODE + 5); CHECK(cpu.esp == 0x80000 - 4);
      cpu.budget = 5; r = run(); CHECK(r == INTERP_NATIVE); }
    // 13b. the call hook sees direct and indirect calls (cold-function coverage)
    { uint8_t p[] = {0xe8, 0x00, 0x00, 0x00, 0x00, /* call +0 -> 0x1005 */ 0xb8, 0x00, 0x00, 0x00, 0x7e, /* mov eax, NATIVE */ 0xff, 0xd0 /* call eax */};
      prog(p, sizeof p); reset(); nseen = 0; interp_call_hook = hook_call; run(); interp_call_hook = NULL; CHECK(nseen == 2 && seen[0] == CODE + 5 && seen[1] == NATIVE); }
    // 14. pause -> rt_spin
    { uint8_t p[] = {0xf3, 0x90, 0xe9, 0, 0, 0, 0}; uint32_t rel = NATIVE - (CODE + 7); memcpy(p + 3, &rel, 4); prog(p, sizeof p); reset(); n_spin = 0; run(); CHECK(n_spin == 1); }
    // 15. fnsave / frstor / fxsave / fxrstor round trips
    { uint8_t p[] = {0xdd, 0x35, 0x00, 0x20, 0x00, 0x00, /* fnsave [0x2000] */ 0xdd, 0x25, 0x00, 0x20, 0x00, 0x00, /* frstor */ 0x0f, 0xae, 0x05, 0x00, 0x30, 0, 0, /* fxsave [0x3000] */
                     0x0f, 0xae, 0x0d, 0x00, 0x30, 0, 0, /* fxrstor */ 0xe9, 0, 0, 0, 0}; uint32_t rel = NATIVE - (CODE + 31); memcpy(p + 27, &rel, 4); prog(p, sizeof p); reset();
      cpu.top = 4; for (int i = 0; i < 8; i++) cpu.st[i] = 1.5 * (i + 1); for (int i = 0; i < 16; i++) cpu.xmm[3][i] = i + 1; double ref[8]; memcpy(ref, cpu.st, sizeof ref); uint8_t x3[16]; memcpy(x3, cpu.xmm[3], 16);
      run(); CHECK(cpu.top == 0 || cpu.top == 4); /* fnsave re-initialises, frstor brings it back */ CHECK(cpu.top == 4); for (int i = 0; i < 8; i++) CHECK(cpu.st[(cpu.top + i) & 7] == ref[(4 + i) & 7]);
      CHECK(memcmp(cpu.xmm[3], x3, 16) == 0); CHECK(M16(0x2000) == 0x27f); }
    // 16. enter/leave level 0
    { uint8_t p[] = {0xc8, 0x10, 0, 0, 0xc9, 0xe9, 0, 0, 0, 0}; uint32_t rel = NATIVE - (CODE + 10); memcpy(p + 6, &rel, 4); prog(p, sizeof p); reset(); cpu.ebp = 0x1234; run(); CHECK(cpu.ebp == 0x1234); CHECK(cpu.esp == 0x80000); }
    // 17. unsupported opcode reports the faulting address through rt_unsupported
    { uint8_t p[] = {0xf4}; prog(p, sizeof p); reset(); uns = NULL; if (!sigsetjmp(jb, 1)) run(); CHECK(uns != NULL); CHECK(cpu.pc == CODE); }
    // 18. cmpxchg8b
    { uint8_t p[] = {0xf0, 0x0f, 0xc7, 0x0d, 0x00, 0x20, 0, 0, 0xe9, 0, 0, 0, 0}; uint32_t rel = NATIVE - (CODE + 13); memcpy(p + 9, &rel, 4); prog(p, sizeof p); reset();
      M32(0x2000) = 5; M32(0x2004) = 6; cpu.eax = 5; cpu.edx = 6; cpu.ebx = 0xaa; cpu.ecx = 0xbb; run(); CHECK(M32(0x2000) == 0xaa && M32(0x2004) == 0xbb && cpu.fl.zf == 1);
      M32(0x2000) = 7; reset(); cpu.eax = 5; cpu.edx = 6; run(); CHECK(cpu.eax == 7 && cpu.fl.zf == 0); }
    // 19. an x87 round: fld1 ; fldpi ; fmulp ; fistp ; with rounding control
    { uint8_t p[] = {0xd9, 0xe8, 0xd9, 0xeb, 0xde, 0xc9, 0xdb, 0x1d, 0x00, 0x20, 0, 0, 0xe9, 0, 0, 0, 0}; uint32_t rel = NATIVE - (CODE + 17); memcpy(p + 13, &rel, 4); prog(p, sizeof p); reset(); run(); CHECK((int32_t)M32(0x2000) == 3); }
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
