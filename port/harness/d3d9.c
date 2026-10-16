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
    uint32_t data, nlev; uint32_t lev_off[16], lev_pitch[16];     // pixel/vertex storage in guest memory
    uint32_t code, codelen;                                       // shader bytecode / vertex declaration copy
    uint32_t parent;                                              // surface -> texture
    uint32_t level, face, lock_off, lock_len, lock_ro;
} Obj;
#define MAXOBJ 16384
static Obj objs[MAXOBJ]; static int nobjs = 1;

// device state
static struct {
    uint32_t rs[256], ss[32][16], tss[8][33];
    float xform[512][16]; uint32_t vp[6];
    uint32_t tex[16], vb[16], vb_off[16], vb_stride[16], ib, vdecl, vs, ps, fvf, rt, ds;
    float vsf[256][4], psf[224][4]; int32_t vsi[16][4], psi[16][4]; uint32_t vsb[16], psb[16];
    uint32_t pp[16];                                              // presentation parameters
    uint32_t backbuf, device;
    uint64_t frames, draws, draws_frame, clears, scenes;
    uint32_t scissor[4];
} st;

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
    uint8_t *tmp = malloc(n + 1); rdbuf(c, base, tmp, n);
    { static int ts = -1; if (ts < 0) ts = getenv("BLUR_TEXSTAT") != NULL; if (ts && level == 0 && lw >= 512) { uint32_t nz = 0; for (uint32_t i = 0; i < n; i++) nz += tmp[i] != 0; fprintf(stderr, "[tex] #%u %ux%u fmt=%u bytes=%u nonzero=%.1f%% frame=%llu\n", oid(o), lw, lh, o->fmt, n, 100.0 * nz / (n ? n : 1), (unsigned long long)st.frames); } }
    if (n >= 4096 && level == 0) { int allz = 1; for (uint32_t i = 0; i < n; i += 4) if (*(uint32_t *)(tmp + i)) { allz = 0; break; } if (allz) suspect_add(oid(o)); }
    uint32_t w[8] = {oid(o), face, level, lw, lh, ld, o->fmt, pitch}; gfx_msg(G_TEX_UPLOAD, w, 8, tmp, n); free(tmp);
}
// Textures that were uploaded all-zero are probably filled later through a lock pointer the game keeps (no second Unlock): re-send them once data shows up.
static void resend_if_filled(Ctx *c) {
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
static void send_buf(Ctx *c, Obj *o, uint32_t off, uint32_t n) {
    if (!gfx_on() || off >= o->len) return; if (off + n > o->len) n = o->len - off;
    uint8_t *tmp = malloc(n + 1); rdbuf(c, o->data + off, tmp, n); uint32_t w[2] = {oid(o), off}; gfx_msg(G_BUF_UPLOAD, w, 2, tmp, n); free(tmp);
}
// surface descriptor words: kind 0 backbuffer, 1 texture level, 2 standalone surface, 3 none
static void surf_desc_words(Ctx *c, uint32_t surf, uint32_t out[4]);

// ---- object creation ---------------------------------------------------------------------------------------------------------------
static uint32_t make_obj(Ctx *c, const char *iface, const ComMethod *m, int n, Kind k, Obj **out) {
    uint32_t g = com_make(c, iface, m, n, 8); int i = nobjs++; if (i >= MAXOBJ) { fprintf(stderr, "d3d9 object table full\n"); exit(1); }
    memset(&objs[i], 0, sizeof objs[i]); objs[i].kind = k; objs[i].guest = g; wr32(c, g + 8, (uint32_t)i); if (out) *out = &objs[i]; return g;
}
#define Z(n, na) { n, na, com_ok }
#define IU Z("QueryInterface", 3), Z("AddRef", 1), Z("Release", 1)
#define RES Z("GetDevice", 2), Z("SetPrivateData", 5), Z("GetPrivateData", 4), Z("FreePrivateData", 2), Z("SetPriority", 2), Z("GetPriority", 1), Z("PreLoad", 1), Z("GetType", 1)
#define NM(m) (int)(sizeof(m) / sizeof((m)[0]))

// buffers (VB/IB share a layout)
static uint32_t buf_lock(Ctx *c, uint32_t *a) { Obj *o = O(c, a[0]); o->lock_off = a[1]; o->lock_len = a[2]; o->lock_ro = (a[4] & 0x10) != 0; wr32(c, a[3], o->data + a[1]); return 0; }
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
    o->lev_pitch[0] = pitch ? pitch : pitch_of(fmt, w); o->data = data ? data : guest_alloc(c, size_of(fmt, w, h) + 16); return g;
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
    if (getenv("BLUR_HOSTWATCH") && (long)(o - objs) == atol(getenv("BLUR_HOSTWATCH")) && a[1] == 0) { g_hw_lo = o->data + o->lev_off[0]; g_hw_hi = g_hw_lo + 0x40000; fprintf(stderr, "[hostw] watching %08x-%08x\n", g_hw_lo, g_hw_hi); }
    { static int armed; const char *e = getenv("BLUR_EMUWATCH"); if (e && !g_recomp && !armed && (long)(o - objs) == atol(e) && a[1] == 0) { armed = 1; uc_hook h; uint32_t lo = o->data + o->lev_off[0]; uc_hook_add(c->uc, &h, UC_HOOK_MEM_WRITE, (void *)emu_wcb, NULL, lo, lo + 0x3ffff); fprintf(stderr, "[emuw] armed %08x..\n", lo); } } if (texw((uint32_t)(o - objs))) { fprintf(stderr, "[texw] lock #%ld lvl=%u flags=%x ret=%08x frame=%llu data=%08x callers:", (long)(o - objs), a[1], a[4], g_api_ret, (unsigned long long)st.frames, o->data + o->lev_off[a[1] < o->nlev ? a[1] : 0]); extern uint32_t g_api_esp; for (int k = 1; k < 80; k++) { uint32_t w = rd32(c, g_api_esp + 4 * k); if (w >= 0x401000 && w < 0xe40000) fprintf(stderr, " %08x", w); } fprintf(stderr, "\n"); } uint32_t l = a[1] < o->nlev ? a[1] : 0; wr32(c, a[2], o->lev_pitch[l]); wr32(c, a[2] + 4, o->data + o->lev_off[l]); return 0; }
static uint32_t tex_unlock(Ctx *c, uint32_t *a) { canary_check(c, O(c, a[0]), "UnlockRect"); if (texw((uint32_t)(O(c, a[0]) - objs))) fprintf(stderr, "[texw] unlock #%ld lvl=%u ret=%08x frame=%llu\n", (long)(O(c, a[0]) - objs), a[1], g_api_ret, (unsigned long long)st.frames); send_level(c, O(c, a[0]), 0, a[1]); return 0; }
static uint32_t tex_getsurf(Ctx *c, uint32_t *a) {
    Obj *o = O(c, a[0]); uint32_t l = a[1] < o->nlev ? a[1] : 0, w = o->w >> l ? o->w >> l : 1, h = o->h >> l ? o->h >> l : 1;
    uint32_t g = new_surface(c, w, h, o->fmt, o->pool, o->usage, 0, o->data + o->lev_off[l], o->lev_pitch[l], (uint32_t)(o - objs)); O(c, g)->level = l; wr32(c, a[2], g); return 0;
}
static uint32_t tex_leveldesc(Ctx *c, uint32_t *a) {
    Obj *o = O(c, a[0]); uint32_t l = a[1]; wr32(c, a[2], o->fmt); wr32(c, a[2] + 4, 3); wr32(c, a[2] + 8, o->usage); wr32(c, a[2] + 12, o->pool);
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
    for (uint32_t l = 0; l < lv; l++) { uint32_t lw = (w >> l) ? (w >> l) : 1, lh = (h >> l) ? (h >> l) : 1; o->lev_off[l] = total; o->lev_pitch[l] = pitch_of(o->fmt, lw); total += size_of(o->fmt, lw, lh); }
    o->len = total * faces; o->data = guest_alloc(c, o->len + 256); memset(G + o->data + o->len, 0xAB, 256);    // canary (BLUR_CANARY)
}

// cube/volume textures (data-only: enough for the game to create and fill them)
static uint32_t cube_lock(Ctx *c, uint32_t *a) { Obj *o = O(c, a[0]); uint32_t face = a[1], l = a[2] < o->nlev ? a[2] : 0, per = o->len / 6; wr32(c, a[3], o->lev_pitch[l]); wr32(c, a[3] + 4, o->data + face * per + o->lev_off[l]); return 0; }
static uint32_t cube_unlock(Ctx *c, uint32_t *a) { send_level(c, O(c, a[0]), a[1], a[2]); return 0; }
static uint32_t cube_surf(Ctx *c, uint32_t *a) {
    Obj *o = O(c, a[0]); uint32_t face = a[1], l = a[2] < o->nlev ? a[2] : 0, per = o->len / 6, w = o->w >> l ? o->w >> l : 1;
    uint32_t g = new_surface(c, w, w, o->fmt, o->pool, o->usage, 0, o->data + face * per + o->lev_off[l], o->lev_pitch[l], (uint32_t)(o - objs)); O(c, g)->level = l; O(c, g)->face = face; wr32(c, a[3], g); return 0;
}
static const ComMethod cube_m[] = { IU, RES, Z("SetLOD", 2), Z("GetLOD", 1), { "GetLevelCount", 1, tex_levels }, Z("SetAutoGenFilterType", 2), Z("GetAutoGenFilterType", 1),
    Z("GenerateMipSubLevels", 1), Z("GetLevelDesc", 3), { "GetCubeMapSurface", 4, cube_surf }, { "LockRect", 6, cube_lock }, { "UnlockRect", 3, cube_unlock }, Z("AddDirtyRect", 3) };
static uint32_t vol_unlock(Ctx *c, uint32_t *a) { send_level(c, O(c, a[0]), 0, a[1]); return 0; }
static uint32_t vol_lock(Ctx *c, uint32_t *a) { Obj *o = O(c, a[0]); uint32_t l = a[1] < o->nlev ? a[1] : 0; uint32_t p = o->lev_pitch[l]; wr32(c, a[2], p); wr32(c, a[2] + 4, p * (o->h >> l ? o->h >> l : 1)); wr32(c, a[2] + 8, o->data + o->lev_off[l]); return 0; }
static const ComMethod vol_m[] = { IU, RES, Z("SetLOD", 2), Z("GetLOD", 1), { "GetLevelCount", 1, tex_levels }, Z("SetAutoGenFilterType", 2), Z("GetAutoGenFilterType", 1),
    Z("GenerateMipSubLevels", 1), Z("GetLevelDesc", 3), Z("GetVolumeLevel", 3), { "LockBox", 5, vol_lock }, { "UnlockBox", 2, vol_unlock }, Z("AddDirtyBox", 2) };

// shaders / declarations / queries / state blocks
static uint32_t obj_getcode(Ctx *c, uint32_t *a) {                 // GetFunction / GetDeclaration(this, pData, pSizeOfData)
    Obj *o = O(c, a[0]);
    if (!a[1]) { wr32(c, a[2], o->codelen); return 0; }
    uint32_t n = rd32(c, a[2]); if (n > o->codelen) n = o->codelen;
    uint8_t *b = malloc(n + 1); rdbuf(c, o->code, b, n); wrbuf(c, a[1], b, n); free(b); wr32(c, a[2], n); return 0;
}
static const ComMethod shader_m[] = { IU, Z("GetDevice", 2), { "GetFunction", 3, obj_getcode } };
static const ComMethod vdecl_m[] = { IU, Z("GetDevice", 2), { "GetDeclaration", 3, obj_getcode } };
static uint32_t query_getdata(Ctx *c, uint32_t *a) { if (a[1] && a[2] >= 4) wr32(c, a[1], 0); return 0; }   // occlusion count 0 / "event done"
static const ComMethod query_m[] = { IU, Z("GetDevice", 2), Z("GetType", 1), Z("GetDataSize", 1), Z("Issue", 2), { "GetData", 4, query_getdata } };
static const ComMethod sb_m[] = { IU, Z("GetDevice", 2), Z("Capture", 1), Z("Apply", 1) };
static uint32_t sc_present(Ctx *c, uint32_t *a) { (void)c; (void)a; return 0; }
static uint32_t sc_backbuffer(Ctx *c, uint32_t *a) { wr32(c, a[3], st.backbuf); return 0; }
static const ComMethod sc_m[] = { IU, { "Present", 6, sc_present }, Z("GetFrontBufferData", 2), { "GetBackBuffer", 4, sc_backbuffer }, Z("GetRasterStatus", 2), Z("GetDisplayMode", 2), Z("GetDevice", 2), Z("GetPresentParameters", 2) };

static uint32_t copy_code(Ctx *c, uint32_t src, uint32_t *len, int is_decl) {
    uint32_t n = 0;
    if (is_decl) { while (rd16(c, src + n) != 0xff || rd16(c, src + n + 2) != 0x00 || 0) { n += 8; if (n > 4096) break; } n += 8; }       // D3DDECL_END: stream 0xFF
    else { n = 4; while (rd32(c, src + n - 4) != 0x0000ffff && n < (1 << 20)) n += 4; }                                                      // D3DSIO_END token
    uint32_t p = guest_alloc(c, n + 4); uint8_t *b = malloc(n); rdbuf(c, src, b, n); wrbuf(c, p, b, n); free(b); *len = n; return p;
}

// ---- device -----------------------------------------------------------------------------------------------------------------------------
#define D_CREATE(NAME, NARGS, CODE) static uint32_t NAME(Ctx *c, uint32_t *a) { CODE }
D_CREATE(d_createtexture, 9, { if (!a[1] || !a[2]) { wr32(c, a[7], 0); LOG("d_createtexture: invalid arguments -> D3DERR_INVALIDCALL\n"); return 0x8876086c; } Obj *o; uint32_t g = make_obj(c, "IDirect3DTexture9", tex_m, NM(tex_m), K_TEXTURE, &o); o->w = a[1]; o->h = a[2]; o->levels = a[3]; o->usage = a[4]; o->fmt = a[5]; o->pool = a[6]; layout_levels(c, o, 1); GFXW(G_TEX_CREATE, oid(o), 0, o->w, o->h, 1, o->nlev, o->fmt, o->usage); LOG("CreateTexture %ux%u lv=%u fmt=%u usage=%x pool=%u -> %08x\n", a[1], a[2], o->nlev, a[5], a[4], a[6], g); wr32(c, a[7], g); return 0; })
D_CREATE(d_createvolume, 10, { if (!a[1] || !a[2] || !a[3]) { wr32(c, a[8], 0); LOG("d_createvolume: invalid arguments -> D3DERR_INVALIDCALL\n"); return 0x8876086c; } Obj *o; uint32_t g = make_obj(c, "IDirect3DVolumeTexture9", vol_m, NM(vol_m), K_VOLUME, &o); o->w = a[1]; o->h = a[2]; o->d = a[3]; o->levels = a[4]; o->usage = a[5]; o->fmt = a[6]; o->pool = a[7]; layout_levels(c, o, a[3] ? a[3] : 1); GFXW(G_TEX_CREATE, oid(o), 2, o->w, o->h, o->d, o->nlev, o->fmt, o->usage); wr32(c, a[8], g); return 0; })
D_CREATE(d_createcube, 8, { if (!a[1]) { wr32(c, a[6], 0); LOG("d_createcube: invalid arguments -> D3DERR_INVALIDCALL\n"); return 0x8876086c; } Obj *o; uint32_t g = make_obj(c, "IDirect3DCubeTexture9", cube_m, NM(cube_m), K_CUBE, &o); o->w = o->h = a[1]; o->levels = a[2]; o->usage = a[3]; o->fmt = a[4]; o->pool = a[5]; layout_levels(c, o, 6); GFXW(G_TEX_CREATE, oid(o), 1, o->w, o->h, 1, o->nlev, o->fmt, o->usage); wr32(c, a[6], g); return 0; })
D_CREATE(d_createvb, 7, { if (!a[1]) { wr32(c, a[5], 0); LOG("d_createvb: invalid arguments -> D3DERR_INVALIDCALL\n"); return 0x8876086c; } Obj *o; uint32_t g = make_obj(c, "IDirect3DVertexBuffer9", buf_m, NM(buf_m), K_VB, &o); o->len = a[1]; o->usage = a[2]; o->fvf = a[3]; o->pool = a[4]; o->data = guest_alloc(c, a[1] + 16); GFXW(G_BUF_CREATE, oid(o), 0, a[1], 0, a[2]); LOG("CreateVertexBuffer len=%u -> %08x\n", a[1], g); wr32(c, a[5], g); return 0; })
D_CREATE(d_createib, 7, { if (!a[1]) { wr32(c, a[5], 0); LOG("d_createib: invalid arguments -> D3DERR_INVALIDCALL\n"); return 0x8876086c; } Obj *o; uint32_t g = make_obj(c, "IDirect3DIndexBuffer9", buf_m, NM(buf_m), K_IB, &o); o->len = a[1]; o->usage = a[2]; o->fmt = a[3]; o->pool = a[4]; o->data = guest_alloc(c, a[1] + 16); GFXW(G_BUF_CREATE, oid(o), 1, a[1], a[3], a[2]); LOG("CreateIndexBuffer len=%u -> %08x\n", a[1], g); wr32(c, a[5], g); return 0; })
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
    static int n; char fn[256]; snprintf(fn, sizeof fn, "%s/game_%04d_%s.bin", dir, n++, stage);
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
D_CREATE(d_createquery, 3, { uint32_t g = make_obj(c, "IDirect3DQuery9", query_m, NM(query_m), K_QUERY, NULL); wr32(c, a[2], g); return 0; })
D_CREATE(d_createsb, 3, { uint32_t g = make_obj(c, "IDirect3DStateBlock9", sb_m, NM(sb_m), K_STATEBLOCK, NULL); wr32(c, a[3], g); return 0; })
D_CREATE(d_endsb, 2, { uint32_t g = make_obj(c, "IDirect3DStateBlock9", sb_m, NM(sb_m), K_STATEBLOCK, NULL); wr32(c, a[1], g); return 0; })

static void surf_desc_words(Ctx *c, uint32_t surf, uint32_t out[4]) {
    if (!surf) { out[0] = 3; out[1] = out[2] = out[3] = 0; return; }
    if (surf == st.backbuf) { out[0] = 0; out[1] = out[2] = out[3] = 0; return; }
    Obj *o = O(c, surf);
    if (o->parent) { out[0] = 1; out[1] = oid(&objs[o->parent]); out[2] = o->level; out[3] = o->face; } else { out[0] = 2; out[1] = oid(o); out[2] = out[3] = 0; }
}
static uint32_t d_setrs(Ctx *c, uint32_t *a) { (void)c; if (a[1] < 256) { if (st.rs[a[1]] != a[2]) GFXW(G_SET_RS, a[1], a[2]); st.rs[a[1]] = a[2]; } return 0; }
static uint32_t d_getrs(Ctx *c, uint32_t *a) { wr32(c, a[2], a[1] < 256 ? st.rs[a[1]] : 0); return 0; }
static uint32_t d_setss(Ctx *c, uint32_t *a) { (void)c; if (a[1] < 32 && a[2] < 16) { if (st.ss[a[1]][a[2]] != a[3]) GFXW(G_SET_SS, a[1], a[2], a[3]); st.ss[a[1]][a[2]] = a[3]; } return 0; }
static uint32_t d_getss(Ctx *c, uint32_t *a) { wr32(c, a[3], a[1] < 32 && a[2] < 16 ? st.ss[a[1]][a[2]] : 0); return 0; }
static uint32_t d_settss(Ctx *c, uint32_t *a) { (void)c; if (a[1] < 8 && a[2] < 33) st.tss[a[1]][a[2]] = a[3]; return 0; }
static uint32_t d_gettss(Ctx *c, uint32_t *a) { wr32(c, a[3], a[1] < 8 && a[2] < 33 ? st.tss[a[1]][a[2]] : 0); return 0; }
static uint32_t d_settransform(Ctx *c, uint32_t *a) { if (a[1] < 512) rdbuf(c, a[2], st.xform[a[1]], 64); return 0; }
static uint32_t d_gettransform(Ctx *c, uint32_t *a) { if (a[1] < 512) wrbuf(c, a[2], st.xform[a[1]], 64); return 0; }
static uint32_t d_setviewport(Ctx *c, uint32_t *a) { rdbuf(c, a[1], st.vp, 24); GFXW(G_SET_VIEWPORT, st.vp[0], st.vp[1], st.vp[2], st.vp[3], st.vp[4], st.vp[5]); return 0; }
static uint32_t d_getviewport(Ctx *c, uint32_t *a) { wrbuf(c, a[1], st.vp, 24); return 0; }
static uint32_t d_settexture(Ctx *c, uint32_t *a) { if (a[1] < 16) { if (st.tex[a[1]] != a[2]) GFXW(G_SET_TEX, a[1], a[2] ? oid(O(c, a[2])) : 0); st.tex[a[1]] = a[2]; } return 0; }
static uint32_t d_gettexture(Ctx *c, uint32_t *a) { wr32(c, a[2], a[1] < 16 ? st.tex[a[1]] : 0); return 0; }
static uint32_t d_setstream(Ctx *c, uint32_t *a) { if (a[1] < 16) { st.vb[a[1]] = a[2]; st.vb_off[a[1]] = a[3]; st.vb_stride[a[1]] = a[4]; GFXW(G_SET_STREAM, a[1], a[2] ? oid(O(c, a[2])) : 0, a[3], a[4]); } return 0; }
static uint32_t d_getstream(Ctx *c, uint32_t *a) { if (a[1] < 16) { wr32(c, a[2], st.vb[a[1]]); wr32(c, a[3], st.vb_off[a[1]]); wr32(c, a[4], st.vb_stride[a[1]]); } return 0; }
static uint32_t d_setindices(Ctx *c, uint32_t *a) { st.ib = a[1]; GFXW(G_SET_INDICES, a[1] ? oid(O(c, a[1])) : 0); return 0; }
static uint32_t d_getindices(Ctx *c, uint32_t *a) { wr32(c, a[1], st.ib); return 0; }
static uint32_t d_setvdecl(Ctx *c, uint32_t *a) { st.vdecl = a[1]; GFXW(G_SET_VDECL, a[1] ? oid(O(c, a[1])) : 0); return 0; }
static uint32_t d_getvdecl(Ctx *c, uint32_t *a) { wr32(c, a[1], st.vdecl); return 0; }
static uint32_t d_setfvf(Ctx *c, uint32_t *a) { (void)c; st.fvf = a[1]; GFXW(G_SET_FVF, a[1]); return 0; }
static uint32_t d_getfvf(Ctx *c, uint32_t *a) { wr32(c, a[1], st.fvf); return 0; }
static uint32_t d_setvs(Ctx *c, uint32_t *a) { st.vs = a[1]; GFXW(G_SET_VS, a[1] ? oid(O(c, a[1])) : 0); return 0; }
static uint32_t d_getvs(Ctx *c, uint32_t *a) { wr32(c, a[1], st.vs); return 0; }
static uint32_t d_setps(Ctx *c, uint32_t *a) { st.ps = a[1]; GFXW(G_SET_PS, a[1] ? oid(O(c, a[1])) : 0); return 0; }
static uint32_t d_getps(Ctx *c, uint32_t *a) { wr32(c, a[1], st.ps); return 0; }
static uint32_t d_setvsf(Ctx *c, uint32_t *a) { if (a[1] + a[3] <= 256) { rdbuf(c, a[2], st.vsf[a[1]], 16 * a[3]); uint32_t w[2] = {a[1], a[3]}; gfx_msg(G_SET_VSC, w, 2, st.vsf[a[1]], 16 * a[3]); } return 0; }
static uint32_t d_getvsf(Ctx *c, uint32_t *a) { if (a[1] + a[3] <= 256) wrbuf(c, a[2], st.vsf[a[1]], 16 * a[3]); return 0; }
static FILE *dump_fp(void); extern uint32_t g_api_ret;
static uint32_t d_setpsf(Ctx *c, uint32_t *a) { if (getenv("BLUR_PSLOG") && a[1] <= 25 && a[1] + a[3] > 21) { FILE *f = stderr; fprintf(f, "[frame %llu ret=%08x ptr=%08x] ", (unsigned long long)st.frames, g_api_ret, a[2]); fprintf(f, "SETPSF start=%u count=%u vals:", a[1], a[3]); if (a[1] == 23 || a[1] == 21) { fprintf(f, " [mem"); for (int k = -4; k < 12; k++) { uint32_t w = rd32(c, a[2] + 4 * k); float fl; memcpy(&fl, &w, 4); fprintf(f, " %g", fl); } fprintf(f, "] "); } for (uint32_t k = 0; k < a[3] * 4 && k < 20; k++) { uint32_t w = rd32(c, a[2] + 4 * k); float fl; memcpy(&fl, &w, 4); fprintf(f, " %g", fl); } fprintf(f, "\n"); }
    if (a[1] + a[3] <= 224) { rdbuf(c, a[2], st.psf[a[1]], 16 * a[3]); uint32_t w[2] = {a[1], a[3]}; gfx_msg(G_SET_PSC, w, 2, st.psf[a[1]], 16 * a[3]); } return 0; }
static uint32_t d_getpsf(Ctx *c, uint32_t *a) { if (a[1] + a[3] <= 224) wrbuf(c, a[2], st.psf[a[1]], 16 * a[3]); return 0; }
static uint32_t d_setvsi(Ctx *c, uint32_t *a) { if (a[1] + a[3] <= 16) { rdbuf(c, a[2], st.vsi[a[1]], 16 * a[3]); uint32_t w[2] = {a[1], a[3]}; gfx_msg(G_SET_VSCI, w, 2, st.vsi[a[1]], 16 * a[3]); } return 0; }
static uint32_t d_setpsi(Ctx *c, uint32_t *a) { if (a[1] + a[3] <= 16) { rdbuf(c, a[2], st.psi[a[1]], 16 * a[3]); uint32_t w[2] = {a[1], a[3]}; gfx_msg(G_SET_PSCI, w, 2, st.psi[a[1]], 16 * a[3]); } return 0; }
static uint32_t d_setvsb(Ctx *c, uint32_t *a) { if (a[1] + a[3] <= 16) { rdbuf(c, a[2], &st.vsb[a[1]], 4 * a[3]); uint32_t w[2] = {a[1], a[3]}; gfx_msg(G_SET_VSCB, w, 2, &st.vsb[a[1]], 4 * a[3]); } return 0; }
static uint32_t d_setpsb(Ctx *c, uint32_t *a) { if (a[1] + a[3] <= 16) { rdbuf(c, a[2], &st.psb[a[1]], 4 * a[3]); uint32_t w[2] = {a[1], a[3]}; gfx_msg(G_SET_PSCB, w, 2, &st.psb[a[1]], 4 * a[3]); } return 0; }
static uint32_t d_setrt(Ctx *c, uint32_t *a) { if (a[1] == 0) st.rt = a[2]; uint32_t d[4]; surf_desc_words(c, a[2], d); GFXW(G_SET_RT, a[1], d[0], d[1], d[2], d[3]); return 0; }
static uint32_t d_getrt(Ctx *c, uint32_t *a) { wr32(c, a[2], a[1] == 0 ? (st.rt ? st.rt : st.backbuf) : 0); return 0; }
static uint32_t d_setds(Ctx *c, uint32_t *a) { st.ds = a[1]; uint32_t d[4]; surf_desc_words(c, a[1], d); GFXW(G_SET_DS, d[0], d[1], d[2], d[3]); return 0; }
static uint32_t d_getds(Ctx *c, uint32_t *a) { wr32(c, a[1], st.ds); return 0; }
static uint32_t d_setscissor(Ctx *c, uint32_t *a) { rdbuf(c, a[1], st.scissor, 16); GFXW(G_SET_SCISSOR, st.scissor[0], st.scissor[1], st.scissor[2], st.scissor[3]); return 0; }
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
    uint32_t nv = vert_count(a[1], a[2]), nb = nv * a[4]; if (gfx_on() && nb < (32u << 20)) { uint8_t *tmp = malloc(nb + 1); rdbuf(c, a[3], tmp, nb); uint32_t w[7] = {a[1], a[2], a[4], nv, 0, 0, 0}; gfx_msg(G_DRAW_UP, w, 7, tmp, nb); free(tmp); }
    return d_draw(c, a);
}
static uint32_t d_drawipup(Ctx *c, uint32_t *a) {                                                                                        // type, minidx, numverts, primcount, idxdata, idxfmt, vdata, stride
    uint32_t ni = vert_count(a[1], a[4]), isz = a[6] == 101 ? 2 : 4, nb_i = ni * isz, nb_v = (a[2] + a[3]) * a[8];
    if (gfx_on() && nb_v < (32u << 20)) { uint8_t *tmp = malloc(nb_i + nb_v + 1); rdbuf(c, a[5], tmp, nb_i); rdbuf(c, a[7], tmp + nb_i, nb_v); uint32_t w[7] = {a[1], a[4], a[8], a[2] + a[3], 1, a[6], ni}; gfx_msg(G_DRAW_UP, w, 7, tmp, nb_i + nb_v); free(tmp); }
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
        for (int i = 0; i < 16; i++) if (st.tex[i]) { Obj *t = O(c, st.tex[i]); fprintf(ef, " s%d=%ux%u/f%u", i, t->w, t->h, t->fmt); }
        fprintf(ef, "  rs: zen=%u blend=%u src=%u dst=%u cull=%u\n", st.rs[7], st.rs[27], st.rs[19], st.rs[20], st.rs[22]);
        if (getenv("BLUR_DRAWCONST") && (v - objs) > 1000 && st.ib) { Obj *ib = O(c, st.ib); fprintf(ef, "    idx args base=%d minidx=%u nverts=%u start=%u  ib fmt=%u len=%u idx:", (int)a[2], a[3], a[4], a[5], ib->fmt, ib->len); for (int k = 0; k < 8; k++) fprintf(ef, " %u", rd16(c, ib->data + (a[5] + k) * 2)); fprintf(ef, "\n"); }
        if (getenv("BLUR_DRAWCONST") && (v - objs) > 1000) fprintf(ef, "    scissor_en=%u rect=%u,%u,%u,%u colorwrite=%x zwrite=%u zfunc=%u alphatest=%u stencil=%u\n", st.rs[174], st.scissor[0], st.scissor[1], st.scissor[2], st.scissor[3], st.rs[168], st.rs[14], st.rs[23], st.rs[15], st.rs[52]);
        if (getenv("BLUR_DRAWCONST") && (v - objs) > 1000 && st.vb[0]) { Obj *vb = O(c, st.vb[0]); fprintf(ef, "    vb0 stride=%u off=%u data=%08x len=%u verts:\n", st.vb_stride[0], st.vb_off[0], vb->data, vb->len); for (int k = 0; k < 4; k++) { fprintf(ef, "      "); for (uint32_t q = 0; q < st.vb_stride[0] / 4 && q < 12; q++) { uint32_t w = rd32(c, vb->data + st.vb_off[0] + k * st.vb_stride[0] + q * 4); float fl; memcpy(&fl, &w, 4); fprintf(ef, "%g(%08x) ", fl, w); } fprintf(ef, "\n"); } }
        if (getenv("BLUR_DRAWCONST") && (v - objs) > 1000) { for (int k = 32; k < 36; k++) fprintf(ef, "    vc%d = %g %g %g %g\n", k, st.vsf[k][0], st.vsf[k][1], st.vsf[k][2], st.vsf[k][3]); for (int k = 21; k < 26; k++) fprintf(ef, "    pc%d = %g %g %g %g\n", k, st.psf[k][0], st.psf[k][1], st.psf[k][2], st.psf[k][3]); }
    }
    return 0;
}
uint64_t g_frames_pub;
static uint32_t d_present(Ctx *c, uint32_t *a) {
    (void)a; st.frames++; g_frames_pub = st.frames;
    if (dlog(c) && (st.frames <= 5 || st.frames % 60 == 0)) fprintf(stderr, "[d3d9] Present #%llu (draws last frame=%llu, clears=%llu)\n", (unsigned long long)st.frames, (unsigned long long)st.draws_frame, (unsigned long long)st.clears);
    { static long dd2 = -2; static int done; if (dd2 == -2) dd2 = getenv("BLUR_DRAWDUMP") ? atol(getenv("BLUR_DRAWDUMP")) : -1;
      if (dd2 >= 0 && !done) { FILE *f = dump_fp(); if ((long)st.draws_frame >= dd2) { done = 1; fflush(f); rewind(f); char ln[1024]; while (fgets(ln, sizeof ln, f)) fputs(ln, stderr); fprintf(stderr, "[drawdump] end frame %llu\n", (unsigned long long)st.frames); } else { fflush(f); rewind(f); if (ftruncate(fileno(f), 0)) {} } } }
    { static int on = -1; static uint64_t last, t_flush, sec0, fr0, dr0, worst, maxdt; static uint64_t pres_last;
      if (on < 0) on = getenv("BLUR_FPSLOG") != NULL;
      { extern void ww_arm(void), ww_rearm(void); static int armed; if (getenv("BLUR_WATCHW")) { if (!armed && st.frames == 1 && getenv("BLUR_WATCHW_AFTER_FRAME1")) { ww_arm(); armed = 1; } if (armed || !getenv("BLUR_WATCHW_AFTER_FRAME1")) ww_rearm(); } }
      { static int dm = -1; if (dm < 0) dm = getenv("BLUR_MEMDUMP") != NULL; if (dm && (st.frames == 5 || st.frames == 300)) { uint32_t lo = (uint32_t)strtoul(getenv("BLUR_MEMDUMP"), 0, 16); fprintf(stderr, "[memdump] frame %llu @%08x:", (unsigned long long)st.frames, lo); for (int k = 0; k < 64; k++) fprintf(stderr, " %02x", rd8(c, lo + k)); fprintf(stderr, "\n"); } }
      resend_if_filled(c); uint64_t t0 = real_us(); st.draws_frame = 0; GFXW(G_PRESENT, (uint32_t)st.frames); gfx_flush(); t_flush += real_us() - t0;
      if (on) { uint64_t n = real_us(); if (pres_last && n - pres_last > maxdt) maxdt = n - pres_last; pres_last = n; if (!sec0) { sec0 = n; fr0 = st.frames; dr0 = st.draws; }
        if (n - sec0 >= 1000000) { fprintf(stderr, "[fps] %.1f fps  draws/frame %.0f  gfx_flush %.2f ms/frame  worst frame %.1f ms\n", (st.frames - fr0) * 1e6 / (n - sec0), (double)(st.draws - dr0) / (st.frames - fr0 ? st.frames - fr0 : 1), t_flush / 1000.0 / (st.frames - fr0 ? st.frames - fr0 : 1), maxdt / 1000.0); sec0 = n; fr0 = st.frames; dr0 = st.draws; t_flush = 0; maxdt = 0; } }
      (void)last; (void)worst; }
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
static uint32_t d_updatetexture(Ctx *c, uint32_t *a) {   // src (sysmem) -> dst (default pool): re-send every level with the destination id
    Obj *src = O(c, a[1]), *dst = O(c, a[2]); if (!gfx_on() || src == &objs[0] || dst == &objs[0]) return 0;
    uint32_t faces = dst->kind == K_CUBE ? 6 : 1;
    for (uint32_t f = 0; f < faces; f++) for (uint32_t l = 0; l < dst->nlev && l < src->nlev; l++) { uint32_t keep = src->guest; (void)keep; Obj tmp = *src; uint32_t id = oid(dst);
        uint32_t lw = pow2lv(dst->w, l), lh = pow2lv(dst->h, l), n = size_of(src->fmt, lw, lh), base = src->data + src->lev_off[l] + (src->kind == K_CUBE ? f * (src->len / 6) : 0);
        uint8_t *buf = malloc(n + 1); rdbuf(c, base, buf, n); uint32_t w[8] = {id, f, l, lw, lh, 1, src->fmt, src->lev_pitch[l]}; gfx_msg(G_TEX_UPLOAD, w, 8, buf, n); free(buf); (void)tmp; }
    return 0;
}
static uint32_t d_getbackbuffer(Ctx *c, uint32_t *a) { wr32(c, a[4], st.backbuf); return 0; }
static uint32_t d_getdirect3d(Ctx *c, uint32_t *a) { (void)c; (void)a; return 0; }
static uint32_t d_testcoop(Ctx *c, uint32_t *a) { (void)c; (void)a; return 0; }                    // D3D_OK: device is not lost
static uint32_t d_getswapchain(Ctx *c, uint32_t *a) { wr32(c, a[2], make_obj(c, "IDirect3DSwapChain9", sc_m, NM(sc_m), K_SWAPCHAIN, NULL)); return 0; }
static uint32_t d_numswap(Ctx *c, uint32_t *a) { (void)c; (void)a; return 1; }
static uint32_t d_getcreation(Ctx *c, uint32_t *a) { wr32(c, a[1], 0); wr32(c, a[1] + 4, 1); wr32(c, a[1] + 8, 0x10001); wr32(c, a[1] + 12, 0x20 | 0x10); return 0; }
static uint32_t g_refresh(void) { static uint32_t v; if (!v) v = getenv("BLUR_REFRESH") ? (uint32_t)atoi(getenv("BLUR_REFRESH")) : 60; return v; }   // display refresh rate reported to the game (it paces frames from it)
static uint32_t d_getdisplaymode(Ctx *c, uint32_t *a) { wr32(c, a[2], 1920); wr32(c, a[2] + 4, 1080); wr32(c, a[2] + 8, g_refresh()); wr32(c, a[2] + 12, 22); return 0; }
static uint32_t d_availtexmem(Ctx *c, uint32_t *a) { (void)c; (void)a; return 0x30000000; }
static uint32_t d_reset(Ctx *c, uint32_t *a) { rdbuf(c, a[1], st.pp, 56); return 0; }

static uint32_t d3d_getcaps_fill(Ctx *c, uint32_t p);
static uint32_t d_getcaps(Ctx *c, uint32_t *a) { return d3d_getcaps_fill(c, a[1]); }

static const ComMethod dev_m[] = {
    IU, { "TestCooperativeLevel", 1, d_testcoop }, { "GetAvailableTextureMem", 1, d_availtexmem }, Z("EvictManagedResources", 1), { "GetDirect3D", 2, d_getdirect3d },
    { "GetDeviceCaps", 2, d_getcaps }, { "GetDisplayMode", 3, d_getdisplaymode }, { "GetCreationParameters", 2, d_getcreation }, Z("SetCursorProperties", 4),
    Z("SetCursorPosition", 4), Z("ShowCursor", 2), Z("CreateAdditionalSwapChain", 3), { "GetSwapChain", 3, d_getswapchain }, { "GetNumberOfSwapChains", 1, d_numswap },
    { "Reset", 2, d_reset }, { "Present", 5, d_present }, { "GetBackBuffer", 5, d_getbackbuffer }, Z("GetRasterStatus", 3), Z("SetDialogBoxMode", 2), Z("SetGammaRamp", 4),
    Z("GetGammaRamp", 3), { "CreateTexture", 9, d_createtexture }, { "CreateVolumeTexture", 10, d_createvolume }, { "CreateCubeTexture", 8, d_createcube },
    { "CreateVertexBuffer", 7, d_createvb }, { "CreateIndexBuffer", 7, d_createib }, { "CreateRenderTarget", 9, d_creatert }, { "CreateDepthStencilSurface", 9, d_createds },
    Z("UpdateSurface", 5), { "UpdateTexture", 3, d_updatetexture }, Z("GetRenderTargetData", 3), Z("GetFrontBufferData", 3), { "StretchRect", 6, d_stretch }, { "ColorFill", 4, d_colorfill },
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
    Z("SetStreamSourceFreq", 3), Z("GetStreamSourceFreq", 3), { "SetIndices", 2, d_setindices }, { "GetIndices", 2, d_getindices }, { "CreatePixelShader", 3, d_createps },
    { "SetPixelShader", 2, d_setps }, { "GetPixelShader", 2, d_getps }, { "SetPixelShaderConstantF", 4, d_setpsf }, { "GetPixelShaderConstantF", 4, d_getpsf },
    { "SetPixelShaderConstantI", 4, d_setpsi }, Z("GetPixelShaderConstantI", 4), { "SetPixelShaderConstantB", 4, d_setpsb }, Z("GetPixelShaderConstantB", 4), Z("DrawRectPatch", 4),
    Z("DrawTriPatch", 4), Z("DeletePatch", 2), { "CreateQuery", 3, d_createquery },
};

// ---- IDirect3D9 ----------------------------------------------------------------------------------------------------------------------------
static uint32_t d3d_getcaps_fill(Ctx *c, uint32_t p) {
    uint8_t z[304] = {0}; wrbuf(c, p, z, sizeof z);
    struct { uint32_t off, v; } f[] = {
        {0, 1}, {8, 0x20000}, {12, 0x80000 | 0x40000000 | 0x20000000}, {16, 0x20 | 0x80}, {20, 0xf}, {24, 0x3},
        {28, 0x000bf0f0 | 0x100000}, {32, 0x0003ffee}, {36, 0x07733ff0}, {40, 0xff}, {44, 0x3fff}, {48, 0x3fff}, {52, 0xff}, {56, 0x08181ff0 & 0xffff}, {60, 0x0005ecff},
        {64, 0x03030303 | 0x0f000f00}, {68, 0x03030303 | 0x0f000f00}, {72, 0x03030303}, {76, 0x3f}, {80, 0x3f}, {84, 0x1f}, {88, 4096}, {92, 4096}, {96, 2048}, {100, 8192},
        {104, 4096}, {108, 16}, {132, 0}, {136, 0x1ff}, {140, 0x10008}, {144, 0x03ffffff & 0xffffff}, {148, 8}, {152, 8}, {156, 0x17b}, {160, 8}, {164, 8}, {168, 4}, {172, 0},
        {180, 0xfffff}, {184, 0xfffff}, {188, 16}, {192, 255}, {196, 0xfffe0300}, {200, 256}, {204, 0xffff0300}, {212, 0x1 | 0x2 | 0x4 | 0x8 | 0x10}, {236, 0x3ff}, {240, 4},
        {244, 0x1111}, {248, 0x1}, {252, 24}, {256, 32}, {260, 24}, {264, 0x1}, {268, 24}, {272, 32}, {276, 24}, {280, 512}, {284, 0x03030303}, {288, 0xffff}, {292, 0xffff},
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
void d3d9_report(Ctx *c) { (void)c; fprintf(stderr, "[d3d9] frames=%llu draws=%llu clears=%llu scenes=%llu objects=%d\n", (unsigned long long)st.frames, (unsigned long long)st.draws, (unsigned long long)st.clears, (unsigned long long)st.scenes, nobjs - 1); }

// shared by the D3DX constant table: write constants straight into the device state (set: 0=bool, 1=int4, 2=float4)
void d3d9_set_const(Ctx *c, int ps, int set, uint32_t reg, const void *data, uint32_t nvec) {
    if (set == 2) { uint32_t lim = ps ? 224 : 256; for (uint32_t i = 0; i < nvec && reg + i < lim; i++) memcpy(ps ? st.psf[reg + i] : st.vsf[reg + i], (const float *)data + 4 * i, 16); }
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
