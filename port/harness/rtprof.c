// BLUR_PROF=<n>: per-frame cost accounting, robust to machine load.  Every n-th Present prints wall ms, process CPU ms, retired instructions and cycles per
// frame (proc_pid_rusage), the time spent inside API handlers (excluding yields), API calls, dispatches (BLUR_PROFCNT=1), scheduler switches and the busiest guest threads.
// At exit: API time by stub.  Frames are counted in rt_api when the Present stub is called, so no other file needs a hook.
#include "emu.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/resource.h>
#endif
#ifdef __APPLE__
#include <libproc.h>
#endif
#include <time.h>
#include <unistd.h>

int g_prof;
uint64_t prof_dispatch, prof_api, prof_api_ns, prof_yield_ns;
extern uint64_t g_switches;
static uint64_t frames, last_frames, last_wall, last_cpu, last_api, last_api_ns, last_dispatch, last_sw, last_instr, last_cyc, g_instr, g_cyc;
static uint64_t thr_ns[MAX_THREADS], last_thr[MAX_THREADS], t_switch;
static int every = 120;
static uint64_t *stub_ns;
static const Stub *present_stub;

uint64_t prof_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC_RAW, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec; }
#ifdef _WIN32
static uint64_t cpu_ns(void) { FILETIME c, e, k, u; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u); return (((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime)) * 100ull; }
#else
static uint64_t cpu_ns(void) { struct rusage r; getrusage(RUSAGE_SELF, &r); return ((uint64_t)r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1000000000ull + ((uint64_t)r.ru_utime.tv_usec + r.ru_stime.tv_usec) * 1000ull; }
#endif
#ifdef __APPLE__
static void ru4(void) { struct rusage_info_v4 r; if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)&r) == 0) { g_instr = r.ri_instructions; g_cyc = r.ri_cycles; } }
#else
static void ru4(void) {}   // retired-instruction counters are macOS-only
#endif

static __thread uint64_t seg0; static __thread int seg_stub = -1;   // an "API segment" = time on this thread between API entry and (yield | API exit)
void prof_seg_begin_stub(int idx) { seg_stub = idx; seg0 = prof_now(); }
void prof_seg_begin(void) { seg0 = prof_now(); }
void prof_seg_end(void) { if (!seg0) return; uint64_t d = prof_now() - seg0; prof_api_ns += d; if (seg_stub >= 0 && stub_ns) stub_ns[seg_stub] += d; seg0 = 0; }
void prof_init(void) { const char *e = getenv("BLUR_PROF"); if (!e) return; g_prof = 1; every = atoi(e) > 0 ? atoi(e) : 120; stub_ns = calloc(STUB_SLOTS, 8); t_switch = prof_now(); }
void prof_thread_switch(int self) { uint64_t n = prof_now(); if (self >= 0 && self < MAX_THREADS) thr_ns[self] += n - t_switch; t_switch = n; }

static uint64_t dts[4096], last_present; static int ndt;
static uint64_t last_d3d_ns, last_d3d_calls;
static void d3d_totals(Ctx *c, uint64_t *ns, uint64_t *calls) {      // all IDirect3D* handlers except Present (which carries the pacing sleep)
    uint64_t a = 0, b = 0; for (int i = 0; i < c->nstubs; i++) { const Stub *s = &c->stubs[i]; if (stub_ns[i] && !strncmp(s->name, "IDirect3D", 9) && strcmp(s->name, "IDirect3DDevice9::Present")) { a += stub_ns[i]; b += s->calls; } }
    *ns = a; *calls = b;
}
static int cmpd(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static void prof_frame(Ctx *c) {
    { uint64_t t = prof_now(); if (last_present && ndt < 4096) dts[ndt++] = t - last_present; last_present = t; }
    frames++; if (frames % every) return;
    uint64_t w = prof_now(), cp = cpu_ns(); ru4(); prof_thread_switch(c->cur);
    if (last_wall) {
        double n = (double)(frames - last_frames), calls = (double)(prof_api - last_api); uint64_t d3d_ns, d3d_calls; d3d_totals(c, &d3d_ns, &d3d_calls);
        fprintf(stderr, "[prof] frames %llu  wall %.2f ms/f  cpu %.2f ms/f  api %.2f ms/f (%.0f calls/f)  d3d %.2f ms/f (%.0f calls/f; clock overhead incl.)  dispatch %.0f/f  guest(cpu-api)~%.2f ms/f  switches %.0f/f  instr %.0fk/f cyc %.0fk/f",
                (unsigned long long)frames, (w - last_wall) / 1e6 / n, (cp - last_cpu) / 1e6 / n, (prof_api_ns - last_api_ns) / 1e6 / n, calls / n, (d3d_ns - last_d3d_ns) / 1e6 / n, (double)(d3d_calls - last_d3d_calls) / n,
                (prof_dispatch - last_dispatch) / n, ((cp - last_cpu) - (prof_api_ns - last_api_ns)) / 1e6 / n, (g_switches - last_sw) / n, (g_instr - last_instr) / n / 1e3, (g_cyc - last_cyc) / n / 1e3);
        int top[3] = {-1, -1, -1};      // busiest guest threads (ms per frame): with the coroutine scheduler this is the only per-thread CPU split
        for (int k = 0; k < 3; k++) { for (int i = 0; i < MAX_THREADS; i++) { int dup = 0; for (int q = 0; q < k; q++) if (top[q] == i) dup = 1; if (!dup && thr_ns[i] - last_thr[i] > (top[k] < 0 ? 0 : thr_ns[top[k]] - last_thr[top[k]])) top[k] = i; } }
        if (ndt > 10) { qsort(dts, (size_t)ndt, 8, cmpd); fprintf(stderr, "  dt p50 %.2f p95 %.2f p99 %.2f max %.2f ms", dts[ndt / 2] / 1e6, dts[ndt * 95 / 100] / 1e6, dts[ndt * 99 / 100] / 1e6, dts[ndt - 1] / 1e6); }
        fprintf(stderr, "  threads:"); for (int k = 0; k < 3; k++) if (top[k] >= 0) fprintf(stderr, " t%d=%.2f", top[k], (thr_ns[top[k]] - last_thr[top[k]]) / 1e6 / n); fprintf(stderr, " ms/f\n");
    }
    ndt = 0;
    if (getenv("BLUR_PROFTOP")) {            // per window: busiest API stubs by time and by call count (per frame)
        static uint64_t *lns, *lcalls; if (!lns) { lns = calloc(STUB_SLOTS, 8); lcalls = calloc(STUB_SLOTS, 8); }
        double n = (double)(frames - last_frames); int top[2][6]; memset(top, -1, sizeof top);
        for (int kind = 0; kind < 2; kind++) for (int k = 0; k < 6; k++) { int best = -1; uint64_t bv = 0;
            for (int i = 0; i < c->nstubs; i++) { uint64_t v = kind ? c->stubs[i].calls - lcalls[i] : stub_ns[i] - lns[i]; int dup = 0; for (int q = 0; q < k; q++) if (top[kind][q] == i) dup = 1; if (!dup && v > bv) { bv = v; best = i; } } top[kind][k] = best; }
        fprintf(stderr, "[proftop] ms/f:"); for (int k = 0; k < 6; k++) if (top[0][k] >= 0) { int i = top[0][k]; fprintf(stderr, " %s=%.2f", c->stubs[i].name, (stub_ns[i] - lns[i]) / 1e6 / n); }
        fprintf(stderr, " | calls/f:"); for (int k = 0; k < 6; k++) if (top[1][k] >= 0) { int i = top[1][k]; fprintf(stderr, " %s=%.0f", c->stubs[i].name, (c->stubs[i].calls - lcalls[i]) / n); } fprintf(stderr, "\n");
        for (int i = 0; i < c->nstubs; i++) { lns[i] = stub_ns[i]; lcalls[i] = c->stubs[i].calls; }
    }
    { uint64_t a, b; d3d_totals(c, &a, &b); last_d3d_ns = a; last_d3d_calls = b; }
    last_wall = w; last_cpu = cp; last_dispatch = prof_dispatch; last_api = prof_api; last_api_ns = prof_api_ns; last_frames = frames; last_sw = g_switches; last_instr = g_instr; last_cyc = g_cyc;
    memcpy(last_thr, thr_ns, sizeof thr_ns);
    { long stop = getenv("BLUR_PROF_STOP") ? atol(getenv("BLUR_PROF_STOP")) : 0; if (stop > 0 && (long)frames >= stop) { fprintf(stderr, "[prof] stop at frame %llu\n", (unsigned long long)frames); fflush(stderr); _exit(0); } }
}
void prof_note_api(Ctx *c, const Stub *s) {      // called for every API call while profiling: counts Presents
    if (!present_stub && !strcmp(s->name, "IDirect3DDevice9::Present")) present_stub = s;
    if (s == present_stub) prof_frame(c);
}
void prof_report(Ctx *c) {
    if (!g_prof || !stub_ns) return;
    fprintf(stderr, "[prof] API time by stub (ns total, calls, ns/call):\n");
    for (int n = 0; n < 25; n++) { int best = -1; for (int i = 0; i < c->nstubs; i++) if (stub_ns[i] && (best < 0 || stub_ns[i] > stub_ns[best])) best = i;
        if (best < 0) break; Stub *s = &c->stubs[best]; fprintf(stderr, "  %12llu ns %10llu calls %8.0f ns/call  %s!%s\n", (unsigned long long)stub_ns[best], (unsigned long long)s->calls, (double)stub_ns[best] / (s->calls ? s->calls : 1), s->dll, s->name); stub_ns[best] = 0; }
}
