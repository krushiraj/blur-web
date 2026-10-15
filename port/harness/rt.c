// Runtime for the statically recompiled game code: API dispatch into the Win32/D3D9 handlers, guest threads on host threads
// (one runs at a time, hand-off at blocking calls, like the cooperative scheduler it replaces), CPUID/RDTSC.
#include "emu.h"
#include "../recomp/rt.h"
#include <unistd.h>
#include <execinfo.h>

extern const struct { uint32_t a; GuestFn f; } rt_table[];
extern const unsigned rt_table_n;

static Ctx *C;
int rt_trace;
void rt_mismatch(Cpu *c, uint32_t a, int found) { static int n; if (n++ < 200) { fprintf(stderr, "[mismatch] returned to %08x depth=%d %s esp=%08x\n", a, c->rn, found ? "propagate" : "DISPATCH", c->esp); void *bt[6]; int k = backtrace(bt, 6); backtrace_symbols_fd(bt + 1, k - 1, 2); } }
void rt_violation(Cpu *c, uint32_t callee, uint32_t be, uint32_t bb, uint32_t bs, uint32_t bd, uint32_t bp) {
    static int n; static uint32_t seen[256]; static int ns;
    for (int i = 0; i < ns; i++) if (seen[i] == callee) return;
    if (ns < 256) seen[ns++] = callee;
    if (n++ < 60) fprintf(stderr, "[viol] callee %08x: esp %08x->%08x%s%s%s%s\n", callee, be, c->esp, c->ebx != bb ? " EBX" : "", c->esi != bs ? " ESI" : "", c->edi != bd ? " EDI" : "", c->ebp != bp ? " EBP" : "");
}
void rt_enter(Cpu *c, uint32_t addr) { static uint64_t n; if (n++ < (getenv("RT_TRACE_MAX") ? strtoull(getenv("RT_TRACE_MAX"), 0, 0) : 2000)) fprintf(stderr, "[fn] %08x esp=%08x eax=%08x esi=%08x\n", addr, c->esp, c->eax, c->esi); }
static GuestFn *htab; static uint32_t *hkey; static uint32_t hmask;

static void build_table(void) {
    uint32_t n = 1; while (n < rt_table_n * 2) n <<= 1;
    hkey = calloc(n, 4); htab = calloc(n, sizeof(GuestFn)); hmask = n - 1;
    for (unsigned i = 0; i < rt_table_n; i++) { uint32_t h = (rt_table[i].a * 2654435761u) >> 7 & hmask; while (hkey[h]) h = (h + 1) & hmask; hkey[h] = rt_table[i].a; htab[h] = rt_table[i].f; }
}
static GuestFn lookup(uint32_t a) {
    uint32_t h = (a * 2654435761u) >> 7 & hmask;
    while (hkey[h]) { if (hkey[h] == a) return htab[h]; h = (h + 1) & hmask; }
    return NULL;
}

// ---- thread hand-off ---------------------------------------------------------------------------------------------------------
static pthread_mutex_t bm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t bcv[MAX_THREADS];
static uint64_t t_end;

static void finish_process(Ctx *c) {
    if (getenv("BLUR_STATS")) {
        fprintf(stderr, "[stats] most called APIs:\n");
        for (int n = 0; n < 25; n++) { int best = -1; for (int i = 0; i < c->nstubs; i++) if (c->stubs[i].calls && (best < 0 || c->stubs[i].calls > c->stubs[best].calls)) best = i;
            if (best < 0) break; fprintf(stderr, "  %10llu  %s!%s\n", (unsigned long long)c->stubs[best].calls, c->stubs[best].dll, c->stubs[best].name); c->stubs[best].calls = 0; }
    }
    d3d9_report(c);
    fprintf(stderr, "[exit] status=%d code=%u\n", c->exited, c->exit_code);
    fflush(stderr); _exit(c->exited == 1 ? (int)c->exit_code : c->exited);
}

static int pick_next(Ctx *c, int self) {
    for (int k = 1; k <= c->nthreads; k++) { int i = (self + k) % c->nthreads; Thread *t = &c->threads[i]; if (t->used && !t->done && !t->suspended) return i; }
    return -1;
}

static void *thread_main(void *arg);
static void give_turn(Ctx *c, int idx) {
    Thread *t = &c->threads[idx];
    pthread_mutex_lock(&bm);
    c->cur = idx;
    if (!t->rt_started) { t->rt_started = 1; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64u << 20); pthread_create(&t->pth, &at, thread_main, (void *)(intptr_t)idx); pthread_detach(t->pth); }
    else pthread_cond_signal(&bcv[idx]);
    pthread_mutex_unlock(&bm);
}
static void wait_turn(Ctx *c, int idx) { pthread_mutex_lock(&bm); while (c->cur != idx) pthread_cond_wait(&bcv[idx], &bm); pthread_mutex_unlock(&bm); }

static uint64_t last_prog; static int idle_n; static uint64_t idle_since;
static void rt_yield(Ctx *c) {
    if (t_end && real_us() > t_end) { c->exited = 5; finish_process(c); }
    int self = c->cur;
    if (c->api_progress != last_prog) { last_prog = c->api_progress; idle_n = 0; idle_since = 0; }
    else if (++idle_n > 2 * c->nthreads + 2) {
        if (!idle_since) idle_since = real_us();
        if (real_us() - idle_since > 20000000ull) {
            fprintf(stderr, "\n[DEADLOCK] all threads blocked for 20s\n");
            for (int i = 0; i < c->nthreads; i++) if (c->threads[i].used && !c->threads[i].done) fprintf(stderr, "  thread %d tid=%x entry=%08x %s\n", i, c->threads[i].tid, c->threads[i].entry, c->threads[i].suspended ? "suspended" : "blocked");
            c->exited = 4; finish_process(c);
        }
        usleep(200);
    }
    int n = pick_next(c, self);
    if (n < 0 || n == self) return;
    give_turn(c, n); wait_turn(c, self);
}

// ---- CPUID / RDTSC -----------------------------------------------------------------------------------------------------------
static void do_cpuid(Cpu *cpu) {
    uint32_t leaf = cpu->eax;
    switch (leaf) {
    case 0: cpu->eax = 1; cpu->ebx = 0x756e6547; cpu->edx = 0x49656e69; cpu->ecx = 0x6c65746e; break;       // "GenuineIntel"
    case 1: cpu->eax = 0x000206a7; cpu->ebx = 0x00100800; cpu->ecx = 0x00000201; cpu->edx = 0x0fabfbff; break; // SSE3, SSE2, SSE, MMX, FXSR, CMOV, TSC, CX8
    case 0x80000000: cpu->eax = 0x80000004; cpu->ebx = cpu->ecx = cpu->edx = 0; break;
    case 0x80000001: cpu->eax = 0; cpu->ebx = 0; cpu->ecx = 0; cpu->edx = 0x00000000; break;
    default: cpu->eax = cpu->ebx = cpu->ecx = cpu->edx = 0;
    }
}
static void do_rdtsc(Cpu *cpu) { uint64_t t = real_us() * 2000ull; cpu->eax = (uint32_t)t; cpu->edx = (uint32_t)(t >> 32); }

void rt_unsupported(Cpu *cpu, uint32_t a, const char *w) { (void)cpu; fprintf(stderr, "[rt] unsupported at %08x: %s\n", a, w); fflush(stderr); _exit(70); }

// ---- API dispatch ------------------------------------------------------------------------------------------------------------
static void rt_api(Cpu *cpu, uint32_t addr) {
    Ctx *c = C; Stub *s = &c->stubs[(addr - STUB_BASE) / 16];
    uint32_t esp = cpu->esp, ret = M32(esp), a[16]; memcpy(a, G + esp + 4, sizeof a);
    if (!s->fn) {
        fprintf(stderr, "\n[UNIMPLEMENTED] %s!%s  nargs=%d %s  ret=%08x\n  args:", s->dll, s->name, s->nargs, s->cdecl_ ? "cdecl" : "stdcall", ret);
        for (int i = 0; i < 6; i++) fprintf(stderr, " %08x", a[i]);
        fprintf(stderr, "\n"); c->exited = 2; finish_process(c);
    }
    uint32_t r;
    for (;;) {
        if (c->verbose) fprintf(stderr, "[api t%d] %s!%s(%08x,%08x,%08x,%08x) ret=%08x esp=%08x\n", c->cur, s->dll, s->name, a[0], a[1], a[2], a[3], ret, esp);
        s->calls++; r = s->fn(c, a);
        if (c->exited) finish_process(c);
        if (!c->block) break;
        c->block = 0; cur_thread(c)->blocked = 1; rt_yield(c);
        memcpy(a, G + esp + 4, sizeof a);                    // the caller's arguments are still on its stack; handlers may have scribbled on `a`
    }
    cur_thread(c)->blocked = 0; c->api_progress++;
    uint32_t nesp = esp + 4 + (s->cdecl_ ? 0 : 4 * (uint32_t)s->nargs);
    if (c->req.pending) {                                    // the handler wants guest code run (WndProc, callback); do it synchronously
        CallReq q = c->req; c->req.pending = 0;
        uint32_t sp = nesp; for (int i = q.nargs - 1; i >= 0; i--) { sp -= 4; M32(sp) = q.args[i]; }
        sp -= 4; M32(sp) = RET_MAGIC; cpu->esp = sp;
        rt_call(cpu, q.fn);
        r = (q.have_cont && !q.use_result) ? q.cont_value : cpu->eax;
    }
    cpu->eax = r; cpu->esp = nesp; cpu->lastret = ret;
    if (c->yield_req) { c->yield_req = 0; rt_yield(c); }
    else if (c->api_slice && ++c->api_since_switch >= c->api_slice) { c->api_since_switch = 0; rt_yield(c); }
}

void rt_call(Cpu *cpu, uint32_t target) {
    if (target - STUB_BASE < STUB_SLOTS * 16u) { rt_api(cpu, target); return; }
    if (target == 0xfffffff0u) { do_cpuid(cpu); return; }
    if (target == 0xfffffff1u) { do_rdtsc(cpu); return; }
    GuestFn f = lookup(target);
    if (!f) {
        fprintf(stderr, "\n[rt] call to code that was not recompiled: %08x (esp=%08x ret=%08x eax=%08x ecx=%08x) [esp+4]=%08x [esp+8]=%08x\n", target, cpu->esp, M32(cpu->esp), cpu->eax, cpu->ecx, M32(cpu->esp + 4), M32(cpu->esp + 8));
        fprintf(stderr, "[rs] depth=%d lastret=%08x top:", cpu->rn, cpu->lastret); for (int k = cpu->rn - 1; k >= 0 && k >= cpu->rn - 12; k--) fprintf(stderr, " %08x", cpu->rs[k]); fprintf(stderr, "\n");
        if (getenv("RT_DUMP")) { uint32_t a = (uint32_t)strtoul(getenv("RT_DUMP"), 0, 16); fprintf(stderr, "[dump %08x]", a); for (int k = 0; k < 24; k++) fprintf(stderr, " %08x", M32(a + 4 * k)); fprintf(stderr, "\n"); }
        { FILE *lf = fopen("../recomp/learned.txt", "a"); if (lf) { fprintf(lf, "%08x\n", target); fclose(lf); } }
        fflush(stderr); C->exited = 6; finish_process(C);
    }
    if (getenv("RT_TRACE")) { static int n; if (n++ < 40) fprintf(stderr, "[call] %08x esp=%08x\n", target, cpu->esp); }
    f(cpu);
}

// ---- threads -----------------------------------------------------------------------------------------------------------------
static void init_cpu(Cpu *cpu, uint32_t teb) { memset(cpu, 0, sizeof *cpu); cpu->teb = teb; cpu->fcw = 0x27f; cpu->mxcsr = 0x1f80; }

static void thread_done(Ctx *c, int idx, uint32_t code) {
    Thread *t = &c->threads[idx]; t->done = 1; t->exit_code = code;
    if (t->handle) c->handles[(t->handle - 0x100) / 4].signaled = 1;
    if (c->verbose) fprintf(stderr, "[thread] tid=%x finished code=%u\n", t->tid, code);
    if (idx == 0) { c->exited = 1; c->exit_code = code; finish_process(c); }
    c->api_progress++;
    int n = pick_next(c, idx);
    if (n < 0) { c->exited = 1; finish_process(c); }
    give_turn(c, n);
}

static void *thread_main(void *arg) {
    Ctx *c = C; int idx = (int)(intptr_t)arg; Thread *t = &c->threads[idx];
    wait_turn(c, idx);
    Cpu *cpu = calloc(1, sizeof(Cpu)); t->cpu = cpu; init_cpu(cpu, t->teb);
    uint32_t esp = t->stack_hi - 0x40; esp -= 4; M32(esp) = t->param; esp -= 4; M32(esp) = RET_MAGIC; cpu->esp = esp;
    rt_call(cpu, t->entry);
    thread_done(c, idx, cpu->eax);
    return NULL;
}

void rt_main(Ctx *c) {
    C = c; build_table(); rt_trace = getenv("RT_TRACE") != NULL;
    if (getenv("BLUR_DUMP_OEP")) { Module *m = find_module(c, "Blur.exe"); FILE *f = fopen(getenv("BLUR_DUMP_OEP"), "wb"); if (f && m) { fwrite(G + m->base, 1, m->size, f); fclose(f); fprintf(stderr, "[dump] exe image at OEP: base=%08x size=%x\n", m->base, m->size); } exit(0); }
    if (getenv("BLUR_DUMP_MSV")) { Module *m = find_module(c, "msvcr90.dll"); FILE *f = fopen(getenv("BLUR_DUMP_MSV"), "wb"); if (f && m) { fwrite(G + m->base, 1, m->size, f); fclose(f); fprintf(stderr, "[dump] msvcr90 base=%08x size=%x\n", m->base, m->size); } exit(0); }
    if (getenv("BLUR_SECONDS")) t_end = real_us() + (uint64_t)atof(getenv("BLUR_SECONDS")) * 1000000ull;
    c->api_slice = getenv("BLUR_API_SLICE") ? (uint32_t)atoi(getenv("BLUR_API_SLICE")) : 50;
    for (int i = 0; i < MAX_THREADS; i++) pthread_cond_init(&bcv[i], NULL);
    fprintf(stderr, "[rt] %u recompiled functions; entering the game at %08x on the recompiled path\n", rt_table_n, g_oep_addr);
    Thread *t0 = &c->threads[0]; Cpu *cpu = calloc(1, sizeof(Cpu)); t0->cpu = cpu; init_cpu(cpu, t0->teb);
    cpu->eax = c->oep_regs[0]; cpu->ecx = c->oep_regs[1]; cpu->edx = c->oep_regs[2]; cpu->ebx = c->oep_regs[3]; cpu->esp = c->oep_regs[4]; cpu->ebp = c->oep_regs[5]; cpu->esi = c->oep_regs[6]; cpu->edi = c->oep_regs[7];
    if (getenv("BLUR_LOG")) fprintf(stderr, "[rt] start regs eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n", cpu->eax, cpu->ecx, cpu->edx, cpu->ebx, cpu->esp, cpu->ebp, cpu->esi, cpu->edi);
    t0->started = 1; t0->rt_started = 1; c->cur = 0;
    rt_call(cpu, g_oep_addr);
    thread_done(c, 0, cpu->eax);
}
