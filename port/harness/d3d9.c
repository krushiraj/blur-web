#include <unistd.h>
// Direct3D 9 object model for the emulated process. Everything the game creates (textures, buffers, shaders, surfaces)
// is a COM object in guest memory backed by a host-side record, with real guest memory behind Lock() so the game can
// fill it. Device state is tracked so Get*() calls answer truthfully. A renderer back end (WebGL later) will consume the
// same records; for now draw calls are counted and logged.
#include "emu.h"

typedef enum { K_NONE, K_D3D, K_DEVICE, K_TEXTURE, K_CUBE, K_VOLUME, K_VB, K_IB, K_SURFACE, K_VDECL, K_VSHADER, K_PSHADER, K_QUERY, K_STATEBLOCK, K_SWAPCHAIN } Kind;
typedef struct {
    Kind kind; uint32_t guest;
    uint32_t w, h, d, levels, usage, fmt, pool, len, fvf, ms;
    uint64_t vol_hash; uint32_t data, nlev; uint32_t lev_off[16], lev_pitch[16];     // pixel/vertex storage in guest memory
    uint32_t code, codelen;                                       // shader bytecode / vertex declaration copy
    uint32_t parent;                                              // surface -> texture
    uint32_t level, face, lock_off, lock_len, lock_ro;
    uint32_t *sc;                                                 // texture: cached surface objects [face*16+level] (GetSurfaceLevel returns the same object, AddRef'd)
    uint32_t site, cframe, cthr;                                  // creation site (2nd/3rd guest return address) / frame / thread, for BLUR_TEXAUDIT
    uint8_t own;                                                  // o->data was allocated for this object (freed with it)
    uint8_t *shadow, *known; uint32_t sh_epoch, nup, nodiff, st_cnt, st_in, st_out, lock_flags; uint64_t up_bytes, up_saved;   // buffer upload dedupe: what the renderer holds (see send_buf)
} Obj;
#define MAXOBJ 1048576         // object ids are never reused (the renderer keys resources by id); freed objects only give back their guest memory. 262144 ('d3d9 object table full') was reached in an hour-long session; only touched entries are resident (BSS)
static Obj objs[MAXOBJ]; static int nobjs = 1;

// device state
static struct {
    uint32_t rs[256], ss[32][16], tss[8][33];
    float xform[512][16]; uint32_t vp[6];
    uint32_t tex[20], vb[16], vb_off[16], vb_stride[16], ib, vdecl, vs, ps, fvf, rt, ds;
    float vsf[256][4], psf[224][4]; int32_t vsi[16][4], psi[16][4]; uint32_t vsb[16], psb[16];
    uint32_t pp[16];                                              // presentation parameters
    uint32_t backbuf, device;
    uint64_t frames, draws, draws_frame, clears, scenes;
    uint32_t scissor[4];
} st;

// What the browser renderer is known to hold (it is driven purely by the message stream), so unchanged state is never re-sent. 0xff = unknown; reset whenever a viewer attaches.
static struct { uint32_t epoch, vp[6], sc[4], rt[8][4], ds[4], stream[16][3], vs, ps, ib, vdecl, rs[256], ss[32][16], tex[20]; float vsf[256][4], psf[224][4]; } sent;
static struct { uint32_t tex[20], stream[16][3], vs, ps, ib, vdecl; } want;     // lazy mode: object ids the game currently has bound
static uint16_t dq[2048]; static int ndq, dq_all; static int cd_lo[2] = {1 << 30, 1 << 30}, cd_hi[2] = {-1, -1};    // dirty render/sampler states (RS: i, SS: 256 + s*16 + k), dirty constant register ranges
static inline void dq_push(uint32_t k) { if (ndq < 2048) dq[ndq++] = (uint16_t)k; else dq_all = 1; }
// BLUR_GFXFILTER: 0 = no filtering beyond the original (A/B baseline), 1 = eager: every set is compared with what the renderer holds, 2 (default) = lazy: state sets only mark
// dirty and are resolved against the renderer's state right before a draw/clear/stretch (gfx_msg hook d3d9_flush_state), so set/restore pairs and repeated sets cost nothing
static int gf_mode = -1; static int gf_init(void) { const char *e = getenv("BLUR_GFXFILTER"); gf_mode = e ? atoi(e) : 2; return gf_mode; }
#define GF() (gf_mode < 0 ? gf_init() : gf_mode)
static void sent_sync(void) {      // a new viewer attached (or first use): it holds nothing -> forget what we believe it has and mark all state dirty
    if (sent.epoch == g_net_epoch + 1) return;
    memset(&sent, 0xff, sizeof sent); sent.epoch = g_net_epoch + 1; dq_all = 1; cd_lo[0] = cd_lo[1] = 0; cd_hi[0] = 255; cd_hi[1] = 223;
}
static int chg(void *slot, const void *v, size_t n) {          // 1 if the renderer needs the new value (and records it as sent)
    if (!gfx_on()) return 0; if (GF() == 0) return 1;
    sent_sync();
    if (!memcmp(slot, v, n)) return 0; memcpy(slot, v, n); return 1;
}
static uint64_t gs_skipped, gs_sent;   // state messages filtered / sent (BLUR_GFXSTAT)
static Obj *O(Ctx *c, uint32_t p) { uint32_t i = rd32(c, p + 8); return i && i < (uint32_t)nobjs ? &objs[i] : &objs[0]; }
static int dlog(Ctx *c) { (void)c; static int v = -1; if (v < 0) v = getenv("BLUR_D3D") != NULL; return v; }
#define LOG(...) do { if (dlog(c)) fprintf(stderr, "[d3d9] " __VA_ARGS__); } while (0)

static uint32_t bits_per_pixel(uint32_t f) {
    switch (f) {
    case 20: return 24; case 21: case 22: case 31: case 34: case 35: case 63: case 75: case 77: case 71: case 112: case 114: return 32;
    case 23: case 24: case 25: case 26: case 28 + 0x100: case 51: case 60: case 80: case 82: case 111: case 70: return 16;
    case 28: case 50: case 41: case 43: return 8;
    case 36: case 113: case 115: case 64: return 64; case 116: return 128;
    }
    return 32;
}
static int is_block(uint32_t f, uint32_t *bytes) {
    switch (f) { case 0x31545844: *bytes = 8; return 1; case 0x33545844: case 0x35545844: case 0x32545844: case 0x34545844: *bytes = 16; return 1; }
    return 0;
}
static uint32_t pitch_of(uint32_t f, uint32_t w) { uint32_t bb; if (is_block(f, &bb)) return ((w + 3) / 4) * bb; return (w * bits_per_pixel(f) + 7) / 8; }
static uint32_t size_of(uint32_t f, uint32_t w, uint32_t h) { uint32_t bb; if (is_block(f, &bb)) return ((w + 3) / 4) * ((h + 3) / 4) * bb; return pitch_of(f, w) * h; }

// Byte offset of a LockRect sub-rectangle inside a level (pRect = guest RECT {left, top, right, bottom}, may be 0). For block-compressed formats D3D
// returns the address of the 4x4 block that contains (left, top): (top / 4) block rows, (left / 4) blocks. The old code multiplied the PIXEL row by the
// BLOCK-row pitch, i.e. wrote 4x too far (outside the level, over neighbouring heap objects) -- and Texture::LockRect ignored pRect altogether.
static uint32_t rect_offset(Ctx *c, uint32_t fmt, uint32_t pitch, uint32_t rect) {
    if (!rect) return 0; uint32_t l = rd32(c, rect), t = rd32(c, rect + 4), bb;
    if (is_block(fmt, &bb)) return (t / 4) * pitch + (l / 4) * bb;
    return t * pitch + (l * bits_per_pixel(fmt)) / 8;
}
static uint32_t oid(Obj *o) { return (uint32_t)(o - objs); }
static uint32_t pow2lv(uint32_t v, uint32_t l) { v >>= l; return v ? v : 1; }
static uint32_t suspects[4096], suspect_age[4096]; static int nsuspect;
static void suspect_add(uint32_t id) { for (int i = 0; i < nsuspect; i++) if (suspects[i] == id) return; if (nsuspect < 4096) { suspects[nsuspect] = id; suspect_age[nsuspect++] = 0; } }
static void resend_if_filled(Ctx *c);
// send one mip level / cube face of a texture to the browser
static void send_level(Ctx *c, Obj *o, uint32_t face, uint32_t level) {
    if (!gfx_on() || level >= o->nlev) return;
    uint32_t lw = pow2lv(o->w, level), lh = pow2lv(o->h, level), ld = o->kind == K_VOLUME ? pow2lv(o->d, level) : 1, pitch = o->lev_pitch[level];
    uint32_t n = size_of(o->fmt, lw, lh) * ld, base = o->data + o->lev_off[level] + (o->kind == K_CUBE ? face * (o->len / 6) : 0);
    uint32_t w[8] = {oid(o), face, level, lw, lh, ld, o->fmt, pitch};
    uint8_t *tmp = gfx_msg_alloc(G_TEX_UPLOAD, w, 8, n); if (!tmp) return; rdbuf(c, base, tmp, n);
    { static int ts = -1; if (ts < 0) ts = getenv("BLUR_TEXSTAT") != NULL; if (ts && level == 0 && lw >= 512) { uint32_t nz = 0; for (uint32_t i = 0; i < n; i++) nz += tmp[i] != 0; fprintf(stderr, "[tex] #%u %ux%u fmt=%u bytes=%u nonzero=%.1f%% frame=%llu\n", oid(o), lw, lh, o->fmt, n, 100.0 * nz / (n ? n : 1), (unsigned long long)st.frames); } }
    if (n >= 4096 && level == 0) { int allz = 1; for (uint32_t i = 0; i < n; i += 4) if (*(uint32_t *)(tmp + i)) { allz = 0; break; } if (allz) suspect_add(oid(o)); }
}
// Textures that were uploaded all-zero are probably filled later through a lock pointer the game keeps (no second Unlock): re-send them once data shows up.
// Volume textures (colour-grading LUTs): the game keeps writing through the LockBox pointer after UnlockBox (streamed/colour-blended data), so the single upload at Unlock is stale.
// Re-send level 0 whenever its guest-memory hash changed (checked every 4th frame, 128 KB per 32^3 LUT).
static void volume_refresh(Ctx *c) {
    if (st.frames % 4) return;
    for (int i = 1; i < nobjs; i++) { Obj *o = &objs[i]; if (o->kind != K_VOLUME || !o->data || !o->nlev) continue;
        uint32_t n = size_of(o->fmt, o->w, o->h) * o->d; if (n > (1u << 22)) continue;
        const uint64_t *q = (const uint64_t *)(G + o->data + o->lev_off[0]); uint64_t h = 0x9e3779b97f4a7c15ull, h2 = 0x853c49e6748fea9bull;      // 2 x 64-bit lanes, ~8 bytes/cycle (was byte-wise FNV + malloc/copy)
        uint32_t k = 0; for (; k + 16 <= n; k += 16, q += 2) { uint64_t a0, a1; memcpy(&a0, q, 8); memcpy(&a1, q + 1, 8); h = (h ^ a0) * 0x100000001b3ull; h = (h << 29 | h >> 35); h2 = (h2 ^ a1) * 0x9fb21c651e98df25ull; h2 = (h2 << 31 | h2 >> 33); }
        for (; k < n; k++) h = (h ^ G[o->data + o->lev_off[0] + k]) * 1099511628211ull; h ^= h2 * 0xff51afd7ed558ccdull;
        if (h != o->vol_hash) { o->vol_hash = h; if (getenv("BLUR_VOLLOG")) { uint8_t hb[48]; rdbuf(c, o->data + o->lev_off[0], hb, 48); fprintf(stderr, "[vol] #%u %ux%ux%u fmt=%u data=%08x changed frame=%llu first48:", oid(o), o->w, o->h, o->d, o->fmt, o->data, (unsigned long long)st.frames); for (int q = 0; q < 48; q++) fprintf(stderr, " %02x", hb[q]); fprintf(stderr, "\n"); } send_level(c, o, 0, 0); } }
}
static void resend_if_filled(Ctx *c) {
    volume_refresh(c);
    if (st.frames % 15) return;                                   // sparse sampling, every 15th frame: this runs over thousands of placeholder textures
    for (int i = 0; i < nsuspect; ) {
        Obj *o = &objs[suspects[i]]; int filled = 0;
        if (o->data && o->nlev) { uint32_t n = size_of(o->fmt, o->w, o->h); if (n > 65536) n = 65536; uint32_t step = (n / 64) & ~3u; if (!step) step = 4; for (uint32_t k = 0; k < n && !filled; k += step) if (rd32(c, o->data + o->lev_off[0] + k)) filled = 1; }
        if (!filled) { if (++suspect_age[i] > 240) { suspects[i] = suspects[--nsuspect]; suspect_age[i] = suspect_age[nsuspect]; continue; } i++; continue; }      // give up after ~1 minute at 60 fps
        uint32_t faces = o->kind == K_CUBE ? 6 : 1; uint32_t id = suspects[i]; suspects[i] = suspects[--nsuspect]; suspect_age[i] = suspect_age[nsuspect];
        for (uint32_t f = 0; f < faces; f++) for (uint32_t l = 0; l < o->nlev; l++) send_level(c, o, f, l);
        if (getenv("BLUR_TEXSTAT")) fprintf(stderr, "[tex] #%u filled after upload; re-sent\n", id);
    }
}
// Buffer uploads: a buffer that is uploaded repeatedly (>= 2nd time) gets a host-side copy of what the renderer holds (64-byte blocks, `known` marks fully-sent blocks); the locked region is
// diffed against it and only the changed runs are sent. Buffers whose uploads turn out to be (nearly) always different drop the shadow again (nodiff) so they only pay the plain copy.
#define BLK 64u
static void send_buf_raw(Obj *o, uint32_t off, uint32_t n) { uint32_t w[2] = {oid(o), off}; uint8_t *tmp = gfx_msg_alloc(G_BUF_UPLOAD, w, 2, n); if (tmp) memcpy(tmp, G + o->data + off, n); }
static uint64_t bu_in, bu_out;     // BLUR_GFXSTAT: bytes locked / actually sent
static void send_buf(Ctx *c, Obj *o, uint32_t off, uint32_t n) {
    (void)c; if (!gfx_on() || off >= o->len) return; if (off + n > o->len) n = o->len - off; if (!n) return;
    if (o->sh_epoch != g_net_epoch + 1) { free(o->shadow); free(o->known); o->shadow = o->known = NULL; o->nup = o->nodiff = 0; o->up_bytes = o->up_saved = 0; o->sh_epoch = g_net_epoch + 1; }
    bu_in += n; o->nup++; o->st_cnt++; o->st_in += n;
    if (GF() == 0 || o->nodiff || o->len > (16u << 20) || o->nup < 2) { send_buf_raw(o, off, n); bu_out += n; o->st_out += n; return; }
    uint32_t nb = (o->len + BLK - 1) / BLK;
    if (!o->shadow) { o->shadow = malloc(o->len); o->known = calloc(nb, 1); if (!o->shadow || !o->known) { free(o->shadow); free(o->known); o->shadow = o->known = NULL; o->nodiff = 1; send_buf_raw(o, off, n); bu_out += n; return; } }
    const uint8_t *src = G + o->data; uint32_t end = off + n, b0 = off / BLK, b1 = (end - 1) / BLK, sent_b = 0;
    for (uint32_t b = b0; b <= b1; ) {
        uint32_t lo = b * BLK > off ? b * BLK : off, hi = (b + 1) * BLK < end ? (b + 1) * BLK : end;
        if (o->known[b] && !memcmp(o->shadow + lo, src + lo, hi - lo)) { b++; continue; }
        uint32_t rs = lo, b2 = b;                                      // dirty run: extend over following dirty blocks
        for (b2 = b + 1; b2 <= b1; b2++) { uint32_t l2 = b2 * BLK, h2 = (b2 + 1) * BLK < end ? (b2 + 1) * BLK : end; if (o->known[b2] && !memcmp(o->shadow + l2, src + l2, h2 - l2)) break; }
        uint32_t re = b2 * BLK < end ? b2 * BLK : end;                 // [rs, re)
        send_buf_raw(o, rs, re - rs); sent_b += re - rs; memcpy(o->shadow + rs, src + rs, re - rs);
        for (uint32_t k = b; k < b2; k++) if (k * BLK >= rs && ((k + 1) * BLK <= re || (re >= o->len && k == nb - 1))) o->known[k] = 1;
        b = b2;
    }
    bu_out += sent_b; o->st_out += sent_b; o->up_bytes += n; o->up_saved += n - sent_b;
    if (o->nup >= 12 && o->up_saved * 10 < o->up_bytes) { o->nodiff = 1; free(o->shadow); free(o->known); o->shadow = o->known = NULL; }     // < 10% saved: not worth the extra pass
}
// surface descriptor words: kind 0 backbuffer, 1 texture level, 2 standalone surface, 3 none
static void surf_desc_words(Ctx *c, uint32_t surf, uint32_t out[4]);

extern uint32_t (*com_zero_hook)(Ctx *, uint32_t); static uint32_t d3d_zero(Ctx *c, uint32_t g);
// BLUR_OBJSTAT=1: objects created per interface (printed at exit) -- finds per-frame churn
static struct { const char *name; uint64_t n; } ostat[32]; static int nostat;
static void objstat(const char *iface) { static int on = -1; if (on < 0) on = getenv("BLUR_OBJSTAT") != NULL; if (!on) return; for (int i = 0; i < nostat; i++) if (!strcmp(ostat[i].name, iface)) { ostat[i].n++; return; } if (nostat < 32) { ostat[nostat].name = iface; ostat[nostat++].n = 1; } }
static void objstat_report(void) { for (int i = 0; i < nostat; i++) fprintf(stderr, "[objstat] %-32s %llu created\n", ostat[i].name, (unsigned long long)ostat[i].n); }
// ---- object creation ---------------------------------------------------------------------------------------------------------------
static Ctx *d3d_ctx;
static uint32_t make_obj(Ctx *c, const char *iface, const ComMethod *m, int n, Kind k, Obj **out) {
    d3d_ctx = c; com_zero_hook = d3d_zero; objstat(iface);
    uint32_t g = com_make(c, iface, m, n, 8); int i = nobjs++; if (i >= MAXOBJ) { fprintf(stderr, "d3d9 object table full\n"); exit(1); }
    memset(&objs[i], 0, sizeof objs[i]); objs[i].kind = k; objs[i].guest = g; wr32(c, g + 8, (uint32_t)i); if (out) *out = &objs[i]; return g;
}
// IDirect3DResource9::GetType (slot 10): the game's texture helper (0x5aa710) switches on it (3/4/5) to fetch the surface format; returning 0 made every
// bank texture upload see format -1 -> row size 0 -> memcpy(dst, src, 0) -> the UI/font atlases of ui_*.bpr stayed all-zero (port/work/pak-re/UIPAK.md)
static uint32_t res_gettype(Ctx *c, uint32_t *a) {
    switch (O(c, a[0])->kind) { case K_SURFACE: return 1; case K_VOLUME: return 4; case K_TEXTURE: return 3; case K_CUBE: return 5; case K_VB: return 6; case K_IB: return 7; default: return 0; }
}
#define Z(n, na) { n, na, com_ok }
#define IU Z("QueryInterface", 3), Z("AddRef", 1), Z("Release", 1)
#define RES Z("GetDevice", 2), Z("SetPrivateData", 5), Z("GetPrivateData", 4), Z("FreePrivateData", 2), Z("SetPriority", 2), Z("GetPriority", 1), Z("PreLoad", 1), { "GetType", 1, res_gettype }
#define NM(m) (int)(sizeof(m) / sizeof((m)[0]))

// buffers (VB/IB share a layout)
static uint32_t buf_lock(Ctx *c, uint32_t *a) { Obj *o = O(c, a[0]); o->lock_off = a[1]; o->lock_len = a[2]; o->lock_flags = a[4]; o->lock_ro = (a[4] & 0x10) != 0; wr32(c, a[3], o->data + a[1]); return 0; }
static uint32_t buf_unlock(Ctx *c, uint32_t *a) { Obj *o = O(c, a[0]); if (!o->lock_ro) send_buf(c, o, o->lock_off, o->lock_len ? o->lock_len : o->len - o->lock_off); return 0; }
static uint32_t buf_getdesc(Ctx *c, uint32_t *a) { Obj *o = O(c, a[0]); wr32(c, a[1], o->kind == K_IB ? o->fmt : 0); wr32(c, a[1] + 4, 10); wr32(c, a[1] + 8, o->usage); wr32(c, a[1] + 12, o->pool); wr32(c, a[1] + 16, o->len); wr32(c, a[1] + 20, o->fvf); return 0; }
static const ComMethod buf_m[] = { IU, RES, { "Lock", 5, buf_lock }, { "Unlock", 1, buf_unlock }, { "GetDesc", 2, buf_getdesc } };

// surfaces
static uint32_t surf_desc(Ctx *c, uint32_t *a) {
    Obj *o = O(c, a[0]); wr32(c, a[1], o->fmt); wr32(c, a[1] + 4, 1); wr32(c, a[1] + 8, o->usage); wr32(c, a[1] + 12, o->pool);
    wr32(c, a[1] + 16, o->ms); wr32(c, a[1] + 20, 0); wr32(c, a[1] + 24, o->w); wr32(c, a[1] + 28, o->h); return 0;
}
static uint32_t surf_lock(Ctx *c, uint32_t *a) {
    Obj *o = O(c, a[0]); wr32(c, a[1], o->lev_pitch[0]); wr32(c, a[1] + 4, o->data + rect_offset(c, o->fmt, o->lev_pitch[0], a[2])); return 0;
}
static void canary_check(Ctx *c, Obj *o, const char *where);
static uint32_t surf_unlock(Ctx *c, uint32_t *a) { Obj *o = O(c, a[0]); if (o->parent) canary_check(c, &objs[o->parent], "surface UnlockRect"); if (o->parent) send_level(c, &objs[o->parent], o->face, o->level); return 0; }
static uint32_t surf_container(Ctx *c, uint32_t *a) { Obj *o = O(c, a[0]); if (o->parent) { wr32(c, a[2], objs[o->parent].guest); return 0; } wr32(c, a[2], st.device); return 0; }
static const ComMethod surf_m[] = { IU, RES, { "GetContainer", 3, surf_container }, { "GetDesc", 2, surf_desc }, { "LockRect", 4, surf_lock }, { "UnlockRect", 1, surf_unlock }, Z("GetDC", 2), Z("ReleaseDC", 2) };

static uint32_t new_surface(Ctx *c, uint32_t w, uint32_t h, uint32_t fmt, uint32_t pool, uint32_t usage, uint32_t ms, uint32_t data, uint32_t pitch, uint32_t parent) {
    Obj *o; uint32_t g = make_obj(c, "IDirect3DSurface9", surf_m, NM(surf_m), K_SURFACE, &o);
    o->w = w; o->h = h; o->fmt = fmt; o->pool = pool; o->usage = usage; o->ms = ms; o->parent = parent;
    o->lev_pitch[0] = pitch ? pitch : pitch_of(fmt, w); if (data) o->data = data; else { o->data = guest_alloc(c, size_of(fmt, w, h) + 16); o->own = 1; } return g;
}

// textures
extern uint32_t g_api_ret;
static int texw(uint32_t id) { static long lo = -2, hi; if (lo == -2) { lo = -1; const char *e = getenv("BLUR_TEXWATCH"); if (e) { lo = atol(e); const char *d = strchr(e, '-'); hi = d ? atol(d + 1) : lo; } } return lo >= 0 && (long)id >= lo && (long)id <= hi; }
#include <unicorn/unicorn.h>
static void emu_wcb(uc_engine *uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void *ud) {
    (void)type; static int n; if (n++ > 60) return; uint32_t eip, esp, ebp; uc_reg_read(uc, UC_X86_REG_EIP, &eip); uc_reg_read(uc, UC_X86_REG_ESP, &esp); uc_reg_read(uc, UC_X86_REG_EBP, &ebp);
    fprintf(stderr, "[emuw] write %d bytes at %08x val=%llx eip=%08x esp=%08x ebp=%08x chain:", size, (uint32_t)addr, (unsigned long long)value, eip, esp, ebp);
    uint32_t b = ebp; for (int k = 0; k < 8 && b > 0x10000 && b < 0x10000000; k++) { uint32_t ret = 0, nb = 0; uc_mem_read(uc, b + 4, &ret, 4); uc_mem_read(uc, b, &nb, 4); fprintf(stderr, " %08x", ret); if (nb <= b) break; b = nb; }
    fprintf(stderr, "\n"); (void)ud;
}
static void canary_check(Ctx *c, Obj *o, const char *where) {
    static int on = -1; if (on < 0) on = getenv("BLUR_CANARY") != NULL; if (!on || !o->data || !o->len) return;
    int last = -1; for (int i = 0; i < 256; i++) if (G[o->data + o->len + i] != 0xAB) last = i;
    if (last >= 0) { static uint32_t seen[64]; static int ns; uint32_t key = o->fmt * 1000003u + o->w * 4099u + o->h;
        int dup = 0; for (int i = 0; i < ns; i++) if (seen[i] == key) dup = 1;
        if (!dup && ns < 64) { seen[ns++] = key; fprintf(stderr, "[canary] texture #%ld %ux%u fmt=%u nlev=%u len=%u data=%08x wrote %d byte(s) past the end (%s, caller %08x, frame %llu)\n", (long)(o - objs), o->w, o->h, o->fmt, o->nlev, o->len, o->data, last + 1, where, g_api_ret, (unsigned long long)st.frames); }
        memset(G + o->data + o->len, 0xAB, 256); }
}
static uint32_t tex_lock(Ctx *c, uint32_t *a) { Obj *o = O(c, a[0]);
    if (a[1] >= o->nlev) { strict_log("IDirect3DTexture9::LockRect", "level >= level count (real D3D9: D3DERR_INVALIDCALL; we used to silently lock level 0)", a[1], o->nlev); return 0x8876086Cu; }
    if (!a[2]) { strict_log("IDirect3DTexture9::LockRect", "NULL pLockedRect", 0, 0); return 0x8876086Cu; }
    if (getenv("BLUR_HOSTWATCH") && (long)(o - objs) == atol(getenv("BLUR_HOSTWATCH")) && a[1] == 0) { g_hw_lo = o->data + o->lev_off[0]; g_hw_hi = g_hw_lo + 0x40000; fprintf(stderr, "[hostw] watching %08x-%08x\n", g_hw_lo, g_hw_hi); }
    { static int armed; const char *e = getenv("BLUR_EMUWATCH"); if (e && !g_recomp && !armed && (long)(o - objs) == atol(e) && a[1] == 0) { armed = 1; uc_hook h; uint32_t lo = o->data + o->lev_off[0]; uc_hook_add(c->uc, &h, UC_HOOK_MEM_WRITE, (void *)emu_wcb, NULL, lo, lo + 0x3ffff); fprintf(stderr, "[emuw] armed %08x..\n", lo); } } if (texw((uint32_t)(o - objs))) { fprintf(stderr, "[texw] lock #%ld lvl=%u flags=%x ret=%08x frame=%llu data=%08x callers:", (long)(o - objs), a[1], a[4], g_api_ret, (unsigned long long)st.frames, o->data + o->lev_off[a[1] < o->nlev ? a[1] : 0]); extern uint32_t g_api_esp; for (int k = 1; k < 80; k++) { uint32_t w = rd32(c, g_api_esp + 4 * k); if (w >= 0x401000 && w < 0xe40000) fprintf(stderr, " %08x", w); } fprintf(stderr, "\n"); } uint32_t l = a[1] < o->nlev ? a[1] : 0; wr32(c, a[2], o->lev_pitch[l]); wr32(c, a[2] + 4, o->data + o->lev_off[l] + rect_offset(c, o->fmt, o->lev_pitch[l], a[3])); return 0; }
static uint32_t tex_unlock(Ctx *c, uint32_t *a) { if (a[1] >= O(c, a[0])->nlev) { strict_log("IDirect3DTexture9::UnlockRect", "level >= level count", a[1], O(c, a[0])->nlev); return 0x8876086Cu; } canary_check(c, O(c, a[0]), "UnlockRect"); if (texw((uint32_t)(O(c, a[0]) - objs))) fprintf(stderr, "[texw] unlock #%ld lvl=%u ret=%08x frame=%llu\n", (long)(O(c, a[0]) - objs), a[1], g_api_ret, (unsigned long long)st.frames); send_level(c, O(c, a[0]), 0, a[1]); return 0; }
static uint32_t tex_getsurf(Ctx *c, uint32_t *a) {
    Obj *o = O(c, a[0]); if (a[1] >= o->nlev || !a[2]) { strict_log("IDirect3DTexture9::GetSurfaceLevel", "level >= level count or NULL out", a[1], o->nlev); if (a[2]) wr32(c, a[2], 0); return 0x8876086Cu; }
    uint32_t l = a[1], w = o->w >> l ? o->w >> l : 1, h = o->h >> l ? o->h >> l : 1;
    if (!o->sc) o->sc = calloc(96, 4);
    if (o->sc[l]) { wr32(c, o->sc[l] + 4, rd32(c, o->sc[l] + 4) + 1); wr32(c, a[2], o->sc[l]); return 0; }      // same surface object every time, AddRef'd (like D3D9)
    uint32_t g = new_surface(c, w, h, o->fmt, o->pool, o->usage, 0, o->data + o->lev_off[l], o->lev_pitch[l], (uint32_t)(o - objs)); O(c, g)->level = l;
    wr32(c, g + 4, 2); o->sc[l] = g;                                                                          // refcount: 1 owned by the texture + 1 for the caller
    wr32(c, a[2], g); return 0;
}
static uint32_t tex_leveldesc(Ctx *c, uint32_t *a) {
    Obj *o = O(c, a[0]); uint32_t l = a[1]; if (l >= o->nlev || !a[2]) { strict_log("IDirect3DTexture9::GetLevelDesc", "level >= level count or NULL out", l, o->nlev); return 0x8876086Cu; }
    wr32(c, a[2], o->fmt); wr32(c, a[2] + 4, 1); wr32(c, a[2] + 8, o->usage); wr32(c, a[2] + 12, o->pool);
    wr32(c, a[2] + 16, 0); wr32(c, a[2] + 20, 0); wr32(c, a[2] + 24, o->w >> l ? o->w >> l : 1); wr32(c, a[2] + 28, o->h >> l ? o->h >> l : 1); return 0;
}
static uint32_t tex_levels(Ctx *c, uint32_t *a) { return O(c, a[0])->nlev; }
static const ComMethod tex_m[] = { IU, RES, Z("SetLOD", 2), Z("GetLOD", 1), { "GetLevelCount", 1, tex_levels }, Z("SetAutoGenFilterType", 2), Z("GetAutoGenFilterType", 1),
    Z("GenerateMipSubLevels", 1), { "GetLevelDesc", 3, tex_leveldesc }, { "GetSurfaceLevel", 3, tex_getsurf }, { "LockRect", 5, tex_lock }, { "UnlockRect", 2, tex_unlock }, Z("AddDirtyRect", 2) };

static void canary_check(Ctx *c, Obj *o, const char *where);
static void layout_levels(Ctx *c, Obj *o, uint32_t faces) {
    uint32_t total = 0, w = o->w, h = o->h, lv = o->levels ? o->levels : 1;
    if (!o->levels) { uint32_t m = w > h ? w : h; lv = 1; while (m > 1) { m >>= 1; lv++; } }
    if (lv > 16) lv = 16; o->nlev = lv;
    for (uint32_t l = 0; l < lv; l++) { uint32_t lw = (w >> l) ? (w >> l) : 1, lh = (h >> l) ? (h >> l) : 1, ld = o->kind == K_VOLUME ? ((o->d >> l) ? (o->d >> l) : 1) : 1; o->lev_off[l] = total; o->lev_pitch[l] = pitch_of(o->fmt, lw); total += size_of(o->fmt, lw, lh) * ld; }   // volume levels hold d>>l slices (was: one slice per level -> level 1 overlapped level 0's slices)
    o->len = total * faces; o->data = guest_alloc(c, o->len + 256); memset(G + o->data + o->len, 0xAB, 256);    o->own = 1;   // canary (BLUR_CANARY)
}

// cube/volume textures (data-only: enough for the game to create and fill them)
static uint32_t cube_lock(Ctx *c, uint32_t *a) { Obj *o = O(c, a[0]); uint32_t face = a[1], l = a[2] < o->nlev ? a[2] : 0, per = o->len / 6; wr32(c, a[3], o->lev_pitch[l]); wr32(c, a[3] + 4, o->data + face * per + o->lev_off[l] + rect_offset(c, o->fmt, o->lev_pitch[l], a[4])); return 0; }
static uint32_t cube_unlock(Ctx *c, uint32_t *a) { send_level(c, O(c, a[0]), a[1], a[2]); return 0; }
static uint32_t cube_surf(Ctx *c, uint32_t *a) {
    Obj *o = O(c, a[0]); uint32_t face = a[1], l = a[2] < o->nlev ? a[2] : 0, per = o->len / 6, w = o->w >> l ? o->w >> l : 1;
    if (face > 5) { wr32(c, a[3], 0); return 0x8876086Cu; }
    if (!o->sc) o->sc = calloc(96, 4);
    if (o->sc[face * 16 + l]) { uint32_t q = o->sc[face * 16 + l]; wr32(c, q + 4, rd32(c, q + 4) + 1); wr32(c, a[3], q); return 0; }
    uint32_t g = new_surface(c, w, w, o->fmt, o->pool, o->usage, 0, o->data + face * per + o->lev_off[l], o->lev_pitch[l], (uint32_t)(o - objs)); O(c, g)->level = l; O(c, g)->face = face;
    wr32(c, g + 4, 2); o->sc[face * 16 + l] = g; wr32(c, a[3], g); return 0;
}
static uint32_t cube_leveldesc(Ctx *c, uint32_t *a) {
    Obj *o = O(c, a[0]); uint32_t l = a[1]; wr32(c, a[2], o->fmt); wr32(c, a[2] + 4, 1); wr32(c, a[2] + 8, o->usage); wr32(c, a[2] + 12, o->pool);
    wr32(c, a[2] + 16, 0); wr32(c, a[2] + 20, 0); wr32(c, a[2] + 24, o->w >> l ? o->w >> l : 1); wr32(c, a[2] + 28, o->h >> l ? o->h >> l : 1); return 0;
}
static const ComMethod cube_m[] = { IU, RES, Z("SetLOD", 2), Z("GetLOD", 1), { "GetLevelCount", 1, tex_levels }, Z("SetAutoGenFilterType", 2), Z("GetAutoGenFilterType", 1),
    Z("GenerateMipSubLevels", 1), { "GetLevelDesc", 3, cube_leveldesc }, { "GetCubeMapSurface", 4, cube_surf }, { "LockRect", 6, cube_lock }, { "UnlockRect", 3, cube_unlock }, Z("AddDirtyRect", 3) };
static uint32_t vol_unlock(Ctx *c, uint32_t *a) { send_level(c, O(c, a[0]), 0, a[1]); return 0; }
static uint32_t vol_lock(Ctx *c, uint32_t *a) { Obj *o = O(c, a[0]); if (getenv("BLUR_VOLLOG")) { uint32_t bx[6] = {0}; if (a[3]) rdbuf(c, a[3], bx, 24); fprintf(stderr, "[vol] LockBox #%u level=%u box=%08x [%u %u %u %u %u %u] flags=%x ret=%08x\n", oid(o), a[1], a[3], bx[0], bx[1], bx[2], bx[3], bx[4], bx[5], a[4], g_api_ret); } uint32_t l = a[1] < o->nlev ? a[1] : 0; uint32_t p = o->lev_pitch[l]; wr32(c, a[2], p); wr32(c, a[2] + 4, size_of(o->fmt, o->w >> l ? o->w >> l : 1, o->h >> l ? o->h >> l : 1)); wr32(c, a[2] + 8, o->data + o->lev_off[l]); return 0; }
static uint32_t vol_leveldesc(Ctx *c, uint32_t *a) {                 // D3DVOLUME_DESC {Format, Type, Usage, Pool, Width, Height, Depth}
    Obj *o = O(c, a[0]); uint32_t l = a[1]; wr32(c, a[2], o->fmt); wr32(c, a[2] + 4, 2); wr32(c, a[2] + 8, o->usage); wr32(c, a[2] + 12, o->pool);
    wr32(c, a[2] + 16, o->w >> l ? o->w >> l : 1); wr32(c, a[2] + 20, o->h >> l ? o->h >> l : 1); wr32(c, a[2] + 24, o->d >> l ? o->d >> l : 1); return 0;
}
static const ComMethod vol_m[] = { IU, RES, Z("SetLOD", 2), Z("GetLOD", 1), { "GetLevelCount", 1, tex_levels }, Z("SetAutoGenFilterType", 2), Z("GetAutoGenFilterType", 1),
    Z("GenerateMipSubLevels", 1), { "GetLevelDesc", 3, vol_leveldesc }, Z("GetVolumeLevel", 3), { "LockBox", 5, vol_lock }, { "UnlockBox", 2, vol_unlock }, Z("AddDirtyBox", 2) };

// shaders / declarations / queries / state blocks
static uint32_t obj_getcode(Ctx *c, uint32_t *a) {                 // GetFunction / GetDeclaration(this, pData, pSizeOfData)
    Obj *o = O(c, a[0]);
    if (!a[1]) { wr32(c, a[2], o->codelen); return 0; }
    uint32_t n = rd32(c, a[2]); if (n > o->codelen) n = o->codelen;
    uint8_t *b = malloc(n + 1); rdbuf(c, o->code, b, n); wrbuf(c, a[1], b, n); free(b); wr32(c, a[2], n); return 0;
}
static const ComMethod shader_m[] = { IU, Z("GetDevice", 2), { "GetFunction", 3, obj_getcode } };
static const ComMethod vdecl_m[] = { IU, Z("GetDevice", 2), { "GetDeclaration", 3, obj_getcode } };
// queries: type 8 EVENT (BOOL TRUE = done), 9 OCCLUSION (visible pixel count), 10 TIMESTAMP, 11 TIMESTAMPDISJOINT, 12 TIMESTAMPFREQ. BLUR_QUERYLOG=1 logs types.
static uint32_t query_getdata(Ctx *c, uint32_t *a) {
    Obj *o = O(c, a[0]); uint32_t t = o->fmt;
    static int lg = -1; static unsigned seen[16]; if (lg < 0) lg = getenv("BLUR_QUERYLOG") != NULL; if (lg && t < 16 && seen[t]++ < 3) fprintf(stderr, "[query] GetData type=%u size=%u flags=%x ret=%08x\n", t, a[3], a[4], g_api_ret);
    if (!a[1]) return 0;
    switch (t) {
    case 9: if (a[2] >= 4) wr32(c, a[1], getenv("BLUR_OCCL0") ? 0 : 1000000); break;                      // plenty of visible pixels (was: 0 = fully occluded)
    case 8: if (a[2] >= 4) wr32(c, a[1], 1); break;
    case 10: if (a[2] >= 8) { uint64_t v = real_us() * 1000; wr32(c, a[1], (uint32_t)v); wr32(c, a[1] + 4, (uint32_t)(v >> 32)); } break;
    case 11: if (a[2] >= 4) wr32(c, a[1], 0); break;
    case 12: if (a[2] >= 8) { wr32(c, a[1], 1000000000u); wr32(c, a[1] + 4, 0); } break;
    default: if (a[2] >= 4) wr32(c, a[1], 0);
    }
    return 0;
}
static const ComMethod query_m[] = { IU, Z("GetDevice", 2), Z("GetType", 1), Z("GetDataSize", 1), Z("Issue", 2), { "GetData", 4, query_getdata } };
static const ComMethod sb_m[] = { IU, Z("GetDevice", 2), Z("Capture", 1), Z("Apply", 1) };
static uint32_t sc_present(Ctx *c, uint32_t *a) { (void)c; (void)a; return 0; }
static uint32_t sc_backbuffer(Ctx *c, uint32_t *a) { if (st.backbuf) wr32(c, st.backbuf + 4, rd32(c, st.backbuf + 4) + 1); wr32(c, a[3], st.backbuf); return 0; }
static const ComMethod sc_m[] = { IU, { "Present", 6, sc_present }, Z("GetFrontBufferData", 2), { "GetBackBuffer", 4, sc_backbuffer }, Z("GetRasterStatus", 2), Z("GetDisplayMode", 2), Z("GetDevice", 2), Z("GetPresentParameters", 2) };

static uint32_t copy_code(Ctx *c, uint32_t src, uint32_t *len, int is_decl) {
    uint32_t n = 0;
    if (is_decl) { while (rd16(c, src + n) != 0xff || rd16(c, src + n + 2) != 0x00 || 0) { n += 8; if (n > 4096) break; } n += 8; }       // D3DDECL_END: stream 0xFF
    else { n = 4; while (rd32(c, src + n - 4) != 0x0000ffff && n < (1 << 20)) n += 4; }                                                      // D3DSIO_END token
    uint32_t p = guest_alloc(c, n + 4); uint8_t *b = malloc(n); rdbuf(c, src, b, n); wrbuf(c, p, b, n); free(b); *len = n; return p;
}

// ---- device -----------------------------------------------------------------------------------------------------------------------------
// BLUR_CREATELOG=lo-hi: log creation site (guest return addresses on the stack) of textures whose object id is in the range
static void texcreate_log(Ctx *c, Obj *o) {
    { extern uint32_t g_api_esp; int n = 0; for (int k = 0; k < 120 && n < 3; k++) { uint32_t w = rd32(c, g_api_esp + 4 * k); if (w >= 0x401000 && w < 0x2070000) { if (n == 1) o->site = w; n++; } } o->cframe = (uint32_t)st.frames; o->cthr = c->cur; }
    static long lo = -2, hi; if (lo == -2) { lo = -1; const char *e = getenv("BLUR_CREATELOG"); if (e) { lo = atol(e); const char *d = strchr(e, '-'); hi = d ? atol(d + 1) : lo; } }
    if (lo < 0 || (long)(o - objs) < lo || (long)(o - objs) > hi) return; extern uint32_t g_api_esp;
    fprintf(stderr, "[create] #%ld %ux%u lv=%u fmt=%08x usage=%x pool=%u data=%08x len=%u frame=%llu thread=%d stack:", (long)(o - objs), o->w, o->h, o->nlev, o->fmt, o->usage, o->pool, o->data, o->len, (unsigned long long)st.frames, c->cur);
    for (int k = 0; k < 120; k++) { uint32_t w = rd32(c, g_api_esp + 4 * k); if (w >= 0x401000 && w < 0x2070000) fprintf(stderr, " %x", w); } fprintf(stderr, "\n");
}
#define D_CREATE(NAME, NARGS, CODE) static uint32_t NAME(Ctx *c, uint32_t *a) { CODE }
D_CREATE(d_createtexture, 9, { if (!a[1] || !a[2]) strict_log("CreateTexture", "zero width/height -> INVALIDCALL (the F58 case)", a[1], a[2]); if (a[1] > 16384 || a[2] > 16384 || !a[7] || ((a[4] & 1) && a[6] != 0) || ((a[4] & 0x200) && a[6] == 1)) { strict_log("CreateTexture", "oversized dims / NULL ppTexture / RENDERTARGET outside DEFAULT pool / DYNAMIC+MANAGED", a[1] << 16 | (a[2] & 0xffff), a[4]); if (a[7]) wr32(c, a[7], 0); return 0x8876086Cu; } if (!a[1] || !a[2]) { wr32(c, a[7], 0); LOG("d_createtexture: invalid arguments -> D3DERR_INVALIDCALL\n"); return 0x8876086c; } Obj *o; uint32_t g = make_obj(c, "IDirect3DTexture9", tex_m, NM(tex_m), K_TEXTURE, &o); o->w = a[1]; o->h = a[2]; o->levels = a[3]; o->usage = a[4]; o->fmt = a[5]; o->pool = a[6]; layout_levels(c, o, 1); GFXW(G_TEX_CREATE, oid(o), 0, o->w, o->h, 1, o->nlev, o->fmt, o->usage); texcreate_log(c, o); LOG("CreateTexture %ux%u lv=%u fmt=%u usage=%x pool=%u -> %08x\n", a[1], a[2], o->nlev, a[5], a[4], a[6], g); wr32(c, a[7], g); return 0; })
D_CREATE(d_createvolume, 10, { if (!a[1] || !a[2] || !a[3]) { wr32(c, a[8], 0); LOG("d_createvolume: invalid arguments -> D3DERR_INVALIDCALL\n"); return 0x8876086c; } Obj *o; uint32_t g = make_obj(c, "IDirect3DVolumeTexture9", vol_m, NM(vol_m), K_VOLUME, &o); o->w = a[1]; o->h = a[2]; o->d = a[3]; o->levels = a[4]; o->usage = a[5]; o->fmt = a[6]; o->pool = a[7]; layout_levels(c, o, 1); GFXW(G_TEX_CREATE, oid(o), 2, o->w, o->h, o->d, o->nlev, o->fmt, o->usage); wr32(c, a[8], g); return 0; })
D_CREATE(d_createcube, 8, { if (!a[1]) { wr32(c, a[6], 0); LOG("d_createcube: invalid arguments -> D3DERR_INVALIDCALL\n"); return 0x8876086c; } Obj *o; uint32_t g = make_obj(c, "IDirect3DCubeTexture9", cube_m, NM(cube_m), K_CUBE, &o); o->w = o->h = a[1]; o->levels = a[2]; o->usage = a[3]; o->fmt = a[4]; o->pool = a[5]; layout_levels(c, o, 6); GFXW(G_TEX_CREATE, oid(o), 1, o->w, o->h, 1, o->nlev, o->fmt, o->usage); wr32(c, a[6], g); return 0; })
D_CREATE(d_createvb, 7, { if (!a[1]) { wr32(c, a[5], 0); LOG("d_createvb: invalid arguments -> D3DERR_INVALIDCALL\n"); return 0x8876086c; } Obj *o; uint32_t g = make_obj(c, "IDirect3DVertexBuffer9", buf_m, NM(buf_m), K_VB, &o); o->len = a[1]; o->usage = a[2]; o->fvf = a[3]; o->pool = a[4]; o->data = guest_alloc(c, a[1] + 16); o->own = 1; GFXW(G_BUF_CREATE, oid(o), 0, a[1], 0, a[2]); LOG("CreateVertexBuffer len=%u -> %08x\n", a[1], g); wr32(c, a[5], g); return 0; })
D_CREATE(d_createib, 7, { if (!a[1]) { wr32(c, a[5], 0); LOG("d_createib: invalid arguments -> D3DERR_INVALIDCALL\n"); return 0x8876086c; } Obj *o; uint32_t g = make_obj(c, "IDirect3DIndexBuffer9", buf_m, NM(buf_m), K_IB, &o); o->len = a[1]; o->usage = a[2]; o->fmt = a[3]; o->pool = a[4]; o->data = guest_alloc(c, a[1] + 16); o->own = 1; GFXW(G_BUF_CREATE, oid(o), 1, a[1], a[3], a[2]); LOG("CreateIndexBuffer len=%u -> %08x\n", a[1], g); wr32(c, a[5], g); return 0; })
D_CREATE(d_creatert, 9, { if (!a[1] || !a[2]) { wr32(c, a[7], 0); LOG("d_creatert: invalid arguments -> D3DERR_INVALIDCALL\n"); return 0x8876086c; } LOG("CreateRenderTarget %ux%u fmt=%u ms=%u\n", a[1], a[2], a[3], a[4]); { uint32_t sg = new_surface(c, a[1], a[2], a[3], 0, 1, a[4], 0, 0, 0); GFXW(G_SURF_CREATE, oid(O(c, sg)), a[1], a[2], a[3], 1, a[4]); wr32(c, a[7], sg); } return 0; })
D_CREATE(d_createds, 9, { if (!a[1] || !a[2]) { wr32(c, a[7], 0); LOG("d_createds: invalid arguments -> D3DERR_INVALIDCALL\n"); return 0x8876086c; } LOG("CreateDepthStencilSurface %ux%u fmt=%u\n", a[1], a[2], a[3]); { uint32_t sg = new_surface(c, a[1], a[2], a[3], 0, 2, a[4], 0, 0, 0); GFXW(G_SURF_CREATE, oid(O(c, sg)), a[1], a[2], a[3], 2, a[4]); wr32(c, a[7], sg); } return 0; })
D_CREATE(d_createoffscreen, 7, { if (!a[1] || !a[2]) { wr32(c, a[5], 0); LOG("d_createoffscreen: invalid arguments -> D3DERR_INVALIDCALL\n"); return 0x8876086c; } wr32(c, a[5], new_surface(c, a[1], a[2], a[3], a[4], 0, 0, 0, 0, 0)); return 0; })
static void send_vdecl(Ctx *c, Obj *o) {
    if (!gfx_on()) return; uint8_t *tmp = malloc(o->codelen + 1); rdbuf(c, o->code, tmp, o->codelen);
    uint32_t w[2] = {oid(o), o->codelen / 8}; gfx_msg(G_VDECL, w, 2, tmp, o->codelen); free(tmp);
}
D_CREATE(d_createvdecl, 3, { Obj *o; uint32_t g = make_obj(c, "IDirect3DVertexDeclaration9", vdecl_m, NM(vdecl_m), K_VDECL, &o); o->code = copy_code(c, a[1], &o->codelen, 1); send_vdecl(c, o); wr32(c, a[2], g); return 0; })
static void dump_shader(Ctx *c, Obj *o, const char *stage) {
    const char *dir = getenv("BLUR_DUMP_SHADERS"); if (!dir) return;
    char fn[256]; snprintf(fn, sizeof fn, "%s/sh%u_%s.bin", dir, oid(o), stage);
    uint8_t *b = malloc(o->codelen); rdbuf(c, o->code, b, o->codelen); FILE *f = fopen(fn, "wb"); if (f) { fwrite(b, 1, o->codelen, f); fclose(f); } free(b);
}
static void send_shader(Ctx *c, Obj *o, uint32_t stage) {
    if (!gfx_on()) return;
    uint8_t *code = malloc(o->codelen + 4); rdbuf(c, o->code, code, o->codelen);
    { static int init, have; static const char *missdir;                 // offline cache (BLUR_SHADER_CACHE=shaders.bin): send the key, the browser has the GLSL
      if (!init) { init = 1; const char *p = getenv("BLUR_SHADER_CACHE"); missdir = getenv("BLUR_SHADER_MISS");
          if (!p && access("web/shaders.bin", R_OK) == 0) p = "web/shaders.bin";          // default: on when the page can fetch the same file; BLUR_SHADER_CACHE=off disables
          if (p && !strcmp(p, "off")) p = NULL;
          if (p) { int n = shc_load(p); have = n > 0; fprintf(stderr, "[shader-cache] %s: %d entries\n", p, n); } }
      uint64_t h = shc_fnv64(code, o->codelen);
      if (have && shc_has(h, o->codelen)) { uint32_t w[5] = {oid(o), stage, (uint32_t)h, (uint32_t)(h >> 32), o->codelen}; gfx_msg(G_SHADER_REF, w, 5, NULL, 0); free(code); return; }
      if (have) { fprintf(stderr, "[shader-cache] MISS %016llx_%u (live translation)\n", (unsigned long long)h, o->codelen);
          if (missdir) { char fn[512]; snprintf(fn, sizeof fn, "%s/%016llx_%u.bin", missdir, (unsigned long long)h, o->codelen); FILE *mf = fopen(fn, "wb"); if (mf) { fwrite(code, 1, o->codelen, mf); fclose(mf); } } } }
    ShaderXlat x; if (shader_translate(code, o->codelen, &x) == 0) {
        // payload: id, stage, glsl_len, nin, nout, nsamp, then IOEnt tables (4 bytes each) and the GLSL text
        uint32_t w[6] = {oid(o), stage, x.glsl_len, (uint32_t)x.nin, (uint32_t)x.nout, (uint32_t)x.nsamp};
        size_t nb = 4 * (size_t)(x.nin + x.nout + x.nsamp) + x.glsl_len + 1; uint8_t *blob = malloc(nb), *p = blob;
        memcpy(p, x.in, 4 * (size_t)x.nin); p += 4 * x.nin; memcpy(p, x.out, 4 * (size_t)x.nout); p += 4 * x.nout; memcpy(p, x.samp, 4 * (size_t)x.nsamp); p += 4 * x.nsamp;
        memcpy(p, x.glsl, x.glsl_len + 1); gfx_msg(G_SHADER, w, 6, blob, (uint32_t)nb); free(blob); free(x.glsl);
    } else fprintf(stderr, "[d3d9] shader #%u translation failed\n", oid(o));
    free(code);
}
D_CREATE(d_createvs, 3, { Obj *o; uint32_t g = make_obj(c, "IDirect3DVertexShader9", shader_m, NM(shader_m), K_VSHADER, &o); o->code = copy_code(c, a[1], &o->codelen, 0); dump_shader(c, o, "vs"); send_shader(c, o, 0); LOG("CreateVertexShader %u bytes -> %08x\n", o->codelen, g); wr32(c, a[2], g); return 0; })
D_CREATE(d_createps, 3, { Obj *o; uint32_t g = make_obj(c, "IDirect3DPixelShader9", shader_m, NM(shader_m), K_PSHADER, &o); o->code = copy_code(c, a[1], &o->codelen, 0); dump_shader(c, o, "ps"); send_shader(c, o, 1); LOG("CreatePixelShader %u bytes -> %08x\n", o->codelen, g); wr32(c, a[2], g); return 0; })
D_CREATE(d_createquery, 3, { Obj *o; uint32_t g = make_obj(c, "IDirect3DQuery9", query_m, NM(query_m), K_QUERY, &o); o->fmt = a[1]; { static int lg = -1; static unsigned n[16]; if (lg < 0) lg = getenv("BLUR_QUERYLOG") != NULL; if (lg && a[1] < 16 && n[a[1]]++ < 3) fprintf(stderr, "[query] CreateQuery type=%u ret=%08x\n", a[1], g_api_ret); } wr32(c, a[2], g); return 0; })
D_CREATE(d_createsb, 3, { uint32_t g = make_obj(c, "IDirect3DStateBlock9", sb_m, NM(sb_m), K_STATEBLOCK, NULL); wr32(c, a[3], g); return 0; })
D_CREATE(d_endsb, 2, { uint32_t g = make_obj(c, "IDirect3DStateBlock9", sb_m, NM(sb_m), K_STATEBLOCK, NULL); wr32(c, a[1], g); return 0; })

static void surf_desc_words(Ctx *c, uint32_t surf, uint32_t out[4]) {
    if (!surf) { out[0] = 3; out[1] = out[2] = out[3] = 0; return; }
    if (surf == st.backbuf) { out[0] = 0; out[1] = out[2] = out[3] = 0; return; }
    Obj *o = O(c, surf);
    if (o->parent) { out[0] = 1; out[1] = oid(&objs[o->parent]); out[2] = o->level; out[3] = o->face; } else { out[0] = 2; out[1] = oid(o); out[2] = out[3] = 0; }
}
// IDirect3DDevice9::SetGammaRamp(iSwapChain, Flags, D3DGAMMARAMP*): 3 x 256 WORD (r,g,b); forwarded as message 80 and applied by the browser in the present pass
static uint16_t g_gamma[768]; static int g_gamma_set;
static uint32_t d_setgamma(Ctx *c, uint32_t *a) {
    uint8_t buf[1536]; rdbuf(c, a[3], buf, 1536); memcpy(g_gamma, buf, 1536); g_gamma_set = 1;
    if (getenv("BLUR_GAMMALOG")) fprintf(stderr, "[gamma] SetGammaRamp flags=%u r[0,64,128,255]=%u %u %u %u g=%u %u %u %u b=%u %u %u %u\n", a[2], g_gamma[0], g_gamma[64], g_gamma[128], g_gamma[255], g_gamma[256], g_gamma[320], g_gamma[384], g_gamma[511], g_gamma[512], g_gamma[576], g_gamma[640], g_gamma[767]);
    uint32_t w[1] = {a[2]}; gfx_msg(80, w, 1, buf, 1536); return 0;
}
static uint32_t d_getgamma(Ctx *c, uint32_t *a) { uint16_t r[768]; for (int i = 0; i < 768; i++) r[i] = g_gamma_set ? g_gamma[i] : (uint16_t)((i & 255) * 257); wrbuf(c, a[2], r, 1536); return 0; }
// IDirect3DDevice9::GetRenderTargetData(srcRT, dstSysmemSurface): the game reads back the 1x1 R32F adapted-luminance target every frame (3-deep ring of destination surfaces) to
// compute its exposure constants (c48/c196). We ask the browser for texel (0,0) of the source (message 81) and write it into the destination surface's memory when the
// answer (input type 10: dst object id, 32-bit texel) arrives; the game's ring tolerates this 1-2 frame latency. Only the first texel is transferred (enough for the 1x1 targets).
static uint32_t d_getrtdata(Ctx *c, uint32_t *a) {
    uint32_t d1[4], d2[4]; surf_desc_words(c, a[1], d1); Obj *dst = O(c, a[2]); surf_desc_words(c, a[1], d1); (void)d2;
    uint32_t w[5] = {d1[0], d1[1], d1[2], d1[3], oid(dst)}; GFXW(81, w[0], w[1], w[2], w[3], w[4]); return 0;
}
void d3d9_readback_result(Ctx *c, uint32_t dstid, uint32_t bits) { if (dstid && dstid < (uint32_t)nobjs && objs[dstid].data) wr32(c, objs[dstid].data, bits); }
static uint32_t d_logstub_rtd(Ctx *c, uint32_t *a) { static int n; if (getenv("BLUR_RTDLOG") && n++ < 40) { uint32_t d1[4], d2[4]; surf_desc_words(c, a[1], d1); surf_desc_words(c, a[2], d2); fprintf(stderr, "[rtd] GetRenderTargetData src kind=%u id=%u dst kind=%u id=%u ret=%08x frame=%llu\n", d1[0], d1[1], d2[0], d2[1], g_api_ret, (unsigned long long)st.frames); } return 0; }
// UpdateSurface(srcSurface, srcRect, dstSurface, dstPoint): sysmem -> default-pool copy (was a no-op). Same-format row copy in guest memory, then the destination's container texture
// level is re-sent. 4x4-block aligned for DXT.
static uint32_t d_logstub_us(Ctx *c, uint32_t *a);
static uint32_t d_updatesurface(Ctx *c, uint32_t *a) {
    d_logstub_us(c, a); Obj *s = O(c, a[1]), *d = O(c, a[3]); uint32_t sr[4] = {0, 0, s->w, s->h}, dp[2] = {0, 0}, bb;
    if (a[2]) rdbuf(c, a[2], sr, 16); if (a[4]) rdbuf(c, a[4], dp, 8);
    if (!s->data || !d->data || s->fmt != d->fmt || sr[2] <= sr[0] || sr[3] <= sr[1] || sr[2] > s->w || sr[3] > s->h) return 0;
    uint32_t w = sr[2] - sr[0], h = sr[3] - sr[1], sp = s->lev_pitch[0], dpi = d->lev_pitch[0]; int blk = is_block(s->fmt, &bb);
    uint32_t rows = blk ? (h + 3) / 4 : h, rowb = blk ? ((w + 3) / 4) * bb : (w * bits_per_pixel(s->fmt) + 7) / 8;
    uint32_t so = blk ? (sr[1] / 4) * sp + (sr[0] / 4) * bb : sr[1] * sp + sr[0] * bits_per_pixel(s->fmt) / 8, dof = blk ? (dp[1] / 4) * dpi + (dp[0] / 4) * bb : dp[1] * dpi + dp[0] * bits_per_pixel(s->fmt) / 8;
    for (uint32_t r = 0; r < rows; r++) memmove(G + d->data + dof + r * dpi, G + s->data + so + r * sp, rowb);
    if (d->parent) send_level(c, &objs[d->parent], d->face, d->level);
    return 0;
}
static uint32_t d_logstub_us(Ctx *c, uint32_t *a) { (void)c; static int n; if (getenv("BLUR_RTDLOG") && n++ < 40) fprintf(stderr, "[rtd] UpdateSurface ret=%08x frame=%llu a1=%08x a3=%08x\n", g_api_ret, (unsigned long long)st.frames, a[1], a[3]); return 0; }
static uint32_t d_setrs(Ctx *c, uint32_t *a) { (void)c; if (a[1] < 256) { if (st.rs[a[1]] != a[2]) { if (GF() == 2) dq_push(a[1]); else GFXW(G_SET_RS, a[1], a[2]); } st.rs[a[1]] = a[2]; } return 0; }
static uint32_t d_getrs(Ctx *c, uint32_t *a) { wr32(c, a[2], a[1] < 256 ? st.rs[a[1]] : 0); return 0; }
// sampler index 257..260 = D3DVERTEXTEXTURESAMPLER0..3: kept as slots 16..19 (the browser binds them to the vertex shader's samplers)
static uint32_t smp_slot(uint32_t s) { return s >= 257 && s <= 260 ? s - 257 + 16 : s; }
static uint32_t d_setss(Ctx *c, uint32_t *a) { (void)c; uint32_t s = smp_slot(a[1]); if (s < 32 && a[2] < 16) { if (st.ss[s][a[2]] != a[3]) { if (GF() == 2) dq_push(256 + s * 16 + a[2]); else GFXW(G_SET_SS, s, a[2], a[3]); } st.ss[s][a[2]] = a[3]; } return 0; }
static uint32_t d_getss(Ctx *c, uint32_t *a) { uint32_t s = smp_slot(a[1]); wr32(c, a[3], s < 32 && a[2] < 16 ? st.ss[s][a[2]] : 0); return 0; }
static uint32_t d_settss(Ctx *c, uint32_t *a) { (void)c; if (a[1] < 8 && a[2] < 33) st.tss[a[1]][a[2]] = a[3]; return 0; }
static uint32_t d_gettss(Ctx *c, uint32_t *a) { wr32(c, a[3], a[1] < 8 && a[2] < 33 ? st.tss[a[1]][a[2]] : 0); return 0; }
static uint32_t d_settransform(Ctx *c, uint32_t *a) { if (a[1] < 512) rdbuf(c, a[2], st.xform[a[1]], 64); return 0; }
static uint32_t d_gettransform(Ctx *c, uint32_t *a) { if (a[1] < 512) wrbuf(c, a[2], st.xform[a[1]], 64); return 0; }
static uint32_t d_setviewport(Ctx *c, uint32_t *a) { rdbuf(c, a[1], st.vp, 24); if (chg(sent.vp, st.vp, 24)) GFXW(G_SET_VIEWPORT, st.vp[0], st.vp[1], st.vp[2], st.vp[3], st.vp[4], st.vp[5]); return 0; }
static uint32_t d_getviewport(Ctx *c, uint32_t *a) { wrbuf(c, a[1], st.vp, 24); return 0; }
// the device holds a reference on whatever is bound (SetTexture/SetStreamSource/SetIndices/Set*Shader/SetVertexDeclaration/SetRenderTarget/SetDepthStencilSurface)
static uint32_t d3d_zero(Ctx *c, uint32_t g); static void d3d_reap(Ctx *c);
static void devset(Ctx *c, uint32_t *slot, uint32_t g) {
    uint32_t old = *slot; if (old == g) return;
    if (g) wr32(c, g + 4, rd32(c, g + 4) + 1);
    *slot = g;
    if (old) { uint32_t n = rd32(c, old + 4); if (n) { wr32(c, old + 4, n - 1); if (n == 1) d3d_zero(c, old); } }
}
static void addref_g(Ctx *c, uint32_t g);
static uint32_t d_settexture(Ctx *c, uint32_t *a) { uint32_t s = smp_slot(a[1]); if (s < 20) { if (st.tex[s] != a[2]) { if (GF() == 2) want.tex[s] = a[2] ? oid(O(c, a[2])) : 0; else GFXW(G_SET_TEX, s, a[2] ? oid(O(c, a[2])) : 0); } devset(c, &st.tex[s], a[2]); } return 0; }
static uint32_t d_gettexture(Ctx *c, uint32_t *a) { uint32_t sl = smp_slot(a[1]); uint32_t g = sl < 20 ? st.tex[sl] : 0; addref_g(c, g); wr32(c, a[2], g); return 0; }
static uint32_t d_setstream(Ctx *c, uint32_t *a) { if (a[1] < 16) { devset(c, &st.vb[a[1]], a[2]); st.vb_off[a[1]] = a[3]; st.vb_stride[a[1]] = a[4]; uint32_t sv[3] = {a[2] ? oid(O(c, a[2])) : 0, a[3], a[4]}; if (GF() == 2) memcpy(want.stream[a[1]], sv, 12); else if (chg(sent.stream[a[1]], sv, 12)) GFXW(G_SET_STREAM, a[1], sv[0], sv[1], sv[2]); } return 0; }
static uint32_t d_getstream(Ctx *c, uint32_t *a) { if (a[1] < 16) { addref_g(c, st.vb[a[1]]); wr32(c, a[2], st.vb[a[1]]); wr32(c, a[3], st.vb_off[a[1]]); wr32(c, a[4], st.vb_stride[a[1]]); } return 0; }
static uint32_t d_setindices(Ctx *c, uint32_t *a) { devset(c, &st.ib, a[1]); uint32_t id = a[1] ? oid(O(c, a[1])) : 0; if (GF() == 2) want.ib = id; else if (chg(&sent.ib, &id, 4)) GFXW(G_SET_INDICES, id); return 0; }
static uint32_t d_getindices(Ctx *c, uint32_t *a) { addref_g(c, st.ib); wr32(c, a[1], st.ib); return 0; }
static uint32_t d_setvdecl(Ctx *c, uint32_t *a) { devset(c, &st.vdecl, a[1]); uint32_t id = a[1] ? oid(O(c, a[1])) : 0; if (GF() == 2) want.vdecl = id; else if (chg(&sent.vdecl, &id, 4)) GFXW(G_SET_VDECL, id); return 0; }
static uint32_t d_getvdecl(Ctx *c, uint32_t *a) { addref_g(c, st.vdecl); wr32(c, a[1], st.vdecl); return 0; }
static uint32_t d_setfvf(Ctx *c, uint32_t *a) { (void)c; st.fvf = a[1]; GFXW(G_SET_FVF, a[1]); return 0; }
static uint32_t d_getfvf(Ctx *c, uint32_t *a) { wr32(c, a[1], st.fvf); return 0; }
static uint32_t d_setvs(Ctx *c, uint32_t *a) { devset(c, &st.vs, a[1]); uint32_t id = a[1] ? oid(O(c, a[1])) : 0; if (GF() == 2) want.vs = id; else if (chg(&sent.vs, &id, 4)) GFXW(G_SET_VS, id); return 0; }
static uint32_t d_getvs(Ctx *c, uint32_t *a) { addref_g(c, st.vs); wr32(c, a[1], st.vs); return 0; }
static uint32_t d_setps(Ctx *c, uint32_t *a) { devset(c, &st.ps, a[1]); uint32_t id = a[1] ? oid(O(c, a[1])) : 0; if (GF() == 2) want.ps = id; else if (chg(&sent.ps, &id, 4)) GFXW(G_SET_PS, id); return 0; }
static uint32_t d_getps(Ctx *c, uint32_t *a) { addref_g(c, st.ps); wr32(c, a[1], st.ps); return 0; }
// float constants: update the device shadow, then send only the registers that differ from what the renderer holds (runs separated by <= 1 clean register are merged)
static void const_f(int ps, uint32_t reg, uint32_t n, const float *v) {
    memcpy(ps ? st.psf[reg] : st.vsf[reg], v, 16 * n);
    if (!gfx_on()) return;
    if (GF() == 0) { uint32_t w[2] = {reg, n}; gfx_msg(ps ? G_SET_PSC : G_SET_VSC, w, 2, v, 16 * n); gs_sent += n; return; }
    if (GF() == 2) { if ((int)reg < cd_lo[ps]) cd_lo[ps] = (int)reg; if ((int)(reg + n - 1) > cd_hi[ps]) cd_hi[ps] = (int)(reg + n - 1); return; }
    sent_sync();
    float (*sn)[4] = ps ? sent.psf : sent.vsf; uint32_t i = 0;
    while (i < n) {
        if (!memcmp(sn[reg + i], v + 4 * i, 16)) { i++; gs_skipped++; continue; }
        uint32_t j = i + 1, last = i; while (j < n && j - last <= 2) { if (memcmp(sn[reg + j], v + 4 * j, 16)) last = j; j++; }
        uint32_t cnt = last - i + 1, w[2] = {reg + i, cnt}; memcpy(sn[reg + i], v + 4 * i, 16 * cnt); gfx_msg(ps ? G_SET_PSC : G_SET_VSC, w, 2, v + 4 * i, 16 * cnt); gs_sent += cnt; i = last + 1;
    }
}
// lazy mode: resolve all pending state against the renderer's before a message that consumes it (draw, clear, stretch, readback, ...)
static void const_flush(int ps) {
    int lo = cd_lo[ps], hi = cd_hi[ps]; cd_lo[ps] = 1 << 30; cd_hi[ps] = -1; if (hi < lo) return;
    const float (*v)[4] = ps ? (const float (*)[4])st.psf : (const float (*)[4])st.vsf; float (*sn)[4] = ps ? sent.psf : sent.vsf; int i = lo;
    while (i <= hi) {
        if (!memcmp(sn[i], v[i], 16)) { i++; gs_skipped++; continue; }
        int j = i + 1, last = i; while (j <= hi && j - last <= 2) { if (memcmp(sn[j], v[j], 16)) last = j; j++; }
        uint32_t cnt = (uint32_t)(last - i + 1), w[2] = {(uint32_t)i, cnt}; memcpy(sn[i], v[i], 16 * cnt); gfx_msg(ps ? G_SET_PSC : G_SET_VSC, w, 2, v[i], 16 * cnt); gs_sent += cnt; i = last + 1;
    }
}
void d3d9_flush_state(void) {
    if (!gfx_on()) { ndq = dq_all = 0; cd_lo[0] = cd_lo[1] = 1 << 30; cd_hi[0] = cd_hi[1] = -1; return; }
    if (GF() != 2) return;
    sent_sync();
    if (dq_all) { for (uint32_t i = 0; i < 256; i++) if (sent.rs[i] != st.rs[i]) { sent.rs[i] = st.rs[i]; GFXW(G_SET_RS, i, st.rs[i]); }
        for (uint32_t s = 0; s < 32; s++) for (uint32_t k = 0; k < 16; k++) if (sent.ss[s][k] != st.ss[s][k]) { sent.ss[s][k] = st.ss[s][k]; GFXW(G_SET_SS, s, k, st.ss[s][k]); } }
    else for (int q = 0; q < ndq; q++) { uint32_t k = dq[q];
        if (k < 256) { if (sent.rs[k] != st.rs[k]) { sent.rs[k] = st.rs[k]; GFXW(G_SET_RS, k, st.rs[k]); } else gs_skipped++; }
        else { uint32_t s = (k - 256) >> 4, kk = (k - 256) & 15; if (sent.ss[s][kk] != st.ss[s][kk]) { sent.ss[s][kk] = st.ss[s][kk]; GFXW(G_SET_SS, s, kk, st.ss[s][kk]); } else gs_skipped++; } }
    ndq = dq_all = 0;
    for (uint32_t s = 0; s < 20; s++) if (sent.tex[s] != want.tex[s]) { sent.tex[s] = want.tex[s]; GFXW(G_SET_TEX, s, want.tex[s]); }
    for (uint32_t i = 0; i < 16; i++) if (memcmp(sent.stream[i], want.stream[i], 12)) { memcpy(sent.stream[i], want.stream[i], 12); GFXW(G_SET_STREAM, i, want.stream[i][0], want.stream[i][1], want.stream[i][2]); }
    if (sent.vs != want.vs) { sent.vs = want.vs; GFXW(G_SET_VS, want.vs); }
    if (sent.ps != want.ps) { sent.ps = want.ps; GFXW(G_SET_PS, want.ps); }
    if (sent.ib != want.ib) { sent.ib = want.ib; GFXW(G_SET_INDICES, want.ib); }
    if (sent.vdecl != want.vdecl) { sent.vdecl = want.vdecl; GFXW(G_SET_VDECL, want.vdecl); }
    const_flush(0); const_flush(1);
}
static uint32_t d_setvsf(Ctx *c, uint32_t *a) { if (a[1] + a[3] <= 256) { float t[256 * 4]; rdbuf(c, a[2], t, 16 * a[3]); const_f(0, a[1], a[3], t); } return 0; }
static uint32_t d_getvsf(Ctx *c, uint32_t *a) { if (a[1] + a[3] <= 256) wrbuf(c, a[2], st.vsf[a[1]], 16 * a[3]); return 0; }
static FILE *dump_fp(void); extern uint32_t g_api_ret;
static uint32_t g_last209;
static uint32_t d_setpsf(Ctx *c, uint32_t *a) { if (a[1] <= 209 && a[1] + a[3] > 209) g_last209 = a[2] + 16 * (209 - a[1]); { static int en = -1; if (en < 0) en = getenv("BLUR_PSLOGR") ? atoi(getenv("BLUR_PSLOGR")) : 0; static int n; static float lastv[4]; float cur[4]; if (en && a[1] <= (uint32_t)en && a[1] + a[3] > (uint32_t)en) { for (int k3 = 0; k3 < 4; k3++) { uint32_t w3 = rd32(c, a[2] + 16 * ((uint32_t)en - a[1]) + 4 * k3); memcpy(&cur[k3], &w3, 4); } } if (en && a[1] <= (uint32_t)en && a[1] + a[3] > (uint32_t)en && memcmp(cur, lastv, 16) && n++ < 3000) { memcpy(lastv, cur, 16); uint32_t o = (uint32_t)en - a[1]; fprintf(stderr, "[pslogr frame %llu ret=%08x ptr=%08x start=%u count=%u] c%d =", (unsigned long long)st.frames, g_api_ret, a[2], a[1], a[3], en); for (int k2 = 0; k2 < 4; k2++) { uint32_t w = rd32(c, a[2] + 16 * o + 4 * k2); float fl; memcpy(&fl, &w, 4); fprintf(stderr, " %g", fl); } fprintf(stderr, "\n"); } }
    static int pslog = -1; if (pslog < 0) pslog = getenv("BLUR_PSLOG") != NULL; if (pslog && a[1] <= 25 && a[1] + a[3] > 21) { FILE *f = stderr; fprintf(f, "[frame %llu ret=%08x ptr=%08x] ", (unsigned long long)st.frames, g_api_ret, a[2]); fprintf(f, "SETPSF start=%u count=%u vals:", a[1], a[3]); if (a[1] == 23 || a[1] == 21) { fprintf(f, " [mem"); for (int k = -4; k < 12; k++) { uint32_t w = rd32(c, a[2] + 4 * k); float fl; memcpy(&fl, &w, 4); fprintf(f, " %g", fl); } fprintf(f, "] "); } for (uint32_t k = 0; k < a[3] * 4 && k < 20; k++) { uint32_t w = rd32(c, a[2] + 4 * k); float fl; memcpy(&fl, &w, 4); fprintf(f, " %g", fl); } fprintf(f, "\n"); }
    if (a[1] + a[3] <= 224) { float t[224 * 4]; rdbuf(c, a[2], t, 16 * a[3]); const_f(1, a[1], a[3], t); } return 0; }
static uint32_t d_getpsf(Ctx *c, uint32_t *a) { if (a[1] + a[3] <= 224) wrbuf(c, a[2], st.psf[a[1]], 16 * a[3]); return 0; }
static uint32_t d_setvsi(Ctx *c, uint32_t *a) { if (a[1] + a[3] <= 16) { rdbuf(c, a[2], st.vsi[a[1]], 16 * a[3]); uint32_t w[2] = {a[1], a[3]}; gfx_msg(G_SET_VSCI, w, 2, st.vsi[a[1]], 16 * a[3]); } return 0; }
static uint32_t d_setpsi(Ctx *c, uint32_t *a) { if (a[1] + a[3] <= 16) { rdbuf(c, a[2], st.psi[a[1]], 16 * a[3]); uint32_t w[2] = {a[1], a[3]}; gfx_msg(G_SET_PSCI, w, 2, st.psi[a[1]], 16 * a[3]); } return 0; }
static uint32_t d_setvsb(Ctx *c, uint32_t *a) { if (a[1] + a[3] <= 16) { rdbuf(c, a[2], &st.vsb[a[1]], 4 * a[3]); uint32_t w[2] = {a[1], a[3]}; gfx_msg(G_SET_VSCB, w, 2, &st.vsb[a[1]], 4 * a[3]); } return 0; }
static uint32_t d_setpsb(Ctx *c, uint32_t *a) { if (a[1] + a[3] <= 16) { rdbuf(c, a[2], &st.psb[a[1]], 4 * a[3]); uint32_t w[2] = {a[1], a[3]}; gfx_msg(G_SET_PSCB, w, 2, &st.psb[a[1]], 4 * a[3]); } return 0; }
static uint32_t d_setrt(Ctx *c, uint32_t *a) {
    if (a[1] == 0) devset(c, &st.rt, a[2]); uint32_t d[4]; surf_desc_words(c, a[2], d); if (a[1] >= 8 || chg(sent.rt[a[1]], d, 16)) GFXW(G_SET_RT, a[1], d[0], d[1], d[2], d[3]);
    // D3D9: SetRenderTarget(0, ...) resets the viewport to cover the whole new target (depth range 0..1) and the scissor rect to the target size. The game relies on it
    // (the rear-view mirror pass never calls SetViewport before its Clear): with the previous pass's 320x192 viewport left in place, the depth clear only covered the
    // left 320 columns of the 576x96 mirror target and the rest of the mirror was depth-rejected (half a mirror).
    if (a[1] == 0 && a[2]) { Obj *s = O(c, a[2]); if (s && s->w && s->h) {
        const float zmin = 0.0f, zmax = 1.0f; uint32_t vp[6] = { 0, 0, s->w, s->h, 0, 0 }; memcpy(&vp[4], &zmin, 4); memcpy(&vp[5], &zmax, 4); memcpy(st.vp, vp, 24);
        if (chg(sent.vp, st.vp, 24)) GFXW(G_SET_VIEWPORT, st.vp[0], st.vp[1], st.vp[2], st.vp[3], st.vp[4], st.vp[5]);
        st.scissor[0] = 0; st.scissor[1] = 0; st.scissor[2] = s->w; st.scissor[3] = s->h; if (chg(sent.sc, st.scissor, 16)) GFXW(G_SET_SCISSOR, st.scissor[0], st.scissor[1], st.scissor[2], st.scissor[3]); } }
    return 0; }
static void addref_g(Ctx *c, uint32_t g) { if (g) wr32(c, g + 4, rd32(c, g + 4) + 1); }
static uint32_t d_getrt(Ctx *c, uint32_t *a) { uint32_t g = a[1] == 0 ? (st.rt ? st.rt : st.backbuf) : 0; addref_g(c, g); wr32(c, a[2], g); return 0; }
static uint32_t d_setds(Ctx *c, uint32_t *a) { devset(c, &st.ds, a[1]); uint32_t d[4]; surf_desc_words(c, a[1], d); if (chg(sent.ds, d, 16)) GFXW(G_SET_DS, d[0], d[1], d[2], d[3]); return 0; }
static uint32_t d_getds(Ctx *c, uint32_t *a) { addref_g(c, st.ds); wr32(c, a[1], st.ds); return 0; }
static uint32_t d_setscissor(Ctx *c, uint32_t *a) { rdbuf(c, a[1], st.scissor, 16); if (chg(sent.sc, st.scissor, 16)) GFXW(G_SET_SCISSOR, st.scissor[0], st.scissor[1], st.scissor[2], st.scissor[3]); return 0; }
static uint32_t d_getscissor(Ctx *c, uint32_t *a) { wrbuf(c, a[1], st.scissor, 16); return 0; }
static uint32_t d_begin(Ctx *c, uint32_t *a) { (void)c; (void)a; st.scenes++; return 0; }
static uint32_t d_clear(Ctx *c, uint32_t *a) {   // Count, pRects, Flags, Color, Z, Stencil
    st.clears++; uint8_t rects[16 * 16]; uint32_t n = a[1] > 16 ? 16 : a[1]; if (n && a[2]) rdbuf(c, a[2], rects, 16 * n); else n = 0;
    uint32_t w[5] = {a[3], a[4], a[5], a[6], n}; gfx_msg(G_CLEAR, w, 5, rects, 16 * n); return 0;
}
static uint32_t d_draw(Ctx *c, uint32_t *a);
static uint32_t vert_count(uint32_t type, uint32_t n) { switch (type) { case 1: return n; case 2: return 2 * n; case 3: return n + 1; case 4: return 3 * n; case 5: case 6: return n + 2; } return n; }
static void ff_log(Ctx *c, const char *what, uint32_t *a) {
    static int on = -1; if (on < 0) on = getenv("BLUR_FFLOG") != NULL; if (!on || st.vs) return;
    static uint32_t seen[32]; static int ns; uint32_t key = st.fvf * 31u + st.ps * 7u + a[1]; for (int i = 0; i < ns; i++) if (seen[i] == key) return; if (ns < 32) seen[ns++] = key;
    fprintf(stderr, "[ff] %s without vertex shader: fvf=%08x vdecl=%u ps=%u (codelen %u) type=%u a1=%u a2=%u a3=%u a4=%u tex0=%u tex1=%u tex2=%u tex3=%u rt=%08x vp=%u,%u,%u,%u\n", what, st.fvf, st.vdecl, st.ps, st.ps ? O(c, st.ps)->codelen : 0,
        a[1], a[1], a[2], a[3], a[4], st.tex[0], st.tex[1], st.tex[2], st.tex[3], st.rt, st.vp[0], st.vp[1], st.vp[2], st.vp[3]);
}
static void ff_dump(Ctx *c, uint32_t *a) {      // decl + first vertices of a DrawPrimitiveUP (a[3]=data a[4]=stride)
    static int n; if (!getenv("BLUR_FFLOG") || st.vs || n++ % 40 != 0 || n > 400) return; Obj *d = O(c, st.vdecl);
    for (uint32_t o = d->code; d->code && o + 8 <= d->code + d->codelen; o += 8) { uint16_t stream = rd16(c, o), off = rd16(c, o + 2); uint8_t ty = rd8(c, o + 4), me = rd8(c, o + 5), us = rd8(c, o + 6), ui = rd8(c, o + 7); if (stream == 0xff) break; fprintf(stderr, "[ff]   elem stream=%u off=%u type=%u method=%u usage=%u idx=%u\n", stream, off, ty, me, us, ui); }
    for (int v = 0; v < 4; v++) { fprintf(stderr, "[ff]   v%d:", v); for (uint32_t k = 0; k < a[4] / 4; k++) { uint32_t w = rd32(c, a[3] + v * a[4] + 4 * k); float f; memcpy(&f, &w, 4); fprintf(stderr, " %g", (double)f); } fprintf(stderr, "\n"); }
    for (int t = 0; t < 3; t++) { Obj *o = O(c, st.tex[t]); uint32_t nz = 0, sum = 0; for (uint32_t i = 0; i < o->lev_off[1] && o->data; i += 7) { uint8_t b = G[o->data + i]; if (b) nz++; sum += b; }
        fprintf(stderr, "[ff]   tex%d #%ld data=%08x %ux%u fmt=%u nlev=%u sampled-nonzero=%u mean=%.1f\n", t, (long)(o - objs), o->data, o->w, o->h, o->fmt, o->nlev, nz, o->lev_off[1] ? (double)sum / (o->lev_off[1] / 7 + 1) : 0.0); }
    fprintf(stderr, "[ff]   ps consts c0..c3:"); for (int k = 0; k < 4; k++) fprintf(stderr, " (%g %g %g %g)", st.psf[k][0], st.psf[k][1], st.psf[k][2], st.psf[k][3]); fprintf(stderr, "\n");
}
static void pre_draw(Ctx *c) {
    { static int force = -1; if (force < 0) force = getenv("BLUR_UIFORCE") != NULL;      // diagnostic: make the UI shaders' colour ramp opaque white
      if (force && st.vs && O(c, st.vs)->codelen == 344) { float v[2][4] = {{1, 1, 1, 1}, {1, 1, 1, 1}}; uint32_t w[2] = {23, 2}; gfx_msg(G_SET_PSC, w, 2, v, 32); GFXW(G_SET_RS, 22, 1); GFXW(G_SET_RS, 7, 0); st.rs[22] = 1; } }
}
static uint32_t d_drawp(Ctx *c, uint32_t *a) { pre_draw(c); ff_log(c, "DrawPrimitive", a); GFXW(G_DRAW, a[1], 0, 0, 0, 0, a[2], a[3]); return d_draw(c, a); }                        // type, startvertex, primcount
static uint32_t d_drawip(Ctx *c, uint32_t *a) { pre_draw(c); ff_log(c, "DrawIndexedPrimitive", a); GFXW(G_DRAW, a[1], 1, a[2], a[3], a[4], a[5], a[6]); return d_draw(c, a); }             // type, base, minidx, numverts, startindex, primcount
static uint32_t d_drawpup(Ctx *c, uint32_t *a) { ff_log(c, "DrawPrimitiveUP", a); ff_dump(c, a);                                                                                         // type, primcount, data, stride
    uint32_t nv = vert_count(a[1], a[2]), nb = nv * a[4]; if (gfx_on() && nb < (32u << 20)) { uint32_t w[7] = {a[1], a[2], a[4], nv, 0, 0, 0}; uint8_t *tmp = gfx_msg_alloc(G_DRAW_UP, w, 7, nb); if (tmp) rdbuf(c, a[3], tmp, nb); }
    return d_draw(c, a);
}
static uint32_t d_drawipup(Ctx *c, uint32_t *a) {                                                                                        // type, minidx, numverts, primcount, idxdata, idxfmt, vdata, stride
    uint32_t ni = vert_count(a[1], a[4]), isz = a[6] == 101 ? 2 : 4, nb_i = ni * isz, nb_v = (a[2] + a[3]) * a[8];
    if (gfx_on() && nb_v < (32u << 20)) { uint32_t w[7] = {a[1], a[4], a[8], a[2] + a[3], 1, a[6], ni}; uint8_t *tmp = gfx_msg_alloc(G_DRAW_UP, w, 7, nb_i + nb_v); if (tmp) { rdbuf(c, a[5], tmp, nb_i); rdbuf(c, a[7], tmp + nb_i, nb_v); } }
    return d_draw(c, a);
}
static FILE *dump_fp(void) { static FILE *f; if (!f) f = tmpfile(); return f; }
static uint32_t d_draw(Ctx *c, uint32_t *a) {
    st.draws++; st.draws_frame++;
    { static int ut = -1; if (ut < 0) ut = getenv("BLUR_UITEX") != NULL; Obj *pp = O(c, st.ps);
      if (ut && (pp->codelen == 596 || pp->codelen == 680) && st.frames % 300 == 100) {
        fprintf(stderr, "[uitex] frame %llu ps#%ld(%u B) tex:", (unsigned long long)st.frames, (long)(pp - objs), pp->codelen);
        for (int i = 0; i < 4; i++) if (st.tex[i]) { Obj *t = O(c, st.tex[i]); uint32_t n = size_of(t->fmt, t->w, t->h), nz = 0, cnt = 0; for (uint32_t k = 0; k < n && t->data; k += 16) { cnt++; nz += rd32(c, t->data + t->lev_off[0] + k) != 0; }
          fprintf(stderr, " s%d=#%ld %ux%u/f%u live-nonzero=%.0f%%", i, (long)(t - objs), t->w, t->h, t->fmt, cnt ? 100.0 * nz / cnt : 0.0); }
        fprintf(stderr, "\n"); } }
    static long dd = -2; if (dd == -2) dd = getenv("BLUR_DRAWDUMP") ? atol(getenv("BLUR_DRAWDUMP")) : -1;       // BLUR_DRAWDUMP=N: print every draw of the first frame having >= N draws
    if ((dlog(c) && st.frames < 4) || dd >= 0) {
        FILE *ef = (dd >= 0) ? dump_fp() : stderr; Obj *v = O(c, st.vs), *p = O(c, st.ps); fprintf(ef, "[d3d9] DRAW type=%u prims=%u indexed=%d  vs=#%ld(%u B) ps=#%ld(%u B) rt=%08x vp=%ux%u tex:", a[1], a[3], (int)(a[0] && st.ib), (long)(v - objs), v->codelen, (long)(p - objs), p->codelen, st.rt ? st.rt : st.backbuf, st.vp[2], st.vp[3]);
        fprintf(ef, "    [mtl] ptr=%08x c209=%g %g %g %g c210=%g %g %g %g\n", g_last209, st.psf[209][0], st.psf[209][1], st.psf[209][2], st.psf[209][3], st.psf[210][0], st.psf[210][1], st.psf[210][2], st.psf[210][3]);
        fprintf(ef, "    [mtlmem]"); for (int q = -8; q < 20; q++) { uint32_t w = rd32(c, g_last209 + 4 * q); float fl; memcpy(&fl, &w, 4); fprintf(ef, " %08x", w); (void)fl; } fprintf(ef, "\n");
        for (int i = 0; i < 16; i++) if (st.tex[i]) { Obj *t = O(c, st.tex[i]); fprintf(ef, " s%d=%ux%u/f%u", i, t->w, t->h, t->fmt); }
        fprintf(ef, "  rs: zen=%u blend=%u src=%u dst=%u cull=%u\n", st.rs[7], st.rs[27], st.rs[19], st.rs[20], st.rs[22]);
        if (getenv("BLUR_DRAWCONST") && (v - objs) > 1000 && st.ib) { Obj *ib = O(c, st.ib); fprintf(ef, "    idx args base=%d minidx=%u nverts=%u start=%u  ib fmt=%u len=%u idx:", (int)a[2], a[3], a[4], a[5], ib->fmt, ib->len); for (int k = 0; k < 8; k++) fprintf(ef, " %u", rd16(c, ib->data + (a[5] + k) * 2)); fprintf(ef, "\n"); }
        if (getenv("BLUR_DRAWCONST") && (v - objs) > 1000) fprintf(ef, "    scissor_en=%u rect=%u,%u,%u,%u colorwrite=%x zwrite=%u zfunc=%u alphatest=%u stencil=%u\n", st.rs[174], st.scissor[0], st.scissor[1], st.scissor[2], st.scissor[3], st.rs[168], st.rs[14], st.rs[23], st.rs[15], st.rs[52]);
        if (getenv("BLUR_DRAWCONST") && (v - objs) > 1000 && st.vb[0]) { Obj *vb = O(c, st.vb[0]); fprintf(ef, "    vb0 stride=%u off=%u data=%08x len=%u verts:\n", st.vb_stride[0], st.vb_off[0], vb->data, vb->len); for (int k = 0; k < 4; k++) { fprintf(ef, "      "); for (uint32_t q = 0; q < st.vb_stride[0] / 4 && q < 12; q++) { uint32_t w = rd32(c, vb->data + st.vb_off[0] + k * st.vb_stride[0] + q * 4); float fl; memcpy(&fl, &w, 4); fprintf(ef, "%g(%08x) ", fl, w); } fprintf(ef, "\n"); } }
        if (getenv("BLUR_DRAWCONST") && (v - objs) > 1000) { for (int k = 32; k < 36; k++) fprintf(ef, "    vc%d = %g %g %g %g\n", k, st.vsf[k][0], st.vsf[k][1], st.vsf[k][2], st.vsf[k][3]); for (int k = 21; k < 26; k++) fprintf(ef, "    pc%d = %g %g %g %g\n", k, st.psf[k][0], st.psf[k][1], st.psf[k][2], st.psf[k][3]); }
    }
    return 0;
}
uint64_t g_frames_pub; static uint64_t rf_us;   // time in resend_if_filled / volume_refresh (BLUR_GFXSTAT)
void d3d9_texaudit(Ctx *c, const char *when); static uint64_t n_destroyed;
static void texref_maybe(Ctx *c);
uint64_t g_last_draws;   // draws of the frame being presented (rt.c pace_present exempts loading screens)
static uint32_t d_present(Ctx *c, uint32_t *a) {
    (void)a; st.frames++;
    // BLUR_REL=1: frame-keyed triggers (BLUR_KEYS "Key@fN", BLUR_CHECKPOINTS, BLUR_MAXFRAMES) count from the first frame that draws >= 150 primitives (front-end/menu visible), so the
    // scenario does not depend on how many loading-screen frames the scheduler happens to need (that number changed by 2.6x with the coroutine scheduler)
    static uint64_t rel_base; static int rel_on = -1; if (rel_on < 0) rel_on = getenv("BLUR_REL") != NULL;
    if (rel_on && !rel_base && st.draws_frame >= 150) { rel_base = st.frames; fprintf(stderr, "[rel] front-end visible at absolute frame %llu\n", (unsigned long long)rel_base); }
    uint64_t trig_frame = rel_on ? (rel_base ? st.frames - rel_base + 1 : 0) : st.frames;
    g_frames_pub = trig_frame; { static long mf = -2; if (mf == -2) mf = getenv("BLUR_MAXFRAMES") ? atol(getenv("BLUR_MAXFRAMES")) : -1; if (mf > 0 && (long)trig_frame >= mf) { fprintf(stderr, "[maxframes] stopping after %llu frames\n", (unsigned long long)trig_frame); c->exited = 5; } } d3d_reap(c); { static int hp_on = -1; if (hp_on < 0) hp_on = getenv("BLUR_OBJSTAT") != NULL; if (hp_on && st.frames % 1000 == 0) fprintf(stderr, "[heaptrace] frame %llu: guest heap %u MB, %llu objects destroyed so far, %d object ids\n", (unsigned long long)st.frames, (c->heap_next - 0x20000000u) >> 20, (unsigned long long)n_destroyed, nobjs); } if (st.frames % 300 == 0) heapchk_scan(c, "at frame scan"); if (st.frames % 1500 == 0) d3d9_texaudit(c, "periodic"); d3d_reap(c); { static int hp_on = -1; if (hp_on < 0) hp_on = getenv("BLUR_OBJSTAT") != NULL; if (hp_on && st.frames % 1000 == 0) fprintf(stderr, "[heaptrace] frame %llu: guest heap %u MB, %llu objects destroyed so far, %d object ids\n", (unsigned long long)st.frames, (c->heap_next - 0x20000000u) >> 20, (unsigned long long)n_destroyed, nobjs); } if (st.frames % 300 == 0) heapchk_scan(c, "at frame scan"); if (st.frames % 1500 == 0) d3d9_texaudit(c, "periodic");
    if (dlog(c) && (st.frames <= 5 || st.frames % 60 == 0)) fprintf(stderr, "[d3d9] Present #%llu (draws last frame=%llu, clears=%llu)\n", (unsigned long long)st.frames, (unsigned long long)st.draws_frame, (unsigned long long)st.clears);
    { static long dd2 = -2; static int done; if (dd2 == -2) dd2 = getenv("BLUR_DRAWDUMP") ? atol(getenv("BLUR_DRAWDUMP")) : -1;
      if (dd2 >= 0 && !done) { FILE *f = dump_fp(); if ((long)st.draws_frame >= dd2) { done = 1; fflush(f); rewind(f); char ln[1024]; while (fgets(ln, sizeof ln, f)) fputs(ln, stderr); fprintf(stderr, "[drawdump] end frame %llu\n", (unsigned long long)st.frames); } else { fflush(f); rewind(f); if (ftruncate(fileno(f), 0)) {} } } }
    { static int on = -1; static uint64_t last, t_flush, sec0, fr0, dr0, worst, maxdt; static uint64_t pres_last;
      if (on < 0) on = getenv("BLUR_FPSLOG") != NULL;
      { extern void ww_arm(void), ww_rearm(void); static int armed; if (getenv("BLUR_WATCHW") || getenv("BLUR_WATCHR")) { if (!armed && st.frames == 1 && getenv("BLUR_WATCHW_AFTER_FRAME1")) { ww_arm(); armed = 1; } if (armed || !getenv("BLUR_WATCHW_AFTER_FRAME1")) ww_rearm(); } }
      { static int dm = -1; if (dm < 0) dm = getenv("BLUR_MEMDUMP") != NULL; if (dm && (st.frames == 5 || st.frames == 300)) { uint32_t lo = (uint32_t)strtoul(getenv("BLUR_MEMDUMP"), 0, 16); fprintf(stderr, "[memdump] frame %llu @%08x:", (unsigned long long)st.frames, lo); for (int k = 0; k < 64; k++) fprintf(stderr, " %02x", rd8(c, lo + k)); fprintf(stderr, "\n"); } }
      { uint64_t r0 = real_us(); resend_if_filled(c); rf_us += real_us() - r0; } texref_maybe(c); uint64_t t0 = real_us(); g_last_draws = st.draws_frame; st.draws_frame = 0; GFXW(G_PRESENT, (uint32_t)st.frames); gfx_flush(); t_flush += real_us() - t0;
      if (on) { uint64_t n = real_us(); if (pres_last && n - pres_last > maxdt) maxdt = n - pres_last; pres_last = n; if (!sec0) { sec0 = n; fr0 = st.frames; dr0 = st.draws; }
        if (n - sec0 >= 1000000) { fprintf(stderr, "[fps] %.1f fps  draws/frame %.0f  gfx_flush %.2f ms/frame  worst frame %.1f ms\n", (st.frames - fr0) * 1e6 / (n - sec0), (double)(st.draws - dr0) / (st.frames - fr0 ? st.frames - fr0 : 1), t_flush / 1000.0 / (st.frames - fr0 ? st.frames - fr0 : 1), maxdt / 1000.0); sec0 = n; fr0 = st.frames; dr0 = st.draws; t_flush = 0; maxdt = 0; } }
      (void)last; (void)worst; }
    { static char *cp; static int cp_init; if (!cp_init) { cp_init = 1; const char *e = getenv("BLUR_CHECKPOINTS"); if (e) cp = strdup(e); }   // BLUR_CHECKPOINTS="8100:name,9000:name": at these frames print "[checkpoint] <frame> <name>" and
      if (cp) { char *q = cp; while (*q) { long f = strtol(q, &q, 10); char nm[40] = "cp"; if (*q == ':') { q++; int k = 0; while (*q && *q != ',' && k < 39) nm[k++] = *q++; nm[k] = 0; } if (*q == ',') q++;        // pause ~BLUR_CHECKPOINT_MS (1200) so a viewer can screenshot the frame just presented
          if (f == (long)trig_frame) { fprintf(stderr, "[checkpoint] %ld %s\n", f, nm); fflush(stderr); if (gfx_on() && net_client_connected()) usleep((useconds_t)(getenv("BLUR_CHECKPOINT_MS") ? atoi(getenv("BLUR_CHECKPOINT_MS")) : 1200) * 1000); } } } }
    c->yield_req = 1; return 0;     // frame boundary: let other threads run
}
static uint32_t d_stretch(Ctx *c, uint32_t *a) {   // src, srcRect, dst, dstRect, filter
    uint32_t sd[4], dd[4]; surf_desc_words(c, a[1], sd); surf_desc_words(c, a[3], dd); uint32_t sr[4] = {0}, dr[4] = {0}, has_s = 0, has_d = 0;
    if (a[2]) { rdbuf(c, a[2], sr, 16); has_s = 1; } if (a[4]) { rdbuf(c, a[4], dr, 16); has_d = 1; }
    uint32_t w[21] = {sd[0], sd[1], sd[2], sd[3], has_s, sr[0], sr[1], sr[2], sr[3], dd[0], dd[1], dd[2], dd[3], has_d, dr[0], dr[1], dr[2], dr[3], a[5]};
    if (gfx_on()) gfx_msg(G_STRETCH, w, 19, NULL, 0); return 0;
}
static uint32_t d_colorfill(Ctx *c, uint32_t *a) {   // surface, rect, color  (encoded as a StretchRect-style message with src kind 3)
    uint32_t dd[4], dr[4] = {0}, has = 0; surf_desc_words(c, a[1], dd); if (a[2]) { rdbuf(c, a[2], dr, 16); has = 1; }
    uint32_t w[21] = {4, a[3], 0, 0, 0, 0, 0, 0, 0, dd[0], dd[1], dd[2], dd[3], has, dr[0], dr[1], dr[2], dr[3], 0};
    if (gfx_on()) gfx_msg(G_STRETCH, w, 19, NULL, 0); return 0;
}
static uint32_t d_updatetexture(Ctx *c, uint32_t *a) { if (texw(oid(O(c, a[2])))) fprintf(stderr, "[texw] UpdateTexture dst #%u src #%u ret=%08x frame=%llu\n", oid(O(c, a[2])), oid(O(c, a[1])), g_api_ret, (unsigned long long)st.frames);   // src (sysmem) -> dst (default pool): re-send every level with the destination id
    Obj *src = O(c, a[1]), *dst = O(c, a[2]); if (!gfx_on() || src == &objs[0] || dst == &objs[0]) return 0;
    uint32_t faces = dst->kind == K_CUBE ? 6 : 1;
    for (uint32_t f = 0; f < faces; f++) for (uint32_t l = 0; l < dst->nlev && l < src->nlev; l++) { uint32_t keep = src->guest; (void)keep; Obj tmp = *src; uint32_t id = oid(dst);
        uint32_t lw = pow2lv(dst->w, l), lh = pow2lv(dst->h, l), n = size_of(src->fmt, lw, lh), base = src->data + src->lev_off[l] + (src->kind == K_CUBE ? f * (src->len / 6) : 0);
        uint32_t w[8] = {id, f, l, lw, lh, 1, src->fmt, src->lev_pitch[l]}; uint8_t *buf = gfx_msg_alloc(G_TEX_UPLOAD, w, 8, n); if (!buf) return 0; rdbuf(c, base, buf, n);
        if (dst->data && dst->fmt == src->fmt && l < dst->nlev && size_of(dst->fmt, lw, lh) == n) memcpy(G + dst->data + dst->lev_off[l] + (dst->kind == K_CUBE ? f * (dst->len / 6) : 0), buf, n);   // mirror into the default-pool texture's own guest storage (texture audit reference; harmless otherwise)
        (void)tmp; }
    return 0;
}
static uint32_t d_getbackbuffer(Ctx *c, uint32_t *a) { if (st.backbuf) wr32(c, st.backbuf + 4, rd32(c, st.backbuf + 4) + 1); wr32(c, a[4], st.backbuf); return 0; }
static uint32_t d_getdirect3d(Ctx *c, uint32_t *a) { (void)c; (void)a; return 0; }
static uint32_t d_testcoop(Ctx *c, uint32_t *a) { (void)c; (void)a; return 0; }                    // D3D_OK: device is not lost
static uint32_t d_getswapchain(Ctx *c, uint32_t *a) { wr32(c, a[2], make_obj(c, "IDirect3DSwapChain9", sc_m, NM(sc_m), K_SWAPCHAIN, NULL)); return 0; }
static uint32_t d_numswap(Ctx *c, uint32_t *a) { (void)c; (void)a; return 1; }
static uint32_t d_getcreation(Ctx *c, uint32_t *a) { wr32(c, a[1], 0); wr32(c, a[1] + 4, 1); wr32(c, a[1] + 8, 0x10001); wr32(c, a[1] + 12, 0x20 | 0x10); return 0; }
static uint32_t g_refresh(void) { static uint32_t v; if (!v) v = getenv("BLUR_REFRESH") ? (uint32_t)atoi(getenv("BLUR_REFRESH")) : 60; return v; }   // display refresh rate reported to the game (it paces frames from it)
static uint32_t d_getdisplaymode(Ctx *c, uint32_t *a) { wr32(c, a[2], 1920); wr32(c, a[2] + 4, 1080); wr32(c, a[2] + 8, g_refresh()); wr32(c, a[2] + 12, 22); return 0; }
static uint32_t d_availtexmem(Ctx *c, uint32_t *a) { (void)c; (void)a; static uint32_t v; if (!v) { const char *e = getenv("BLUR_TEXMEM"); v = e ? (uint32_t)atoi(e) << 20 : 0x30000000; } return v; }   /* BLUR_TEXMEM=MB */
static uint32_t d_reset(Ctx *c, uint32_t *a) { rdbuf(c, a[1], st.pp, 56); return 0; }

static uint32_t d3d_getcaps_fill(Ctx *c, uint32_t p);
static uint32_t d_getcaps(Ctx *c, uint32_t *a) { return d3d_getcaps_fill(c, a[1]); }

// getters that used to be S_OK stubs leaving the caller's out parameter untouched
static uint32_t d_getsfreq(Ctx *c, uint32_t *a) { if (a[2]) wr32(c, a[2], 1); return 0; }                      // GetStreamSourceFreq(stream, &divider): no instancing -> 1
static uint32_t d_getraster(Ctx *c, uint32_t *a) { if (a[2]) { wr32(c, a[2], 0); wr32(c, a[2] + 4, 0); } return 0; }   // D3DRASTER_STATUS {InVBlank, ScanLine}
static const ComMethod dev_m[] = {
    IU, { "TestCooperativeLevel", 1, d_testcoop }, { "GetAvailableTextureMem", 1, d_availtexmem }, Z("EvictManagedResources", 1), { "GetDirect3D", 2, d_getdirect3d },
    { "GetDeviceCaps", 2, d_getcaps }, { "GetDisplayMode", 3, d_getdisplaymode }, { "GetCreationParameters", 2, d_getcreation }, Z("SetCursorProperties", 4),
    Z("SetCursorPosition", 4), Z("ShowCursor", 2), Z("CreateAdditionalSwapChain", 3), { "GetSwapChain", 3, d_getswapchain }, { "GetNumberOfSwapChains", 1, d_numswap },
    { "Reset", 2, d_reset }, { "Present", 5, d_present }, { "GetBackBuffer", 5, d_getbackbuffer }, { "GetRasterStatus", 3, d_getraster }, Z("SetDialogBoxMode", 2), { "SetGammaRamp", 4, d_setgamma },
    { "GetGammaRamp", 3, d_getgamma }, { "CreateTexture", 9, d_createtexture }, { "CreateVolumeTexture", 10, d_createvolume }, { "CreateCubeTexture", 8, d_createcube },
    { "CreateVertexBuffer", 7, d_createvb }, { "CreateIndexBuffer", 7, d_createib }, { "CreateRenderTarget", 9, d_creatert }, { "CreateDepthStencilSurface", 9, d_createds },
    { "UpdateSurface", 5, d_updatesurface }, { "UpdateTexture", 3, d_updatetexture }, { "GetRenderTargetData", 3, d_getrtdata }, Z("GetFrontBufferData", 3), { "StretchRect", 6, d_stretch }, { "ColorFill", 4, d_colorfill },
    { "CreateOffscreenPlainSurface", 7, d_createoffscreen }, { "SetRenderTarget", 3, d_setrt }, { "GetRenderTarget", 3, d_getrt }, { "SetDepthStencilSurface", 2, d_setds },
    { "GetDepthStencilSurface", 2, d_getds }, { "BeginScene", 1, d_begin }, Z("EndScene", 1), { "Clear", 7, d_clear }, { "SetTransform", 3, d_settransform },
    { "GetTransform", 3, d_gettransform }, Z("MultiplyTransform", 3), { "SetViewport", 2, d_setviewport }, { "GetViewport", 2, d_getviewport }, Z("SetMaterial", 2),
    Z("GetMaterial", 2), Z("SetLight", 3), Z("GetLight", 3), Z("LightEnable", 3), Z("GetLightEnable", 3), Z("SetClipPlane", 3), Z("GetClipPlane", 3),
    { "SetRenderState", 3, d_setrs }, { "GetRenderState", 3, d_getrs }, { "CreateStateBlock", 3, d_createsb }, Z("BeginStateBlock", 1), { "EndStateBlock", 2, d_endsb },
    Z("SetClipStatus", 2), Z("GetClipStatus", 2), { "GetTexture", 3, d_gettexture }, { "SetTexture", 3, d_settexture }, { "GetTextureStageState", 4, d_gettss },
    { "SetTextureStageState", 4, d_settss }, { "GetSamplerState", 4, d_getss }, { "SetSamplerState", 4, d_setss }, Z("ValidateDevice", 2), Z("SetPaletteEntries", 3),
    Z("GetPaletteEntries", 3), Z("SetCurrentTexturePalette", 2), Z("GetCurrentTexturePalette", 2), { "SetScissorRect", 2, d_setscissor }, { "GetScissorRect", 2, d_getscissor },
    Z("SetSoftwareVertexProcessing", 2), Z("GetSoftwareVertexProcessing", 1), Z("SetNPatchMode", 2), Z("GetNPatchMode", 1), { "DrawPrimitive", 4, d_drawp },
    { "DrawIndexedPrimitive", 7, d_drawip }, { "DrawPrimitiveUP", 5, d_drawpup }, { "DrawIndexedPrimitiveUP", 9, d_drawipup }, Z("ProcessVertices", 7),
    { "CreateVertexDeclaration", 3, d_createvdecl }, { "SetVertexDeclaration", 2, d_setvdecl }, { "GetVertexDeclaration", 2, d_getvdecl }, { "SetFVF", 2, d_setfvf },
    { "GetFVF", 2, d_getfvf }, { "CreateVertexShader", 3, d_createvs }, { "SetVertexShader", 2, d_setvs }, { "GetVertexShader", 2, d_getvs },
    { "SetVertexShaderConstantF", 4, d_setvsf }, { "GetVertexShaderConstantF", 4, d_getvsf }, { "SetVertexShaderConstantI", 4, d_setvsi }, Z("GetVertexShaderConstantI", 4),
    { "SetVertexShaderConstantB", 4, d_setvsb }, Z("GetVertexShaderConstantB", 4), { "SetStreamSource", 5, d_setstream }, { "GetStreamSource", 5, d_getstream },
    Z("SetStreamSourceFreq", 3), { "GetStreamSourceFreq", 3, d_getsfreq }, { "SetIndices", 2, d_setindices }, { "GetIndices", 2, d_getindices }, { "CreatePixelShader", 3, d_createps },
    { "SetPixelShader", 2, d_setps }, { "GetPixelShader", 2, d_getps }, { "SetPixelShaderConstantF", 4, d_setpsf }, { "GetPixelShaderConstantF", 4, d_getpsf },
    { "SetPixelShaderConstantI", 4, d_setpsi }, Z("GetPixelShaderConstantI", 4), { "SetPixelShaderConstantB", 4, d_setpsb }, Z("GetPixelShaderConstantB", 4), Z("DrawRectPatch", 4),
    Z("DrawTriPatch", 4), Z("DeletePatch", 2), { "CreateQuery", 3, d_createquery },
};

// ---- IDirect3D9 ----------------------------------------------------------------------------------------------------------------------------
static uint32_t d3d_getcaps_fill(Ctx *c, uint32_t p) {
    uint8_t z[304] = {0}; wrbuf(c, p, z, sizeof z);
    struct { uint32_t off, v; } f[] = {
        {0, 1}, {8, 0x20000}, {12, 0x80000 | 0x40000000 | 0x20000000}, {16, 0x20 | 0x80}, {20, 0xf}, {24, 0x3},
        {28, 0x000bf0f0 | 0x100000}, {32, 0x0003ffee}, {36, 0x07733ff0}, {40, 0xff}, {44, 0x3fff}, {48, 0x3fff}, {52, 0xff}, {56, 0x08181ff0 & 0xffff}, {60, 0x0001ecc5},
        {64, 0x03030303 | 0x0f000f00}, {68, 0x03030303 | 0x0f000f00}, {72, 0x03030303}, {76, 0x3f}, {80, 0x3f}, {84, 0x1f}, {88, 4096}, {92, 4096}, {96, 2048}, {100, 8192},
        {104, 4096}, {108, 16}, {132, 0}, {136, 0x1ff}, {140, 0x10008}, {144, 0x03ffffff & 0xffffff}, {148, 8}, {152, 8}, {156, 0x17b}, {160, 8}, {164, 8}, {168, 4}, {172, 0},
        {180, 0xfffff}, {184, 0xfffff}, {188, 16}, {192, 255}, {196, 0xfffe0300}, {200, 256}, {204, 0xffff0300}, {212, 0x1 | 0x2 | 0x4 | 0x8 | 0x10}, {236, 0x3ff}, {240, 4},
        {244, 0x03000300}, {248, 0x1}, {252, 24}, {256, 32}, {260, 24}, {264, 0x1}, {268, 24}, {272, 32}, {276, 24}, {280, 512}, {284, 0x03030303}, {288, 0xffff}, {292, 0xffff},
        {296, 32768}, {300, 32768},
    };
    for (size_t i = 0; i < sizeof f / sizeof f[0]; i++) wr32(c, p + f[i].off, f[i].v);
    float one = 1.0f, big = 3.4e38f, ps = 8192.0f; uint32_t u;
    memcpy(&u, &big, 4); wr32(c, p + 112, u); memcpy(&u, &ps, 4); wr32(c, p + 176, u); memcpy(&u, &one, 4); wr32(c, p + 208, u);
    return 0;
}
static uint32_t d3d_adaptercount(Ctx *c, uint32_t *a) { (void)c; (void)a; return 1; }
static uint32_t d3d_adapterid(Ctx *c, uint32_t *a) {
    uint8_t z[1100] = {0}; wrbuf(c, a[3], z, sizeof z);
    wrbuf(c, a[3], "nvd3dum.dll", 12); wrbuf(c, a[3] + 512, "Emulated Direct3D 9 GPU", 24); wrbuf(c, a[3] + 1024, "\\\\.\\DISPLAY1", 13);
    wr32(c, a[3] + 1056, 0x00090000 | 0x0); wr32(c, a[3] + 1060, 0x000f1234); wr32(c, a[3] + 1064, 0x10de); wr32(c, a[3] + 1068, 0x0640); wr32(c, a[3] + 1076, 1); return 0;
}
static const uint32_t g_modes[][2] = {{1920, 1080}, {1600, 900}, {1280, 720}, {1024, 768}, {800, 600}};
static uint32_t d3d_modecount(Ctx *c, uint32_t *a) { (void)c; return a[2] == 22 ? 5 : 0; }
static uint32_t d3d_enummodes(Ctx *c, uint32_t *a) { if (a[3] >= 5 || a[2] != 22) return 0x8876086c; wr32(c, a[4], g_modes[a[3]][0]); wr32(c, a[4] + 4, g_modes[a[3]][1]); wr32(c, a[4] + 8, g_refresh()); wr32(c, a[4] + 12, 22); return 0; }
static uint32_t d3d_displaymode(Ctx *c, uint32_t *a) { wr32(c, a[2], 1920); wr32(c, a[2] + 4, 1080); wr32(c, a[2] + 8, g_refresh()); wr32(c, a[2] + 12, 22); return 0; }
static uint32_t d3d_msaa(Ctx *c, uint32_t *a) { if (a[6]) wr32(c, a[6], 1); return 0; }
static uint32_t d3d_getcaps(Ctx *c, uint32_t *a) { return d3d_getcaps_fill(c, a[3]); }
static uint32_t d3d_monitor(Ctx *c, uint32_t *a) { (void)c; (void)a; return 0x20001; }
static uint32_t d3d_createdevice(Ctx *c, uint32_t *a) {
    memset(&st, 0, sizeof st); rdbuf(c, a[5], st.pp, 56);
    { const char *bb = getenv("BLUR_BB"); unsigned bw, bh; if (bb && sscanf(bb, "%ux%u", &bw, &bh) == 2) { st.pp[0] = bw; st.pp[1] = bh; wr32(c, a[5], bw); wr32(c, a[5] + 4, bh); fprintf(stderr, "[d3d9] BLUR_BB: backbuffer forced to %ux%u\n", bw, bh); } }   // experiment: does the game follow a larger backbuffer?
    st.rs[7] = 1; st.rs[14] = 1; st.rs[23] = 4; st.rs[22] = 3; st.rs[19] = 2; st.rs[20] = 1; st.rs[168] = st.rs[190] = st.rs[191] = st.rs[192] = 15; st.rs[171] = 1; st.rs[209] = 1;
    st.rs[207] = 2; st.rs[208] = 1; st.rs[56] = 8; st.rs[53] = st.rs[54] = st.rs[55] = 1; st.rs[58] = st.rs[59] = 0xffffffffu; st.rs[25] = 8; st.rs[193] = 0xffffffffu;
    st.rs[186] = st.rs[187] = st.rs[188] = 1; st.rs[189] = 8;
    for (int i = 0; i < 32; i++) { st.ss[i][1] = st.ss[i][2] = st.ss[i][3] = 1; st.ss[i][5] = st.ss[i][6] = 1; st.ss[i][10] = 1; }
    LOG("CreateDevice adapter=%u type=%u behavior=%x  backbuffer %ux%u fmt=%u windowed=%u depth=%u/%u vsync=%x\n", a[1], a[2], a[4], st.pp[0], st.pp[1], st.pp[2], st.pp[8], st.pp[9], st.pp[10], st.pp[13]);
    uint32_t w = st.pp[0] ? st.pp[0] : 1280, h = st.pp[1] ? st.pp[1] : 720, fmt = st.pp[2] ? st.pp[2] : 22;
    st.backbuf = new_surface(c, w, h, fmt, 0, 1, 0, 0, 0, 0); GFXW(G_SURF_CREATE, oid(O(c, st.backbuf)), w, h, fmt, 0, 0);
    st.vp[0] = 0; st.vp[1] = 0; st.vp[2] = w; st.vp[3] = h; ((float *)st.vp)[4] = 0.0f; ((float *)st.vp)[5] = 1.0f;
    Obj *o; uint32_t g = make_obj(c, "IDirect3DDevice9", dev_m, NM(dev_m), K_DEVICE, &o); st.device = g; wr32(c, a[6], g);
    if (st.pp[9]) { st.ds = new_surface(c, w, h, st.pp[10], 0, 2, 0, 0, 0, 0); GFXW(G_SURF_CREATE, oid(O(c, st.ds)), w, h, st.pp[10], 2, 0); uint32_t dw[4]; surf_desc_words(c, st.ds, dw); GFXW(G_SET_DS, dw[0], dw[1], dw[2], dw[3]); }
    return 0;
}
static const ComMethod d3d_m[] = {
    IU, Z("RegisterSoftwareDevice", 2), { "GetAdapterCount", 1, d3d_adaptercount }, { "GetAdapterIdentifier", 4, d3d_adapterid }, { "GetAdapterModeCount", 3, d3d_modecount },
    { "EnumAdapterModes", 5, d3d_enummodes }, { "GetAdapterDisplayMode", 3, d3d_displaymode }, Z("CheckDeviceType", 6), Z("CheckDeviceFormat", 7),
    { "CheckDeviceMultiSampleType", 7, d3d_msaa }, Z("CheckDepthStencilMatch", 6), Z("CheckDeviceFormatConversion", 5), { "GetDeviceCaps", 4, d3d_getcaps },
    { "GetAdapterMonitor", 2, d3d_monitor }, { "CreateDevice", 7, d3d_createdevice },
};
uint32_t com_create_direct3d9(Ctx *c) { uint32_t g = make_obj(c, "IDirect3D9", d3d_m, NM(d3d_m), K_D3D, NULL); LOG("Direct3DCreate9 -> %08x\n", g); return g; }
// ---- destruction: an object whose refcount hits 0 is destroyed ZOMBIE_FRAMES frames later (the real device also keeps references for bound textures/streams, which we do
// not model; the delay stands in for that) and only if it is still at 0 and not currently bound. BLUR_NODESTROY=1 disables it.
#define ZOMBIE_FRAMES 240
static struct { uint32_t g; uint64_t due; } *zq; static size_t zq_n, zq_cap, zq_head; static uint64_t n_destroyed, bytes_freed;
static uint32_t d3d_zero(Ctx *c, uint32_t g) {
    static int off = -1; if (off < 0) off = getenv("BLUR_NODESTROY") != NULL; if (off) return 0;
    uint32_t i = rd32(c, g + 8); if (!i || i >= (uint32_t)nobjs || objs[i].guest != g) return 0;
    Kind k = objs[i].kind; if (k != K_TEXTURE && k != K_CUBE && k != K_VOLUME && k != K_VB && k != K_IB && k != K_VSHADER && k != K_PSHADER && k != K_VDECL && !(k == K_SURFACE && !objs[i].parent && objs[i].own)) return 0;
    if (zq_n == zq_cap) { if (zq_head > zq_cap / 2) { memmove(zq, zq + zq_head, (zq_n - zq_head) * sizeof *zq); zq_n -= zq_head; zq_head = 0; } else { zq_cap = zq_cap ? zq_cap * 2 : 4096; zq = realloc(zq, zq_cap * sizeof *zq); } }
    zq[zq_n].g = g; zq[zq_n++].due = st.frames + ZOMBIE_FRAMES; return 0;
}
static int is_bound(uint32_t g) { if (g == st.rt || g == st.ds || g == st.backbuf || g == st.vs || g == st.ps || g == st.ib) return 1; for (int i = 0; i < 16; i++) if (st.tex[i] == g || st.vb[i] == g) return 1; return g == st.vdecl; }
// BLUR_DESTROYDBG=1: destroyed D3D objects are poisoned (0xdd) and leaked instead of freed, so a game use-after-Release shows up as a crash instead of silent reuse
static void d3d_free(Ctx *c, uint32_t p) {
    static int dd = -1; if (dd < 0) dd = getenv("BLUR_DESTROYDBG") != NULL;
    if (!dd) { guest_free(c, p); return; }
    uint32_t cap = guest_blockcap(c, p); if (!cap) return; uint8_t *z = malloc(cap); memset(z, 0xdd, cap); wrbuf(c, p, z, cap); free(z);
}
static void d3d_reap(Ctx *c) {
    while (zq_head < zq_n && zq[zq_head].due <= st.frames) {
        uint32_t g = zq[zq_head++].g, i = rd32(c, g + 8);
        if (!i || i >= (uint32_t)nobjs || objs[i].guest != g || rd32(c, g + 4) != 0 || is_bound(g)) continue;       // resurrected (AddRef from 0 never happens in practice) or still bound
        Obj *o = &objs[i];
        { int tk = (o->kind == K_TEXTURE || o->kind == K_CUBE || o->kind == K_VOLUME) && !(o->usage & 3), bk = o->kind == K_VB || o->kind == K_IB;      // tell the renderer to free its GL object (it never did: every texture/buffer ever created stayed in WebGL memory)
          if (tk || bk) GFXW(G_DESTROY, (uint32_t)bk, (uint32_t)i); }
        if (o->sc) { for (int k = 0; k < 96; k++) if (o->sc[k]) { uint32_t sg = o->sc[k], si = rd32(c, sg + 8); if (si && si < (uint32_t)nobjs) memset(&objs[si], 0, sizeof objs[si]); wr32(c, sg + 8, 0); d3d_free(c, sg); } free(o->sc); }
        if (o->own && o->data) { bytes_freed += o->len; d3d_free(c, o->data); }
        if (o->code) d3d_free(c, o->code);
        free(o->shadow); free(o->known);
        wr32(c, g + 8, 0); d3d_free(c, g); memset(o, 0, sizeof *o); n_destroyed++;
    }
}
// A viewer attached (first load or a reload): it holds no resources. Re-emit the creation message of every live object (creation order = id order), then re-upload texture and
// buffer contents from guest memory (render targets are redrawn every frame). State (render states, bindings) is re-sent lazily by sent_sync().
void d3d9_replay_resources(void) {
    Ctx *c = d3d_ctx; if (!c) return; uint64_t t0 = real_us(); int nt = 0, nb = 0, ns = 0;
    for (int i = 1; i < nobjs; i++) { Obj *o = &objs[i]; uint32_t id = (uint32_t)i;
        switch (o->kind) {
        case K_TEXTURE: GFXW(G_TEX_CREATE, id, 0, o->w, o->h, 1, o->nlev, o->fmt, o->usage); nt++; break;
        case K_VOLUME: GFXW(G_TEX_CREATE, id, 2, o->w, o->h, o->d, o->nlev, o->fmt, o->usage); nt++; break;
        case K_CUBE: GFXW(G_TEX_CREATE, id, 1, o->w, o->h, 1, o->nlev, o->fmt, o->usage); nt++; break;
        case K_VB: GFXW(G_BUF_CREATE, id, 0, o->len, 0, o->usage); nb++; break;
        case K_IB: GFXW(G_BUF_CREATE, id, 1, o->len, o->fmt, o->usage); nb++; break;
        case K_SURFACE: if (!o->parent) { if (o->guest == st.backbuf) GFXW(G_SURF_CREATE, id, o->w, o->h, o->fmt, 0, 0); else if (o->usage == 1 || o->usage == 2) GFXW(G_SURF_CREATE, id, o->w, o->h, o->fmt, o->usage, o->ms); } break;
        case K_VDECL: send_vdecl(c, o); break;
        case K_VSHADER: send_shader(c, o, 0); ns++; break;
        case K_PSHADER: send_shader(c, o, 1); ns++; break;
        default: break; } }
    for (int i = 1; i < nobjs; i++) { Obj *o = &objs[i];
        if ((o->kind == K_TEXTURE || o->kind == K_CUBE || o->kind == K_VOLUME) && !(o->usage & 3) && o->data && o->nlev) { uint32_t faces = o->kind == K_CUBE ? 6 : 1; for (uint32_t f = 0; f < faces; f++) for (uint32_t l = 0; l < o->nlev; l++) send_level(c, o, f, l); }
        else if ((o->kind == K_VB || o->kind == K_IB) && o->data && o->len) send_buf_raw(o, 0, o->len); }
    fprintf(stderr, "[net] viewer attached: replayed %d textures, %d buffers, %d shaders in %.0f ms\n", nt, nb, ns, (real_us() - t0) / 1e3);
}
// BLUR_HEAPSITES: why are texture objects still alive? histogram of live K_TEXTURE objects by COM refcount, whether currently bound, and zombie-queue membership
void d3d9_leak_report(void) {
    Ctx *c = d3d_ctx; if (!c) return; static uint8_t *inz; static size_t inz_cap;
    if (inz_cap < (size_t)nobjs) { inz = realloc(inz, nobjs + 4096); inz_cap = nobjs + 4096; } memset(inz, 0, inz_cap);
    for (size_t k = zq_head; k < zq_n; k++) { uint32_t i = rd32(c, zq[k].g + 8); if (i && i < (uint32_t)nobjs) inz[i] = 1; }
    int rc_hist[6] = {0}, bound = 0, inzq = 0, tot = 0; uint64_t bytes_rc0 = 0, bytes_rc1 = 0, bytes_rcn = 0;
    for (int i = 1; i < nobjs; i++) { Obj *o = &objs[i]; if (o->kind != K_TEXTURE || !o->guest) continue; uint32_t rc = rd32(c, o->guest + 4); tot++; rc_hist[rc > 4 ? 5 : rc]++; if (is_bound(o->guest)) bound++; if (inz[i]) inzq++;
        if (rc == 0) bytes_rc0 += o->len; else if (rc == 1) bytes_rc1 += o->len; else bytes_rcn += o->len; }
    { static struct { uint32_t pool, usage, fmt; int n; double mb; int rc1; } g[64]; int ng = 0;
      for (int i = 1; i < nobjs; i++) { Obj *o = &objs[i]; if (o->kind != K_TEXTURE || !o->guest) continue; uint32_t rc = rd32(c, o->guest + 4); int k = 0; for (; k < ng; k++) if (g[k].pool == o->pool && g[k].usage == o->usage && g[k].fmt == o->fmt) break;
        if (k == ng) { if (ng >= 64) continue; g[ng].pool = o->pool; g[ng].usage = o->usage; g[ng].fmt = o->fmt; g[ng].n = 0; g[ng].mb = 0; g[ng].rc1 = 0; ng++; } g[k].n++; g[k].mb += o->len / 1048576.0; if (rc == 1) g[k].rc1++; }
      fprintf(stderr, "[leak] live textures by pool/usage/format (n, MB, rc==1):");
      for (int r = 0; r < 8; r++) { int b = -1; double bm = 0; for (int k = 0; k < ng; k++) if (g[k].mb > bm) { bm = g[k].mb; b = k; } if (b < 0) break; fprintf(stderr, " | pool%u usage%x fmt%u: %d, %.0f MB, rc1 %d", g[b].pool, g[b].usage, g[b].fmt, g[b].n, g[b].mb, g[b].rc1); g[b].mb = 0; }
      fprintf(stderr, "\n"); }
    { int all = 0, withg = 0, ownd = 0, idbad = 0, noguest_data = 0; uint64_t dbytes = 0;
      for (int i = 1; i < nobjs; i++) { Obj *o = &objs[i]; if (o->kind != K_TEXTURE) continue; all++; if (o->guest) { withg++; if (rd32(c, o->guest + 8) != (uint32_t)i) idbad++; } else if (o->data) noguest_data++; if (o->own && o->data) { ownd++; dbytes += o->len; } }
      fprintf(stderr, "[leak] K_TEXTURE entries: %d total, %d with COM block, %d own data (%.0f MB), %d COM blocks whose id slot mismatches, %d without COM block but with data\n", all, withg, ownd, dbytes / 1048576.0, idbad, noguest_data); }
    fprintf(stderr, "[leak] live textures %d: refcount 0:%d 1:%d 2:%d 3:%d 4:%d >4:%d | bound now %d, in zombie queue %d | guest bytes rc0 %.0f MB, rc1 %.0f MB, rc>1 %.0f MB | zombie queue pending %zu, destroyed so far %llu\n",
            tot, rc_hist[0], rc_hist[1], rc_hist[2], rc_hist[3], rc_hist[4], rc_hist[5], bound, inzq, bytes_rc0 / 1048576.0, bytes_rc1 / 1048576.0, bytes_rcn / 1048576.0, zq_n - zq_head, (unsigned long long)n_destroyed);
}
// BLUR_HEAPSITES: is this live guest-heap block referenced by any D3D object the host still tracks (COM object, data, shader code, cached level surfaces)?
static uint32_t *own_tab; static uint8_t *own_tag; static size_t own_n, own_cap;
static void own_add(uint32_t p, uint8_t tag) { if (!p) return; size_t h = (p * 2654435761u) & (own_cap - 1); while (own_tab[h]) { if (own_tab[h] == p) return; h = (h + 1) & (own_cap - 1); } own_tab[h] = p; own_tag[h] = tag; own_n++; }
void d3d9_owner_snapshot(void) {
    size_t need = 16; while (need < (size_t)nobjs * 8) need <<= 1; if (need != own_cap) { free(own_tab); free(own_tag); own_cap = need; own_tab = calloc(own_cap, 4); own_tag = calloc(own_cap, 1); } else memset(own_tab, 0, own_cap * 4); own_n = 0;
    for (int i = 1; i < nobjs; i++) { Obj *o = &objs[i]; if (!o->kind) continue; own_add(o->guest, (uint8_t)(o->kind << 2 | 0)); if (o->own) own_add(o->data, (uint8_t)(o->kind << 2 | 1)); own_add(o->code, (uint8_t)(o->kind << 2 | 2)); if (o->sc) for (int k = 0; k < 96; k++) own_add(o->sc[k], (uint8_t)(o->kind << 2 | 3)); }
}
int d3d9_owns_block(uint32_t p) { if (!own_tab) return 1; size_t h = (p * 2654435761u) & (own_cap - 1); while (own_tab[h]) { if (own_tab[h] == p) return 1 + own_tag[h]; h = (h + 1) & (own_cap - 1); } return 0; }
void d3d9_report(Ctx *c) {
    objstat_report(); fprintf(stderr, "[d3d9] %llu objects destroyed (%llu KB of data freed), %zu zombies pending\n", (unsigned long long)n_destroyed, (unsigned long long)(bytes_freed >> 10), zq_n - zq_head); (void)c; fprintf(stderr, "[d3d9] frames=%llu draws=%llu clears=%llu scenes=%llu objects=%d\n", (unsigned long long)st.frames, (unsigned long long)st.draws, (unsigned long long)st.clears, (unsigned long long)st.scenes, nobjs - 1); }

// shared by the D3DX constant table: write constants straight into the device state (set: 0=bool, 1=int4, 2=float4)
void d3d9_set_const(Ctx *c, int ps, int set, uint32_t reg, const void *data, uint32_t nvec) {
    if (set == 2) { uint32_t lim = ps ? 224 : 256; if (reg < lim) const_f(ps, reg, nvec < lim - reg ? nvec : lim - reg, (const float *)data); return; }
    else if (set == 1) { for (uint32_t i = 0; i < nvec && reg + i < 16; i++) memcpy(ps ? st.psi[reg + i] : st.vsi[reg + i], (const int32_t *)data + 4 * i, 16); }
    else { for (uint32_t i = 0; i < nvec && reg + i < 16; i++) (ps ? st.psb : st.vsb)[reg + i] = ((const uint32_t *)data)[i]; }
    { uint32_t lim = set == 2 ? (ps ? 224u : 256u) : 16u, n = reg < lim ? (nvec < lim - reg ? nvec : lim - reg) : 0;      // forward to the renderer too (constant-table writes used to stay host-side only)
      if (n) { uint32_t w[2] = {reg, n};
        if (set == 2) gfx_msg(ps ? G_SET_PSC : G_SET_VSC, w, 2, ps ? (void *)st.psf[reg] : (void *)st.vsf[reg], 16 * n);
        else if (set == 1) gfx_msg(ps ? G_SET_PSCI : G_SET_VSCI, w, 2, ps ? (void *)st.psi[reg] : (void *)st.vsi[reg], 16 * n);
        else gfx_msg(ps ? G_SET_PSCB : G_SET_VSCB, w, 2, ps ? (void *)&st.psb[reg] : (void *)&st.vsb[reg], 4 * n); } }
    LOG("const %s set=%d reg=%u n=%u\n", ps ? "ps" : "vs", set, reg, nvec); (void)c;
}
void d3d9_get_const_f(int ps, uint32_t reg, float *out) { if (reg < (ps ? 224u : 256u)) memcpy(out, ps ? st.psf[reg] : st.vsf[reg], 16); }

// ---- snapshot (see snap_ctx.c): the D3D9 object table must still be empty at the snapshot point (the game has not created a device at the OEP)
int d3d9_snap_busy(void) { return nobjs - 1 + (int)st.frames + (int)st.draws; }

// post-mortem helper (rt.c rt_missing): describe the d3d9 object behind a guest COM pointer
void d3d9_dbg_obj(Ctx *c, uint32_t p, const char *tag) {
    if (p < 0x20000000u || p >= VIRT_BASE) return;
    uint32_t i = rd32(c, p + 8); if (!i || i >= (uint32_t)nobjs) { fprintf(stderr, "[obj %s %08x] no object (idx=%u)\n", tag, p, i); return; }
    Obj *o = &objs[i];
    fprintf(stderr, "[obj %s %08x] #%u kind=%d %ux%u lv=%u nlev=%u fmt=%08x usage=%x pool=%u len=%u data=%08x parent=%u level=%u lev_off=", tag, p, i, o->kind, o->w, o->h, o->levels, o->nlev, o->fmt, o->usage, o->pool, o->len, o->data, o->parent, o->level);
    for (uint32_t l = 0; l < o->nlev && l < 16; l++) fprintf(stderr, "%x/%u ", o->lev_off[l], o->lev_pitch[l]);
    fprintf(stderr, "\n");
}

// BLUR_TEXAUDIT=1: how many textures (>=64 wide) still hold only zeros? Printed every 1500 frames and at exit; also the ids the UI / car-paint investigation cares about.
void d3d9_texaudit(Ctx *c, const char *when) {
    static int on = -1; if (on < 0) on = getenv("BLUR_TEXAUDIT") != NULL; if (!on) return;
    static const uint32_t watch[] = { 1320, 1348, 1350, 1382, 1383, 1384, 5450, 5636, 5638, 5641 };
    uint32_t nz = 0, z = 0, zb = 0; char zl[300] = "";
    for (int i = 1; i < nobjs; i++) { Obj *o = &objs[i]; if ((o->kind != K_TEXTURE && o->kind != K_CUBE) || !o->data || o->w < 64) continue; uint32_t n = o->len > 65536 ? 65536 : o->len, nonzero = 0;
        for (uint32_t k = 0; k < n && !nonzero; k += 4) nonzero = rd32(c, o->data + k) != 0;
        if (nonzero) nz++; else { z++; zb += o->len; if (strlen(zl) < 250 && (o->pool == 1 || o->pool == 2) && o->w >= 256) { char t[40]; snprintf(t, sizeof t, " #%d(%ux%u)", i, o->w, o->h); strcat(zl, t); } } }
    { struct { uint32_t fmt, pool; unsigned f, z; } h[24]; int nh = 0; memset(h, 0, sizeof h);
      for (int i = 1; i < nobjs; i++) { Obj *o = &objs[i]; if (o->kind != K_TEXTURE || !o->data) continue; uint32_t n = o->len > 65536 ? 65536 : o->len, nonzero = 0; for (uint32_t k = 0; k < n && !nonzero; k += 4) nonzero = rd32(c, o->data + k) != 0;
        int j; for (j = 0; j < nh; j++) if (h[j].fmt == o->fmt && h[j].pool == o->pool) break; if (j == nh && nh < 24) { h[nh].fmt = o->fmt; h[nh].pool = o->pool; nh++; } if (j < 24) { if (nonzero) h[j].f++; else h[j].z++; } }
      fprintf(stderr, "[texaudit] by format/pool (filled/zero):"); for (int j = 0; j < nh; j++) fprintf(stderr, " f%08x/p%u=%u/%u", h[j].fmt, h[j].pool, h[j].f, h[j].z); fprintf(stderr, "\n"); }
    { struct { uint32_t site, thr; unsigned f, z; } h[32]; int nh = 0; memset(h, 0, sizeof h);
      for (int i = 1; i < nobjs; i++) { Obj *o = &objs[i]; if (o->kind != K_TEXTURE || !o->data) continue; uint32_t n = o->len > 65536 ? 65536 : o->len, nonzero = 0; for (uint32_t k = 0; k < n && !nonzero; k += 4) nonzero = rd32(c, o->data + k) != 0;
        int j; for (j = 0; j < nh; j++) if (h[j].site == o->site && h[j].thr == o->cthr) break; if (j == nh && nh < 32) { h[nh].site = o->site; h[nh].thr = o->cthr; nh++; } if (j < 32) { if (nonzero) h[j].f++; else h[j].z++; } }
      fprintf(stderr, "[texaudit] by creation site (site@thread filled/zero):"); for (int j = 0; j < nh; j++) fprintf(stderr, " %x@t%u=%u/%u", h[j].site, h[j].thr, h[j].f, h[j].z); fprintf(stderr, "\n"); }
    fprintf(stderr, "[texaudit] %s frame=%llu: %u textures filled, %u all-zero (%u KB); big zero:%s\n[texaudit] watch:", when, (unsigned long long)st.frames, nz, z, zb >> 10, zl);
    for (unsigned k = 0; k < sizeof watch / sizeof *watch; k++) { uint32_t i = watch[k]; if (i >= (uint32_t)nobjs) { fprintf(stderr, " #%u=n/a", i); continue; } Obj *o = &objs[i];
        uint32_t n = o->len > 65536 ? 65536 : o->len, nonzero = 0; for (uint32_t q = 0; o->data && q < n && !nonzero; q += 4) nonzero = rd32(c, o->data + q) != 0; fprintf(stderr, " #%u(%ux%u)=%s", i, o->w, o->h, nonzero ? "data" : "ZERO"); }
    fprintf(stderr, "\n");
}
// ---- BLUR_TEXREF=<frame>[,<frame>...]: texture data-integrity audit. At those frames the host re-sends the CURRENT guest-memory content of every texture level/face
// (message 85: id, face, level, w, h, d, fmt, pitch, usage, kind, nlev + raw bytes) followed by 86 (frame); the renderer decodes it with an independent reference decoder,
// reads the real GL texture back and compares (renderer.js audit()). Default-pool textures (never locked; filled by UpdateTexture) have no guest data of their own.
static void texref_maybe(Ctx *c) {
    static int on = -1; static long fr[32]; static int nfr;
    if (on < 0) { on = 0; const char *e = getenv("BLUR_TEXREF"); if (e) { on = 1; while (*e && nfr < 32) { fr[nfr++] = strtol(e, (char **)&e, 10); if (*e == ',') e++; else break; } } }
    if (!on || !gfx_on()) return; int hit = 0; for (int i = 0; i < nfr; i++) if ((long)st.frames == fr[i]) hit = 1; if (!hit) return;
    unsigned nt = 0, nl = 0;
    for (int i = 1; i < nobjs; i++) { Obj *o = &objs[i]; if ((o->kind != K_TEXTURE && o->kind != K_CUBE && o->kind != K_VOLUME) || !o->data || !o->nlev || (o->usage & 3)) continue;
        uint32_t faces = o->kind == K_CUBE ? 6 : 1; nt++;
        for (uint32_t f = 0; f < faces; f++) for (uint32_t l = 0; l < o->nlev; l++) {
            uint32_t lw = pow2lv(o->w, l), lh = pow2lv(o->h, l), ld = o->kind == K_VOLUME ? pow2lv(o->d, l) : 1, n = size_of(o->fmt, lw, lh) * ld, base = o->data + o->lev_off[l] + (o->kind == K_CUBE ? f * (o->len / 6) : 0);
            uint8_t *tmp = malloc(n + 1); rdbuf(c, base, tmp, n); uint32_t w[11] = {oid(o), f, l, lw, lh, ld, o->fmt, o->lev_pitch[l], o->usage | (o->pool << 8), (uint32_t)o->kind, o->nlev};
            gfx_msg(85, w, 11, tmp, n); free(tmp); nl++; } }
    GFXW(86, (uint32_t)st.frames, nt, nl); fprintf(stderr, "[texref] frame %llu: sent %u textures, %u levels\n", (unsigned long long)st.frames, nt, nl);
}

// BLUR_GFXSTAT companion (called from gfx.c every N frames): effect of the source-side filters
void d3d9_gfxstat(uint64_t frames) {
    static uint64_t in0, out0, sk0, sn0; if (!frames) return;
    fprintf(stderr, "[gfxstat] filters/frame: const regs skipped %.0f sent %.0f | buffer bytes locked %.1fK sent %.1fK\n", (double)(gs_skipped - sk0) / frames, (double)(gs_sent - sn0) / frames, (bu_in - in0) / 1024.0 / frames, (bu_out - out0) / 1024.0 / frames);
    { static uint64_t rf0; fprintf(stderr, "[gfxstat] present-time texture refresh (volume hash + suspects) %.3f ms/frame\n", (rf_us - rf0) / 1000.0 / frames); rf0 = rf_us; }
    if (getenv("BLUR_BUFLOG")) {      // busiest uploaded buffers in this window: id usage pool kind len | uploads, bytes locked, bytes sent, last lock flags
        for (int k = 0; k < 10; k++) { int best = -1; for (int i = 1; i < nobjs; i++) if (objs[i].st_out && (best < 0 || objs[i].st_out > objs[best].st_out)) best = i; if (best < 0) break; Obj *o = &objs[best];
            fprintf(stderr, "[buflog]  #%d %s usage=%x pool=%u len=%u | %.1f uploads/f, in %.1fK/f out %.1fK/f flags=%x nodiff=%u\n", best, o->kind == K_VB ? "VB" : "IB", o->usage, o->pool, o->len, (double)o->st_cnt / frames, o->st_in / 1024.0 / frames, o->st_out / 1024.0 / frames, o->lock_flags, o->nodiff); o->st_out = 0; }
        for (int i = 1; i < nobjs; i++) objs[i].st_cnt = objs[i].st_in = objs[i].st_out = 0; }
    in0 = bu_in; out0 = bu_out; sk0 = gs_skipped; sn0 = gs_sent;
}
