// Runtime for the statically recompiled game code: API dispatch into the Win32/D3D9 handlers, guest threads on host threads
// (one runs at a time, hand-off at blocking calls, like the cooperative scheduler it replaces), CPUID/RDTSC.
#include "emu.h"
#include "../recomp/rt.h"
#include <unistd.h>
#include <execinfo.h>
#include "apilog.h"
#include "snap.h"
void snapshot_write(Ctx *c, const char *dir, uint32_t oep);

extern const struct { uint32_t a; GuestFn f; } rt_table[];
extern const unsigned rt_table_n;

extern int g_det_on; extern double g_det_step; extern uint64_t (*audio_clock_us)(void);
static Ctx *C;
int rt_trace;
void rt_enter(Cpu *c, uint32_t addr) { static uint64_t n; if (n++ < (getenv("RT_TRACE_MAX") ? strtoull(getenv("RT_TRACE_MAX"), 0, 0) : 2000)) fprintf(stderr, "[fn] %08x esp=%08x eax=%08x esi=%08x\n", addr, c->esp, c->eax, c->esi); }
static GuestFn *htab; static uint32_t *hkey; static uint32_t hmask;

static void build_table(void) {
    uint32_t n = 1; while (n < (rt_table_n + rt_table_x_n) * 2) n <<= 1;
    hkey = calloc(n, 4); htab = calloc(n, sizeof(GuestFn)); hmask = n - 1;
    for (unsigned i = 0; i < rt_table_n + rt_table_x_n; i++) {
        uint32_t a = i < rt_table_n ? rt_table[i].a : rt_table_x[i - rt_table_n].a; GuestFn f = i < rt_table_n ? rt_table[i].f : rt_table_x[i - rt_table_n].f;
        uint32_t h = (a * 2654435761u) >> 7 & hmask; while (hkey[h] && hkey[h] != a) h = (h + 1) & hmask; hkey[h] = a; htab[h] = f;
    }
}
static uint8_t *cov; static int cov_on = -1;     // BLUR_COVER=<file>: record every recompiled function entered (sizes the wasm hot set); written at exit
static GuestFn lookup(uint32_t a) {
    uint32_t h = (a * 2654435761u) >> 7 & hmask;
    while (hkey[h]) { if (hkey[h] == a) { if (cov_on > 0) cov[h] = 1; return htab[h]; } h = (h + 1) & hmask; }
    return NULL;
}
static void cov_init(void) { cov_on = getenv("BLUR_COVER") != NULL; if (cov_on) cov = calloc(hmask + 1, 1); }
static void cov_write(void) { const char *p = getenv("BLUR_COVER"); if (!p || !cov) return; FILE *f = fopen(p, "w"); if (!f) return; uint32_t n = 0; for (uint32_t h = 0; h <= hmask; h++) if (cov[h]) { fprintf(f, "%08x\n", hkey[h]); n++; } fclose(f); fprintf(stderr, "[cover] %u functions entered -> %s\n", n, p); }

// ---- thread hand-off ---------------------------------------------------------------------------------------------------------
static pthread_mutex_t bm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t bcv[MAX_THREADS];
static uint64_t t_end;

static void finish_process(Ctx *c) {
    cov_write();
    if (getenv("BLUR_DUMPMEM")) { FILE *f = fopen(getenv("BLUR_DUMPMEM"), "wb"); if (f) { fwrite(G + 0x20000000u, 1, 0x40000000u, f); fclose(f); fprintf(stderr, "[dumpmem] guest 0x20000000..0x60000000 written\n"); } }
    if (getenv("BLUR_STATS")) {
        fprintf(stderr, "[stats] most called APIs:\n");
        for (int n = 0; n < 120; n++) { int best = -1; for (int i = 0; i < c->nstubs; i++) if (c->stubs[i].calls && (best < 0 || c->stubs[i].calls > c->stubs[best].calls)) best = i;
            if (best < 0) break; fprintf(stderr, "  %10llu  %s!%s\n", (unsigned long long)c->stubs[best].calls, c->stubs[best].dll, c->stubs[best].name); c->stubs[best].calls = 0; }
    }
    if (getenv("BLUR_THREADS")) for (int i = 0; i < c->nthreads; i++) { Thread *t = &c->threads[i]; if (!t->used) continue;
        fprintf(stderr, "[thread %2d] %s%s%s apis=%u last=%s(%08x,%08x) ret=%08x wake_at=%llu\n", i, t->done ? "done " : "", t->suspended ? "suspended " : "", t->blocked ? "blocked " : "", t->n_api, t->last_api ? t->last_api : "-", t->last_a0, t->last_a1, t->last_ret, (unsigned long long)t->wake_at); }
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

static uint64_t g_switches;
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
    if (getenv("RT_SCHED")) fprintf(stderr, "[sched] n=%llu yield %d -> %d (nthreads=%d)\n", (unsigned long long)g_apilog_n, self, n, c->nthreads);
    g_switches++; give_turn(c, n); wait_turn(c, self);
}

void rt_preempt(Cpu *cpu) { cpu->budget = 200000; rt_yield(C); }
void rt_spin(Cpu *cpu) { (void)cpu; Thread *t = cur_thread(C); if (++t->spin_n >= 40) { t->spin_n = 0; rt_yield(C); } }

// ---- CPUID / RDTSC -----------------------------------------------------------------------------------------------------------
void rt_cpuid(Cpu *cpu) {
    // identical to what Unicorn's default CPU reports: the packer derives jump targets from CPUID results
    static const struct { uint32_t leaf, a, b, c, d; } T[] = {
        {0, 0x0000000d, 0x756e6547, 0x6c65746e, 0x49656e69}, {1, 0x000306c4, 0x00000800, 0xced82203, 0x078bfbfd}, {2, 1, 0, 0, 0x002c307d},
        {4, 0x121, 0x01c0003f, 0x3f, 1}, {7, 0, 0x389, 0, 0}, {0x80000000, 0x80000008, 0x756e6547, 0x6c65746e, 0x49656e69},
        {0x80000001, 0x000306c4, 0, 0x21, 0x28100800}, {0x80000002, 0x65746e49, 0x6f43206c, 0x50206572, 0x65636f72}, {0x80000008, 0x3028, 0, 0, 0}};
    uint32_t leaf = cpu->eax; cpu->eax = cpu->ebx = cpu->ecx = cpu->edx = 0;
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) if (T[i].leaf == leaf) { cpu->eax = T[i].a; cpu->ebx = T[i].b; cpu->ecx = T[i].c; cpu->edx = T[i].d; break; }
}
void rt_rdtsc(Cpu *cpu) { uint64_t t = (g_det_on ? now_us() : real_us()) * 2000ull; cpu->eax = (uint32_t)t; cpu->edx = (uint32_t)(t >> 32); }

void rt_unsupported(Cpu *cpu, uint32_t a, const char *w) { (void)cpu; fprintf(stderr, "[rt] unsupported at %08x: %s\n", a, w); fflush(stderr); _exit(70); }

// ---- diagnostic write watch: BLUR_WATCHW=<hexaddr>-<hexaddr>  logs which recompiled guest function writes into the range (first 40 hits) ----------------------
#include <signal.h>
#include <dlfcn.h>
#include <sys/mman.h>
static uint32_t ww_lo, ww_hi; static int ww_hits;
static uintptr_t ww_open[512]; static int ww_nopen;
void ww_rearm(void) { for (int i = 0; i < ww_nopen; i++) mprotect((void *)ww_open[i], 0x4000, PROT_READ); ww_nopen = 0; }
static void ww_handler(int sig, siginfo_t *si, void *uctx) {
    (void)sig; (void)uctx; uintptr_t fa = (uintptr_t)si->si_addr; uintptr_t base = (uintptr_t)G;
    if (fa < base + ww_lo || fa >= base + ww_hi) { signal(SIGSEGV, SIG_DFL); return; }
    void *bt[10]; int n = backtrace(bt, 10); int host = 0; char names[400] = ""; 
    for (int i = 0; i < n; i++) { Dl_info di; if (dladdr(bt[i], &di) && di.dli_sname) { if (!strcmp(di.dli_sname, "guest_alloc") || !strcmp(di.dli_sname, "rt_api") || !strncmp(di.dli_sname, "h_", 2) || !strncmp(di.dli_sname, "rd", 2) || !strncmp(di.dli_sname, "wr", 2) || strstr(di.dli_sname, "uc_mem")) host = 1; if (i > 1 && strlen(names) < 340) { strcat(names, " "); strcat(names, di.dli_sname); } } }
    if (!host && ww_hits < 60) { ww_hits++; fprintf(stderr, "[watchw] write to %08x by:%s\n", (uint32_t)(fa - base), names); }
    uintptr_t pg = fa & ~(uintptr_t)0x3fff; mprotect((void *)pg, 0x4000, PROT_READ | PROT_WRITE); if (ww_nopen < 512) ww_open[ww_nopen++] = pg;
}
void ww_arm(void) {
    const char *e = getenv("BLUR_WATCHW"); if (!e) return; ww_lo = (uint32_t)strtoul(e, 0, 16); const char *d = strchr(e, '-'); ww_hi = d ? (uint32_t)strtoul(d + 1, 0, 16) : ww_lo + 4;
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = ww_handler; sa.sa_flags = SA_SIGINFO; sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    uint32_t lo = ww_lo & ~0x3fffu, hi = (ww_hi + 0x3fff) & ~0x3fffu; mprotect(G + lo, hi - lo, PROT_READ);
    fprintf(stderr, "[watchw] armed %08x-%08x\n", ww_lo, ww_hi);
}
// ---- API dispatch ------------------------------------------------------------------------------------------------------------
uint32_t g_api_ret, g_api_esp;
static uint32_t cont_addr;      // stub a guest callback returns to, when the API handler wants its result adjusted afterwards

// Run one host API called with the guest stack [ret][args...]; returns the guest address to continue at.
void audio_tick(Ctx *c);
static uint32_t rt_api(Cpu *cpu, uint32_t addr) {
    Ctx *c = C; Stub *s = &c->stubs[(addr - STUB_BASE) / 16];
    if (addr == cont_addr) {                                  // a callback requested by an API handler returned
        Thread *t = cur_thread(c); Cont k = t->cont[--t->ncont];
        if (!k.use_result) cpu->eax = k.value;
        c->api_progress++;
        return k.real_ret;
    }
    audio_tick(c);
    uint32_t esp = cpu->esp, ret = M32(esp), a[16]; memcpy(a, G + esp + 4, sizeof a);
    if (!s->fn) {
        fprintf(stderr, "\n[UNIMPLEMENTED] %s!%s  nargs=%d %s  ret=%08x\n  args:", s->dll, s->name, s->nargs, s->cdecl_ ? "cdecl" : "stdcall", ret);
        for (int i = 0; i < 6; i++) fprintf(stderr, " %08x", a[i]);
        fprintf(stderr, "\n"); c->exited = 2; finish_process(c);
    }
    uint32_t r;
    for (;;) {
        if (c->verbose) fprintf(stderr, "[api t%d] %s!%s(%08x,%08x,%08x,%08x) ret=%08x esp=%08x\n", c->cur, s->dll, s->name, a[0], a[1], a[2], a[3], ret, esp);
        g_api_ret = ret; g_api_esp = esp; g_cur_api = s->name; g_det_ticks++; apilog_call(c->cur, s->name, a); { Thread *tt = cur_thread(c); tt->last_api = s->name; tt->last_ret = ret; tt->last_a0 = a[0]; tt->last_a1 = a[1]; tt->n_api++; } s->calls++; r = s->fn(c, a);
        if (c->exited) finish_process(c);
        if (!c->block) break;
        c->block = 0; cur_thread(c)->blocked = 1; rt_yield(c);
        memcpy(a, G + esp + 4, sizeof a);                    // the caller's arguments are still on its stack; handlers may have scribbled on `a`
    }
    cur_thread(c)->blocked = 0; c->api_progress++;
    uint32_t nesp = esp + 4 + (s->cdecl_ ? 0 : 4 * (uint32_t)s->nargs), next = ret;
    if (c->req.pending) {                                    // the handler wants guest code run (WndProc, thread start routine); it returns to `back`
        CallReq q = c->req; c->req.pending = 0; Thread *t = cur_thread(c); uint32_t back = ret;
        if (q.have_cont) { t->cont[t->ncont++] = (Cont){ ret, q.cont_value, q.use_result }; back = cont_addr; }
        for (int i = q.nargs - 1; i >= 0; i--) { nesp -= 4; M32(nesp) = q.args[i]; }
        nesp -= 4; M32(nesp) = back;
        next = q.fn;
    } else cpu->eax = r;
    cpu->esp = nesp;
    if (c->yield_req) { c->yield_req = 0; rt_yield(c); }
    else if (c->api_slice && ++c->api_since_switch >= c->api_slice) { c->api_since_switch = 0; rt_yield(c); }
    return next;
}

static void hook_dump(void);
static void rt_missing(Cpu *cpu, uint32_t target) {
    fprintf(stderr, "\n[rt] jump to code that was not recompiled: %08x (esp=%08x [esp]=%08x eax=%08x ecx=%08x) [esp+4]=%08x [esp+8]=%08x\n", target, cpu->esp, M32(cpu->esp), cpu->eax, cpu->ecx, M32(cpu->esp + 4), M32(cpu->esp + 8));
    if (getenv("RT_DUMP")) { uint32_t a = (uint32_t)strtoul(getenv("RT_DUMP"), 0, 16); fprintf(stderr, "[dump %08x]", a); for (int k = 0; k < 24; k++) fprintf(stderr, " %08x", M32(a + 4 * k)); fprintf(stderr, "\n"); }
    // only image/msvcr90 addresses are recompile candidates (garbage targets from heap corruption are not); BLUR_LEARNED overrides the output file
    if ((target >= 0x400000 && target < 0x2070000) || (target >= 0x78520000 && target < 0x78600000)) { const char *lp = getenv("BLUR_LEARNED"); FILE *lf = fopen(lp ? lp : "../recomp/learned_x.txt", "a"); if (lf) { fprintf(lf, "%08x\n", target); fclose(lf); } }
    hook_dump(); fflush(stderr); C->exited = 6; finish_process(C);
}

// The trampoline every lifted function ends in (as a tail call): continue at cpu->pc.
// BLUR_HOOK=addr[,addr..]: log entries (cdecl args) and return values of the guest functions at those addresses
static void lua_init(void);
static uint32_t hook_addr[16]; static int nhook = -1; static struct { uint32_t ret, esp, fn; } hook_pend[256]; static int nhook_pend; static int hook_budget = 400;
static void hook_init(void) { nhook = (getenv("BLUR_POKE") || getenv("BLUR_LUATRACE")) ? 1 : 0; if (getenv("BLUR_LUATRACE")) lua_init(); hook_addr[0] = 0; const char *e = getenv("BLUR_HOOK"); if (nhook) { nhook = 1; hook_addr[0] = 0xffffffffu; } while (e && *e && nhook < 16) { hook_addr[nhook++] = (uint32_t)strtoul(e, (char **)&e, 16); if (*e == ',') e++; } }
// BLUR_LUATRACE=1: log every entry into one of the 255 Lua C bindings (table from ../work/net-re/script_bindings.txt: <slot> <name> <fn address>)
static struct { uint32_t a; char name[48]; uint32_t n; } lua_b[300]; static int nlua; extern uint64_t g_frames_pub;
static void lua_init(void) { const char *p = getenv("BLUR_LUATRACE_TABLE"); FILE *f = fopen(p ? p : "../work/net-re/script_bindings.txt", "r"); char l[256]; if (!f) { fprintf(stderr, "[lua] no binding table\n"); return; }
    while (fgets(l, sizeof l, f) && nlua < 300) { unsigned slot, fn; char nm[64]; if (sscanf(l, "%x %63s %x", &slot, nm, &fn) == 3) { lua_b[nlua].a = fn; snprintf(lua_b[nlua].name, 48, "%s", nm); nlua++; } } fclose(f); fprintf(stderr, "[lua] tracing %d bindings\n", nlua); }
static void lua_check(Cpu *cpu, uint32_t pc) { for (int i = 0; i < nlua; i++) if (lua_b[i].a == pc) { lua_b[i].n++; static uint64_t t0; uint64_t t = real_us(); if (!t0) t0 = t;
    if (lua_b[i].n <= 40 || (lua_b[i].n & (lua_b[i].n - 1)) == 0) fprintf(stderr, "[lua] %s #%u frame=%llu t=%llums thread=%d L=%08x\n", lua_b[i].name, lua_b[i].n, (unsigned long long)g_frames_pub, (unsigned long long)((t - t0) / 1000), C->cur, M32(cpu->esp + 4)); return; } }
static struct { uint32_t pc, ret, a[4]; int t; } hook_ring[128]; static unsigned hook_n;    // BLUR_HOOK_TAIL=1: keep the last 128 entries, dumped when the run dies on a missing target
static void hook_dump(void) { for (unsigned i = hook_n > 128 ? hook_n - 128 : 0; i < hook_n; i++) { typeof(hook_ring[0]) *e = &hook_ring[i & 127]; fprintf(stderr, "[hooktail] %08x t%d ret=%08x args %08x %08x %08x %08x\n", e->pc, e->t, e->ret, e->a[0], e->a[1], e->a[2], e->a[3]); } }
static void hook_check(Cpu *cpu, uint32_t pc) {
    if (nlua) lua_check(cpu, pc);
    { static uint32_t pat = 1, pa, pv; static uint32_t ppc; if (pat == 1) { pat = 0; const char *e = getenv("BLUR_POKE"); if (e) { ppc = (uint32_t)strtoul(e, (char **)&e, 16); if (*e == ',') pa = (uint32_t)strtoul(e + 1, (char **)&e, 16); if (*e == ',') pv = (uint32_t)strtoul(e + 1, 0, 16); pat = 2; } }
      if (pat == 2 && pc == ppc) { G[pa] = (uint8_t)pv; fprintf(stderr, "[poke] at pc %08x: [%08x] = %02x\n", pc, pa, pv); } }
    for (int i = 0; i < nhook_pend; i++) if (hook_pend[i].ret == pc && hook_pend[i].esp == cpu->esp) { if (hook_budget > 0) { hook_budget--; fprintf(stderr, "[hook] ret from %08x eax=%08x (t%d)\n", hook_pend[i].fn, cpu->eax, C->cur); } hook_pend[i] = hook_pend[--nhook_pend]; break; }
    for (int k = 0; k < nhook; k++) if (pc == hook_addr[k]) {
        if (getenv("BLUR_HOOK_TAIL")) { typeof(hook_ring[0]) *e = &hook_ring[hook_n++ & 127]; e->pc = pc; e->t = C->cur; e->ret = M32(cpu->esp); for (int q = 0; q < 4; q++) e->a[q] = M32(cpu->esp + 4 + 4 * q); }
        else if (hook_budget > 0) { hook_budget--; fprintf(stderr, "[hook] enter %08x t%d ret=%08x args %08x %08x %08x %08x\n", pc, C->cur, M32(cpu->esp), M32(cpu->esp + 4), M32(cpu->esp + 8), M32(cpu->esp + 12), M32(cpu->esp + 16)); }
        if (nhook_pend < 256) { hook_pend[nhook_pend].ret = M32(cpu->esp); hook_pend[nhook_pend].esp = cpu->esp + 4; hook_pend[nhook_pend++].fn = pc; }
    }
}
void rt_dispatch(Cpu *cpu) {
    for (;;) {
        uint32_t pc = cpu->pc;
        if (nhook < 0) hook_init();
        if (nhook) hook_check(cpu, pc);
        if (pc - STUB_BASE < STUB_SLOTS * 16u) { cpu->pc = rt_api(cpu, pc); continue; }
        if (pc == RET_MAGIC) return;                         // the thread's start routine returned
        GuestFn f = lookup(pc);
        if (!f) rt_missing(cpu, pc);
        MUSTTAIL return f(cpu);
    }
}

// ---- threads -----------------------------------------------------------------------------------------------------------------
static void init_cpu(Cpu *cpu, uint32_t teb) { memset(cpu, 0, sizeof *cpu); cpu->budget = 200000; cpu->teb = teb; cpu->fcw = 0x27f; cpu->mxcsr = 0x1f80; }

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
    cpu->pc = t->entry; rt_dispatch(cpu);
    thread_done(c, idx, cpu->eax);
    return NULL;
}

static void *watchdog(void *arg) {
    Ctx *c = arg; uint64_t last = 0; int still = 0;
    for (;;) { sleep(2); uint64_t now = g_switches + c->api_progress; if (now == last) { if (++still >= 2) { fprintf(stderr, "[watchdog] no scheduling progress; cur=%d nthreads=%d\n", c->cur, c->nthreads); for (int i = 0; i < c->nthreads; i++) { Thread *t = &c->threads[i]; fprintf(stderr, "  #%d used=%d done=%d susp=%d blocked=%d started=%d\n", i, t->used, t->done, t->suspended, t->blocked, t->rt_started); } still = 0; } } else still = 0; last = now; }
    return NULL;
}

void rt_main(Ctx *c) {
    C = c;
    if (getenv("BLUR_DET")) { g_det_step = atof(getenv("BLUR_DET")); g_time_base = 0; g_det_on = 1; g_det_ticks = 0; audio_clock_us = now_us; fprintf(stderr, "[det] deterministic clock: %g us per API call\n", g_det_step); }
    build_table(); cov_init(); rt_trace = getenv("RT_TRACE") != NULL; cont_addr = cont_stub_addr(c);
    if (getenv("BLUR_DUMP_OEP")) { Module *m = find_module(c, "Blur.exe"); FILE *f = fopen(getenv("BLUR_DUMP_OEP"), "wb"); if (f && m) { fwrite(G + m->base, 1, m->size, f); fclose(f); fprintf(stderr, "[dump] exe image at OEP: base=%08x size=%x\n", m->base, m->size); } exit(0); }
    if (getenv("BLUR_DUMP_MSV")) { Module *m = find_module(c, "msvcr90.dll"); FILE *f = fopen(getenv("BLUR_DUMP_MSV"), "wb"); if (f && m) { fwrite(G + m->base, 1, m->size, f); fclose(f); fprintf(stderr, "[dump] msvcr90 base=%08x size=%x\n", m->base, m->size); } exit(0); }
    if (getenv("BLUR_SECONDS")) t_end = real_us() + (uint64_t)atof(getenv("BLUR_SECONDS")) * 1000000ull;
    c->api_slice = getenv("BLUR_API_SLICE") ? (uint32_t)atoi(getenv("BLUR_API_SLICE")) : 50;
    for (int i = 0; i < MAX_THREADS; i++) pthread_cond_init(&bcv[i], NULL);
    { pthread_t wd; pthread_create(&wd, NULL, watchdog, c); pthread_detach(wd); }
    if (!getenv("BLUR_WATCHW_AFTER_FRAME1")) ww_arm();
    fprintf(stderr, "[rt] %u recompiled functions; entering the game at %08x on the recompiled path\n", rt_table_n, g_oep_addr);
    Thread *t0 = &c->threads[0]; Cpu *cpu = calloc(1, sizeof(Cpu)); t0->cpu = cpu; init_cpu(cpu, t0->teb);
    cpu->eax = c->oep_regs[0]; cpu->ecx = c->oep_regs[1]; cpu->edx = c->oep_regs[2]; cpu->ebx = c->oep_regs[3]; cpu->esp = c->oep_regs[4]; cpu->ebp = c->oep_regs[5]; cpu->esi = c->oep_regs[6]; cpu->edi = c->oep_regs[7];
    if (getenv("BLUR_LOG")) fprintf(stderr, "[rt] start regs eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n", cpu->eax, cpu->ecx, cpu->edx, cpu->ebx, cpu->esp, cpu->ebp, cpu->esi, cpu->edi);
    t0->started = 1; t0->rt_started = 1; c->cur = 0;
    cpu->pc = g_oep_addr;
    if (getenv("BLUR_SNAPSHOT")) { snapshot_write(c, getenv("BLUR_SNAPSHOT"), g_oep_addr); if (getenv("BLUR_SNAPSHOT_EXIT")) exit(0); }
    rt_dispatch(cpu);
    thread_done(c, 0, cpu->eax);
}
