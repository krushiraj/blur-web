// Win32 API implementations (high-level emulation). Single-threaded, no real OS behind it.
// Each handler gets the guest stack arguments in a[]; the dispatcher does stdcall/cdecl cleanup.
#include "emu.h"
#include <time.h>
#include <ctype.h>
#include <strings.h>
#include <sys/time.h>

#define H(n) static uint32_t h_##n(Ctx *c, uint32_t *a)
#define UNUSED (void)c; (void)a

static uint32_t cached_str(Ctx *c, uint32_t *slot, const void *data, int n) {
    if (!*slot) { *slot = guest_alloc(c, n); wrbuf(c, *slot, data, n); }
    return *slot;
}
static int vlog(Ctx *c) { return c->verbose; }

// ---- module handles ---------------------------------------------------------------------
static struct { char name[64]; uint32_t base; } fake_mods[32]; static int nfake;
static void stem(const char *in, char *out, int n) {
    snprintf(out, n, "%s", in);
    char *s = strrchr(out, '\\'); if (s) memmove(out, s + 1, strlen(s));
    char *d = strrchr(out, '.'); if (d) *d = 0;
    for (char *p = out; *p; p++) *p = (char)tolower(*p);
}
static uint32_t module_handle(Ctx *c, const char *name) {
    char st[64]; stem(name, st, sizeof st);
    for (int i = 0; i < c->nmods; i++) { char m[64]; stem(c->mods[i].name, m, sizeof m); if (!strcmp(m, st)) return c->mods[i].base; }
    for (int i = 0; i < nfake; i++) if (!strcmp(fake_mods[i].name, st)) return fake_mods[i].base;
    uint32_t base = 0x7a000000u + (uint32_t)nfake * 0x10000u;
    uc_mem_map(c->uc, base, 0x1000, UC_PROT_ALL); wr16(c, base, 0x5a4d);
    snprintf(fake_mods[nfake].name, 64, "%s", st); fake_mods[nfake++].base = base;
    if (vlog(c)) fprintf(stderr, "[mod] fake module %s at %08x\n", st, base);
    return base;
}
static const char *fake_dll_name(uint32_t base) { for (int i = 0; i < nfake; i++) if (fake_mods[i].base == base) return fake_mods[i].name; return NULL; }

H(GetModuleHandleA) { char n[260]; if (!a[0]) return 0x400000; rd_cstr(c, a[0], n, sizeof n); return module_handle(c, n); }
H(GetModuleHandleW) { char n[260]; if (!a[0]) return 0x400000; rd_wstr(c, a[0], n, sizeof n); return module_handle(c, n); }
H(LoadLibraryA) { char n[260]; rd_cstr(c, a[0], n, sizeof n); if (vlog(c)) fprintf(stderr, "[LoadLibrary] %s\n", n); return module_handle(c, n); }
H(LoadLibraryW) { char n[260]; rd_wstr(c, a[0], n, sizeof n); if (vlog(c)) fprintf(stderr, "[LoadLibrary] %s\n", n); return module_handle(c, n); }
H(LoadLibraryExA) { return h_LoadLibraryA(c, a); }
H(LoadLibraryExW) { return h_LoadLibraryW(c, a); }
H(GetProcAddress) {
    char n[128];
    if (a[1] < 0x10000) snprintf(n, sizeof n, "Ordinal_%u", a[1]); else rd_cstr(c, a[1], n, sizeof n);
    for (int i = 0; i < c->nmods; i++) if (c->mods[i].base == a[0]) { uint32_t r = find_export(c, &c->mods[i], n); if (vlog(c)) fprintf(stderr, "[GetProcAddress] %s!%s -> %08x\n", c->mods[i].name, n, r); return r; }
    const char *d = fake_dll_name(a[0]); char dll[80]; snprintf(dll, sizeof dll, "%s.dll", d ? d : "unknown");
    uint32_t r = get_stub_addr(c, dll, n);
    if (vlog(c)) fprintf(stderr, "[GetProcAddress] %s!%s -> stub %08x\n", dll, n, r);
    return r;
}
H(FreeLibrary) { UNUSED; return 1; }
H(GetModuleFileNameA) { UNUSED; const char *p = "C:\\BLUR\\Blur.exe"; wrbuf(c, a[1], p, strlen(p) + 1); return (uint32_t)strlen(p); }
H(GetModuleFileNameW) {
    const char *p = "C:\\BLUR\\Blur.exe"; for (size_t i = 0; i <= strlen(p); i++) wr16(c, a[1] + 2 * i, (uint16_t)p[i]);
    return (uint32_t)strlen(p);
}

// ---- process / thread identity ------------------------------------------------------------
H(GetCurrentProcess) { UNUSED; return 0xffffffffu; }
H(GetCurrentThread) { UNUSED; return 0xfffffffeu; }
H(GetCurrentProcessId) { UNUSED; return 0x1000; }
H(GetCurrentThreadId) { return cur_thread(c)->tid; }
H(GetVersion) { UNUSED; return 0x1db10106u; }                                // Windows 7 SP1, 6.1.7601
H(GetVersionExA) { UNUSED; uint32_t sz = rd32(c, a[0]); wr32(c, a[0] + 4, 6); wr32(c, a[0] + 8, 1); wr32(c, a[0] + 12, 7601); wr32(c, a[0] + 16, 2); (void)sz; return 1; }
H(GetVersionExW) { return h_GetVersionExA(c, a); }
H(IsDebuggerPresent) { UNUSED; return 0; }
H(ExitProcess) { UNUSED; c->exited = 1; c->exit_code = a[0]; return 0; }
H(GetLastError) { return rd32(c, cur_thread(c)->teb + 0x34); }
H(SetLastError) { wr32(c, cur_thread(c)->teb + 0x34, a[0]); return 0; }
H(SetUnhandledExceptionFilter) { UNUSED; return 0; }
H(UnhandledExceptionFilter) { UNUSED; return 0; }
H(IsProcessorFeaturePresent) { UNUSED; switch (a[0]) { case 6: case 10: case 13: return 1; } return 0; }
H(OutputDebugStringA) { char s[512]; rd_cstr(c, a[0], s, sizeof s); fprintf(stderr, "[OutputDebugString] %s", s); return 0; }
H(GetSystemInfo) {
    uint8_t z[36] = {0}; wrbuf(c, a[0], z, 36);
    wr32(c, a[0] + 4, 0x1000); wr32(c, a[0] + 8, 0x10000); wr32(c, a[0] + 12, 0x7ffeffff); wr32(c, a[0] + 16, 0xff);
    wr32(c, a[0] + 20, 8); wr32(c, a[0] + 24, 586); wr32(c, a[0] + 28, 0x10000); wr16(c, a[0] + 32, 6);
    return 0;
}

// ---- heap -------------------------------------------------------------------------------------
H(GetProcessHeap) { UNUSED; return 0x1111; }
H(HeapCreate) { UNUSED; return 0x1111; }
H(HeapDestroy) { UNUSED; return 1; }
H(HeapAlloc) { return guest_alloc(c, a[2]); }
H(HeapFree) { guest_free(c, a[2]); return 1; }
H(HeapSize) { return guest_blocksize(c, a[2]); }
H(HeapValidate) { UNUSED; return 1; }
H(HeapSetInformation) { UNUSED; return 1; }
H(HeapReAlloc) {
    uint32_t old = a[2], osz = guest_blocksize(c, old);
    if (old && a[3] <= guest_blockcap(c, old)) { wr32(c, old - 16, a[3]); return old; }       // still fits in the block
    uint32_t n = guest_alloc(c, a[3]);
    if (old && n) { uint8_t *tmp = malloc(osz + 1); rdbuf(c, old, tmp, osz); wrbuf(c, n, tmp, osz < a[3] ? osz : a[3]); free(tmp); guest_free(c, old); }
    return n;
}
H(HeapLock) { UNUSED; return 1; }
H(HeapUnlock) { UNUSED; return 1; }
H(HeapCompact) { UNUSED; return 0x1000; }
H(LocalAlloc) { return guest_alloc(c, a[1]); }
H(LocalFree) { guest_free(c, a[0]); return 0; }
H(GlobalAlloc) { return guest_alloc(c, a[1]); }
H(GlobalFree) { guest_free(c, a[0]); return 0; }

// ---- virtual memory -----------------------------------------------------------------------------
static uint32_t virt_next = 0x58000000u;
static struct { uint32_t base, size; } vallocs[4096]; static int nvallocs;
static void zero_range(Ctx *c, uint32_t addr, uint32_t size) {            // Windows returns zero pages after decommit/release
    static uint8_t z[1 << 16]; for (uint32_t o = 0; o < size; o += sizeof z) { uint32_t n = size - o < sizeof z ? size - o : (uint32_t)sizeof z; uc_mem_write(c->uc, addr + o, z, n); }
}
H(VirtualAlloc) {                                                          // lpAddress, size, type, protect
    uint32_t addr = a[0], size = (a[1] + 0xfff) & ~0xfffu, type = a[2];
    if (!addr) { addr = virt_next; uint32_t span = (size + 0xffff) & ~0xffffu; virt_next += span; if (nvallocs < 4096) { vallocs[nvallocs].base = addr; vallocs[nvallocs++].size = span; } }
    uint32_t pg = addr & ~0xfffu, end = (addr + size + 0xfff) & ~0xfffu;
    for (uint32_t p = pg; p < end; p += 0x1000) uc_mem_map(c->uc, p, 0x1000, UC_PROT_ALL);   // already-mapped pages keep their contents
    if (vlog(c)) fprintf(stderr, "[VirtualAlloc] %08x size=%x type=%x -> %08x\n", a[0], a[1], type, addr);
    return addr;
}
H(VirtualFree) {                                                           // lpAddress, size, freeType
    uint32_t addr = a[0], size = a[1], type = a[2];
    if (type & 0x8000) {                                                   // MEM_RELEASE: whole allocation
        for (int i = 0; i < nvallocs; i++) if (vallocs[i].base == addr) { zero_range(c, addr, vallocs[i].size); return 1; }
        if (size) zero_range(c, addr, (size + 0xfff) & ~0xfffu);
    } else if (type & 0x4000) zero_range(c, addr & ~0xfffu, (size + 0xfff + (addr & 0xfff)) & ~0xfffu);   // MEM_DECOMMIT
    return 1;
}
H(VirtualProtect) { if (a[3]) wr32(c, a[3], 4); return 1; }
H(VirtualLock) { UNUSED; return 1; }
H(VirtualQuery) {
    uint32_t b = a[0] & ~0xfffu; uint8_t z[28] = {0}; wrbuf(c, a[1], z, 28);
    wr32(c, a[1], b); wr32(c, a[1] + 4, b); wr32(c, a[1] + 8, 4); wr32(c, a[1] + 12, 0x1000);
    wr32(c, a[1] + 16, 0x1000); wr32(c, a[1] + 20, 4); wr32(c, a[1] + 24, 0x20000);
    return 28;
}

// ---- TLS / FLS (same slot space; single-threaded) --------------------------------------------------
H(TlsAlloc) { return c->tls_next++; }
H(TlsFree) { UNUSED; return 1; }
H(TlsGetValue) { return rd32(c, rd32(c, cur_thread(c)->teb + 0x2c) + 4 * a[0]); }
H(TlsSetValue) { wr32(c, rd32(c, cur_thread(c)->teb + 0x2c) + 4 * a[0], a[1]); return 1; }

H(FlsAlloc) { UNUSED; return c->tls_next++; }
H(FlsFree) { UNUSED; return 1; }
H(FlsGetValue) { return h_TlsGetValue(c, a); }
H(FlsSetValue) { return h_TlsSetValue(c, a); }

// ---- synchronisation (no-ops) -------------------------------------------------------------------------
H(InterlockedIncrement) { uint32_t v = rd32(c, a[0]) + 1; wr32(c, a[0], v); return v; }
H(InterlockedDecrement) { uint32_t v = rd32(c, a[0]) - 1; wr32(c, a[0], v); return v; }
// Busy-wait detection: the same thread polling the same address from the same call site many times in a row is a spin loop.
// After a while it sleeps briefly (guest time) so the thread it is waiting for gets the CPU instead of competing with the poller.
static int spinning(Ctx *c, uint32_t addr) {
    Thread *t = cur_thread(c); uint32_t esp = cur_esp(c), ret = rd32(c, esp);
    if (t->wake_at) { if (now_us() >= t->wake_at) { t->wake_at = 0; t->spin_n = 0; return 0; } c->block = 1; return 1; }      // still napping
    if (t->spin_ret == ret && t->spin_addr == addr) t->spin_n++; else { t->spin_ret = ret; t->spin_addr = addr; t->spin_n = 0; }
    if (t->spin_n > 300) { t->wake_at = now_us() + 300; c->block = 1; return 1; }
    return 0;
}
H(InterlockedExchange) { if (spinning(c, a[0])) return 0; uint32_t o = rd32(c, a[0]); wr32(c, a[0], a[1]); if (o != 0 && a[1] != 0) c->yield_req = 1; return o; }
H(InterlockedExchangeAdd) { uint32_t o = rd32(c, a[0]); wr32(c, a[0], o + a[1]); return o; }
H(InterlockedCompareExchange) { uint32_t o = rd32(c, a[0]); if (o == a[2]) wr32(c, a[0], a[1]); else c->yield_req = 1; return o; }

// ---- pointer encoding, time ----------------------------------------------------------------------------
H(EncodePointer) { UNUSED; return a[0]; }
H(DecodePointer) { UNUSED; return a[0]; }
H(GetTickCount) { UNUSED; return (uint32_t)(now_us() / 1000); }
H(QueryPerformanceCounter) { uint64_t t = now_us() * 10; wr32(c, a[0], (uint32_t)t); wr32(c, a[0] + 4, (uint32_t)(t >> 32)); return 1; }   // 10 MHz, on the guest clock
H(QueryPerformanceFrequency) { wr32(c, a[0], 10000000); wr32(c, a[0] + 4, 0); return 1; }
H(GetSystemTimeAsFileTime) { uint64_t t = (11644473600ull * 1000000ull + now_us()) * 10; wr32(c, a[0], (uint32_t)t); wr32(c, a[0] + 4, (uint32_t)(t >> 32)); return 0; }

// ---- startup info / environment / code pages -------------------------------------------------------------
static uint32_t s_cmdA, s_cmdW, s_envW, s_envA;
H(GetCommandLineA) { UNUSED; return cached_str(c, &s_cmdA, "Blur.exe\0", 10); }
H(GetCommandLineW) { UNUSED; static const uint16_t w[] = {'B','l','u','r','.','e','x','e',0}; return cached_str(c, &s_cmdW, w, sizeof w); }
H(GetStartupInfoA) { uint8_t z[68] = {0}; wrbuf(c, a[0], z, 68); wr32(c, a[0], 68); return 0; }
H(GetStartupInfoW) { return h_GetStartupInfoA(c, a); }
H(GetEnvironmentStringsW) { UNUSED; static const uint16_t w[] = {'A','=','1',0,0}; return cached_str(c, &s_envW, w, sizeof w); }
H(GetEnvironmentStringsA) { UNUSED; return cached_str(c, &s_envA, "A=1\0\0", 6); }
H(FreeEnvironmentStringsW) { UNUSED; return 1; }
H(FreeEnvironmentStringsA) { UNUSED; return 1; }
H(GetEnvironmentVariableA) { UNUSED; return 0; }
H(GetEnvironmentVariableW) { UNUSED; return 0; }
H(GetStdHandle) { UNUSED; return 0x10 + a[0]; }
H(SetHandleCount) { UNUSED; return a[0]; }
H(GetFileType) { UNUSED; return 2; }
H(GetACP) { UNUSED; return 1252; }
H(GetOEMCP) { UNUSED; return 437; }
H(IsValidCodePage) { UNUSED; return 1; }
H(GetCPInfo) { uint8_t z[20] = {0}; wrbuf(c, a[1], z, 20); wr32(c, a[1], 1); wr8(c, a[1] + 4, '?'); return 1; }
H(MultiByteToWideChar) {
    uint32_t len = a[3] == 0xffffffffu ? 0 : a[3];
    if (a[3] == 0xffffffffu) { char t[1]; len = 0; while (rd8(c, a[2] + len)) len++; len++; (void)t; }
    if (a[5]) { if (a[5] < len) return 0; for (uint32_t i = 0; i < len; i++) wr16(c, a[4] + 2 * i, rd8(c, a[2] + i)); }
    return len;
}
H(WideCharToMultiByte) {
    uint32_t len = a[3];
    if (a[3] == 0xffffffffu) { len = 0; while (rd16(c, a[2] + 2 * len)) len++; len++; }
    if (a[5]) { if (a[5] < len) return 0; for (uint32_t i = 0; i < len; i++) { uint16_t ch = rd16(c, a[2] + 2 * i); wr8(c, a[4] + i, ch < 256 ? (uint8_t)ch : '?'); } }
    return len;
}
H(GetStringTypeW) {   // CT_CTYPE1 classification for ASCII
    for (uint32_t i = 0; i < a[2]; i++) {
        uint16_t ch = rd16(c, a[1] + 2 * i), t = 0;
        if (ch < 128) { if (isupper(ch)) t |= 1; if (islower(ch)) t |= 2; if (isdigit(ch)) t |= 4; if (isspace(ch)) t |= 8; if (ispunct(ch)) t |= 0x10; if (iscntrl(ch)) t |= 0x20; if (ch == ' ') t |= 0x40; if (isxdigit(ch)) t |= 0x80; if (isalpha(ch)) t |= 0x100; }
        wr16(c, a[3] + 2 * i, t);
    }
    return 1;
}
H(LCMapStringW) { // copy-through
    uint32_t n = a[3] == 0xffffffffu ? 0 : a[3]; if (a[3] == 0xffffffffu) { while (rd16(c, a[2] + 2 * n)) n++; n++; }
    if (a[5]) for (uint32_t i = 0; i < n && i < a[5]; i++) wr16(c, a[4] + 2 * i, rd16(c, a[2] + 2 * i)); return n;
}
H(lstrlenA) { char s[1024]; return (uint32_t)rd_cstr(c, a[0], s, sizeof s); }


// ---- printf-style formatting for wsprintf (cdecl varargs: a[0]=buf, a[1]=fmt, a[2..]=args) --------------
static uint32_t guest_format(Ctx *c, uint32_t fmtp, uint32_t *va, char *out, int outmax, int wide) {
    char fmt[512]; if (wide) rd_wstr(c, fmtp, fmt, sizeof fmt); else rd_cstr(c, fmtp, fmt, sizeof fmt);
    int o = 0, ai = 0;
    for (char *p = fmt; *p && o < outmax - 1; p++) {
        if (*p != '%') { out[o++] = *p; continue; }
        char spec[16]; int k = 0; spec[k++] = *p++;
        while (*p && strchr("-+ #0123456789.", *p) && k < 12) spec[k++] = *p++;
        while (*p && strchr("lhI", *p) && k < 14) { if (*p != 'l' && *p != 'h') spec[k++] = *p; p++; }   // length modifiers ignored (32-bit)
        char t = *p; if (!t) break;
        char tmp[300];
        if (t == '%') { out[o++] = '%'; continue; }
        spec[k++] = t == 's' || t == 'S' ? 's' : t; spec[k] = 0;
        if (t == 's' || t == 'S') { char sv[256]; if (wide || t == 'S') rd_wstr(c, va[ai++], sv, sizeof sv); else rd_cstr(c, va[ai++], sv, sizeof sv); snprintf(tmp, sizeof tmp, spec, sv); }
        else if (t == 'c') snprintf(tmp, sizeof tmp, spec, (int)va[ai++]);
        else snprintf(tmp, sizeof tmp, spec, va[ai++]);
        for (char *q = tmp; *q && o < outmax - 1; q++) out[o++] = *q;
    }
    out[o] = 0; return (uint32_t)o;
}
H(wsprintfA) { char buf[1024]; uint32_t n = guest_format(c, a[1], a + 2, buf, sizeof buf, 0); wrbuf(c, a[0], buf, n + 1); return n; }
H(wsprintfW) { char buf[1024]; uint32_t n = guest_format(c, a[1], a + 2, buf, sizeof buf, 1); for (uint32_t i = 0; i <= n; i++) wr16(c, a[0] + 2 * i, (uint8_t)buf[i]); return n; }

// ---- threads, kernel objects, waiting ---------------------------------------------------------------------------
// Blocking handlers set c->block and return; the call is re-run after a thread switch until it can complete.
static uint32_t cur_teb(Ctx *c) { return cur_thread(c)->teb; }
static void set_err(Ctx *c, uint32_t v) { wr32(c, cur_teb(c) + 0x34, v); }
#define WAIT_TIMEOUT_  0x102u
#define WAIT_FAILED_   0xffffffffu
static Handle *hp(Ctx *c, uint32_t h) { if (h < 0x104 || (h & 3) || (h - 0x100) / 4 >= (uint32_t)c->nhandles) return NULL; return &c->handles[(h - 0x100) / 4]; }
static void log_name(Ctx *c, const char *what, uint32_t namep, int wide) {
    if (!vlog(c)) return; char n[128] = "(null)"; if (namep) { if (wide) rd_wstr(c, namep, n, sizeof n); else rd_cstr(c, namep, n, sizeof n); }
    fprintf(stderr, "[%s] name=%s\n", what, n);
}
// named kernel objects are shared process-wide: the same name returns the same handle (ERROR_ALREADY_EXISTS)
static struct { char name[96]; int type; uint32_t h; } named[256]; static int nnamed;
static uint32_t named_lookup(Ctx *c, int type, uint32_t namep, int wide) {
    if (!namep) return 0; char n[96]; if (wide) rd_wstr(c, namep, n, sizeof n); else rd_cstr(c, namep, n, sizeof n);
    for (int i = 0; i < nnamed; i++) if (named[i].type == type && !strcmp(named[i].name, n)) return named[i].h; return 0;
}
static void named_register(Ctx *c, int type, uint32_t namep, int wide, uint32_t h) {
    if (!namep || nnamed >= 256) return; char n[96]; if (wide) rd_wstr(c, namep, n, sizeof n); else rd_cstr(c, namep, n, sizeof n);
    snprintf(named[nnamed].name, sizeof named[nnamed].name, "%s", n); named[nnamed].type = type; named[nnamed++].h = h;
}
static uint32_t make_event(Ctx *c, uint32_t manual, uint32_t initial) { uint32_t h = new_handle(c, H_EVENT); Handle *o = hp(c, h); o->manual = manual != 0; o->signaled = initial != 0; return h; }
H(CreateEventA) { log_name(c, "CreateEvent", a[3], 0); uint32_t h = named_lookup(c, H_EVENT, a[3], 0); if (h) { set_err(c, 183); return h; }
    set_err(c, 0); h = make_event(c, a[1], a[2]); named_register(c, H_EVENT, a[3], 0, h); return h; }
H(CreateEventW) { log_name(c, "CreateEvent", a[3], 1); uint32_t h = named_lookup(c, H_EVENT, a[3], 1); if (h) { set_err(c, 183); return h; }
    set_err(c, 0); h = make_event(c, a[1], a[2]); named_register(c, H_EVENT, a[3], 1, h); return h; }
H(CreateMutexA) { log_name(c, "CreateMutex", a[2], 0); uint32_t h = named_lookup(c, H_MUTEX, a[2], 0); if (h) { set_err(c, 183); return h; }
    set_err(c, 0); h = new_handle(c, H_MUTEX); if (a[1]) { hp(c, h)->owner = cur_thread(c)->tid; hp(c, h)->count = 1; } named_register(c, H_MUTEX, a[2], 0, h); return h; }
H(CreateMutexW) { log_name(c, "CreateMutex", a[2], 1); uint32_t h = named_lookup(c, H_MUTEX, a[2], 1); if (h) { set_err(c, 183); return h; }
    set_err(c, 0); h = new_handle(c, H_MUTEX); if (a[1]) { hp(c, h)->owner = cur_thread(c)->tid; hp(c, h)->count = 1; } named_register(c, H_MUTEX, a[2], 1, h); return h; }
H(CreateSemaphoreA) { uint32_t h = named_lookup(c, H_SEMAPHORE, a[3], 0); if (h) { set_err(c, 183); return h; }
    set_err(c, 0); h = new_handle(c, H_SEMAPHORE); hp(c, h)->count = (int)a[1]; hp(c, h)->max = (int)a[2]; named_register(c, H_SEMAPHORE, a[3], 0, h); return h; }
H(CreateSemaphoreW) { uint32_t h = named_lookup(c, H_SEMAPHORE, a[3], 1); if (h) { set_err(c, 183); return h; }
    set_err(c, 0); h = new_handle(c, H_SEMAPHORE); hp(c, h)->count = (int)a[1]; hp(c, h)->max = (int)a[2]; named_register(c, H_SEMAPHORE, a[3], 1, h); return h; }
H(ReleaseSemaphore) { Handle *o = hp(c, a[0]); if (!o) return 0; if (a[2]) wr32(c, a[2], (uint32_t)o->count); o->count += (int)a[1]; c->yield_req = 1; return 1; }
H(OpenEventA) { uint32_t h = named_lookup(c, H_EVENT, a[2], 0); if (!h) set_err(c, 2); return h; }
H(OpenMutexA) { uint32_t h = named_lookup(c, H_MUTEX, a[2], 0); if (!h) set_err(c, 2); return h; }
H(CloseHandle) { Handle *o = hp(c, a[0]); if (o && o->type == H_FILE && o->fp) { fclose(o->fp); o->fp = NULL; } return 1; }
H(SetEvent) { Handle *o = hp(c, a[0]); if (!o) return 0; o->signaled = 1; c->yield_req = 1; return 1; }
void hle_signal_event(Ctx *c, uint32_t h) { Handle *o = hp(c, h); if (o && o->type == H_EVENT) o->signaled = 1; }
H(ResetEvent) { Handle *o = hp(c, a[0]); if (!o) return 0; o->signaled = 0; return 1; }
H(PulseEvent) { UNUSED; return 1; }
H(ReleaseMutex) { Handle *o = hp(c, a[0]); if (!o) return 0; if (o->count > 0 && --o->count == 0) { o->owner = 0; c->yield_req = 1; } return 1; }     // hand off to a waiting thread
H(CreateFileMappingA) { log_name(c, "CreateFileMapping", a[5], 0); set_err(c, 0); return new_handle(c, H_EVENT); }
H(MapViewOfFile) { UNUSED; uint32_t sz = a[4] ? a[4] : 0x10000; uint32_t args[4] = {0, sz, 0x3000, 4}; return h_VirtualAlloc(c, args); }
H(UnmapViewOfFile) { UNUSED; return 1; }

// try to take ownership of / consume a waitable object; returns 1 if the wait is satisfied
static int try_acquire(Ctx *c, Handle *o) {
    uint32_t me = cur_thread(c)->tid;
    switch (o->type) {
    case H_EVENT: if (!o->signaled) return 0; if (!o->manual) o->signaled = 0; return 1;
    case H_MUTEX: if (o->count == 0 || o->owner == me) { o->owner = me; o->count++; return 1; } return 0;
    case H_SEMAPHORE: if (o->count > 0) { o->count--; return 1; } return 0;
    case H_THREAD: return o->signaled;
    }
    return 1;
}
// returns 1 once the deadline has passed (and resets it); arms the deadline on the first call
static int timed_out(Ctx *c, uint32_t ms) {
    Thread *t = cur_thread(c);
    if (ms == 0xffffffffu) return 0;
    if (!t->wake_at) { t->wake_at = now_us() + (uint64_t)ms * 1000ull; if (ms == 0) { t->wake_at = 1; } }
    if (now_us() >= t->wake_at) { t->wake_at = 0; return 1; }
    return 0;
}
H(WaitForSingleObject) {
    Handle *o = hp(c, a[0]); if (!o) { if (vlog(c)) fprintf(stderr, "[wait] WaitForSingleObject on invalid handle %08x (t%d)\n", a[0], c->cur); set_err(c, 6); return WAIT_FAILED_; }
    if (try_acquire(c, o)) { cur_thread(c)->wake_at = 0; return 0; }
    if (timed_out(c, a[1])) return WAIT_TIMEOUT_;
    c->block = 1; return 0;
}
H(SignalObjectAndWait) {        // (hSignal, hWait, ms, alertable): signal once, then wait like WaitForSingleObject (the call is re-run until it completes)
    Thread *t = cur_thread(c); Handle *s = hp(c, a[0]), *w = hp(c, a[1]);
    if (!w) { set_err(c, 6); t->sig_done = 0; return WAIT_FAILED_; }
    if (!t->sig_done) {
        t->sig_done = 1;
        if (s) { if (s->type == H_EVENT) s->signaled = 1; else if (s->type == H_MUTEX) { if (s->count > 0 && --s->count == 0) s->owner = 0; } else if (s->type == H_SEMAPHORE) s->count++; }
    }
    if (try_acquire(c, w)) { t->sig_done = 0; t->wake_at = 0; return 0; }
    if (timed_out(c, a[2])) { t->sig_done = 0; return WAIT_TIMEOUT_; }
    c->block = 1; return 0;
}
H(WaitForMultipleObjects) {
    uint32_t n = a[0], all = a[2], hs[64]; if (n > 64) return WAIT_FAILED_;
    for (uint32_t i = 0; i < n; i++) { hs[i] = rd32(c, a[1] + 4 * i); if (!hp(c, hs[i])) return WAIT_FAILED_; }
    if (all) {
        int ready = 1; for (uint32_t i = 0; i < n; i++) { Handle *o = hp(c, hs[i]); if (!(o->type == H_MUTEX ? (o->count == 0 || o->owner == cur_thread(c)->tid) : o->type == H_SEMAPHORE ? o->count > 0 : o->signaled)) ready = 0; }
        if (ready) { for (uint32_t i = 0; i < n; i++) try_acquire(c, hp(c, hs[i])); cur_thread(c)->wake_at = 0; return 0; }
    } else for (uint32_t i = 0; i < n; i++) if (try_acquire(c, hp(c, hs[i]))) { cur_thread(c)->wake_at = 0; return i; }
    if (timed_out(c, a[3])) return WAIT_TIMEOUT_;
    c->block = 1; return 0;
}
extern uint32_t g_api_ret;
static void sleep_hist(Ctx *c, uint32_t ms, uint32_t a1) {
    static struct { int tid; uint32_t ms, ret; uint64_t n; } h[64]; static int nh; static uint64_t last; static int on = -1;
    if (on < 0) on = getenv("BLUR_SLEEPHIST") != NULL; if (!on) return;
    int tid = c->cur, i; for (i = 0; i < nh; i++) if (h[i].tid == tid && h[i].ms == ms) break;
    if (i == nh && nh < 64) { h[nh].tid = tid; h[nh].ms = ms; h[nh].ret = a1; nh++; } h[i].n++;
    uint64_t n = real_us(); if (n - last > 5000000) { last = n; for (int k = 0; k < nh; k++) fprintf(stderr, "[sleep] thread %d Sleep(%u) ret=%08x x%llu\n", h[k].tid, h[k].ms, h[k].ret, (unsigned long long)h[k].n); fprintf(stderr, "[sleep] ----\n"); }
}
H(Sleep) { static int nolimit = -1; if (nolimit < 0) nolimit = getenv("BLUR_NOLIMIT") != NULL; if (nolimit && a[1] == 0x6cb983) return 0;   // experiment: drop the game's frame-limiter sleep
    if (!cur_thread(c)->wake_at) sleep_hist(c, a[0], a[1]); if (timed_out(c, a[0])) return 0; c->block = 1; return 0; }
H(SleepEx) { return h_Sleep(c, a); }
H(SwitchToThread) { Thread *t = cur_thread(c); if (t->wake_at) { t->wake_at = 0; return 1; } t->wake_at = 1; c->block = 1; return 0; }
H(CreateThread) {
    uint32_t h = spawn_thread(c, a[2], a[3], (a[4] & 4) != 0, a[1]);
    if (h && a[5]) wr32(c, a[5], c->threads[hp(c, h)->thread].tid);
    return h;
}
H(ResumeThread) { Handle *o = hp(c, a[0]); if (!o) return WAIT_FAILED_; Thread *t = &c->threads[o->thread]; uint32_t was = t->suspended; t->suspended = 0; return was; }
H(SuspendThread) { Handle *o = hp(c, a[0]); if (!o) return WAIT_FAILED_; Thread *t = &c->threads[o->thread]; uint32_t was = t->suspended; t->suspended = 1; return was; }
H(ExitThread) {
    Thread *t = cur_thread(c); t->done = 1; t->exit_code = a[0]; if (t->handle) hp(c, t->handle)->signaled = 1;
    if (c->cur == 0) { c->exited = 1; c->exit_code = a[0]; } else c->block = 1;
    return 0;
}
H(GetExitCodeThread) { Handle *o = hp(c, a[0]); if (!o) return 0; Thread *t = &c->threads[o->thread]; wr32(c, a[1], t->done ? t->exit_code : 259); return 1; }
H(TerminateThread) { Handle *o = hp(c, a[0]); if (!o) return 0; Thread *t = &c->threads[o->thread]; t->done = 1; t->exit_code = a[1]; o->signaled = 1; return 1; }
H(SetThreadPriority) { UNUSED; return 1; }
H(GetThreadPriority) { UNUSED; return 0; }
H(SetThreadAffinityMask) { UNUSED; return 0xff; }
H(SetThreadIdealProcessor) { UNUSED; return 0; }
H(GetCurrentThreadId2) { return cur_thread(c)->tid; }

// critical sections: owner/recursion tracked in the guest struct (+8 recursion, +12 owner)
H(InitializeCriticalSection) { wr32(c, a[0] + 4, 0xffffffff); wr32(c, a[0] + 8, 0); wr32(c, a[0] + 12, 0); return 0; }
H(InitializeCriticalSectionAndSpinCount) { h_InitializeCriticalSection(c, a); return 1; }
H(InitializeCriticalSectionEx) { h_InitializeCriticalSection(c, a); return 1; }
static int cs_take(Ctx *c, uint32_t cs) {
    uint32_t me = cur_thread(c)->tid, owner = rd32(c, cs + 12);
    if (owner && owner != me) return 0;
    wr32(c, cs + 12, me); wr32(c, cs + 8, rd32(c, cs + 8) + 1); return 1;
}
H(EnterCriticalSection) { if (cs_take(c, a[0])) return 0; c->block = 1; return 0; }
H(TryEnterCriticalSection) { return cs_take(c, a[0]); }
H(LeaveCriticalSection) { uint32_t r = rd32(c, a[0] + 8); if (r) { r--; wr32(c, a[0] + 8, r); if (!r) wr32(c, a[0] + 12, 0); } return 0; }
H(DeleteCriticalSection) { UNUSED; return 0; }

// ---- table ---------------------------------------------------------------------------------------------------
#define E(n, na) { #n, na, 0, h_##n }
static const HleDef table[] = {
    E(GetModuleHandleA,1), E(GetModuleHandleW,1), E(LoadLibraryA,1), E(LoadLibraryW,1), E(LoadLibraryExA,3), E(LoadLibraryExW,3),
    E(GetProcAddress,2), E(FreeLibrary,1), E(GetModuleFileNameA,3), E(GetModuleFileNameW,3),
    E(GetCurrentProcess,0), E(GetCurrentThread,0), E(GetCurrentProcessId,0), E(GetCurrentThreadId,0),
    E(GetVersion,0), E(GetVersionExA,1), E(GetVersionExW,1), E(IsDebuggerPresent,0), E(ExitProcess,1), 
    E(GetLastError,0), E(SetLastError,1), E(SetUnhandledExceptionFilter,1), E(UnhandledExceptionFilter,1),
    E(IsProcessorFeaturePresent,1), E(OutputDebugStringA,1), E(GetSystemInfo,1),
    E(GetProcessHeap,0), E(HeapCreate,3), E(HeapDestroy,1), E(HeapAlloc,3), E(HeapFree,3), E(HeapSize,3), E(HeapValidate,3),
    E(HeapSetInformation,4), E(HeapReAlloc,4), E(HeapLock,1), E(HeapUnlock,1), E(HeapCompact,2),
    E(LocalAlloc,2), E(LocalFree,1), E(GlobalAlloc,2), E(GlobalFree,1),
    E(VirtualAlloc,4), E(VirtualFree,3), E(VirtualProtect,4), E(VirtualLock,2), E(VirtualQuery,3),
    E(FlsAlloc,1), E(FlsFree,1), E(FlsGetValue,1), E(FlsSetValue,2),
    E(TlsAlloc,0), E(TlsFree,1), E(TlsGetValue,1), E(TlsSetValue,2),
    E(InitializeCriticalSection,1), E(InitializeCriticalSectionAndSpinCount,2), E(InitializeCriticalSectionEx,3),
    E(EnterCriticalSection,1), E(LeaveCriticalSection,1), E(DeleteCriticalSection,1), E(TryEnterCriticalSection,1),
    E(InterlockedIncrement,1), E(InterlockedDecrement,1), E(InterlockedExchange,2), E(InterlockedExchangeAdd,2), E(InterlockedCompareExchange,3),
    E(EncodePointer,1), E(DecodePointer,1), E(GetTickCount,0), E(QueryPerformanceCounter,1), E(QueryPerformanceFrequency,1),
    E(GetSystemTimeAsFileTime,1),
    E(GetCommandLineA,0), E(GetCommandLineW,0), E(GetStartupInfoA,1), E(GetStartupInfoW,1),
    E(GetEnvironmentStringsW,0), E(GetEnvironmentStringsA,0), E(FreeEnvironmentStringsW,1), E(FreeEnvironmentStringsA,1),
    E(GetEnvironmentVariableA,3), E(GetEnvironmentVariableW,3), E(GetStdHandle,1), E(SetHandleCount,1), E(GetFileType,1),
    E(GetACP,0), E(GetOEMCP,0), E(IsValidCodePage,1), E(GetCPInfo,2), E(MultiByteToWideChar,6), E(WideCharToMultiByte,8),
    { "wsprintfA", 2, 1, h_wsprintfA }, { "wsprintfW", 2, 1, h_wsprintfW },
    E(CreateEventA,4), E(CreateEventW,4), E(CreateMutexA,3), E(CreateMutexW,3), E(CreateSemaphoreA,4), E(CreateSemaphoreW,4), E(ReleaseSemaphore,3),
    E(OpenEventA,3), E(OpenMutexA,3), E(CloseHandle,1), E(SetEvent,1), E(ResetEvent,1), E(PulseEvent,1), E(ReleaseMutex,1),
    E(WaitForSingleObject,2), E(SignalObjectAndWait,4), E(WaitForMultipleObjects,4), E(Sleep,1), E(SleepEx,2), E(SwitchToThread,0),
    E(CreateThread,6), E(ResumeThread,1), E(SuspendThread,1), E(ExitThread,1), E(GetExitCodeThread,2), E(TerminateThread,2),
    E(SetThreadPriority,2), E(GetThreadPriority,1), E(SetThreadAffinityMask,2), E(SetThreadIdealProcessor,2),
    E(CreateFileMappingA,6), E(MapViewOfFile,5), E(UnmapViewOfFile,1),
    E(GetStringTypeW,4), E(LCMapStringW,6), E(lstrlenA,1),
};
const HleDef *hle_lookup(const char *name) {
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) if (!strcmp(table[i].name, name)) return &table[i];
    return hle2_lookup(name);
}

// ---- snapshot of the host-side state of this file (see snap_ctx.c) ------------------------------------------------
void hle_snap(void (*cb)(void *, void *, size_t), void *u) {
    cb(u, &virt_next, sizeof virt_next); cb(u, &nvallocs, sizeof nvallocs); cb(u, vallocs, sizeof vallocs);
    cb(u, &nfake, sizeof nfake); cb(u, fake_mods, sizeof fake_mods); cb(u, &nnamed, sizeof nnamed); cb(u, named, sizeof named);
    cb(u, &s_cmdA, 4); cb(u, &s_cmdW, 4); cb(u, &s_envW, 4); cb(u, &s_envA, 4);
}
