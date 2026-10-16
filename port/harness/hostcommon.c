// Host-side pieces shared by the native harness (main.c / sched.c) and the wasm build (port/web): guest heap, API stub table,
// module lookup, handle table, thread creation, guest clock. Nothing here touches the Unicorn engine except through uc_mem_map()/
// rd*/wr* (which the wasm emu.h turns into plain memory accesses).
#include "emu.h"
#include <time.h>
#include <unistd.h>
#include <strings.h>

char g_gamedir[512], g_savedir[512];
// ---------------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------------
int rd_cstr(Ctx *c, uint32_t a, char *out, int max) {
    int i = 0;
    for (; i < max - 1; i++) { char ch = (char)rd8(c, a + i); out[i] = ch; if (!ch) break; }
    out[i] = 0; return i;
}
int rd_wstr(Ctx *c, uint32_t a, char *out, int max) {
    int i = 0;
    for (; i < max - 1; i++) { uint16_t ch = rd16(c, a + 2 * i); out[i] = ch < 128 ? (char)ch : '?'; if (!ch) break; }
    out[i] = 0; return i;
}
static int ieq(const char *a, const char *b) { return strcasecmp(a, b) == 0; }
static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

// ---------------------------------------------------------------------------------
// guest heap: size-class free lists on top of a bump region (blocks are zeroed on allocation)
//   header (16 bytes before the block): [requested size][capacity]
// ---------------------------------------------------------------------------------
static uint32_t heap_class_size(uint32_t n) {               // round up: 16-byte steps to 1 KB, then 1/8-octave steps
    if (n <= 1024) return (n + 15) & ~15u;
    int lg = 31 - __builtin_clz(n); uint32_t step = 1u << (lg - 3); return (n + step - 1) & ~(step - 1);
}
#define HEAP_CLASSES 4096
// Free lists live OUTSIDE guest memory (per size class: a host stack of block addresses). Linking free blocks through their first dword, as the
// first version did, lets any write to a freed block (the game does use freed memory) corrupt the allocator: the next allocation of that class
// then returns a wild pointer and two live objects overlap (the "vtable overwritten by texture data" crash at frame ~7,200).
typedef struct { uint32_t cap, n, alloc; uint32_t *v; } FreeList;
static FreeList free_lists[HEAP_CLASSES]; static int nfree_lists;
static FreeList *freelist_for(uint32_t cap, int create) {
    for (int i = 0; i < nfree_lists; i++) if (free_lists[i].cap == cap) return &free_lists[i];
    if (!create || nfree_lists >= HEAP_CLASSES) return NULL; FreeList *f = &free_lists[nfree_lists++]; memset(f, 0, sizeof *f); f->cap = cap; return f;
}
// BLUR_HEAPCHK=1: every guest_alloc'd block gets HC_PAD canary bytes after its requested size; they are checked on free, every ~300 frames
// (heapchk_scan from Present) and at exit/crash. A corrupted block is reported once with the API call that allocated it.
#define HC_PAD 32
typedef struct { uint32_t p, size, ret; const char *api; } HcEnt;
static HcEnt *hc_ents; static size_t hc_n, hc_cap; static int hc_on = -1;
extern const char *g_cur_api; extern uint32_t g_api_ret;
static int hc_enabled(void) { if (hc_on < 0) hc_on = getenv("BLUR_HEAPCHK") != NULL; return hc_on; }
static int hc_check(Ctx *c, HcEnt *e, const char *when) {
    if (!e->p) return 0; uint8_t b[HC_PAD]; rdbuf(c, e->p + e->size, b, HC_PAD); int bad = 0; for (int i = 0; i < HC_PAD; i++) if (b[i] != 0xAB) bad++;
    if (!bad) return 0;
    fprintf(stderr, "[heapchk] OVERRUN of block %08x size=%u (alloc by %s ret=%08x) found %s: %d/%d canary bytes changed:", e->p, e->size, e->api ? e->api : "?", e->ret, when, bad, HC_PAD);
    for (int i = 0; i < HC_PAD; i++) fprintf(stderr, " %02x", b[i]); fprintf(stderr, "\n"); e->p = 0; return 1;
}
int heapchk_scan(Ctx *c, const char *when) {
    if (!hc_enabled()) return 0; int bad = 0;
    for (size_t i = 0; i < hc_n; i++) { HcEnt *e = &hc_ents[i]; if (e->p && rd32(c, e->p - 12) && rd32(c, e->p - 16) == e->size) bad += hc_check(c, e, when); }
    if (!strcmp(when, "at exit")) fprintf(stderr, "[heapchk] %zu allocations tracked, %d overruns reported in the final scan\n", hc_n, bad);
    return bad;
}
// BLUR_STRICT=1: log (first 5 per api+reason) every call where real Windows/D3D9 would fail but this HLE reports success, or where an argument is suspicious
void strict_log(const char *api, const char *why, uint32_t v0, uint32_t v1) {
    static int on = -1; static struct { const char *api, *why; int n; } seen[256]; static int ns;
    if (on < 0) on = getenv("BLUR_STRICT") != NULL; if (!on) return;
    for (int i = 0; i < ns; i++) if (seen[i].api == api && seen[i].why == why) { if (++seen[i].n > 5) return; goto print; }
    if (ns < 256) { seen[ns].api = api; seen[ns].why = why; seen[ns].n = 1; ns++; }
print: fprintf(stderr, "[strict] %s: %s (%08x, %08x) ret=%08x\n", api, why, v0, v1, g_api_ret);
}
// BLUR_HEAPDBG: freed blocks are poisoned (0xdd), never reused, and logged (with BLUR_HEAPCHK also the allocation site) so a crash can say "this pointer was freed by <api ret> thread N"
typedef struct { uint32_t p, size, cap, free_ret; const char *free_api; uint32_t alloc_ret; const char *alloc_api; int thr; } FreeEnt;
static FreeEnt *fd; static size_t fd_n, fd_cap;
void heapdbg_describe(Ctx *c, uint32_t v, const char *tag) {
    (void)c; for (size_t i = fd_n; i-- > 0; ) if (v >= fd[i].p && v < fd[i].p + fd[i].cap) { FreeEnt *f = &fd[i];
        fprintf(stderr, "[heapdbg] %s=%08x lies in FREED block %08x (+%x) size=%u: freed by %s ret=%08x thread %d; allocated by %s ret=%08x\n", tag, v, f->p, v - f->p, f->size, f->free_api ? f->free_api : "?", f->free_ret, f->thr, f->alloc_api ? f->alloc_api : "?(use BLUR_HEAPCHK=1)", f->alloc_ret); return; }
}
uint32_t guest_alloc(Ctx *c, uint32_t size) {
    if (size > 0x7f000000u) { strict_log("guest_alloc", "request > 2 GB (rounding would wrap)", size, 0); return 0; }
    uint32_t rsize = hc_enabled() ? size + HC_PAD : size;
    uint32_t cap = heap_class_size(rsize ? rsize : 1), p; FreeList *fl = freelist_for(cap, 0);
    if (fl && fl->n) p = fl->v[--fl->n];
    else {
        if (cap > HEAP_SIZE || c->heap_next + cap + 16 > HEAP_BASE + HEAP_SIZE) { fprintf(stderr, "guest heap exhausted (%u MB used)\n", (c->heap_next - HEAP_BASE) >> 20); return 0; }
        p = c->heap_next + 16; c->heap_next += cap + 16; c->heap_next = (c->heap_next + 15) & ~15u;
    }
    static uint8_t z[1 << 16]; for (uint32_t o = 0; o < cap; o += sizeof z) uc_mem_write(c->uc, p + o, z, cap - o < sizeof z ? cap - o : sizeof z);   // always zero-filled
    wr32(c, p - 16, size); wr32(c, p - 12, cap);
    if (hc_enabled()) {
        uint8_t pad[HC_PAD]; memset(pad, 0xAB, sizeof pad); wrbuf(c, p + size, pad, HC_PAD);
        if (hc_n == hc_cap) { hc_cap = hc_cap ? hc_cap * 2 : 1 << 16; hc_ents = realloc(hc_ents, hc_cap * sizeof *hc_ents); }
        hc_ents[hc_n++] = (HcEnt){ p, size, g_api_ret, g_cur_api };
    }
    return p;
}
void guest_free(Ctx *c, uint32_t p) {
    if (!p || p < HEAP_BASE + 16 || p >= HEAP_BASE + HEAP_SIZE) return;
    { static int nf = -1; if (nf < 0) nf = getenv("BLUR_NOFREE") != NULL; if (nf) return; }      // debugging: never reuse memory (finds use-after-free)
    uint32_t cap = rd32(c, p - 12); if (!cap) return;                 // double free / foreign pointer
    FreeList *fl = freelist_for(cap, 1); if (!fl) return;
    HcEnt fe = { p, rd32(c, p - 16), 0, "?" };
    if (hc_enabled()) { for (size_t i = hc_n; i-- > 0 && i + 4096 > hc_n; ) if (hc_ents[i].p == p) { fe = hc_ents[i]; hc_ents[i].p = 0; break; } hc_check(c, &fe, "at free"); }
    wr32(c, p - 12, 0);
    { static int dbg = -1; if (dbg < 0) dbg = getenv("BLUR_HEAPDBG") != NULL;
      if (dbg) { if (fd_n == fd_cap) { fd_cap = fd_cap ? fd_cap * 2 : 1 << 16; fd = realloc(fd, fd_cap * sizeof *fd); } fd[fd_n++] = (FreeEnt){ p, fe.size, cap, g_api_ret, g_cur_api, fe.ret, fe.api, c->cur };
        static uint8_t z[1 << 16]; memset(z, 0xdd, sizeof z); for (uint32_t o = 0; o < cap; o += sizeof z) uc_mem_write(c->uc, p + o, z, cap - o < sizeof z ? cap - o : sizeof z); return; } }
    if (fl->n == fl->alloc) { fl->alloc = fl->alloc ? fl->alloc * 2 : 64; fl->v = realloc(fl->v, fl->alloc * 4); }
    fl->v[fl->n++] = p;
}
uint32_t guest_blocksize(Ctx *c, uint32_t p) { return p ? rd32(c, p - 16) : 0; }
uint32_t guest_blockcap(Ctx *c, uint32_t p) { return p ? rd32(c, p - 12) : 0; }


// ---------------------------------------------------------------------------------
// import signature table (exported from Ghidra) for stack cleanup of unimplemented calls
// ---------------------------------------------------------------------------------
typedef struct { char dll[64], name[96]; int cdecl_, nargs; } Sig;
static Sig *sigs; static int nsigs;
void load_sigs(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) { fprintf(stderr, "no %s\n", path); return; }
    char line[1024]; sigs = calloc(2000, sizeof(Sig));
    while (fgets(line, sizeof line, f) && nsigs < 2000) {
        char *t[5] = {0}; int n = 0; char *p = line;
        while (n < 5) { t[n++] = p; char *q = strchr(p, '\t'); if (!q) break; *q = 0; p = q + 1; }
        if (n < 4) continue;
        Sig *s = &sigs[nsigs++];
        snprintf(s->dll, sizeof s->dll, "%s", t[0]); snprintf(s->name, sizeof s->name, "%s", t[1]);
        s->cdecl_ = strstr(t[2], "cdecl") != NULL; s->nargs = (strstr(t[2], "std") || strstr(t[2], "cdecl")) ? atoi(t[3]) : -1;   // 'unknown' => no usable signature
    }
    fclose(f);
}
static Sig *find_sig(const char *name) {
    for (int i = 0; i < nsigs; i++) if (!strcmp(sigs[i].name, name)) return &sigs[i];
    return NULL;
}

// ---------------------------------------------------------------------------------
// API stubs
// ---------------------------------------------------------------------------------
uint32_t get_stub_addr(Ctx *c, const char *dll, const char *name) {
    for (int i = 0; i < c->nstubs; i++)
        if (!strcmp(c->stubs[i].name, name) && ieq(c->stubs[i].dll, dll)) return STUB_BASE + i * 16;
    if (c->nstubs >= STUB_SLOTS) { fprintf(stderr, "out of stub slots\n"); exit(1); }
    Stub *s = &c->stubs[c->nstubs];
    snprintf(s->dll, sizeof s->dll, "%s", dll); snprintf(s->name, sizeof s->name, "%s", name);
    Sig *sg = find_sig(name);
    s->nargs = sg ? sg->nargs : -1; s->cdecl_ = sg ? sg->cdecl_ : 0; s->fn = NULL;
    const HleDef *h = hle_lookup(name);
    if (h) {
        if (sg && sg->nargs >= 0 && !sg->cdecl_ && sg->nargs != h->nargs)
            fprintf(stderr, "[sig-mismatch] %s: handler says %d args, Ghidra says %d\n", name, h->nargs, sg->nargs);
        s->fn = h->fn; s->nargs = h->nargs; s->cdecl_ = h->cdecl_;
    }
    return STUB_BASE + (c->nstubs++) * 16;
}

uint32_t make_stub(Ctx *c, const char *dll, const char *name, int nargs, hle_fn fn) {
    uint32_t addr = get_stub_addr(c, dll, name); Stub *s = &c->stubs[(addr - STUB_BASE) / 16];
    s->fn = fn; s->nargs = nargs; s->cdecl_ = 0; return addr;
}

Module *find_module(Ctx *c, const char *name) {
    for (int i = 0; i < c->nmods; i++) if (ieq(c->mods[i].name, name)) return &c->mods[i];
    return NULL;
}

uint32_t find_export(Ctx *c, Module *m, const char *name) {
    if (!m->exp_dir) return 0;
    uint32_t d = m->base + m->exp_dir;
    uint32_t nnames = rd32(c, d + 24), funcs = m->base + rd32(c, d + 28), names = m->base + rd32(c, d + 32), ords = m->base + rd32(c, d + 36);
    for (uint32_t i = 0; i < nnames; i++) {
        char n[128]; rd_cstr(c, m->base + rd32(c, names + 4 * i), n, sizeof n);
        if (!strcmp(n, name)) return m->base + rd32(c, funcs + 4 * rd16(c, ords + 2 * i));
    }
    return 0;
}

#define THREAD_TEB_BASE   0x7ff00000u   // one 0x2000 block per thread: TEB page + TLS slot array
#define THREAD_STACK_TOP  0x0e000000u   // thread stacks grow downward from here, 0x200000 apart

// The emulator is far slower than the hardware the game expects, so guest time runs at BLUR_TIMESCALE x real time (default 1/25):
// game timeouts and frame timing then behave as they would on a machine that is simply fast enough.
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
uint64_t real_us(void) { return (uint64_t)(emscripten_get_now() * 1000.0); }
#else
uint64_t real_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000; }
#endif
// Deterministic clock (BLUR_DET=<us per API-handler call>): guest time advances only by API calls, so two runs of the same code (native
// and wasm) see the same time and take the same branches. g_time_base shifts the clock (restored from a snapshot).
uint64_t g_time_base, g_det_ticks; int g_det_on; double g_det_step;
uint64_t now_us(void) {
    if (g_det_on) return 1000000ull * 1000 + g_time_base + (uint64_t)(g_det_step * (double)g_det_ticks);
    static double scale = -1; static uint64_t t0;
    if (scale < 0) { scale = getenv("BLUR_TIMESCALE") ? atof(getenv("BLUR_TIMESCALE")) : (g_recomp ? 1.0 : 0.04); t0 = real_us(); }
    return 1000000ull * 1000 + g_time_base + (uint64_t)((double)(real_us() - t0) * scale);
}
void heap_snap(void (*cb)(void *, void *, size_t), void *u) {      // free lists: count, then per list {cap, n, n block addresses}; the visitor writes or reads depending on the callback
    cb(u, &nfree_lists, sizeof nfree_lists);
    for (int i = 0; i < nfree_lists; i++) { FreeList *f = &free_lists[i]; cb(u, &f->cap, 4); cb(u, &f->n, 4); if (f->alloc < f->n) { f->alloc = f->n + 16; f->v = realloc(f->v, f->alloc * 4); } if (f->n) cb(u, f->v, 4 * f->n); }
}
Thread *cur_thread(Ctx *c) { return &c->threads[c->cur]; }

uint32_t new_handle(Ctx *c, int type) {
    if (c->nhandles == 0) c->nhandles = 1;                  // slot 0 unused so handle values are never 0
    if (c->nhandles >= MAX_HANDLES) { fprintf(stderr, "out of handles\n"); exit(1); }
    extern uint32_t g_api_ret; int i = c->nhandles++; c->handles[i].type = type; c->handles[i].cret = g_api_ret;
    return 0x100 + 4 * (uint32_t)i;
}

static void init_teb(Ctx *c, uint32_t teb, uint32_t tlsarr, uint32_t lo, uint32_t hi, uint32_t tid) {
    wr32(c, teb + 0x00, 0xffffffff); wr32(c, teb + 0x04, hi); wr32(c, teb + 0x08, lo);
    wr32(c, teb + 0x18, teb); wr32(c, teb + 0x20, 0x1000); wr32(c, teb + 0x24, tid);
    wr32(c, teb + 0x2c, tlsarr); wr32(c, teb + 0x30, PEB_ADDR);
}

static void alloc_static_tls(Ctx *c, uint32_t tlsarr) {
    for (int i = 0; i < c->nmods; i++) {
        Module *m = &c->mods[i]; if (!m->has_tls) continue;
        uint32_t n = (m->tls_raw1 - m->tls_raw0) + m->tls_zero, blk = guest_alloc(c, n ? n : 4);
        uint8_t *tmp = malloc(m->tls_raw1 - m->tls_raw0 + 1); rdbuf(c, m->tls_raw0, tmp, m->tls_raw1 - m->tls_raw0);
        wrbuf(c, blk, tmp, m->tls_raw1 - m->tls_raw0); free(tmp);
        wr32(c, tlsarr + 4 * m->tls_idx, blk);
    }
}

// Register the already-running main thread (uses the TEB/stack built by setup_process).
void register_main_thread(Ctx *c, uint32_t entry) {
    Thread *t = &c->threads[0]; memset(t, 0, sizeof *t);
    t->used = 1; t->teb = TEB_ADDR; t->stack_hi = STACK_TOP; t->stack_lo = STACK_TOP - STACK_SIZE;
    t->entry = entry; t->tid = 0x1004; t->ctx = NULL; c->nthreads = 1; c->cur = 0;
}

uint32_t spawn_thread(Ctx *c, uint32_t start, uint32_t param, int suspended, uint32_t stack_size) {
    int idx = -1;
    for (int i = 1; i < MAX_THREADS; i++) if (!c->threads[i].used) { idx = i; break; }
    if (idx < 0) { fprintf(stderr, "too many threads\n"); return 0; }
    Thread *t = &c->threads[idx]; memset(t, 0, sizeof *t);
    t->used = 1; t->suspended = suspended; t->entry = start; t->param = param; t->tid = 0x1004 + 4 * (uint32_t)idx;
    t->teb = THREAD_TEB_BASE + (uint32_t)idx * 0x2000;
    t->stack_hi = THREAD_STACK_TOP - (uint32_t)idx * 0x200000u;
    uint32_t ssz = stack_size ? (stack_size > 0x1f0000 ? 0x1f0000 : (stack_size + 0xfff) & ~0xfffu) : 0x100000;
    if (ssz < 0x40000) ssz = 0x40000;                       // generous minimum: emulated frames are bigger than people expect
    t->stack_lo = t->stack_hi - ssz;
    uc_mem_map(c->uc, t->teb, 0x2000, UC_PROT_ALL); uc_mem_map(c->uc, t->stack_lo, ssz, UC_PROT_ALL);
    init_teb(c, t->teb, t->teb + 0x1000, t->stack_lo, t->stack_hi, t->tid);
    alloc_static_tls(c, t->teb + 0x1000);
    if (idx >= c->nthreads) c->nthreads = idx + 1;
    uint32_t h = new_handle(c, H_THREAD); c->handles[(h - 0x100) / 4].thread = idx; t->handle = h;
    if (c->verbose) fprintf(stderr, "[thread] spawn #%d tid=%x entry=%08x param=%08x stack=%08x-%08x\n", idx, t->tid, start, param, t->stack_lo, t->stack_hi);
    return h;
}

