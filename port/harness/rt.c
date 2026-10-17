// Runtime for the statically recompiled game code: API dispatch into the Win32/D3D9 handlers, guest threads on host threads
// (one runs at a time, hand-off at blocking calls, like the cooperative scheduler it replaces), CPUID/RDTSC.
#include "emu.h"
#include "../recomp/rt.h"
#include <unistd.h>
#include <execinfo.h>
#include <sys/mman.h>
#include "apilog.h"
#include "snap.h"
void snapshot_write(Ctx *c, const char *dir, uint32_t oep);

extern const struct { uint32_t a; GuestFn f; } rt_table[];
extern const unsigned rt_table_n;

extern int g_det_on; extern double g_det_step; extern uint64_t (*audio_clock_us)(void);
// BLUR_RING=1: cheap per-thread ring of the last RING_N function entries (addr, esp), printed when control reaches unrecompiled code.
#define RING_N 256
static uint32_t ring_a[MAX_THREADS][RING_N], ring_s[MAX_THREADS][RING_N]; static uint32_t ring_i[MAX_THREADS]; static int ring_on;
extern int g_prof; extern uint64_t prof_api, prof_dispatch, prof_api_ns, prof_yield_ns;
void prof_init(void); void prof_report(Ctx *c); void prof_note_api(Ctx *c, const Stub *s); uint64_t prof_now(void); void prof_thread_switch(int self); void prof_seg_begin(void), prof_seg_end(void), prof_seg_begin_stub(int);
static Ctx *C;
int rt_trace;
void rt_enter(Cpu *c, uint32_t addr) {
    if (ring_on) { int t = C->cur; uint32_t i = ring_i[t]++ & (RING_N - 1); ring_a[t][i] = addr; ring_s[t][i] = c->esp; return; }
    static uint64_t n; if (n++ < (getenv("RT_TRACE_MAX") ? strtoull(getenv("RT_TRACE_MAX"), 0, 0) : 2000)) fprintf(stderr, "[fn] %08x esp=%08x eax=%08x esi=%08x\n", addr, c->esp, c->eax, c->esi);
}
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
// interpreter fallback (port/work/interp): code that is not in the recompiled table is run by interp.c until control reaches compiled code / an API stub / RET_MAGIC
int rt_is_native(uint32_t pc) {
    if (pc - STUB_BASE < STUB_SLOTS * 16u || pc == RET_MAGIC) return 1;
    uint32_t h = (pc * 2654435761u) >> 7 & hmask;
    while (hkey[h]) { if (hkey[h] == pc) return 1; h = (h + 1) & hmask; }
    return 0;
}
extern int rt_cold(Cpu *cpu);
uint64_t rt_api_count(void) { return 0; }      // used only by the interpreter's optional call-trace window
static int cold_mode = -1;      // BLUR_COLD=0 disables the fallback (the old 'learn' behaviour), default on
static void cov_init(void) { cov_on = getenv("BLUR_COVER") != NULL; if (cov_on) cov = calloc(hmask + 1, 1); }
static void cov_write(void) { const char *p = getenv("BLUR_COVER"); if (!p || !cov) return; FILE *f = fopen(p, "w"); if (!f) return; uint32_t n = 0; for (uint32_t h = 0; h <= hmask; h++) if (cov[h]) { fprintf(f, "%08x\n", hkey[h]); n++; } fclose(f); fprintf(stderr, "[cover] %u functions entered -> %s\n", n, p); }

// ---- thread hand-off ---------------------------------------------------------------------------------------------------------
static pthread_mutex_t bm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t bcv[MAX_THREADS];
static struct { uint32_t pc, first; uint64_t n[32]; } pcc[8]; static int npcc;
static uint64_t t_end;

static void prof_dump(void);
void audio_shutdown(void);
static void finish_process(Ctx *c) {
    audio_shutdown();
    cov_write();
    if (getenv("BLUR_DUMPMEM")) { FILE *f = fopen(getenv("BLUR_DUMPMEM"), "wb"); if (f) { fwrite(G + 0x20000000u, 1, 0x40000000u, f); fclose(f); fprintf(stderr, "[dumpmem] guest 0x20000000..0x60000000 written\n"); } }
    if (getenv("BLUR_STATS")) {
        fprintf(stderr, "[stats] most called APIs:\n");
        for (int n = 0; n < 3000; n++) { int best = -1; for (int i = 0; i < c->nstubs; i++) if (c->stubs[i].calls && (best < 0 || c->stubs[i].calls > c->stubs[best].calls)) best = i;
            if (best < 0) break; fprintf(stderr, "  %10llu  %s!%s\n", (unsigned long long)c->stubs[best].calls, c->stubs[best].dll, c->stubs[best].name); c->stubs[best].calls = 0; }
    }
    if (getenv("BLUR_THREADS")) for (int i = 0; i < c->nthreads; i++) { Thread *t = &c->threads[i]; if (!t->used) continue;
        fprintf(stderr, "[thread %2d] entry=%08x param=%08x [p+4]=%08x [p+8]=%08x %s%s%s apis=%u last=%s(%08x,%08x) ret=%08x wake_at=%llu\n", i, t->entry, t->param, t->param > 0x10000 ? M32(t->param + 4) : 0, t->param > 0x10000 ? M32(t->param + 8) : 0, t->done ? "done " : "", t->suspended ? "suspended " : "", t->blocked ? "blocked " : "", t->n_api, t->last_api ? t->last_api : "-", t->last_a0, t->last_a1, t->last_ret, (unsigned long long)t->wake_at); }
    if (getenv("BLUR_FINDBYTES")) {      // BLUR_FINDBYTES=<file>[,nprobe]: look for 32-byte probes of a (host-decompressed) file in guest memory at exit; probes are spread over the file, one per 64 KB chunk index
        char fn[512]; snprintf(fn, sizeof fn, "%s", getenv("BLUR_FINDBYTES")); int np = 24; char *cm = strchr(fn, ','); if (cm) { *cm = 0; np = atoi(cm + 1); }
        FILE *f = fopen(fn, "rb"); if (f) { fseek(f, 0, SEEK_END); long sz = ftell(f); uint8_t *d = malloc((size_t)sz); fseek(f, 0, SEEK_SET); if (fread(d, 1, (size_t)sz, f)) {}
            fclose(f); long nch = sz / 65536; fprintf(stderr, "[findbytes] %s: %ld bytes, %ld chunks\n", fn, sz, nch);
            for (int i = 0; i < np; i++) { long ch = nch > 1 ? (long)i * (nch - 1) / (np > 1 ? np - 1 : 1) : 0; long off = ch * 65536 + 1000; if (off + 32 > sz) continue;
                uint8_t *base = G + 0x20000000u; size_t len = 0x40000000u, found = 0, hits = 0; uint8_t *q = base; uint32_t first = 0;
                while (len >= 32 && (q = memmem(q, len, d + off, 32))) { hits++; if (!found) { found = (size_t)(q - G); first = (uint32_t)found; } q++; len = (size_t)(base + 0x40000000u - q); }
                fprintf(stderr, "[findbytes] chunk %ld (file+%lx): %s first=%08x hits=%zu\n", ch, off, hits ? "FOUND" : "missing", first, hits); }
            free(d); } }
    if (getenv("BLUR_PCCOUNT")) for (int q = 0; q < 8 && pcc[q].pc; q++) { fprintf(stderr, "[pccount] %08x:", pcc[q].pc); for (int t = 0; t < 32; t++) if (pcc[q].n[t]) fprintf(stderr, " t%d=%llu", t, (unsigned long long)pcc[q].n[t]); fprintf(stderr, "\n"); }
    if (getenv("BLUR_PCPROF")) prof_dump();
    if (getenv("BLUR_SYNCSTAT")) hle_sync_report(c);
    if (getenv("BLUR_DUMPAT")) { uint32_t a = (uint32_t)strtoul(getenv("BLUR_DUMPAT"), 0, 16), n = 64; const char *q = strchr(getenv("BLUR_DUMPAT"), ','); if (q) n = (uint32_t)strtoul(q + 1, 0, 10);
        for (uint32_t i = 0; i < n; i += 8) { fprintf(stderr, "[dumpat] %08x:", a + 4 * i); for (uint32_t k = 0; k < 8 && i + k < n; k++) fprintf(stderr, " %08x", M32(a + 4 * (i + k))); fprintf(stderr, "\n"); } }
    if (getenv("BLUR_FINDPTR")) {       // BLUR_FINDPTR=hexA,hexB: list every aligned dword in the guest heap/VirtualAlloc area equal to one of the values, with 16 words of context before/after
        uint32_t v[4]; int nv = 0; const char *p = getenv("BLUR_FINDPTR"); while (*p && nv < 4) { v[nv++] = (uint32_t)strtoul(p, (char **)&p, 16); if (*p == ',') p++; }
        int hits = 0; for (uint64_t a = 0x20000000u; a < 0x60000000u && hits < 80; a += 4) { uint32_t w = M32((uint32_t)a); for (int i = 0; i < nv; i++) if (w == v[i]) { hits++;
            fprintf(stderr, "[findptr] %08x = %08x  words@-0x40..:", (uint32_t)a, w); for (int k = -16; k < 8; k++) fprintf(stderr, " %08x", M32((uint32_t)a + 4 * (uint32_t)k)); fprintf(stderr, "\n"); } } }
    { extern void d3d9_texaudit(Ctx *, const char *); extern void io_report(void); if (getenv("BLUR_SYNCSTAT")) io_report(); d3d9_texaudit(c, "exit"); } heapchk_scan(c, "at exit");
    d3d9_report(c); prof_report(c);
    fprintf(stderr, "[heap] %u MB of the guest heap used\n", (c->heap_next - 0x20000000u) >> 20);
    fprintf(stderr, "[exit] status=%d code=%u\n", c->exited, c->exit_code);
    fflush(stderr); _exit(c->exited == 1 ? (int)c->exit_code : c->exited);
}

static int sched_filter = 1;     // BLUR_SCHED_NOFILTER=1: old behaviour (switch to every blocked thread so it can re-poll its wait)
static int pick_next(Ctx *c, int self) {
    g_sched_now = 0; { int need = 0; for (int i = 0; i < c->nthreads; i++) if (c->threads[i].blocked && c->threads[i].wake_at) need = 1; if (need) g_sched_now = now_us(); }
    for (int k = 1; k <= c->nthreads; k++) { int i = (self + k) % c->nthreads; Thread *t = &c->threads[i]; if (t->used && !t->done && !t->suspended && (!sched_filter || thread_can_run(c, t))) return i; }
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
// ---- coroutine hand-off (default; BLUR_NOCORO=1 for the pthread baton): each guest thread is a coroutine with its own host stack, all on the main host thread.
// A switch is a register save/restore instead of condvar signal + OS context switch (~15 us, 54% of process time in the kernel before).  arm64 only; other hosts fall back.
static int coro_mode; static void *coro_sp[MAX_THREADS]; static int coro_start_idx;
#if defined(__aarch64__)
__asm__(".text\n.globl _coro_switch\n.p2align 2\n_coro_switch:\n"
        "sub sp, sp, #176\n stp x19, x20, [sp, #0]\n stp x21, x22, [sp, #16]\n stp x23, x24, [sp, #32]\n stp x25, x26, [sp, #48]\n stp x27, x28, [sp, #64]\n stp x29, x30, [sp, #80]\n"
        "stp d8, d9, [sp, #96]\n stp d10, d11, [sp, #112]\n stp d12, d13, [sp, #128]\n stp d14, d15, [sp, #144]\n mov x9, sp\n str x9, [x0]\n mov sp, x1\n"
        "ldp x19, x20, [sp, #0]\n ldp x21, x22, [sp, #16]\n ldp x23, x24, [sp, #32]\n ldp x25, x26, [sp, #48]\n ldp x27, x28, [sp, #64]\n ldp x29, x30, [sp, #80]\n"
        "ldp d8, d9, [sp, #96]\n ldp d10, d11, [sp, #112]\n ldp d12, d13, [sp, #128]\n ldp d14, d15, [sp, #144]\n add sp, sp, #176\n ret\n");
void coro_switch(void **save, void *to);
#else
static void coro_switch(void **save, void *to) { (void)save; (void)to; abort(); }
#endif
static void thread_body(int idx);
static void coro_tramp(void) { thread_body(coro_start_idx); __builtin_trap(); }
static void coro_prepare(int idx) {
    size_t sz = 16u << 20; uint8_t *st = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); if (st == MAP_FAILED) { perror("coro stack"); exit(1); }
    uint64_t *sp = (uint64_t *)(st + sz - 176); memset(sp, 0, 176); sp[11] = (uint64_t)(uintptr_t)coro_tramp; coro_sp[idx] = sp;     // x30 slot: the first `ret` enters the trampoline
}
static void wait_turn(Ctx *c, int idx) { pthread_mutex_lock(&bm); while (c->cur != idx) pthread_cond_wait(&bcv[idx], &bm); pthread_mutex_unlock(&bm); }

static void transfer(Ctx *c, int self, int n) {
    if (coro_mode) { c->cur = n; if (!coro_sp[n]) { coro_prepare(n); coro_start_idx = n; } coro_switch(&coro_sp[self], coro_sp[n]); return; }
    give_turn(c, n); wait_turn(c, self);
}
static void transfer_exit(Ctx *c, int n) {          // the calling thread is finished: never resumed
    if (coro_mode) { void *dummy; c->cur = n; if (!coro_sp[n]) { coro_prepare(n); coro_start_idx = n; } coro_switch(&dummy, coro_sp[n]); }
    give_turn(c, n);
}
uint64_t g_switches;
// every thread waits: sleep until the earliest timer instead of polling.  nanosleep to ~BLUR_SPIN_US before the deadline (macOS timer coalescing makes wake-ups late and uneven), then spin.
static uint64_t g_pace_until;      // deadline of the current vsync wait: only that one is worth spinning for
static void sleep_until_us(uint64_t dl) {
    static long spin = -1; if (spin < 0) spin = getenv("BLUR_SPIN_US") ? atol(getenv("BLUR_SPIN_US")) : 400;
    long spin_now = dl == g_pace_until ? spin : 0;
    for (;;) { uint64_t n = now_us(); if (n >= dl) return; uint64_t rem = dl - n;
        if (rem > (uint64_t)spin_now + 50) { struct timespec ts = {0, (long)((rem - (uint64_t)spin_now) * 1000)}; nanosleep(&ts, NULL); } }
}
static uint64_t last_prog; static int idle_n; static uint64_t idle_since;
// ---- BLUR_PAR=1: guest threads run in parallel on host threads --------------------------------------------------------------------------------------------
// The cooperative scheduler runs every guest thread on one host thread (3 heavy threads x ~15 ms per race frame = 46 ms).  In parallel mode each guest thread is a host
// pthread running lifted code concurrently; a global lock is held only while inside a host API handler (the HLE is not thread-safe), so HLE state, c->cur and the
// handlers' "block and retry" protocol are unchanged.  A blocked thread parks on its own condition variable (releases the lock) and is woken by par_wake() after any
// API that can make a wait satisfiable, or by a timeout (sleep deadlines, 2 ms safety).  Guest atomics are real __atomic ops (lift.py), so lock-free code keeps working.
int par_mode;
static pthread_mutex_t par_lock = PTHREAD_MUTEX_INITIALIZER; static __thread int t_idx = -1;
static uint8_t par_flag[STUB_SLOTS];            // 0 unknown, 1 plain, 2 may unblock waiters, 4 may start threads (|)
static __thread int t_depth;
static uint64_t par_lockwait_ns[MAX_THREADS], par_parked_ns[MAX_THREADS], par_held_ns[MAX_THREADS], par_acq_t[MAX_THREADS]; static int par_stat = -1;
static void par_enter(Ctx *c) {
    if (t_depth++ == 0) { if (par_stat < 0) par_stat = getenv("BLUR_PARSTAT") != NULL ? 1 : 0;
        if (par_stat) { uint64_t t0 = real_us(); pthread_mutex_lock(&par_lock); uint64_t t1 = real_us(); par_lockwait_ns[t_idx] += t1 - t0; par_acq_t[t_idx] = t1; } else pthread_mutex_lock(&par_lock); }
    c->cur = t_idx; }
static void par_leave(void) { if (--t_depth == 0) { if (par_stat > 0) par_held_ns[t_idx] += real_us() - par_acq_t[t_idx]; pthread_mutex_unlock(&par_lock); } }
static void par_wake(Ctx *c) {
    for (int i = 0; i < c->nthreads; i++) { Thread *t = &c->threads[i]; if (i != t_idx && t->used && !t->done && t->par_waiting && ((!t->wk && t->wake_at) ? now_us() >= t->wake_at : thread_can_run(c, t))) pthread_cond_signal(&bcv[i]); }
}
static void *par_thread_main(void *arg);
static void par_start_pending(Ctx *c) {
    for (int i = 1; i < c->nthreads; i++) { Thread *t = &c->threads[i];
        if (t->used && !t->done && !t->suspended && !t->rt_started) { t->rt_started = 1; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 16u << 20);
            if (pthread_create(&t->pth, &at, par_thread_main, (void *)(intptr_t)i)) { fprintf(stderr, "[par] pthread_create failed for guest thread %d\n", i); exit(1); } pthread_detach(t->pth); } }
}
static void par_wait(Ctx *c) {             // called with par_lock held, from inside an API handler that wants to block: park until woken / deadline, then retry
    int self = t_idx; Thread *t = &c->threads[self];
    if (t->done) { par_wake(c); t_depth = 0; pthread_mutex_unlock(&par_lock); pthread_exit(NULL); }
    uint64_t wait_us = 2000;
    if (t->wake_at) { uint64_t n = now_us(); if (t->wake_at <= n) return; if (t->wake_at - n < wait_us) wait_us = t->wake_at - n; }
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += (long)(wait_us * 1000); if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    uint64_t pw0 = par_stat > 0 ? real_us() : 0; if (par_stat > 0) par_held_ns[self] += pw0 - par_acq_t[self];
    t->par_waiting = 1; int rc = pthread_cond_timedwait(&bcv[self], &par_lock, &ts); t->par_waiting = 0; c->cur = self; if (par_stat > 0) { uint64_t n = real_us(); par_parked_ns[self] += n - pw0; par_acq_t[self] = n; }
    { static int on = -1; if (on < 0) on = getenv("BLUR_PARSTAT") != NULL;
      if (on) { static struct { int t; const char *api; uint8_t wk; uint64_t sig, to, us; } h[96]; static int nh; static uint64_t last; int k = 0; uint64_t dur = real_us() - pw0;
        for (; k < nh; k++) if (h[k].t == self && h[k].api == t->last_api && h[k].wk == t->wk) break;
        if (k == nh && nh < 96) { h[nh].t = self; h[nh].api = t->last_api; h[nh].wk = t->wk; nh++; } if (k < 96) { if (rc) h[k].to++; else h[k].sig++; h[k].us += dur; }
        uint64_t n = real_us(); if (n - last > 5000000) { last = n; fprintf(stderr, "[parstat] per thread (ms over the last period): lock-wait / parked / lock-held:"); for (int q = 0; q < c->nthreads; q++) if (par_parked_ns[q] || par_held_ns[q] || par_lockwait_ns[q]) { fprintf(stderr, " t%d=%.0f/%.0f/%.0f", q, par_lockwait_ns[q] / 1e3, par_parked_ns[q] / 1e3, par_held_ns[q] / 1e3); par_lockwait_ns[q] = par_parked_ns[q] = par_held_ns[q] = 0; } fprintf(stderr, "\n");
          fprintf(stderr, "[parstat] top waits (thread api wk: signalled/timeouts, parked ms, avg us):");
          for (int r = 0; r < 12; r++) { int b = -1; uint64_t bu = 0; for (int q = 0; q < nh; q++) if (h[q].us > bu) { bu = h[q].us; b = q; } if (b < 0) break; fprintf(stderr, " | t%d %s wk%d: %llu/%llu %.0fms avg %.0fus", h[b].t, h[b].api ? h[b].api : "?", h[b].wk, (unsigned long long)h[b].sig, (unsigned long long)h[b].to, h[b].us / 1e3, (double)h[b].us / (double)(h[b].sig + h[b].to)); h[b].us = 0; h[b].sig = h[b].to = 0; }
          fprintf(stderr, "\n"); } } }
}
static void par_flags_for(const Stub *s, int idx) {
    // Any API may complete a wait (events set from IO/audio handlers, handle state changes...), so waiters are re-checked after every API except those known to be irrelevant
    // (graphics/input/clock/TLS calls: ~60% of all calls).  par_wake costs one thread_can_run() per parked thread.
    static const char *quiet[] = { "IDirect3D", "IDirectInput", "D3DX", "XInput", "QueryPerformanceCounter", "GetLastError", "SetLastError", "TlsGetValue", "TlsSetValue", "GetCurrentThreadId", "GetTickCount", "timeGetTime", "FlsGetValue", "FlsSetValue", "HeapAlloc", "HeapFree", "HeapSize", "GetProcessHeap", "EnterCriticalSection", "TryEnterCriticalSection", NULL };
    uint8_t f = 1 | 2; for (int i = 0; quiet[i]; i++) if (!strncmp(s->name, quiet[i], strlen(quiet[i]))) f = 1;
    if (!strcmp(s->name, "CreateThread") || !strcmp(s->name, "ResumeThread")) f |= 4; par_flag[idx] = f;
}

static void rt_yield(Ctx *c) {
    if (t_end && real_us() > t_end) { c->exited = 5; finish_process(c); }
    if (par_mode) { if (cur_thread(c)->blocked) par_wait(c); return; }          // forced fairness yields (api_slice) are pointless with real threads; only a blocked thread parks
    int self = c->cur;
    if (c->api_progress != last_prog) { last_prog = c->api_progress; idle_n = 0; idle_since = 0; }
    else if (++idle_n > 2 * c->nthreads + 2) {
        if (!idle_since) idle_since = real_us();
        if (real_us() - idle_since > 20000000ull) {
            fprintf(stderr, "\n[DEADLOCK] all threads blocked for 20s\n");
            for (int i = 0; i < c->nthreads; i++) if (c->threads[i].used && !c->threads[i].done) fprintf(stderr, "  thread %d tid=%x entry=%08x %s\n", i, c->threads[i].tid, c->threads[i].entry, c->threads[i].suspended ? "suspended" : "blocked");
            c->exited = 4; finish_process(c);
        }
        { static int on = -1; if (on < 0) on = getenv("BLUR_IDLESTAT") != NULL;       // which APIs are the blocked threads sitting in while the whole process idles (usleep below)?
          if (on) { static struct { const char *api; uint32_t a0; uint64_t n; } tab[64]; static int nt; static uint64_t tot, t_last;
            for (int i = 0; i < c->nthreads; i++) { Thread *t = &c->threads[i]; if (!t->used || t->done || !t->blocked) continue; int k = 0; for (; k < nt; k++) if (tab[k].api == t->last_api && tab[k].a0 == t->last_a0) break;
              if (k == nt && nt < 64) { tab[nt].api = t->last_api; tab[nt].a0 = t->last_a0; tab[nt++].n = 0; } if (k < 64) tab[k].n++; }
            tot++; uint64_t now = real_us(); if (now - t_last > 5000000) { t_last = now; fprintf(stderr, "[idlestat] %llu idle polls in the last period; blocked-thread API histogram:", (unsigned long long)tot);
              for (int r = 0; r < 6; r++) { int b = -1; uint64_t bn = 0; for (int k = 0; k < nt; k++) if (tab[k].n > bn) { bn = tab[k].n; b = k; } if (b < 0) break; fprintf(stderr, " %s(%08x)=%llu", tab[b].api ? tab[b].api : "?", tab[b].a0, (unsigned long long)bn); tab[b].n = 0; }
              fprintf(stderr, "\n"); tot = 0; } } }
        usleep(200);
    }
    int n = pick_next(c, self);
    if (n < 0 && !g_det_on) { uint64_t dl = thread_next_deadline(c); if (dl) { sleep_until_us(dl); return; } }
    if (n < 0 || n == self) return;
    if (getenv("RT_SCHED")) fprintf(stderr, "[sched] n=%llu yield %d -> %d (nthreads=%d)\n", (unsigned long long)g_apilog_n, self, n, c->nthreads);
    uint64_t py0 = 0; if (g_prof) { py0 = prof_now(); prof_seg_end(); }
    if (g_prof) prof_thread_switch(self);
    g_switches++; transfer(c, self, n);
    if (g_prof) { prof_yield_ns += prof_now() - py0; prof_seg_begin(); }
}

static int g_budget = -1;
static int get_budget(void) { if (g_budget < 0) g_budget = getenv("BLUR_BUDGET") ? atoi(getenv("BLUR_BUDGET")) : 200000; return g_budget; }   // BLUR_BUDGET=n: function entries per time slice (default 200000)
void rt_preempt(Cpu *cpu) { cpu->budget = get_budget(); if (par_mode) return; rt_yield(C); }
void rt_spin(Cpu *cpu) { (void)cpu; if (par_mode) { static __thread int n; if (++n >= 40) { n = 0; sched_yield(); } return; } Thread *t = cur_thread(C); if (++t->spin_n >= 40) { t->spin_n = 0; rt_yield(C); } }

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
static uint32_t ww_lo, ww_hi; static int ww_hits, ww_prot = PROT_READ;
static uintptr_t ww_open[512]; static int ww_nopen;
void ww_rearm(void) { for (int i = 0; i < ww_nopen; i++) mprotect((void *)ww_open[i], 0x4000, ww_prot); ww_nopen = 0; }
static void ww_handler(int sig, siginfo_t *si, void *uctx) {
    (void)sig; (void)uctx; uintptr_t fa = (uintptr_t)si->si_addr; uintptr_t base = (uintptr_t)G;
    if (fa < base + (ww_lo & ~0x3fffu) || fa >= base + ((ww_hi + 0x3fffu) & ~0x3fffu)) { signal(SIGSEGV, SIG_DFL); return; }
    void *bt[10]; int n = backtrace(bt, 10); int host = 0; char names[400] = ""; 
    for (int i = 0; i < n; i++) { Dl_info di; if (dladdr(bt[i], &di) && di.dli_sname) { if (!strcmp(di.dli_sname, "guest_alloc") || !strcmp(di.dli_sname, "rt_api") || !strncmp(di.dli_sname, "h_", 2) || !strncmp(di.dli_sname, "rd", 2) || !strncmp(di.dli_sname, "wr", 2) || strstr(di.dli_sname, "uc_mem")) host = 1; if (i > 1 && strlen(names) < 340) { strcat(names, " "); strcat(names, di.dli_sname); } } }
    if (fa >= base + ww_lo && fa < base + ww_hi && ww_prot == PROT_NONE && ww_hits < 80) { ww_hits++; Thread *t = &C->threads[C->cur]; Cpu *cp = t->cpu;
        fprintf(stderr, "[watchr] read %08x t%d pc=%08x esp=%08x stack:", (uint32_t)(fa - base), C->cur, cp ? cp->pc : 0, cp ? cp->esp : 0);
        if (cp) for (int k = 0; k < 40; k++) { uint32_t w = *(uint32_t *)(G + cp->esp + 4 * k); if (w >= 0x401000 && w < 0xe40000) fprintf(stderr, " %08x", w); } fprintf(stderr, "\n"); }
    else if (!host && ww_hits < (getenv("BLUR_WATCHMAX") ? atoi(getenv("BLUR_WATCHMAX")) : 60) && ww_prot != PROT_NONE && (uint32_t)(fa - base) >= ww_lo && (uint32_t)(fa - base) < ww_hi) { ww_hits++; fprintf(stderr, "[watchw] %s %08x (thread %d, val now %08x) by:%s\n", ww_prot == PROT_NONE ? "access to" : "write to", (uint32_t)(fa - base), C->cur, M32((uint32_t)(fa - base) & ~3u), names); }
    uintptr_t pg = fa & ~(uintptr_t)0x3fff; mprotect((void *)pg, 0x4000, PROT_READ | PROT_WRITE); if (ww_nopen < 512) ww_open[ww_nopen++] = pg;
}
void ww_arm(void) {
    const char *e = getenv("BLUR_WATCHW"); if (!e) { e = getenv("BLUR_WATCHR"); if (!e) return; ww_prot = PROT_NONE; } ww_lo = (uint32_t)strtoul(e, 0, 16); const char *d = strchr(e, '-'); ww_hi = d ? (uint32_t)strtoul(d + 1, 0, 16) : ww_lo + 4;
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = ww_handler; sa.sa_flags = SA_SIGINFO; sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    uint32_t lo = ww_lo & ~0x3fffu, hi = (ww_hi + 0x3fff) & ~0x3fffu; mprotect(G + lo, hi - lo, ww_prot);
    fprintf(stderr, "[watchw] armed %08x-%08x\n", ww_lo, ww_hi);
}
// ---- API dispatch ------------------------------------------------------------------------------------------------------------
uint32_t g_api_ret, g_api_esp;
// BLUR_ARGREC=<frame>: per API stub call counts + the first 6 distinct (caller, args) tuples, dumped to stderr when that frame is presented
extern uint64_t g_frames_pub;
static struct { const void *s; const char *kind, *dll, *name; int nargs; uint64_t n; int nrec; struct { uint32_t ret, a[8]; } r[6]; } *argtab; static int nargtab, argrec_on = -1; static uint64_t argrec_frame;
extern hle_fn g_triv_fns[]; extern uint32_t com_ok(Ctx *, uint32_t *);
static const char *argrec_kind(const Stub *s) { static const char *nm[] = {"t_0","t_1","t_m1","t_handle","t_err2","t_fail","t_nodata","t_nodev"}; if (s->fn == com_ok) return "COM-OK-STUB"; for (int i = 0; i < 8; i++) if (s->fn == g_triv_fns[i]) return nm[i]; return ""; }
static void argrec_dump(void) { for (int i = 0; i < nargtab; i++) { fprintf(stderr, "[argrec] %s!%s n=%llu nargs=%d %s\n", argtab[i].dll, argtab[i].name, (unsigned long long)argtab[i].n, argtab[i].nargs, argtab[i].kind); for (int k = 0; k < argtab[i].nrec; k++) { fprintf(stderr, "[argrec]    ret=%08x:", argtab[i].r[k].ret); for (int j = 0; j < argtab[i].nargs && j < 8; j++) fprintf(stderr, " %08x", argtab[i].r[k].a[j]); fprintf(stderr, "\n"); } } }
static void argrec_note(const Stub *s, uint32_t ret, const uint32_t *a) {
    if (argrec_on < 0) { const char *e = getenv("BLUR_ARGREC"); argrec_on = e != NULL; argrec_frame = e ? strtoull(e, 0, 0) : 0; if (argrec_on) argtab = calloc(8192, sizeof *argtab); }
    if (!argrec_on) return;
    if (argrec_on == 1 && g_frames_pub >= argrec_frame) { argrec_on = 2; argrec_dump(); return; }
    if (argrec_on != 1) return;
    int i; for (i = 0; i < nargtab; i++) if (argtab[i].s == s) break;
    if (i == nargtab) { if (nargtab >= 8192) return; nargtab++; argtab[i].s = s; argtab[i].kind = argrec_kind(s); argtab[i].dll = s->dll; argtab[i].name = s->name; argtab[i].nargs = s->nargs; }
    argtab[i].n++;
    for (int k = 0; k < argtab[i].nrec; k++) { int same = argtab[i].r[k].ret == ret; for (int j = 0; j < 8 && same; j++) if (argtab[i].r[k].a[j] != a[j]) same = 0; if (same) return; }
    if (argtab[i].nrec < 6) { argtab[i].r[argtab[i].nrec].ret = ret; memcpy(argtab[i].r[argtab[i].nrec].a, a, 32); argtab[i].nrec++; }
}

static uint32_t cont_addr;      // stub a guest callback returns to, when the API handler wants its result adjusted afterwards

// Run one host API called with the guest stack [ret][args...]; returns the guest address to continue at.
void audio_tick(Ctx *c);
// vsync emulation: the game presents flat out in loading screens (2500 Present/s measured, thousands of D3D objects per second, a flooded viewer); a real
// display would hold Present to the refresh rate.  The thread sleeps (other guest threads keep running) until the next refresh tick.  BLUR_NOVSYNC=1 disables, BLUR_REFRESH=<hz> (default 60).
extern volatile uint32_t g_net_ack; extern volatile int g_net_ack_seen;
static const Stub *present_stub;
// BLUR_FRAMESTAT=1: per-frame Present-to-Present time histogram (p50/p95/p99/max, frames > 20 and > 33 ms) every 10 s, and every API call that held the global lock > 2 ms
static struct { const char *api; uint32_t us; } fs_top[6]; static uint64_t fs_api_us, fs_ncalls;     // main (Present-calling) thread: slowest non-blocked API calls of the current frame
static void fs_note(const char *api, uint64_t d) { fs_api_us += d; fs_ncalls++; int w = 0; for (int i = 1; i < 6; i++) if (fs_top[i].us < fs_top[w].us) w = i; if (d > fs_top[w].us) { fs_top[w].api = api; fs_top[w].us = (uint32_t)d; } }
static int fs_cmp(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }
static void framestat(void) {
    static int on = -1; if (on < 0) on = getenv("BLUR_FRAMESTAT") != NULL; if (!on) return;
    static uint64_t last, t10; static uint32_t dts[4096]; static int n; uint64_t now = real_us();
    if (last && n < 4096) dts[n++] = (uint32_t)(now - last);
    if (last && now - last > 30000) { fprintf(stderr, "[hitch] frame %.1f ms: API %.1f ms in %llu calls, guest/other %.1f ms; slowest calls:", (now - last) / 1e3, fs_api_us / 1e3, (unsigned long long)fs_ncalls, ((now - last) - fs_api_us) / 1e3);
        for (int i = 0; i < 6; i++) if (fs_top[i].us > 300) fprintf(stderr, " %s=%.1fms", fs_top[i].api, fs_top[i].us / 1e3); fprintf(stderr, "\n"); }
    memset(fs_top, 0, sizeof fs_top); fs_api_us = fs_ncalls = 0; last = now; if (!t10) t10 = now;
    if (now - t10 >= 10000000ull && n > 20) {
        qsort(dts, (size_t)n, 4, fs_cmp); int o20 = 0, o33 = 0; for (int i = 0; i < n; i++) { if (dts[i] > 20000) o20++; if (dts[i] > 33400) o33++; }
        fprintf(stderr, "[framestat] %d frames: p50 %.1f p95 %.1f p99 %.1f max %.1f ms; >20ms: %d, >33ms: %d\n", n, dts[n / 2] / 1e3, dts[n * 95 / 100] / 1e3, dts[n * 99 / 100] / 1e3, dts[n - 1] / 1e3, o20, o33); n = 0; t10 = now; }
}
static void pace_present(Ctx *c) {
    framestat();
    static uint32_t sent;
    // viewer back-pressure: the page acks every presented frame; do not run more than BLUR_MAXLAG (4) frames ahead of it (it would only queue them in browser memory = latency). Gives up after 100 ms (hidden tab, old page).
    if (g_net_ack_seen && net_client_connected()) { static int maxlag = -1; if (maxlag < 0) maxlag = getenv("BLUR_MAXLAG") ? atoi(getenv("BLUR_MAXLAG")) : 4; sent++;
        if (g_det_on && (int32_t)(sent - g_net_ack) > maxlag) { uint64_t until = real_us() + 100000; while ((int32_t)(sent - g_net_ack) > maxlag && real_us() < until) usleep(200); }   // BLUR_DET: wait in REAL time without yielding/ticking the virtual clock, otherwise the number of virtual ticks (= guest progress) would depend on when the viewer's acks arrive
        else if ((int32_t)(sent - g_net_ack) > maxlag) { uint64_t until = now_us() + 100000; Thread *t = cur_thread(c); t->wake_at = until; t->wk = 2; t->blocked = 1; g_pace_until = until;
            while ((int32_t)(sent - g_net_ack) > maxlag && now_us() < until) { t->wake_at = now_us() + 1000; rt_yield(c); } g_pace_until = 0; t->blocked = 0; t->wake_at = 0; t->wk = 0; if (g_prof) prof_seg_end(); } }
    static uint64_t next; static uint64_t period; static int on = -1;
    if (on < 0) { on = !getenv("BLUR_NOVSYNC") && !g_det_on; uint32_t hz = getenv("BLUR_REFRESH") ? (uint32_t)atoi(getenv("BLUR_REFRESH")) : 60; period = 1000000ull / (hz ? hz : 60); }
    if (!on) return;
    { extern uint64_t g_last_draws; if (g_last_draws <= 8) { next = 0; return; } }       // loading screens (<= 8 draws): the game loads a slice per frame, so pacing them would triple the load time (wasm-core measured 51 s -> 69 s)
    uint64_t n = now_us(); if (!next || n > next + 2 * period) next = n;       // behind by more than 2 frames: do not burst to catch up
    next += period; if (n >= next) return;
    Thread *t = cur_thread(c); t->wake_at = next; t->wk = 2; t->blocked = 1; g_pace_until = next;
    while (now_us() < next) rt_yield(c);
    g_pace_until = 0;
    if (g_prof) prof_seg_end();
    t->blocked = 0; t->wake_at = 0; t->wk = 0;
}
static uint32_t rt_api_locked(Cpu *cpu, uint32_t addr) {
    Ctx *c = C; Stub *s = &c->stubs[(addr - STUB_BASE) / 16];
    if (addr == cont_addr) {                                  // a callback requested by an API handler returned
        Thread *t = cur_thread(c); Cont k = t->cont[--t->ncont];
        if (!k.use_result) cpu->eax = k.value;
        c->api_progress++;
        return k.real_ret;
    }
    audio_tick(c);
    uint32_t esp = cpu->esp, ret = M32(esp), a[16]; memcpy(a, G + esp + 4, sizeof a);
    if (g_prof) { prof_api++; prof_note_api(c, s); prof_seg_begin_stub((int)((addr - STUB_BASE) / 16)); }
    if (!s->fn) {
        fprintf(stderr, "\n[UNIMPLEMENTED] %s!%s  nargs=%d %s  ret=%08x\n  args:", s->dll, s->name, s->nargs, s->cdecl_ ? "cdecl" : "stdcall", ret);
        for (int i = 0; i < 6; i++) fprintf(stderr, " %08x", a[i]);
        fprintf(stderr, "\n"); c->exited = 2; finish_process(c);
    }
    { static const char *lst = (const char *)1; if (lst == (const char *)1) lst = getenv("BLUR_APITRACE");      // BLUR_APITRACE=CreateFileA,GetFileSize: distinct call chains (code addresses on the guest stack) + first args
      if (lst && strstr(lst, s->name)) { static struct { const Stub *s; uint32_t h; } seen[256]; static int ns; uint32_t ch[10]; int n = 0; uint32_t hh = 0;
        for (int k = 0; k < 120 && n < 10; k++) { uint32_t w = M32(esp + 4 * k); if (w >= 0x401000 && w < 0xe40000) { ch[n++] = w; hh = hh * 31 + w; } }
        int dup = 0; for (int i = 0; i < ns; i++) if (seen[i].s == s && seen[i].h == hh) dup = 1;
        if (!dup && ns < 256) { seen[ns].s = s; seen[ns++].h = hh; { char nm[160] = ""; if (strstr(s->name, "File") || strstr(s->name, "Directory")) { rd_cstr(c, a[0], nm, sizeof nm); } fprintf(stderr, "[apitrace] t%d %s(%08x,%08x,%08x,%08x) \"%s\" chain:", c->cur, s->name, a[0], a[1], a[2], a[3], nm); } for (int i = 0; i < n; i++) fprintf(stderr, " %08x", ch[i]); fprintf(stderr, "\n"); } } }
    uint32_t r;
    for (;;) {
        if (c->verbose) fprintf(stderr, "[api t%d] %s!%s(%08x,%08x,%08x,%08x) ret=%08x esp=%08x\n", c->cur, s->dll, s->name, a[0], a[1], a[2], a[3], ret, esp);
        cur_thread(c)->wk = 0;
        g_api_ret = ret; g_api_esp = esp; g_cur_api = s->name; g_det_ticks++; apilog_call(c->cur, s->name, a); argrec_note(s, ret, a); { Thread *tt = cur_thread(c); tt->last_api = s->name; tt->last_ret = ret; tt->last_a0 = a[0]; tt->last_a1 = a[1]; tt->n_api++; } s->calls++; r = s->fn(c, a);
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
    if (g_prof) prof_seg_end();
    if (!present_stub) { if (s->name[0] == 'I' && !strcmp(s->name, "IDirect3DDevice9::Present")) present_stub = s; }
    if (s == present_stub) pace_present(c);
    if (c->yield_req) { c->yield_req = 0; rt_yield(c); }
    else if (c->api_slice && ++c->api_since_switch >= c->api_slice) { c->api_since_switch = 0; rt_yield(c); }
    return next;
}
static uint32_t rt_api(Cpu *cpu, uint32_t addr) {
    if (!par_mode) return rt_api_locked(cpu, addr);
    Ctx *c = C; int si = (int)((addr - STUB_BASE) / 16); par_enter(c);
    static int fs_on = -1; if (fs_on < 0) fs_on = getenv("BLUR_FRAMESTAT") != NULL; uint64_t fs0 = fs_on ? real_us() : 0;
    uint32_t r = rt_api_locked(cpu, addr);
    if (fs_on) { uint64_t d = real_us() - fs0; static int shown; if (t_idx == 0 && strcmp(c->stubs[si].name, "IDirect3DDevice9::Present")) fs_note(c->stubs[si].name, d);
      if (d > 2000 && shown < 200 && !c->threads[t_idx].blocked) { shown++; fprintf(stderr, "[slowapi] t%d %s took %.1f ms (lock held; includes any wait inside the handler)\n", t_idx, c->stubs[si].name, d / 1e3); } }
    uint8_t f = par_flag[si]; if (!f) { par_flags_for(&c->stubs[si], si); f = par_flag[si]; }
    if (f & 4) par_start_pending(c);
    if (f & 2) par_wake(c);
    par_leave(); return r;
}

static void hook_dump(void);
static void rt_missing(Cpu *cpu, uint32_t target) {
    fprintf(stderr, "\n[rt] jump to code that was not recompiled: %08x (esp=%08x [esp]=%08x eax=%08x ecx=%08x) [esp+4]=%08x [esp+8]=%08x\n", target, cpu->esp, M32(cpu->esp), cpu->eax, cpu->ecx, M32(cpu->esp + 4), M32(cpu->esp + 8));
    {   // post-mortem: registers, last functions entered by this thread, and the guest stack with code-address candidates
        Thread *ct = cur_thread(C); fprintf(stderr, "[miss] thread %d tid=%x entry=%08x stack=%08x-%08x teb=%08x last_api=%s ret=%08x n_api=%u\n", C->cur, ct->tid, ct->entry, ct->stack_lo, ct->stack_hi, ct->teb, ct->last_api ? ct->last_api : "-", ct->last_ret, ct->n_api);
        fprintf(stderr, "[miss] regs eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n", cpu->eax, cpu->ecx, cpu->edx, cpu->ebx, cpu->esp, cpu->ebp, cpu->esi, cpu->edi);
        if (ring_on) { int t = C->cur; fprintf(stderr, "[miss] last functions (oldest first, addr@esp):"); for (int k = 48; k >= 1; k--) { uint32_t i = (ring_i[t] - k) & (RING_N - 1); fprintf(stderr, " %x@%x", ring_a[t][i], ring_s[t][i]); } fprintf(stderr, "\n"); }
        fprintf(stderr, "[miss] stack:"); for (int k = 0; k < 48; k++) { uint32_t v = M32(cpu->esp + 4 * k); fprintf(stderr, " %s%08x", (v >= 0x401000 && v < 0x2070000) ? "*" : "", v); } fprintf(stderr, "\n");
        for (int k = 0; k < C->nthreads; k++) { Thread *t = &C->threads[k]; if (!t->used) continue; Cpu *tc = t->cpu; fprintf(stderr, "[miss] thread %2d %s%s%s esp=%08x apis=%u last=%s(%08x,%08x) ret=%08x\n", k, t->done ? "done " : "", t->suspended ? "susp " : "", t->blocked ? "blocked " : "", tc ? tc->esp : 0, t->n_api, t->last_api ? t->last_api : "-", t->last_a0, t->last_a1, t->last_ret); }
    }
    { extern void d3d9_dbg_obj(Ctx *, uint32_t, const char *); d3d9_dbg_obj(C, cpu->esi, "esi"); d3d9_dbg_obj(C, cpu->edi, "edi"); for (int k = 0; k < 16; k++) { uint32_t v = M32(cpu->esp + 4 * k); if (v >= 0x20000000u && v < 0x58000000u) d3d9_dbg_obj(C, v, "stk"); } }
    if (getenv("RT_DUMP")) { uint32_t a = (uint32_t)strtoul(getenv("RT_DUMP"), 0, 16); fprintf(stderr, "[dump %08x]", a); for (int k = 0; k < 24; k++) fprintf(stderr, " %08x", M32(a + 4 * k)); fprintf(stderr, "\n"); }
    heapchk_scan(C, "at crash");
    { uint32_t rv[8] = { cpu->eax, cpu->ecx, cpu->edx, cpu->ebx, cpu->ebp, cpu->esi, cpu->edi, target }; static const char *rn[8] = { "eax", "ecx", "edx", "ebx", "ebp", "esi", "edi", "target" };
      for (int k = 0; k < 8; k++) heapdbg_describe(C, rv[k], rn[k]); for (int k = 0; k < 32; k++) { char tg[16]; snprintf(tg, sizeof tg, "[esp+%d]", 4 * k); heapdbg_describe(C, M32(cpu->esp + 4 * k), tg); } }
    // only image/msvcr90 addresses are recompile candidates (garbage targets from heap corruption are not); BLUR_LEARNED overrides the output file
    if ((target >= 0x400000 && target < 0x2070000) || (target >= 0x78520000 && target < 0x78600000)) { const char *lp = getenv("BLUR_LEARNED"); FILE *lf = fopen(lp ? lp : "../recomp/learned_x.txt", "a"); if (lf) { fprintf(lf, "%08x\n", target); fclose(lf); } }
    hook_dump(); fflush(stderr); C->exited = 6; finish_process(C);
}

// The trampoline every lifted function ends in (as a tail call): continue at cpu->pc.
// BLUR_HOOK=addr[,addr..]: log entries (cdecl args) and return values of the guest functions at those addresses
static void lua_init(void);
static uint32_t hook_addr[16]; static int nhook = -1, hook_user; static struct { uint32_t ret, esp, fn; } hook_pend[256]; static int nhook_pend; static int hook_budget = 400; static int hook_budget_init;
static void hook_init(void) { nhook = 1; lua_init(); hook_addr[0] = 0; const char *e = getenv("BLUR_HOOK"); if (nhook) { nhook = 1; hook_addr[0] = 0xffffffffu; } while (e && *e && nhook < 16) { hook_addr[nhook++] = (uint32_t)strtoul(e, (char **)&e, 16); if (*e == ',') e++; } hook_user = nhook > 1 || getenv("BLUR_HOOK_TAIL") != NULL; }
// BLUR_LUATRACE=1: log every entry into one of the 255 Lua C bindings (table from ../work/net-re/script_bindings.txt: <slot> <name> <fn address>)
static struct { uint32_t a; char name[48]; uint32_t n; long first; double first_t; } lua_b[300]; static int nlua, lua_quiet; extern uint64_t g_frames_pub;
double lua_binding_first_time(const char *nm) { for (int i = 0; i < nlua; i++) if (lua_b[i].n && !strcmp(lua_b[i].name, nm)) return lua_b[i].first_t; return -1; }
long lua_binding_first_frame(const char *nm) { for (int i = 0; i < nlua; i++) if (lua_b[i].n && !strcmp(lua_b[i].name, nm)) return lua_b[i].first; return -1; }
static void lua_init(void) { const char *p = getenv("BLUR_LUATRACE_TABLE"); FILE *f = fopen(p ? p : "../work/net-re/script_bindings.txt", "r"); char l[256]; if (!f) { if (getenv("BLUR_LUATRACE")) fprintf(stderr, "[lua] no binding table\n"); return; }
    while (fgets(l, sizeof l, f) && nlua < 300) { unsigned slot, fn; char nm[64]; if (sscanf(l, "%x %63s %x", &slot, nm, &fn) == 3) { lua_b[nlua].a = fn; snprintf(lua_b[nlua].name, 48, "%s", nm); nlua++; } } fclose(f); if (getenv("BLUR_LUATRACE")) fprintf(stderr, "[lua] tracing %d bindings\n", nlua); else lua_quiet = 1; }
static void seed_account(void) {       // BLUR_SEEDACCT=1: give the primary profile an account name/password (UTF-16 at profile+0xff9 / +0x1019) so ReadOnlineAccountDetails returns true
    int32_t idx = *(int32_t *)(G + 0x10e5874); if (idx < 0 || idx >= 7) { fprintf(stderr, "[seed] no primary profile yet (idx=%d)\n", idx); return; }
    uint32_t prof = 0x1212180u + 0x10 + (uint32_t)idx * 0x2100u; const char *nm = "BlurPlayer", *pw = "blurpass1";
    for (int k = 0; ; k++) { *(uint16_t *)(G + prof + 0xff9 + 2 * k) = (uint8_t)nm[k]; if (!nm[k]) break; } for (int k = 0; ; k++) { *(uint16_t *)(G + prof + 0x1019 + 2 * k) = (uint8_t)pw[k]; if (!pw[k]) break; }
    fprintf(stderr, "[seed] account name/password written into profile %08x\n", prof); }
static void lua_check(Cpu *cpu, uint32_t pc) { for (int i = 0; i < nlua; i++) if (lua_b[i].a == pc) { lua_b[i].n++; if (lua_b[i].n == 1) { lua_b[i].first = (long)g_frames_pub; lua_b[i].first_t = (double)real_us() / 1e6; } if (!strcmp(lua_b[i].name, "ReadOnlineAccountDetails") && !(getenv("BLUR_SEEDACCT") && getenv("BLUR_SEEDACCT")[0] == '0')) seed_account(); static uint64_t t0; uint64_t t = real_us(); if (!t0) t0 = t;
    if (!lua_quiet && (lua_b[i].n <= 40 || (lua_b[i].n & (lua_b[i].n - 1)) == 0)) fprintf(stderr, "[lua] %s #%u frame=%llu t=%llums thread=%d L=%08x\n", lua_b[i].name, lua_b[i].n, (unsigned long long)g_frames_pub, (unsigned long long)((t - t0) / 1000), C->cur, M32(cpu->esp + 4)); return; } }
static struct { uint32_t pc, ret, a[4]; int t; } hook_ring[128]; static unsigned hook_n;    // BLUR_HOOK_TAIL=1: keep the last 128 entries, dumped when the run dies on a missing target
static void hook_dump(void) { for (unsigned i = hook_n > 128 ? hook_n - 128 : 0; i < hook_n; i++) { typeof(hook_ring[0]) *e = &hook_ring[i & 127]; fprintf(stderr, "[hooktail] %08x t%d ret=%08x args %08x %08x %08x %08x\n", e->pc, e->t, e->ret, e->a[0], e->a[1], e->a[2], e->a[3]); } }
static void hook_check(Cpu *cpu, uint32_t pc) {
    if (!hook_budget_init) { hook_budget_init = 1; if (getenv("BLUR_HOOKMAX")) hook_budget = atoi(getenv("BLUR_HOOKMAX")); }
    if (nlua) lua_check(cpu, pc);
    { static uint32_t pat = 1, pa, pv; static uint32_t ppc; if (pat == 1) { pat = 0; const char *e = getenv("BLUR_POKE"); if (e) { ppc = (uint32_t)strtoul(e, (char **)&e, 16); if (*e == ',') pa = (uint32_t)strtoul(e + 1, (char **)&e, 16); if (*e == ',') pv = (uint32_t)strtoul(e + 1, 0, 16); pat = 2; } }
      if (pat == 2 && pc == ppc) { G[pa] = (uint8_t)pv; fprintf(stderr, "[poke] at pc %08x: [%08x] = %02x\n", pc, pa, pv); } }
    for (int i = 0; i < nhook_pend; i++) if (hook_pend[i].ret == pc && hook_pend[i].esp == cpu->esp) { if (hook_budget > 0) { hook_budget--; fprintf(stderr, "[hook] ret from %08x eax=%08x (t%d)\n", hook_pend[i].fn, cpu->eax, C->cur); } hook_pend[i] = hook_pend[--nhook_pend]; break; }
    for (int k = 0; k < nhook; k++) if (pc == hook_addr[k]) {
        if (getenv("BLUR_HOOK_TAIL")) { typeof(hook_ring[0]) *e = &hook_ring[hook_n++ & 127]; e->pc = pc; e->t = C->cur; e->ret = M32(cpu->esp); for (int q = 0; q < 4; q++) e->a[q] = M32(cpu->esp + 4 + 4 * q); }
        else if (hook_budget > 0) { hook_budget--; fprintf(stderr, "[hook] enter %08x t%d ecx=%08x edx=%08x ret=%08x args %08x %08x %08x %08x\n", pc, C->cur, cpu->ecx, cpu->edx, M32(cpu->esp), M32(cpu->esp + 4), M32(cpu->esp + 8), M32(cpu->esp + 12), M32(cpu->esp + 16)); }
        if (nhook_pend < 256) { hook_pend[nhook_pend].ret = M32(cpu->esp); hook_pend[nhook_pend].esp = cpu->esp + 4; hook_pend[nhook_pend++].fn = pc; }
    }
}
static struct { uint32_t pc; uint32_t n; } prof_t[MAX_THREADS][512]; static int prof_on = -1;
static void prof_hit(int t, uint32_t pc) { pc &= ~0xfu; unsigned h = (pc >> 4) * 2654435761u >> 23; for (int k = 0; k < 512; k++) { unsigned i = (h + (unsigned)k) & 511; if (prof_t[t][i].pc == pc || !prof_t[t][i].pc) { prof_t[t][i].pc = pc; prof_t[t][i].n++; return; } } }
static void prof_dump(void) { for (int t = 0; t < MAX_THREADS; t++) { int any = 0; for (int i = 0; i < 512; i++) if (prof_t[t][i].pc) any = 1; if (!any) continue;
    unsigned long long tot = 0; for (int i = 0; i < 512; i++) tot += prof_t[t][i].n; fprintf(stderr, "[pcprof] thread %d total=%llu top return/entry blocks (16-byte buckets):", t, tot);
    for (int r = 0; r < 12; r++) { int b = -1; for (int i = 0; i < 512; i++) if (prof_t[t][i].n && (b < 0 || prof_t[t][i].n > prof_t[t][b].n)) b = i; if (b < 0) break; fprintf(stderr, " %08x:%u", prof_t[t][b].pc, prof_t[t][b].n); prof_t[t][b].n = 0; } fprintf(stderr, "\n"); } }
// fast dispatch table: interleaved {key, fn}, multiplicative hash, linear probing into a padded tail (no wrap, no mask); used unless a diagnostic hook is on
typedef struct { uint32_t k; GuestFn f; } FEnt;
static FEnt *ftab; static unsigned fshift; static int rt_slow;
static void build_fast(void) {
    unsigned total = rt_table_n + rt_table_x_n, bits = 4; while ((1u << bits) < total * 2) bits++;
    fshift = 32 - bits; ftab = calloc((size_t)(1u << bits) + 512, sizeof(FEnt));
    for (unsigned i = 0; i < total; i++) {
        uint32_t a = i < rt_table_n ? rt_table[i].a : rt_table_x[i - rt_table_n].a; GuestFn f = i < rt_table_n ? rt_table[i].f : rt_table_x[i - rt_table_n].f;
        { int special = 0; for (int q = 0; q < nlua; q++) if (lua_b[q].a == a) special = 1; if (special) continue; }     // Lua binding entries must reach hook_check() (account seeding / tracing)
        FEnt *e = &ftab[(a * 2654435761u) >> fshift]; while (e->k && e->k != a) e++;
        if (e - ftab >= (long)(1u << bits) + 500) { fprintf(stderr, "[rt] fast table overflow\n"); exit(1); }
        e->k = a; e->f = f;
    }
}
void rt_dispatch(Cpu *cpu) {
    for (;;) {
        uint32_t pc = cpu->pc;
        if (__builtin_expect(!rt_slow, 1)) {                 // hot path: one probe sequence, tail call (RET_MAGIC misses and falls through)
            if (pc - STUB_BASE < STUB_SLOTS * 16u) {         // host API stub (~44k calls per race frame): straight to the handler; no failed hash probe, no hook_check
                if (__builtin_expect(g_prof != 0, 0)) prof_dispatch++;
                if (nhook < 0) hook_init();
                if (hook_user) hook_check(cpu, pc);          // BLUR_HOOK may name stub addresses
                cpu->pc = rt_api(cpu, pc); continue; }
            FEnt *e = &ftab[(pc * 2654435761u) >> fshift];
            while (e->k && e->k != pc) e++;
            if (__builtin_expect(e->k == pc, 1)) MUSTTAIL return e->f(cpu);
        }
        if (g_prof) prof_dispatch++;
        if (prof_on < 0) { prof_on = getenv("BLUR_PCPROF") != NULL; const char *e = getenv("BLUR_PCCOUNT"); while (e && *e && npcc < 8) { pcc[npcc++].pc = (uint32_t)strtoul(e, (char **)&e, 16); if (*e == ',') e++; } }
        if (prof_on) prof_hit(C->cur, pc);
        if (npcc) for (int q = 0; q < npcc; q++) if (pcc[q].pc == pc) { pcc[q].n[C->cur & 31]++; if (!pcc[q].first) pcc[q].first = 1; }
        if (nhook < 0) hook_init();
        if (nhook && (hook_user || pc - STUB_BASE >= STUB_SLOTS * 16u)) hook_check(cpu, pc);      // stubs are never Lua bindings: skip the 255-entry scan on every API call
        if (pc - STUB_BASE < STUB_SLOTS * 16u) { cpu->pc = rt_api(cpu, pc); continue; }
        if (pc == RET_MAGIC) return;                         // the thread's start routine returned
        GuestFn f = lookup(pc);
        if (!f) {
            if (cold_mode < 0) cold_mode = getenv("BLUR_COLD") ? atoi(getenv("BLUR_COLD")) : 1;
            if (cold_mode) { int ok; if (par_mode) par_enter(C); ok = rt_cold(cpu); if (par_mode) par_leave(); if (ok) continue; }          // interpreter fallback (one shared interpreter: serialised in parallel mode)
            rt_missing(cpu, pc);
        }
        MUSTTAIL return f(cpu);
    }
}

// ---- threads -----------------------------------------------------------------------------------------------------------------
static void init_cpu(Cpu *cpu, uint32_t teb) { memset(cpu, 0, sizeof *cpu); cpu->budget = get_budget(); cpu->teb = teb; cpu->fcw = 0x27f; cpu->mxcsr = 0x1f80; }

static void thread_done(Ctx *c, int idx, uint32_t code) {
    if (par_mode) { par_enter(c); Thread *t = &c->threads[idx]; t->done = 1; t->exit_code = code; hle_thread_exit(c, t); if (t->handle) c->handles[(t->handle - 0x100) / 4].signaled = 1;
        c->api_progress++; if (idx == 0) { c->exited = 1; c->exit_code = code; finish_process(c); } par_wake(c); par_leave(); return; }
    Thread *t = &c->threads[idx]; t->done = 1; t->exit_code = code; hle_thread_exit(c, t);
    if (t->handle) c->handles[(t->handle - 0x100) / 4].signaled = 1;
    if (c->verbose) fprintf(stderr, "[thread] tid=%x finished code=%u\n", t->tid, code);
    if (idx == 0) { c->exited = 1; c->exit_code = code; finish_process(c); }
    c->api_progress++;
    int n = pick_next(c, idx);
    if (n < 0) { c->exited = 1; finish_process(c); }
    transfer_exit(c, n);
}

static void thread_body(int idx) {
    Ctx *c = C; Thread *t = &c->threads[idx];
    Cpu *cpu = calloc(1, sizeof(Cpu)); t->cpu = cpu; init_cpu(cpu, t->teb);
    uint32_t esp = t->stack_hi - 0x40; esp -= 4; M32(esp) = t->param; esp -= 4; M32(esp) = RET_MAGIC; cpu->esp = esp;
    cpu->pc = t->entry; rt_dispatch(cpu);
    thread_done(c, idx, cpu->eax);
}
static void *thread_main(void *arg) { int idx = (int)(intptr_t)arg; wait_turn(C, idx); thread_body(idx); return NULL; }
static void *par_thread_main(void *arg) { t_idx = (int)(intptr_t)arg; thread_body(t_idx); return NULL; }

// Spin diagnosis: a guest loop that stays inside one lifted function never reaches a scheduler check, so the stale Cpu snapshot below says nothing about it. The watchdog
// therefore signals the host thread running the guest (SIGUSR2) and the handler prints the host PC with its symbol (f_<guest address> = the lifted function that spins).
#include <pthread.h>
static pthread_t g_main_thr; static volatile int g_spin_dumped;
static void spin_sig(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si; ucontext_t *uc = uc_; uintptr_t pc = 0, sp = 0;
#if defined(__aarch64__)
    pc = (uintptr_t)uc->uc_mcontext->__ss.__pc; sp = (uintptr_t)uc->uc_mcontext->__ss.__sp;
#elif defined(__x86_64__)
    pc = (uintptr_t)uc->uc_mcontext->__ss.__rip; sp = (uintptr_t)uc->uc_mcontext->__ss.__rsp;
#endif
    Dl_info di; const char *nm = "?"; uintptr_t off = 0; if (pc && dladdr((void *)pc, &di) && di.dli_sname) { nm = di.dli_sname; off = pc - (uintptr_t)di.dli_saddr; }
    fprintf(stderr, "[spin] host pc=%p sp=%p in %s+0x%lx (guest function f_<addr> = the lifted function the guest thread is spinning in)\n", (void *)pc, (void *)sp, nm, (unsigned long)off); g_spin_dumped = 1;
}
static void *watchdog(void *arg) {
    Ctx *c = arg; uint64_t last = 0; int still = 0;
    for (;;) { sleep(2); uint64_t now = g_switches + c->api_progress; if (now == last) { if (++still >= 2) { fprintf(stderr, "[watchdog] no scheduling progress; cur=%d nthreads=%d\n", c->cur, c->nthreads);
            if (!g_spin_dumped) { pthread_kill(g_main_thr, SIGUSR2); for (int w = 0; w < 50 && !g_spin_dumped; w++) usleep(20000); g_spin_dumped = 0; }
            { Thread *ct = &c->threads[c->cur]; Cpu *cp = ct->cpu; if (cp) { fprintf(stderr, "[watchdog] thread %d last Cpu snapshot: pc=%08x eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x last_api=%s\n", c->cur, cp->pc, cp->eax, cp->ecx, cp->edx, cp->ebx, cp->esp, cp->ebp, cp->esi, cp->edi, ct->last_api ? ct->last_api : "-");
                fprintf(stderr, "[watchdog] stack:"); for (int k = 0; k < 24; k++) fprintf(stderr, " %08x", M32(cp->esp + 4 * k)); fprintf(stderr, "\n"); if (cp->esi > 0x10000) { for (int k = 0; k < c->nthreads; k++) { Thread *t = &c->threads[k]; if (t->used) fprintf(stderr, "[watchdog] thread %2d %s%s%s apis=%u last=%s(%08x,%08x) ret=%08x\n", k, t->done ? "done " : "", t->suspended ? "susp " : "", t->blocked ? "blocked " : "", t->n_api, t->last_api ? t->last_api : "-", t->last_a0, t->last_a1, t->last_ret); }
                fprintf(stderr, "[watchdog] [esi]:"); for (int k = 0; k < 40; k++) fprintf(stderr, " %08x", M32(cp->esi + 4 * k)); fprintf(stderr, "\n"); uint32_t n = M32(cp->esi + 0x44); fprintf(stderr, "[watchdog] list from [esi+0x44]:"); for (int k = 0; k < 12; k++) { fprintf(stderr, " %08x", n); if (n < 0x1000) break; n = M32(n + 4); } fprintf(stderr, "\n"); } } } for (int i = 0; i < c->nthreads; i++) { Thread *t = &c->threads[i]; fprintf(stderr, "  #%d used=%d done=%d susp=%d blocked=%d started=%d\n", i, t->used, t->done, t->suspended, t->blocked, t->rt_started); } still = 0; if (getenv("BLUR_HANGEXIT")) { hle_sync_report(c); fprintf(stderr, "[watchdog] BLUR_HANGEXIT: exiting\n"); fflush(stderr); _exit(8); } } } else still = 0; last = now; }
    return NULL;
}

void rt_main(Ctx *c) {
    C = c;
    if (getenv("BLUR_DET")) { g_det_step = atof(getenv("BLUR_DET")); g_time_base = 0; g_det_on = 1; g_det_ticks = 0; audio_clock_us = now_us; fprintf(stderr, "[det] deterministic clock: %g us per API call\n", g_det_step); }
    prof_init(); if (nhook < 0) hook_init(); build_table(); build_fast(); cov_init(); ring_on = getenv("BLUR_RING") != NULL; rt_trace = getenv("RT_TRACE") != NULL || ring_on; cont_addr = cont_stub_addr(c);
    if (getenv("BLUR_SCHED_NOFILTER")) sched_filter = 0;
#if defined(__aarch64__)
    coro_mode = !getenv("BLUR_NOCORO");
#endif
    par_mode = getenv("BLUR_PAR") != NULL && atoi(getenv("BLUR_PAR")) != 0; if (par_mode) { coro_mode = 0; t_idx = 0; fprintf(stderr, "[par] parallel guest threads: one host thread per guest thread, API handlers serialised by a global lock\n"); }
    rt_slow = cov_on > 0 || getenv("BLUR_PROFCNT") || getenv("BLUR_HOOK") || getenv("BLUR_POKE") || getenv("BLUR_PCPROF") || getenv("BLUR_PCCOUNT") || getenv("BLUR_LUATRACE") || getenv("BLUR_SLOWDISP");
    if (getenv("BLUR_DUMP_OEP")) { Module *m = find_module(c, "Blur.exe"); FILE *f = fopen(getenv("BLUR_DUMP_OEP"), "wb"); if (f && m) { fwrite(G + m->base, 1, m->size, f); fclose(f); fprintf(stderr, "[dump] exe image at OEP: base=%08x size=%x\n", m->base, m->size); } exit(0); }
    if (getenv("BLUR_DUMP_MSV")) { Module *m = find_module(c, "msvcr90.dll"); FILE *f = fopen(getenv("BLUR_DUMP_MSV"), "wb"); if (f && m) { fwrite(G + m->base, 1, m->size, f); fclose(f); fprintf(stderr, "[dump] msvcr90 base=%08x size=%x\n", m->base, m->size); } exit(0); }
    if (getenv("BLUR_SECONDS")) t_end = real_us() + (uint64_t)atof(getenv("BLUR_SECONDS")) * 1000000ull;
    c->api_slice = getenv("BLUR_API_SLICE") ? (uint32_t)atoi(getenv("BLUR_API_SLICE")) : 300;   // was 50: every forced yield is a scheduler pass (pick_next over all threads)
    for (int i = 0; i < MAX_THREADS; i++) pthread_cond_init(&bcv[i], NULL);
    { struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = spin_sig; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigemptyset(&sa.sa_mask); sigaction(SIGUSR2, &sa, NULL); g_main_thr = pthread_self(); }
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
