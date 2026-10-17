// Shared declarations for the Blur HLE harness (x86 emulation + Win32 API implementation).
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#ifdef __EMSCRIPTEN__
// ---- WebAssembly build (port/web): no Unicorn; guest address == wasm linear-memory address (G == 0) ------------------------------------
#define G ((uint8_t *)0)
typedef struct uc_engine uc_engine; typedef struct uc_context uc_context; typedef int uc_err;
#define UC_ERR_OK 0
#define UC_PROT_ALL 7
#define uc_mem_map(uc, a, s, p) (UC_ERR_OK)
#define uc_strerror(e) "n/a"
static inline int uc_mem_write(void *uc, uint64_t a, const void *p, size_t n) { (void)uc; memcpy((void *)(uintptr_t)(uint32_t)a, p, n); return 0; }
static inline int uc_mem_read(void *uc, uint64_t a, void *p, size_t n) { (void)uc; memcpy(p, (const void *)(uintptr_t)(uint32_t)a, n); return 0; }
#else
#include <unicorn/unicorn.h>

// All guest memory lives in one flat 4 GB host mapping (guest address A = G + A). Unicorn maps the same bytes (uc_mem_map_ptr) so the boot
// phase under emulation and the recompiled code see one memory.
extern uint8_t *G;
#define uc_mem_map(uc, a, s, p) uc_mem_map_ptr((uc), (a), (s), (p), G + (uint32_t)(a))
#endif

// ---- guest memory map -------------------------------------------------------
#define STACK_TOP   0x0f800000u
#define STACK_SIZE  0x00800000u
#define HEAP_BASE   0x20000000u
#define HEAP_SIZE   0x38000000u   // 896 MB virtual (0x20000000..0x58000000); the VirtualAlloc area starts at 0x58000000
#define TEB_ADDR    0x7ffdf000u
#define PEB_ADDR    0x7ffde000u
#define KUSER_ADDR  0x7ffe0000u
#define GDT_ADDR    0x7ffdd000u
#define STUB_BASE   0x7f000000u   // one 16-byte slot per emulated API
#define STUB_SLOTS  0x4000u
#define RET_MAGIC   0x7efff000u   // guest->host return trampoline
#define MODMAP_BASE 0x10000000u   // where relocatable DLLs get placed

typedef struct { uint8_t reg, usage, idx, tex; } IOEnt;
typedef struct { char *glsl; uint32_t glsl_len; int is_ps; uint32_t version; int nin, nout, nsamp; IOEnt in[32], out[32], samp[16]; } ShaderXlat;
int shader_translate(const uint8_t *code, uint32_t len, ShaderXlat *x);
#include "shcache.h"
typedef struct { uint32_t type; int32_t a, b, c, d; } NetInput;
typedef struct { int active; uint16_t btn; uint8_t lt, rt; int16_t lx, ly, rx, ry; } PadState;      // browser Gamepad -> XInput (input type 6)
typedef struct { int32_t dx, dy, dz; uint8_t btn[8]; } MouseState;                                  // DirectInput mouse (input types 4, 5, 7)
extern PadState g_pad; extern MouseState g_mouse;
int net_start(int port); void net_send(const void *data, size_t n); int net_client_connected(void); int net_poll_input(NetInput *out);
enum { G_TEX_CREATE = 20, G_TEX_UPLOAD = 21, G_SURF_CREATE = 22, G_BUF_CREATE = 30, G_BUF_UPLOAD = 31, G_SHADER = 40, G_VDECL = 41, G_SHADER_REF = 42,
       G_SET_RS = 50, G_SET_SS = 51, G_SET_TEX = 52, G_SET_VS = 53, G_SET_PS = 54, G_SET_VDECL = 55, G_SET_FVF = 56, G_SET_STREAM = 57, G_SET_INDICES = 58,
       G_SET_VSC = 59, G_SET_PSC = 60, G_SET_VIEWPORT = 61, G_SET_SCISSOR = 62, G_SET_RT = 63, G_SET_DS = 64, G_DRAW = 70, G_DRAW_UP = 71, G_CLEAR = 72,
       G_PRESENT = 73, G_STRETCH = 74, G_SET_VSCI = 75, G_SET_PSCI = 76, G_SET_VSCB = 77, G_SET_PSCB = 78 };
int gfx_on(void); void gfx_flush(void);
void gfx_msg(uint32_t type, const uint32_t *w, int nw, const void *data, uint32_t ndata);
uint8_t *gfx_msg_alloc(uint32_t type, const uint32_t *w, int nw, uint32_t ndata);
extern volatile uint32_t g_net_epoch;   // incremented each time a viewer attaches: source-side state filters must forget what they believe the renderer holds
#define GFXW(type, ...) do { if (gfx_on()) { uint32_t _w[] = {__VA_ARGS__}; gfx_msg(type, _w, (int)(sizeof _w / 4), NULL, 0); } } while (0)
void md5_digest(const uint8_t *m, size_t n, uint8_t out[16]); void sha1_digest(const uint8_t *m, size_t n, uint8_t out[20]); void sha256_digest(const uint8_t *m, size_t n, uint8_t out[32]);
typedef struct Ctx Ctx;
typedef uint32_t (*hle_fn)(Ctx *c, uint32_t *a);

typedef struct {
    char dll[64], name[96];
    int nargs;      // -1 = unknown
    uint64_t calls; int raw;      // raw: handler sets registers itself (continuations)
    int cdecl_;     // caller cleans stack
    hle_fn fn;      // NULL => unimplemented
} Stub;

typedef struct {
    char name[64];
    uint32_t base, size, entry;
    uint32_t tls_raw0, tls_raw1, tls_zero, tls_idx; int has_tls;
    uint32_t exp_dir, exp_size;   // export directory RVA/size
    uint32_t tls_dir;
    int is_real;                  // real PE executed in the emulator
} Module;

#define MAX_THREADS 64
#define MAX_HANDLES 4096
enum { H_NONE = 0, H_EVENT, H_MUTEX, H_SEMAPHORE, H_THREAD, H_FILE, H_FIND };
typedef struct { int type, signaled, manual; uint32_t owner; int count, max, thread; void *fp; char pat[260]; int abandoned; uint32_t cret; uint64_t nset, nacq; char nm[48]; uint32_t setter[4], setter_n[4]; } Handle;
typedef struct { uint32_t real_ret, value; int use_result; } Cont;
typedef struct {
    Cont cont[16]; int ncont;
    int used, started, done, suspended, blocked, sig_done;
    uint32_t spin_ret, spin_addr; int spin_n;
    uc_context *ctx;
    uint32_t teb, stack_lo, stack_hi, entry, param, tid, exit_code;
    uint8_t wk, nwh, wall; uint32_t wh[8], wcs;   // what a blocked thread waits for (scheduler skips it until satisfiable): 1 handles, 2 sleep, 3 critical section
    uint64_t wake_at;     // Sleep / timed wait deadline in microseconds (0 = none)
    uint32_t handle;
    void *cpu; int rt_started;    // recompiled-code mode: register file and host thread of this guest thread
    pthread_t pth;
    const char *last_api; uint32_t last_ret, last_a0, last_a1, n_api; uint64_t last_api_us;    // diagnostics: what the thread last called (BLUR_THREADS)
} Thread;

// A handler can ask the dispatcher to call guest code (e.g. a WndProc) and then resume the original caller.
typedef struct { int pending; uint32_t fn; int nargs; uint32_t args[8]; int have_cont, use_result; uint32_t cont_value; } CallReq;
struct Ctx {
    CallReq req;
    Thread threads[MAX_THREADS]; int nthreads, cur;
    Handle handles[MAX_HANDLES]; int nhandles;
    int yield_req;
    uint64_t slices, slice_full; uint32_t api_since_switch, api_slice;
    uint64_t api_progress;  // bumps on every completed API call (scheduler idle detection)
    int block;            // set by an API handler to say "retry this call after switching threads"

    uc_engine *uc;
    Stub stubs[STUB_SLOTS]; int nstubs;
    Module mods[32]; int nmods;
    uint32_t heap_next;
    int exited; uint32_t exit_code;
    uint32_t last_error;
    uint32_t tls_next;
    int verbose;
    int oep_hit; uint32_t oep_regs[8];   // set when the packed loader stub reaches the original entry point (recompiled mode takes over there)
};

// ---- guest memory helpers -----------------------------------------------------
#ifndef SLOW_MEM
// guest memory is one flat host mapping that Unicorn shares (uc_mem_map_ptr), so the HLE can access it directly instead of through uc_mem_read/uc_mem_write
static inline uint32_t rd32(Ctx *c, uint32_t a) { (void)c; uint32_t v; memcpy(&v, G + a, 4); return v; }
static inline uint16_t rd16(Ctx *c, uint32_t a) { (void)c; uint16_t v; memcpy(&v, G + a, 2); return v; }
static inline uint8_t  rd8 (Ctx *c, uint32_t a) { (void)c; return G[a]; }
static inline void wr32(Ctx *c, uint32_t a, uint32_t v) { (void)c; memcpy(G + a, &v, 4); }
static inline void wr16(Ctx *c, uint32_t a, uint16_t v) { (void)c; memcpy(G + a, &v, 2); }
static inline void wr8 (Ctx *c, uint32_t a, uint8_t v)  { (void)c; G[a] = v; }
#else
static inline uint32_t rd32(Ctx *c, uint32_t a) { uint32_t v = 0; uc_mem_read(c->uc, a, &v, 4); return v; }
static inline uint16_t rd16(Ctx *c, uint32_t a) { uint16_t v = 0; uc_mem_read(c->uc, a, &v, 2); return v; }
static inline uint8_t  rd8 (Ctx *c, uint32_t a) { uint8_t  v = 0; uc_mem_read(c->uc, a, &v, 1); return v; }
static inline void wr32(Ctx *c, uint32_t a, uint32_t v) { uc_mem_write(c->uc, a, &v, 4); }
static inline void wr16(Ctx *c, uint32_t a, uint16_t v) { uc_mem_write(c->uc, a, &v, 2); }
static inline void wr8 (Ctx *c, uint32_t a, uint8_t v)  { uc_mem_write(c->uc, a, &v, 1); }
#endif
extern uint32_t g_hw_lo, g_hw_hi; extern const char *g_cur_api;
#ifndef SLOW_MEM
static inline void wrbuf(Ctx *c, uint32_t a, const void *p, size_t n) { (void)c; if (g_hw_hi && a < g_hw_hi && a + n > g_hw_lo) fprintf(stderr, "[hostw] %zu bytes at %08x by %s\n", n, a, g_cur_api ? g_cur_api : "?"); memcpy(G + a, p, n); }
static inline void rdbuf(Ctx *c, uint32_t a, void *p, size_t n) { (void)c; memcpy(p, G + a, n); }
#else
static inline void wrbuf(Ctx *c, uint32_t a, const void *p, size_t n) { if (g_hw_hi && a < g_hw_hi && a + n > g_hw_lo) fprintf(stderr, "[hostw] %zu bytes at %08x by %s\n", n, a, g_cur_api ? g_cur_api : "?"); uc_err e = uc_mem_write(c->uc, a, p, n); if (e != UC_ERR_OK) { static int k; if (k++ < 20) fprintf(stderr, "[mem] guest write failed at %08x size %zu: %s\n", a, n, uc_strerror(e)); } }
static inline void rdbuf(Ctx *c, uint32_t a, void *p, size_t n) { uc_err e = uc_mem_read(c->uc, a, p, n); if (e != UC_ERR_OK) { static int k; if (k++ < 20) fprintf(stderr, "[mem] guest read failed at %08x size %zu: %s\n", a, n, uc_strerror(e)); } }
#endif
int  rd_cstr(Ctx *c, uint32_t a, char *out, int max);
int  rd_wstr(Ctx *c, uint32_t a, char *out, int max);   // UTF-16 -> ASCII (lossy)

// ---- allocator & loader (main.c) -------------------------------------------------
uint32_t guest_alloc(Ctx *c, uint32_t size); int heapchk_scan(Ctx *c, const char *when); void heapdbg_describe(Ctx *c, uint32_t v, const char *tag); void strict_log(const char *api, const char *why, uint32_t v0, uint32_t v1);
void     guest_free(Ctx *c, uint32_t p);
uint32_t guest_blocksize(Ctx *c, uint32_t p);
uint32_t guest_blockcap(Ctx *c, uint32_t p);
uint32_t get_stub_addr(Ctx *c, const char *dll, const char *name);
uint32_t make_stub(Ctx *c, const char *dll, const char *name, int nargs, hle_fn fn);   // stub with an explicit handler
// COM (com.c): objects are {vtable*, refcount, user data...} in guest memory, every method a stub with `this` as a[0]
typedef struct { const char *name; int nargs; hle_fn fn; } ComMethod;
uint32_t com_make(Ctx *c, const char *iface, const ComMethod *m, int n, uint32_t extra);
uint32_t com_ok(Ctx *c, uint32_t *a);
uint32_t com_create_directinput8(Ctx *c);
void win_post(Ctx *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
uint32_t cont_stub_addr(Ctx *c);
uint32_t com_create_direct3d9(Ctx *c);
void d3d9_report(Ctx *c);
void d3d9_set_const(Ctx *c, int ps, int set, uint32_t reg, const void *data, uint32_t nvec);
void d3d9_get_const_f(int ps, uint32_t reg, float *out);
Module  *find_module(Ctx *c, const char *name);
void     load_sigs(const char *path);
uint32_t load_module(Ctx *c, const char *path, const char *name);
uint32_t find_export(Ctx *c, Module *m, const char *name);
uint32_t call_guest(Ctx *c, uint32_t addr, int nargs, const uint32_t *args);
// scheduler (sched.c)
uint32_t spawn_thread(Ctx *c, uint32_t start, uint32_t param, int suspended, uint32_t stack_size);
void     run_scheduler(Ctx *c);
uint64_t now_us(void); uint64_t real_us(void);
uint32_t new_handle(Ctx *c, int type);
void hle_thread_exit(Ctx *c, Thread *t);   // release the mutexes the thread owned (abandoned), signal nothing else
void hle_sync_report(Ctx *c);
Thread  *cur_thread(Ctx *c); int thread_can_run(Ctx *c, Thread *t); uint64_t thread_next_deadline(Ctx *c); extern uint64_t g_sched_now;
void     register_main_thread(Ctx *c, uint32_t entry);
uint32_t cur_esp(Ctx *c);   // stack pointer of the calling guest thread (valid inside an API handler)
void rt_main(Ctx *c);       // recompiled-code scheduler (rt.c)
extern int g_recomp; extern uint32_t g_oep_addr;

// ---- API implementations (hle.c) ---------------------------------------------------
typedef struct { const char *name; int nargs; int cdecl_; hle_fn fn; } HleDef;
const HleDef *hle_lookup(const char *name);
const HleDef *hle2_lookup(const char *name);
const HleDef *d3dx_lookup(const char *name);
const HleDef *win_lookup(const char *name);
const HleDef *wsock_lookup(const char *name);
extern char g_gamedir[512], g_savedir[512];
