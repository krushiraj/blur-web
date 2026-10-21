// Blur HLE harness: loads Blur.exe + the real msvcr90.dll into a Unicorn x86 emulator,
// builds a fake TEB/PEB, and traps every other imported API into hle.c.
// Usage: blurhle <path/to/BLUR/Blur.exe> [path/to/msvcr90.dll]
#include "emu.h"
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#else
#include <sys/mman.h>
#endif
#include <ctype.h>
#include <unistd.h>

#ifdef _WIN32
#define MAP_FAILED_W ((void *)-1)
#else
#define MAP_FAILED_W MAP_FAILED
#endif
void load_sigs(const char *path);
static int ieq(const char *a, const char *b) { return strcasecmp(a, b) == 0; }
static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

// (moved to hostcommon.c: lines 11-60 of the original)
// (moved to hostcommon.c: lines 61-82 of the original)

// ordinal -> name for the winsock DLLs (names per the public ws2_32 export table)
static const char *ws_ordinal_(int o);
static const char *ordinal_name(const char *dll, int o) {
    if (!strncasecmp(dll, "XINPUT", 6)) return o == 2 ? "XInputGetState" : o == 3 ? "XInputSetState" : NULL;
    if (!strncasecmp(dll, "DSOUND", 6)) return o == 1 ? "DirectSoundCreate" : o == 2 ? "DirectSoundEnumerateA" : o == 3 ? "DirectSoundEnumerateW" : o == 11 ? "DirectSoundCreate8" : NULL;
    return ws_ordinal_(o);
}
static const char *ws_ordinal_(int o) {
    switch (o) {
    case 1: return "accept"; case 2: return "bind"; case 3: return "closesocket"; case 4: return "connect";
    case 5: return "getpeername"; case 6: return "getsockname"; case 7: return "getsockopt";
    case 8: return "htonl"; case 9: return "htons"; case 10: return "ioctlsocket"; case 11: return "inet_addr";
    case 12: return "inet_ntoa"; case 13: return "listen"; case 14: return "ntohl"; case 15: return "ntohs";
    case 16: return "recv"; case 17: return "recvfrom"; case 18: return "select"; case 19: return "send";
    case 20: return "sendto"; case 21: return "setsockopt"; case 22: return "shutdown"; case 23: return "socket";
    case 52: return "gethostbyname"; case 57: return "gethostname";
    case 111: return "WSAGetLastError"; case 112: return "WSASetLastError"; case 115: return "WSAStartup";
    case 116: return "WSACleanup"; case 151: return "__WSAFDIsSet";
    }
    return NULL;
}

// (moved to hostcommon.c: lines 106-129 of the original)

uint32_t g_hw_lo, g_hw_hi; const char *g_cur_api;
uint8_t *G; int g_recomp; uint32_t g_oep_addr; static int g_after_oep;
static uc_hook emu_hh, emu_rh; static int emu_budget = 300;       // BLUR_EMUHOOK=addr (cdecl): log entry args and return value under emulation
static void emuhook_ret(uc_engine *u, uint64_t addr, uint32_t size, void *ud) { (void)size; (void)ud; (void)addr; uint32_t eax; uc_reg_read(u, UC_X86_REG_EAX, &eax); if (emu_budget-- > 0) fprintf(stderr, "[emuhook] return eax=%08x\n", eax); uc_hook_del(u, emu_rh); }
static void emuhook_enter(uc_engine *u, uint64_t addr, uint32_t size, void *ud) { (void)size; (void)ud; uint32_t esp, w[5]; uc_reg_read(u, UC_X86_REG_ESP, &esp); uc_mem_read(u, esp, w, 20);
    if (emu_budget-- > 0) fprintf(stderr, "[emuhook] enter %08x ret=%08x args %08x %08x %08x %08x\n", (uint32_t)addr, w[0], w[1], w[2], w[3], w[4]); uc_hook_add(u, &emu_rh, UC_HOOK_CODE, (void *)emuhook_ret, NULL, w[0], w[0]); }
static uc_hook oep_hook;
static void oep_cb(uc_engine *uc, uint64_t addr, uint32_t size, void *ud) {
    Ctx *c = ud; if (g_recomp) { int ids[8] = {UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI}; for (int i = 0; i < 8; i++) uc_reg_read(uc, ids[i], &c->oep_regs[i]); g_oep_addr = (uint32_t)addr; c->oep_hit = 1; uc_hook_del(uc, oep_hook); uc_emu_stop(uc); return; }
    g_after_oep = 1;
    uint32_t esp = 0, v[4] = {0}; uc_reg_read(uc, UC_X86_REG_ESP, &esp); uc_mem_read(uc, esp, v, 16);
    fprintf(stderr, "[oep] first instruction in .text at %08llx  esp=%08x [esp]=%08x %08x %08x\n", (unsigned long long)addr, esp, v[0], v[1], v[2]);
    uc_hook_del(uc, oep_hook);
}
static void stub_hook(uc_engine *uc, uint64_t addr, uint32_t size, void *ud) {
    Ctx *c = ud; (void)size;
    Stub *s = &c->stubs[(addr - STUB_BASE) / 16];
    if (s->raw) { s->fn(c, NULL); return; }
    uint32_t esp, ret, a[16] = {0};
    uc_reg_read(uc, UC_X86_REG_ESP, &esp);
    ret = rd32(c, esp);
    rdbuf(c, esp + 4, a, sizeof a);
    if (!s->fn) {
        fprintf(stderr, "\n[UNIMPLEMENTED] %s!%s  nargs=%d %s  ret=%08x\n  args:", s->dll, s->name, s->nargs, s->cdecl_ ? "cdecl" : "stdcall", ret);
        for (int i = 0; i < 6; i++) fprintf(stderr, " %08x", a[i]);
        fprintf(stderr, "\n");
        c->exited = 2; uc_emu_stop(uc); return;
    }
    if (c->verbose) {
        fprintf(stderr, "[api t%d] %s!%s(%08x,%08x,%08x,%08x) ret=%08x\n", c->cur, s->dll, s->name, a[0], a[1], a[2], a[3], ret);
    }
    s->calls++;
    uint32_t r = s->fn(c, a);
    if (c->exited) { uc_emu_stop(uc); return; }
    if (c->block) { c->block = 0; cur_thread(c)->blocked = 1; uc_emu_stop(uc); return; }   // leave EIP on the stub: the call re-runs on resume
    c->api_progress++;
    uint32_t nesp = esp + 4 + (s->cdecl_ ? 0 : 4 * (uint32_t)s->nargs);
    if (c->req.pending) {                                  // call guest code, then resume the original caller (see CallReq)
        CallReq q = c->req; c->req.pending = 0; Thread *t = cur_thread(c); uint32_t back = ret;
        if (q.have_cont) { t->cont[t->ncont++] = (Cont){ ret, q.cont_value, q.use_result }; back = cont_stub_addr(c); }
        for (int i = q.nargs - 1; i >= 0; i--) { nesp -= 4; wr32(c, nesp, q.args[i]); }
        nesp -= 4; wr32(c, nesp, back);
        uc_reg_write(uc, UC_X86_REG_ESP, &nesp); uc_reg_write(uc, UC_X86_REG_EIP, &q.fn); return;
    }
    uc_reg_write(uc, UC_X86_REG_EAX, &r);
    uc_reg_write(uc, UC_X86_REG_ESP, &nesp);
    uc_reg_write(uc, UC_X86_REG_EIP, &ret);
    if (c->yield_req) { c->yield_req = 0; uc_emu_stop(uc); }                                                      // cooperative yield requested by the handler
    if (c->api_slice && ++c->api_since_switch >= c->api_slice) { c->api_since_switch = 0; uc_emu_stop(uc); }   // preempt at an API boundary (never mid-sequence)
}

// ---------------------------------------------------------------------------------
// diagnostics: ring buffer of recent basic blocks, fault reporting
// ---------------------------------------------------------------------------------
uint32_t ring[64]; int ring_n;
static uint8_t *fnbits; static int fnlog_n; static uint8_t *blkbits; static const char *blklog;
static void block_hook(uc_engine *uc, uint64_t addr, uint32_t size, void *ud) {
    (void)size; (void)ud; ring[ring_n++ & 63] = (uint32_t)addr;
    if (blkbits && addr < 0x78600000) blkbits[addr >> 3] |= 1 << (addr & 7);
    if (fnbits && g_after_oep && addr < 0x2100000 && (fnbits[addr >> 3] >> (addr & 7) & 1) && fnlog_n < 400000) {
        uint32_t esp, eax, esi; uc_reg_read(uc, UC_X86_REG_ESP, &esp); uc_reg_read(uc, UC_X86_REG_EAX, &eax); uc_reg_read(uc, UC_X86_REG_ESI, &esi);
        fprintf(stderr, "[efn] %08x esp=%08x eax=%08x esi=%08x\n", (uint32_t)addr, esp, eax, esi); fnlog_n++;
    }
}
// BLUR_ALLOCTRACK=1: track the game's malloc (0x42c730 / 0x42c760) and free (0x42c7c0); report overlapping live blocks
typedef struct { uint32_t p, n; int tid; } Blk;
static Blk live[1 << 18]; static int nlive; static struct { uc_hook h; uint32_t size, ret; int tid; int used; } pend[64];
static void alloc_ret_hook(uc_engine *uc, uint64_t addr, uint32_t sz, void *ud) {
    Ctx *c = ud; (void)sz; int cur = c->cur;
    for (int i = 0; i < 64; i++) if (pend[i].used && pend[i].ret == (uint32_t)addr && pend[i].tid == cur) {
        uint32_t eax; uc_reg_read(uc, UC_X86_REG_EAX, &eax); uint32_t n = pend[i].size;
        for (int k = 0; k < nlive; k++) if (live[k].p && live[k].tid != cur && eax < live[k].p + live[k].n && live[k].p < eax + n) {
            fprintf(stderr, "[alloc] OVERLAP: t%d got %08x+%u overlapping live %08x+%u (t%d)\n", cur, eax, n, live[k].p, live[k].n, live[k].tid); break; }
        if (nlive < (1 << 18)) live[nlive++] = (Blk){eax, n, cur};
        uc_hook_del(uc, pend[i].h); pend[i].used = 0; return;
    }
}
static void alloc_entry_hook(uc_engine *uc, uint64_t addr, uint32_t sz, void *ud) {
    Ctx *c = ud; (void)sz; uint32_t esp; uc_reg_read(uc, UC_X86_REG_ESP, &esp);
    if (addr == 0x42c7c0) { uint32_t p = rd32(c, esp + 4); for (int k = 0; k < nlive; k++) if (live[k].p == p) { live[k] = live[--nlive]; break; } return; }
    if (rd32(c, esp + 4) > (16u << 20)) { fprintf(stderr, "[alloc] HUGE request %u bytes by t%d, chain:", rd32(c, esp + 4), c->cur); int n = 0;
        for (int i = 0; i < 400 && n < 12; i++) { uint32_t v = rd32(c, esp + 4 * i); if (v >= 0x401000 && v < 0xe51000) { fprintf(stderr, " %08x", v); n++; } } fprintf(stderr, "\n"); }
    for (int i = 0; i < 64; i++) if (!pend[i].used) { pend[i].used = 1; pend[i].size = rd32(c, esp + 4); pend[i].ret = rd32(c, esp); pend[i].tid = c->cur;
        uc_hook_add(uc, &pend[i].h, UC_HOOK_CODE, alloc_ret_hook, c, pend[i].ret, pend[i].ret); return; }
}
// BLUR_WATCH=addr[,addr...]: print registers and the dwords at ecx/esp whenever one of those addresses executes
static uint32_t watch_addrs[8]; static int nwatch;
static void watch_hook(uc_engine *uc, uint64_t addr, uint32_t size, void *ud) {
    Ctx *c = ud; (void)size; uint32_t r[5], edi = 0, esi = 0; int ids[5] = {UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_ESP, UC_X86_REG_EBP};
    uc_reg_read(uc, UC_X86_REG_EDI, &edi); uc_reg_read(uc, UC_X86_REG_ESI, &esi);
    for (int i = 0; i < 5; i++) uc_reg_read(uc, ids[i], &r[i]);
    fprintf(stderr, "[watch %08x] eax=%08x ecx=%08x edx=%08x esp=%08x ebp=%08x  [esp]=%08x %08x %08x\n", (uint32_t)addr, r[0], r[1], r[2], r[3], r[4], rd32(c, r[3]), rd32(c, r[3] + 4), rd32(c, r[3] + 8));
    if (getenv("BLUR_WATCH_ARGS")) { fprintf(stderr, "   args:"); for (int i = 1; i <= 8; i++) fprintf(stderr, " %08x", rd32(c, r[3] + 4 * i)); fprintf(stderr, "\n");
        if (getenv("BLUR_WATCH_DEREF")) for (int i = 1; i <= 7; i++) { uint32_t p = rd32(c, r[3] + 4 * i); if (p > 0x10000 && p < 0x7f000000) { fprintf(stderr, "   [arg%d]=%08x:", i, p); for (int k = 0; k < 12; k++) fprintf(stderr, " %08x", rd32(c, p + 4 * k)); fprintf(stderr, "\n"); } } }
    if (getenv("BLUR_WATCH_EDI") && edi > 0x10000) { fprintf(stderr, "   edi=%08x:", edi); for (int i = -4; i < 12; i++) fprintf(stderr, " %08x", rd32(c, edi + 4 * i)); fprintf(stderr, "\n"); }
    if (getenv("BLUR_WATCH_LIST")) { uint32_t lp = rd32(c, r[1] + 0xc), n = rd32(c, r[1] + 0x14); fprintf(stderr, "   mode list @%08x n=%u:", lp, n); for (uint32_t i = 0; i < n * 5 && i < 40; i++) fprintf(stderr, "%s%u", i % 5 ? " " : " | ", rd32(c, lp + 4 * i)); fprintf(stderr, "\n"); }
    if (r[1] > 0x10000 && r[1] < 0x7f000000) { fprintf(stderr, "   ecx:"); for (int i = 0; i < 12; i++) fprintf(stderr, " %08x", rd32(c, r[1] + 4 * i)); fprintf(stderr, "\n"); }
}
// BLUR_MEMW=addr[,addr]: log writes (and reads with BLUR_MEMR) to those guest addresses with the writing EIP and thread
static void memw_hook(uc_engine *uc, uc_mem_type type, uint64_t addr, int size, int64_t val, void *ud) {
    Ctx *c = ud; uint32_t eip; uc_reg_read(uc, UC_X86_REG_EIP, &eip);
    fprintf(stderr, "[mem%s t%d] %08x size=%d val=%08x by eip=%08x", type == UC_MEM_WRITE ? "W" : "R", c->cur, (uint32_t)addr, size, (uint32_t)val, eip);
    { uint32_t esp; uc_reg_read(uc, UC_X86_REG_ESP, &esp); int n = 0; fprintf(stderr, "  chain:"); for (int i = 0; i < 300 && n < 10; i++) { uint32_t v = rd32(c, esp + 4 * i); if (v >= 0x401000 && v < 0x206f000) { fprintf(stderr, " %08x", v); n++; } } }
    fprintf(stderr, "\n");
}
static void dump_regs(Ctx *c, const char *why) {
    uint32_t r[9]; int ids[9] = {UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_ESI,
                                 UC_X86_REG_EDI, UC_X86_REG_EBP, UC_X86_REG_ESP, UC_X86_REG_EIP};
    for (int i = 0; i < 9; i++) uc_reg_read(c->uc, ids[i], &r[i]);
    fprintf(stderr, "\n[FAULT] %s\n  eax=%08x ebx=%08x ecx=%08x edx=%08x\n  esi=%08x edi=%08x ebp=%08x esp=%08x eip=%08x\n",
            why, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8]);
    fprintf(stderr, "  stack:"); for (int i = 0; i < 8; i++) fprintf(stderr, " %08x", rd32(c, r[7] + 4 * i));
    fprintf(stderr, "\n  call chain (stack words that look like code addresses):");
    { int n = 0; for (int i = 0; i < 400 && n < 14; i++) { uint32_t v = rd32(c, r[7] + 4 * i); if (v >= 0x401000 && v < 0xe51000 && n < 14) { fprintf(stderr, " %08x", v); n++; } } }
    fprintf(stderr, "\n  last blocks:");
    for (int i = 16; i >= 1; i--) fprintf(stderr, " %08x", ring[(ring_n - i) & 63]);
    fprintf(stderr, "\n");
}
static bool mem_hook(uc_engine *uc, uc_mem_type type, uint64_t addr, int size, int64_t val, void *ud) {
    (void)uc; (void)size; (void)val;
    char msg[96]; const char *k = type == UC_MEM_READ_UNMAPPED ? "read" : type == UC_MEM_WRITE_UNMAPPED ? "write" : "fetch";
    snprintf(msg, sizeof msg, "unmapped %s at %08x", k, (uint32_t)addr);
    dump_regs(ud, msg); return false;
}
static bool insn_invalid_hook(uc_engine *uc, void *ud) { (void)uc; dump_regs(ud, "invalid instruction"); return false; }
static void intr_hook(uc_engine *uc, uint32_t intno, void *ud) { (void)uc; char m[64]; snprintf(m, sizeof m, "interrupt %u", intno); dump_regs(ud, m); }
static void sysenter_hook(uc_engine *uc, void *ud) { (void)uc; dump_regs(ud, "sysenter (direct syscall)"); uc_emu_stop(uc); }

// ---------------------------------------------------------------------------------
// PE loading
// ---------------------------------------------------------------------------------
// (moved to hostcommon.c: lines 257-271 of the original)
static uint32_t find_export_ord(Ctx *c, Module *m, uint32_t ord) {
    uint32_t d = m->base + m->exp_dir, ob = rd32(c, d + 16), nf = rd32(c, d + 20), funcs = m->base + rd32(c, d + 28);
    if (ord < ob || ord - ob >= nf) return 0;
    return m->base + rd32(c, funcs + 4 * (ord - ob));
}

static void setup_tls(Ctx *c, Module *m, uint32_t tls_rva) {
    uint32_t d = m->base + tls_rva;
    uint32_t raw0 = rd32(c, d), raw1 = rd32(c, d + 4), idx_addr = rd32(c, d + 8), zero = rd32(c, d + 16);
    uint32_t sz = (raw1 - raw0) + zero, idx = c->tls_next++;
    uint32_t blk = guest_alloc(c, sz ? sz : 4);
    for (uint32_t i = 0; i < raw1 - raw0; i++) wr8(c, blk + i, rd8(c, raw0 + i));
    wr32(c, idx_addr, idx);
    wr32(c, rd32(c, TEB_ADDR + 0x2c) + 4 * idx, blk);
    m->has_tls = 1; m->tls_raw0 = raw0; m->tls_raw1 = raw1; m->tls_zero = zero; m->tls_idx = idx;
    if (c->verbose) fprintf(stderr, "[tls] %s index=%u block=%08x size=%u\n", m->name, idx, blk, sz);
}

uint32_t load_module(Ctx *c, const char *path, const char *name) {
    FILE *f = fopen(path, "rb"); if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(len); if (fread(b, 1, len, f) != (size_t)len) exit(1); fclose(f);
    uint32_t pe = *(uint32_t *)(b + 0x3c);
    uint16_t nsec = *(uint16_t *)(b + pe + 6), osz = *(uint16_t *)(b + pe + 20), chars = *(uint16_t *)(b + pe + 22);
    uint8_t *opt = b + pe + 24;
    uint32_t entry = *(uint32_t *)(opt + 16), pref = *(uint32_t *)(opt + 28), simg = *(uint32_t *)(opt + 56), shdr = *(uint32_t *)(opt + 60);
    uint32_t *dd = (uint32_t *)(opt + 96);
    uint32_t base = pref, msize = align_up(simg, 0x1000);
    if (uc_mem_map(c->uc, base, msize, UC_PROT_ALL) != UC_ERR_OK) {
        static uint32_t next = MODMAP_BASE; base = next; next += align_up(msize, 0x10000);
        if (uc_mem_map(c->uc, base, msize, UC_PROT_ALL) != UC_ERR_OK) { fprintf(stderr, "cannot map %s\n", name); exit(1); }
    }
    uc_mem_write(c->uc, base, b, shdr);
    uint8_t *sh = b + pe + 24 + osz;
    for (int i = 0; i < nsec; i++) {
        uint32_t va = *(uint32_t *)(sh + 40 * i + 12), rsz = *(uint32_t *)(sh + 40 * i + 16), roff = *(uint32_t *)(sh + 40 * i + 20);
        if (rsz && roff + rsz <= (uint32_t)len) uc_mem_write(c->uc, base + va, b + roff, rsz);
    }
    // relocations
    int32_t delta = (int32_t)(base - pref);
    if (delta && dd[10]) {
        uint32_t rva = dd[10], end = rva + dd[11];
        while (rva < end) {
            uint32_t page = *(uint32_t *)(b + 0), bsz; (void)page;
            uint32_t pg = rd32(c, base + rva); bsz = rd32(c, base + rva + 4);
            for (uint32_t o = 8; o < bsz; o += 2) {
                uint16_t e = rd16(c, base + rva + o);
                if ((e >> 12) == 3) { uint32_t a = base + pg + (e & 0xfff); wr32(c, a, rd32(c, a) + delta); }
            }
            rva += bsz;
        }
    }
    Module *m = &c->mods[c->nmods++];
    snprintf(m->name, sizeof m->name, "%s", name);
    m->base = base; m->size = msize; m->entry = entry ? base + entry : 0; m->is_real = 1;
    m->exp_dir = dd[0]; m->exp_size = dd[1]; m->tls_dir = dd[18];
    // imports
    for (uint32_t d = dd[2]; d; d += 20) {
        uint32_t oft = rd32(c, base + d), nm = rd32(c, base + d + 12), ft = rd32(c, base + d + 16);
        if (!nm) break;
        char dll[64]; rd_cstr(c, base + nm, dll, sizeof dll);
        char stem[64]; snprintf(stem, sizeof stem, "%s", dll); char *dot = strrchr(stem, '.'); if (dot) *dot = 0;
        Module *real = NULL;
        for (int i = 0; i < c->nmods; i++) { char s2[64]; snprintf(s2, sizeof s2, "%s", c->mods[i].name); char *d2 = strrchr(s2, '.'); if (d2) *d2 = 0; if (ieq(s2, stem) && c->mods[i].is_real) real = &c->mods[i]; }
        uint32_t src = oft ? oft : ft;
        for (uint32_t i = 0;; i++) {
            uint32_t th = rd32(c, base + src + 4 * i); if (!th) break;
            char fname[128]; uint32_t addr = 0;
            if (th & 0x80000000) {
                uint32_t ord = th & 0xffff; const char *wn = ordinal_name(dll, ord);
                if (real) addr = find_export_ord(c, real, ord);
                if (!real) { if (wn) snprintf(fname, sizeof fname, "%s", wn); else snprintf(fname, sizeof fname, "Ordinal_%u", ord); addr = get_stub_addr(c, dll, fname); }
            } else {
                rd_cstr(c, base + th + 2, fname, sizeof fname);
                addr = real ? find_export(c, real, fname) : get_stub_addr(c, dll, fname);
                if (real && !addr) { fprintf(stderr, "[import] %s: %s not exported by %s\n", name, fname, real->name); addr = get_stub_addr(c, dll, fname); }
            }
            wr32(c, base + ft + 4 * i, addr);
        }
    }
    if (m->tls_dir) setup_tls(c, m, m->tls_dir);
    (void)chars; free(b);
    fprintf(stderr, "[load] %-14s base=%08x size=%08x entry=%08x\n", name, base, msize, m->entry);
    return base;
}

// ---------------------------------------------------------------------------------
// call into guest code (nested calls from API handlers are not supported)
// ---------------------------------------------------------------------------------
uint32_t call_guest(Ctx *c, uint32_t addr, int nargs, const uint32_t *args) {
    uint32_t esp; uc_reg_read(c->uc, UC_X86_REG_ESP, &esp);
    for (int i = nargs - 1; i >= 0; i--) { esp -= 4; wr32(c, esp, args[i]); }
    esp -= 4; wr32(c, esp, RET_MAGIC);
    uc_reg_write(c->uc, UC_X86_REG_ESP, &esp);
    uint64_t maxins = getenv("BLUR_MAXINS") ? strtoull(getenv("BLUR_MAXINS"), 0, 0) : 0;
    uc_err e = uc_emu_start(c->uc, addr, RET_MAGIC, 0, maxins);
    uint32_t eax; uc_reg_read(c->uc, UC_X86_REG_EAX, &eax);
    if (e != UC_ERR_OK && !c->exited) { fprintf(stderr, "uc_emu_start: %s\n", uc_strerror(e)); c->exited = 3; }
    return eax;
}

// ---------------------------------------------------------------------------------
// process setup
// ---------------------------------------------------------------------------------
static void put_desc(uint8_t *d, uint32_t base, uint32_t limit, int code) {
    uint32_t g = 0; if (limit > 0xfffff) { limit >>= 12; g = 1; }
    d[0] = limit & 0xff; d[1] = (limit >> 8) & 0xff; d[2] = base & 0xff; d[3] = (base >> 8) & 0xff; d[4] = (base >> 16) & 0xff;
    d[5] = (code ? 0x0b : 0x03) | 0x10 | 0x80;                            // type | S | DPL0 | P
    d[6] = ((limit >> 16) & 0xf) | (1 << 6) | (g << 7);             // D/B | G
    d[7] = (base >> 24) & 0xff;
}
static void setup_process(Ctx *c) {
    uc_engine *uc = c->uc;
#define MAP(a, s) do { if (uc_mem_map(uc, (a), (s), UC_PROT_ALL) != UC_ERR_OK) { fprintf(stderr, "map %08x failed\n", (a)); exit(1); } } while (0)
    MAP(STACK_TOP - STACK_SIZE, STACK_SIZE); MAP(HEAP_BASE, HEAP_SIZE);
    MAP(TEB_ADDR, 0x1000); MAP(PEB_ADDR, 0x1000); MAP(KUSER_ADDR, 0x1000); MAP(GDT_ADDR, 0x1000);
    MAP(0x7ffdc000, 0x1000);                       // TLS slot array
    MAP(STUB_BASE, STUB_SLOTS * 16); MAP(RET_MAGIC, 0x1000);
    c->heap_next = HEAP_BASE;
    uint8_t cc[16]; memset(cc, 0xcc, 16);
    for (int i = 0; i < STUB_SLOTS; i++) uc_mem_write(uc, STUB_BASE + 16 * i, cc, 16);
    // Flat ring-0 segments plus FS -> TEB. Unicorn runs at CPL 0, so descriptors and selectors must be DPL/RPL 0
    // (ring-3 ones silently fail to load). Real selectors are needed for anti-debug probes like `push ss; pop ss`.
    uint8_t gdt[32] = {0}; put_desc(gdt + 8, 0, 0xffffffff, 1); put_desc(gdt + 16, 0, 0xffffffff, 0); put_desc(gdt + 24, TEB_ADDR, 0xfff, 0);
    uc_mem_write(uc, GDT_ADDR, gdt, sizeof gdt);
    uc_x86_mmr g = {0, GDT_ADDR, 31, 0}; uc_reg_write(uc, UC_X86_REG_GDTR, &g);
    int sel_cs = 0x08, sel_ds = 0x10, sel_fs = 0x18;
    uc_reg_write(uc, UC_X86_REG_DS, &sel_ds); uc_reg_write(uc, UC_X86_REG_ES, &sel_ds); uc_reg_write(uc, UC_X86_REG_GS, &sel_ds);
    uc_reg_write(uc, UC_X86_REG_SS, &sel_ds); uc_reg_write(uc, UC_X86_REG_CS, &sel_cs); uc_reg_write(uc, UC_X86_REG_FS, &sel_fs);
    uint32_t esp = STACK_TOP - 0x100; uc_reg_write(uc, UC_X86_REG_ESP, &esp);
    // TEB
    wr32(c, TEB_ADDR + 0x00, 0xffffffff); wr32(c, TEB_ADDR + 0x04, STACK_TOP); wr32(c, TEB_ADDR + 0x08, STACK_TOP - STACK_SIZE);
    wr32(c, TEB_ADDR + 0x18, TEB_ADDR); wr32(c, TEB_ADDR + 0x20, 0x1000); wr32(c, TEB_ADDR + 0x24, 0x1004);
    wr32(c, TEB_ADDR + 0x2c, 0x7ffdc000); wr32(c, TEB_ADDR + 0x30, PEB_ADDR);
    // PEB: version fields report Windows 7 SP1 (6.1.7601)
    wr8(c, PEB_ADDR + 0x02, 0); wr32(c, PEB_ADDR + 0x18, HEAP_BASE);
    wr32(c, PEB_ADDR + 0xa4, 6); wr32(c, PEB_ADDR + 0xa8, 1); wr16(c, PEB_ADDR + 0xac, 7601); wr32(c, PEB_ADDR + 0xb0, 2);
    // KUSER_SHARED_DATA: NtMajor/Minor at 0x26c/0x270
    wr32(c, KUSER_ADDR + 0x26c, 6); wr32(c, KUSER_ADDR + 0x270, 1);
    wr32(c, KUSER_ADDR + 0x300, STUB_BASE);        // SystemCall pointer (placeholder; direct syscalls unsupported)
    // hooks
    uc_hook h;
    if (getenv("BLUR_BLOCKLOG")) { blklog = getenv("BLUR_BLOCKLOG"); blkbits = calloc(0x78600000u / 8 + 8, 1); }
    if (getenv("BLUR_FNLIST")) { fnbits = calloc(0x2100000 / 8 + 8, 1); FILE *fl = fopen(getenv("BLUR_FNLIST"), "r"); unsigned a; while (fl && fscanf(fl, "%x", &a) == 1) if (a < 0x2100000) fnbits[a >> 3] |= 1 << (a & 7); if (fl) fclose(fl); setenv("BLUR_FIND_OEP", "1", 1); }
    if (getenv("BLUR_EMUHOOK")) uc_hook_add(uc, &emu_hh, UC_HOOK_CODE, (void *)emuhook_enter, NULL, (uint32_t)strtoul(getenv("BLUR_EMUHOOK"), 0, 16), (uint32_t)strtoul(getenv("BLUR_EMUHOOK"), 0, 16));
    if (g_recomp || getenv("BLUR_FIND_OEP")) uc_hook_add(uc, &oep_hook, UC_HOOK_CODE, oep_cb, c, 0x401000, 0xe51fff);
    uc_hook_add(uc, &h, UC_HOOK_CODE, stub_hook, c, STUB_BASE, STUB_BASE + STUB_SLOTS * 16 - 1);
    if (!getenv("BLUR_NORING")) uc_hook_add(uc, &h, UC_HOOK_BLOCK, block_hook, c, 1, 0);   // crash-context ring buffer; costs speed
    if (getenv("BLUR_ALLOCTRACK")) { uint32_t fn[3] = {0x42c730, 0x42c760, 0x42c7c0}; for (int i = 0; i < 3; i++) uc_hook_add(uc, &h, UC_HOOK_CODE, alloc_entry_hook, c, fn[i], fn[i]); }
    if (getenv("BLUR_WATCH")) { char *w = strdup(getenv("BLUR_WATCH")); for (char *t = strtok(w, ","); t && nwatch < 8; t = strtok(NULL, ",")) { watch_addrs[nwatch] = (uint32_t)strtoul(t, 0, 16); uc_hook_add(uc, &h, UC_HOOK_CODE, watch_hook, c, watch_addrs[nwatch], watch_addrs[nwatch]); nwatch++; } }
    uc_hook_add(uc, &h, UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED | UC_HOOK_MEM_FETCH_UNMAPPED, mem_hook, c, 1, 0);
    if (getenv("BLUR_MEMW")) { char *w = strdup(getenv("BLUR_MEMW")); for (char *t = strtok(w, ","); t; t = strtok(NULL, ",")) { uint32_t a = (uint32_t)strtoul(t, 0, 16);
        uc_hook_add(uc, &h, UC_HOOK_MEM_WRITE | (getenv("BLUR_MEMR") ? UC_HOOK_MEM_READ : 0), memw_hook, c, a, a + 3); } }
    uc_hook_add(uc, &h, UC_HOOK_INSN_INVALID, insn_invalid_hook, c, 1, 0);
    uc_hook_add(uc, &h, UC_HOOK_INTR, intr_hook, c, 1, 0);
    uc_hook_add(uc, &h, UC_HOOK_INSN, sysenter_hook, c, 1, 0, UC_X86_INS_SYSENTER);
}

#ifdef _WIN32
// Guest memory at the fixed base the lifted code hard-codes. Windows has no MAP_NORESERVE: commit the whole 4 GB window (it only costs commit charge, pages are
// zero-filled on first touch). If the commit limit is too low, fall back to reserve-only + a vectored handler that commits 256 KB chunks on first access
// (host-side file reads straight into never-touched guest pages can then fail; the guest-side faults are handled).
#define GUEST_SPAN ((1ull << 32) + 0x10000)
static LONG CALLBACK guest_commit_veh(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *r = ep->ExceptionRecord; if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t a = (uintptr_t)r->ExceptionInformation[1], base = 0x7000000000ull; if (a < base || a >= base + GUEST_SPAN) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t pg = a & ~(uintptr_t)0x3ffff; return VirtualAlloc((void *)pg, 0x40000, MEM_COMMIT, PAGE_READWRITE) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}
static void *guest_mem_map(void) {
    void *p = VirtualAlloc((void *)0x7000000000ull, GUEST_SPAN, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); if (p) return p;
    p = VirtualAlloc((void *)0x7000000000ull, GUEST_SPAN, MEM_RESERVE, PAGE_READWRITE); if (!p) return NULL;
    fprintf(stderr, "[mem] could not commit 4 GB (low commit limit / pagefile): committing guest memory on demand\n"); AddVectoredExceptionHandler(1, guest_commit_veh); return p;
}
#endif

int main(int argc, char **argv) {
#ifdef _WIN32
    timeBeginPeriod(1);                                         // 1 ms timer granularity (usleep/Sleep/waits default to 15.6 ms)
#endif
    if (argc < 2) { fprintf(stderr, "usage: %s Blur.exe [msvcr90.dll]\n", argv[0]); return 2; }
    static Ctx ctx; Ctx *c = &ctx;
    g_recomp = getenv("BLUR_RECOMP") != NULL;
#ifdef _WIN32
    G = guest_mem_map(); if (!G) G = (uint8_t *)MAP_FAILED_W;
#else
    G = mmap((void *)0x7000000000ull, (1ull << 32) + 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);      // fixed-address hint: lifted code built with -DBLUR_G_FIXED=0x7000000000ull hard-codes this base
#endif
    if (G != (uint8_t *)0x7000000000ull) fprintf(stderr, "[mem] guest memory mapped at %p, not 0x7000000000: code built with BLUR_G_FIXED will crash (build without it)\n", (void *)G);
#ifdef BLUR_G_CHECK
    if (G != (uint8_t *)0x7000000000ull) { fprintf(stderr, "[mem] FATAL: guest memory mapped at %p but the lifted code was built with BLUR_G_FIXED=0x7000000000\n", (void *)G); exit(2); }
#endif
    if (G == (uint8_t *)MAP_FAILED_W) { perror("mmap guest memory"); return 1; }
    c->verbose = getenv("BLUR_LOG") != NULL;
    c->tls_next = 0;
    { char *d = strdup(argv[1]); char *sl = strrchr(d, '/');
#ifdef _WIN32
    { char *bs = strrchr(d, '\\'); if (bs && (!sl || bs > sl)) sl = bs; }
#endif
    if (sl) *sl = 0; else strcpy(d, "."); snprintf(g_gamedir, sizeof g_gamedir, "%s", d); free(d); }
    snprintf(g_savedir, sizeof g_savedir, "%s", getenv("BLUR_SAVEDIR") ? getenv("BLUR_SAVEDIR") : "./save");
    if (getenv("BLUR_WEB")) {                                   // browser renderer: BLUR_WEB=1 (port 8765) or BLUR_WEB=<port>
        int port = atoi(getenv("BLUR_WEB")); if (port < 2) port = 8765;
        if (net_start(port) == 0 && !getenv("BLUR_NOWAIT")) { fprintf(stderr, "[net] waiting for a browser to connect...\n"); while (!net_client_connected()) usleep(100000); }
    }
    load_sigs("imports.tsv");
    if (uc_open(UC_ARCH_X86, UC_MODE_32, &c->uc) != UC_ERR_OK) { fprintf(stderr, "uc_open failed\n"); return 1; }
    if (getenv("BLUR_CPU")) { int m = atoi(getenv("BLUR_CPU")); uc_err e = uc_ctl_set_cpu_model(c->uc, m); fprintf(stderr, "[cpu] model %d: %s\n", m, uc_strerror(e)); }
    setup_process(c);
    const char *crt = argc > 2 ? argv[2] : "../vendor/msvcr90.dll";
    uint32_t crt_base = load_module(c, crt, "msvcr90.dll");
    Module *m = find_module(c, "msvcr90.dll");
    uint32_t args[3] = {crt_base, 1, 0};
    if (m->entry) { uint32_t r = call_guest(c, m->entry, 3, args); fprintf(stderr, "[crt] DllMain(ATTACH) -> %u\n", r); }
    if (c->exited) { dump_regs(c, "during msvcr90 init"); return 1; }
    uint32_t exe = load_module(c, argv[1], "Blur.exe");
    Module *x = find_module(c, "Blur.exe");
    // TLS callbacks (PROCESS_ATTACH)
    if (x->tls_dir) {
        uint32_t cb = rd32(c, exe + x->tls_dir + 12);
        for (; cb && rd32(c, cb); cb += 4) { uint32_t a[3] = {exe, 1, 0}; fprintf(stderr, "[tls] callback %08x\n", rd32(c, cb)); call_guest(c, rd32(c, cb), 3, a); if (c->exited) break; }
    }
    if (!c->exited) { fprintf(stderr, "[run] entry %08x\n", x->entry); register_main_thread(c, x->entry); run_scheduler(c); }
    if (getenv("BLUR_DUMP_IMAGE")) {                         // decrypted image for Ghidra: raw bytes of the exe mapping (base 0x400000)
        Module *xm = find_module(c, "Blur.exe"); FILE *f = fopen(getenv("BLUR_DUMP_IMAGE"), "wb");
        if (f && xm) { static uint8_t chunk[1 << 16]; for (uint32_t o = 0; o < xm->size; o += sizeof chunk) { uint32_t n = xm->size - o < sizeof chunk ? xm->size - o : (uint32_t)sizeof chunk; uc_mem_read(c->uc, xm->base + o, chunk, n); fwrite(chunk, 1, n, f); } fclose(f); fprintf(stderr, "[dump] wrote %u bytes of decrypted image\n", xm->size); }
    }
    if (getenv("BLUR_STATS")) {
        fprintf(stderr, "[stats] most called APIs:\n");
        for (int n = 0; n < 25; n++) { int best = -1; for (int i = 0; i < c->nstubs; i++) if (c->stubs[i].calls && (best < 0 || c->stubs[i].calls > c->stubs[best].calls)) best = i;
            if (best < 0) break; fprintf(stderr, "  %10llu  %s!%s\n", (unsigned long long)c->stubs[best].calls, c->stubs[best].dll, c->stubs[best].name); c->stubs[best].calls = 0; }
    }
    if (blkbits) { FILE *bf = fopen(blklog, "w"); if (bf) { unsigned n = 0; for (uint32_t a = 0x400000; a < 0x2100000; a++) if (blkbits[a >> 3] >> (a & 7) & 1) { fprintf(bf, "%x\n", a); n++; } for (uint32_t a = 0x78520000; a < 0x78600000; a++) if (blkbits[a >> 3] >> (a & 7) & 1) { fprintf(bf, "%x\n", a); n++; } fclose(bf); fprintf(stderr, "[blocklog] %u block starts\n", n); } }
    d3d9_report(c);
    fprintf(stderr, "[exit] status=%d code=%u\n", c->exited, c->exit_code);
    if (c->exited >= 2) dump_regs(c, "stopped");
    return c->exited;
}
