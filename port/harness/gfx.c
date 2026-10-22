// Render command stream to the browser. Every message: [u32 payload_len][u32 type][payload]. Messages are batched and
// flushed on Present (or when large). The browser implements a Direct3D 9 device on WebGL2 (web/renderer.js).
#include "emu.h"
#include <time.h>
#include <unistd.h>

volatile uint32_t g_net_epoch;   // see emu.h
static uint8_t *buf; static size_t blen, bcap;
// BLUR_FRAMEHASH=1: pretend a viewer is attached (so the whole command stream is generated) and print "[fh] <flush#> <fnv1a64> <bytes>" for every flush (one per Present);
// with BLUR_DET this makes frame hashes comparable across runs (tools/regress)
static int fh_on = -1;
// BLUR_GFXFAKE=1: generate the whole command stream without hashing it and without a viewer (host-side CPU/bytes measurements)
int gfx_on(void) { if (fh_on < 0) fh_on = getenv("BLUR_FRAMEHASH") ? 1 : getenv("BLUR_GFXFAKE") ? 2 : 0; return fh_on || net_client_connected(); }
// BLUR_GFXSTAT=N: every N flushes print per-message-type count/bytes per frame (averaged) and gfx_msg encode time
void d3d9_gfxstat(uint64_t frames); void d3d9_flush_state(void);
#define CONSUMER(t) ((t) == 70 || (t) == 71 || (t) == 72 || (t) == 74 || (t) == 81 || (t) == 85)   // draw, draw-up, clear, stretch/colorfill, readback, texref audit: messages that read the state
static int st_every = -1; static uint64_t st_cnt[128], st_bytes[128], st_frames, st_ns, st_tot;
static uint64_t st_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static void st_report(void) {
    fprintf(stderr, "[gfxstat] %llu frames: %.1f KB/frame, encode %.3f ms/frame |", (unsigned long long)st_frames, st_tot / 1024.0 / st_frames, st_ns / 1e6 / st_frames);
    for (int i = 0; i < 128; i++) if (st_cnt[i]) fprintf(stderr, " t%d:%.0f/%.1fK", i, (double)st_cnt[i] / st_frames, st_bytes[i] / 1024.0 / st_frames);
    fprintf(stderr, "\n"); d3d9_gfxstat(st_frames); memset(st_cnt, 0, sizeof st_cnt); memset(st_bytes, 0, sizeof st_bytes); st_frames = st_ns = st_tot = 0;
}

// ---- content-hash cache (the page keeps texture / buffer upload payloads in IndexedDB keyed by this hash; web/hcache.js) ----------------------------------------------------------
// Protocol: page -> host  [10, flags(bit0 last chunk, bit1 cache enabled), count, count x (lo, hi)] at connect = hashes it holds; [12, 0, count, hashes] = "now stored" acks (so a hash is only
// treated as known after the page confirmed it). host -> page: an upload (type 21/31, >= HC_MIN payload bytes, see hc_cacheable) becomes either
//   98 [T, lo, hi, original payload...]      "tagged": process as type T, then store the payload under the hash
//   96 [T, lo, hi, n, header words(T)...]    "reference": the payload (n bytes) is in the page's cache; rebuild the type-T message from it
// Everything is decided in gfx_flush on the finished batch, so the producers (send_level, send_buf_raw, ...) are untouched. BLUR_HCACHE=0 disables it on the host, #hcache=0 on the page.
// Hash: 64-bit multiply-fold ("wyhash"-style, 2 lanes of 16 bytes), ~8-10 GB/s; picked over FNV-1a 64 (1 byte per step, ~1 GB/s, a 100+ MB texture burst would stall the emulation thread) and xxh3
// (needs the 3 kloc vendored). The length is mixed in. Non-cryptographic: accidental collisions at ~1e5 blobs are ~1e-9; nobody is attacking a local texture cache.
#define HC_MIN 2048u
static inline uint64_t hc_mum(uint64_t a, uint64_t b) { __uint128_t r = (__uint128_t)a * b; return (uint64_t)r ^ (uint64_t)(r >> 64); }
static uint64_t hc_hash(const uint8_t *p, size_t n) {
    const uint64_t k0 = 0xa0761d6478bd642full, k1 = 0xe7037ed1a0b428dbull, k2 = 0x8ebc6af09c88c6e3ull, k3 = 0x589965cc75374cc3ull;
    uint64_t s0 = k0 ^ n, s1 = k1; size_t i = 0;
    for (; i + 32 <= n; i += 32) { uint64_t a, b, c, d; memcpy(&a, p + i, 8); memcpy(&b, p + i + 8, 8); memcpy(&c, p + i + 16, 8); memcpy(&d, p + i + 24, 8); s0 = hc_mum(a ^ k1, b ^ s0); s1 = hc_mum(c ^ k2, d ^ s1); }
    for (; i + 8 <= n; i += 8) { uint64_t a; memcpy(&a, p + i, 8); s0 = hc_mum(a ^ k3, s0 ^ k1); }
    if (i < n) { uint64_t a = 0; memcpy(&a, p + i, n - i); s1 = hc_mum(a ^ k2, s1 ^ k3); }
    uint64_t h = hc_mum(s0 ^ k2, s1 ^ k0 ^ n); return h ? h : 1;
}
static pthread_mutex_t hc_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t *hc_tab; static size_t hc_cap, hc_cnt;                  // open addressing, 0 = empty
static volatile int hc_ready, hc_on;                                      // page announced / page cache enabled
static int hc_env = -1;                                                   // BLUR_HCACHE (default on)
static int hc_env_on(void) { if (hc_env < 0) hc_env = !(getenv("BLUR_HCACHE") && atoi(getenv("BLUR_HCACHE")) == 0); return hc_env; }
static void hc_insert_locked(uint64_t h) {
    if (!h) return;
    if ((hc_cnt + 1) * 2 > hc_cap) { size_t nc = hc_cap ? hc_cap * 2 : 4096; uint64_t *nt = calloc(nc, 8);
        for (size_t i = 0; i < hc_cap; i++) if (hc_tab[i]) { size_t j = (hc_tab[i] * 0x9e3779b97f4a7c15ull >> 20) & (nc - 1); while (nt[j]) j = (j + 1) & (nc - 1); nt[j] = hc_tab[i]; }
        free(hc_tab); hc_tab = nt; hc_cap = nc; }
    size_t j = (h * 0x9e3779b97f4a7c15ull >> 20) & (hc_cap - 1);
    while (hc_tab[j]) { if (hc_tab[j] == h) return; j = (j + 1) & (hc_cap - 1); }
    hc_tab[j] = h; hc_cnt++;
}
static int hc_known(uint64_t h) {
    int r = 0; pthread_mutex_lock(&hc_mu);
    if (hc_cap) { size_t j = (h * 0x9e3779b97f4a7c15ull >> 20) & (hc_cap - 1); while (hc_tab[j]) { if (hc_tab[j] == h) { r = 1; break; } j = (j + 1) & (hc_cap - 1); } }
    pthread_mutex_unlock(&hc_mu); return r;
}
void hc_viewer_reset(void) { pthread_mutex_lock(&hc_mu); free(hc_tab); hc_tab = NULL; hc_cap = hc_cnt = 0; hc_ready = hc_on = 0; pthread_mutex_unlock(&hc_mu); }
// page -> host cache messages use their own type codes: plain NetInput types (1..10; 10 = GetRenderTargetData answer) must never be mistaken for them
#define HC_ANNOUNCE 0x48430Au
#define HC_STORED 0x48430Cu
void hc_net_msg(const void *pv, size_t n) {
    const uint32_t *w = pv; uint32_t type = w[0], cnt = n >= 12 ? w[2] : 0; if (cnt > (n - 12) / 8) cnt = (uint32_t)((n - 12) / 8);
    pthread_mutex_lock(&hc_mu);
    if (type == HC_ANNOUNCE || (type == HC_STORED && hc_on)) for (uint32_t i = 0; i < cnt; i++) hc_insert_locked((uint64_t)w[4 + 2 * i] << 32 | w[3 + 2 * i]);
    if (type == HC_ANNOUNCE) { if (w[1] & 2) hc_on = 1; if (w[1] & 1) hc_ready = 1; if (!(w[1] & 2)) hc_on = 0; }
    pthread_mutex_unlock(&hc_mu);
}
static void hc_wait_ready(void) {       // a new viewer announces what it holds right after the socket opens; replaying before that would resend everything
    if (!hc_env_on()) return; int ms = 0; while (!hc_ready && net_client_connected() && ms < 3000) { usleep(1000); ms++; }
    if (!hc_ready) fprintf(stderr, "[hc] viewer did not announce its cache within 3 s: sending everything\n");
}
// per-object upload counter: only content an object uploads once or twice is worth keeping (dynamic buffers / video textures upload new bytes every frame)
static uint8_t hc_up[1u << 20]; static uint32_t hc_up_epoch;
// stats (BLUR_HCSTAT=1): raw bytes by message type vs. bytes after the cache
static int hc_stat_on = -1; static uint64_t hs_msgs[128], hs_raw[128], hs_wire[128], hs_ref[128], hs_refn[128], hs_tag[128], hs_flushes, hs_rawtot, hs_wiretot;
static uint64_t hs_ncid[1u << 16], hs_nctot; static uint32_t hs_ncw[1u << 16];      // BLUR_HCSTAT: bytes of big uploads that were not cacheable, per object id (low 16 bits)
static void hc_stat_report(void) {
    fprintf(stderr, "[hcstat] %llu flushes: raw %.1f MB wire %.1f MB (hcache %s, %zu known) |", (unsigned long long)hs_flushes, hs_rawtot / 1048576.0, hs_wiretot / 1048576.0, hc_on ? "on" : "off", hc_cnt);
    for (int i = 0; i < 128; i++) if (hs_msgs[i]) fprintf(stderr, " t%d:n%llu raw%.2fM wire%.2fM", i, (unsigned long long)hs_msgs[i], hs_raw[i] / 1048576.0, hs_wire[i] / 1048576.0);
    fprintf(stderr, " | not cacheable (>2 uploads/object) %.1f MB, top ids:", hs_nctot / 1048576.0);
    for (int k = 0; k < 6; k++) { int b = 0; for (int i = 1; i < (1 << 16); i++) if (hs_ncid[i] > hs_ncid[b]) b = i; if (!hs_ncid[b]) break; fprintf(stderr, " #%d:%.0fM(%ux%u)", b, hs_ncid[b] / 1048576.0, hs_ncw[b] >> 16, hs_ncw[b] & 0xffff); hs_ncid[b] = 0; }
    fprintf(stderr, " | refs %llu (%.1f MB saved) tags %llu\n", (unsigned long long)(hs_refn[21] + hs_refn[31]), (hs_ref[21] + hs_ref[31]) / 1048576.0, (unsigned long long)(hs_tag[21] + hs_tag[31]));
}
static int hc_verify(void) { static int v = -1; if (v < 0) v = getenv("BLUR_HCVERIFY") && atoi(getenv("BLUR_HCVERIFY")); return v; }      // BLUR_HCVERIFY=1: send known content in full anyway; the page (#hcverify=1) compares it with its cached copy
static uint8_t *hc_out; static size_t hc_ocap;
// Rewrites the batch for the page; returns the new length (data in hc_out) or 0 when nothing was changed.
static size_t hc_xform(const uint8_t *in, size_t n) {
    if (hc_up_epoch != g_net_epoch + 1) { hc_up_epoch = g_net_epoch + 1; memset(hc_up, 0, sizeof hc_up); }
    int act = hc_on && hc_env_on(); size_t off = 0, o = 0, run = 0, saved = 0;
    if (act && n * 2 + 4096 > hc_ocap) { hc_ocap = n * 2 + 4096; hc_out = realloc(hc_out, hc_ocap); }
    while (off + 8 <= n) {
        uint32_t plen, type; memcpy(&plen, in + off, 4); memcpy(&type, in + off + 4, 4); const uint8_t *pl = in + off + 8; size_t mend = off + 8 + plen;
        int nw = type == G_TEX_UPLOAD ? 8 : type == G_BUF_UPLOAD ? 2 : 0; uint32_t t7 = type & 127;
        hs_msgs[t7]++; hs_raw[t7] += 8 + plen; size_t wire = 8 + plen;
        if (type == G_DESTROY && plen >= 4) { uint32_t id; memcpy(&id, pl, 4); if (id < sizeof hc_up) hc_up[id] = 0; }
        if (nw && plen >= 4u * nw + HC_MIN) {
            uint32_t w[8]; memcpy(w, pl, 4u * nw); uint32_t id = w[0]; int cacheable = 1;
            if (id < sizeof hc_up) { if (type == G_TEX_UPLOAD ? (w[1] == 0 && w[2] == 0) : 1) { if (hc_up[id] < 255) hc_up[id]++; } cacheable = hc_up[id] <= 2; }
            if (!cacheable) { hs_ncid[id & 0xffff] += plen; hs_ncw[id & 0xffff] = w[3] << 16 ^ w[4]; hs_nctot += plen; }
            if (act && cacheable) {
                uint32_t nb = plen - 4u * nw; uint64_t h = hc_hash(pl + 4u * nw, nb); uint32_t lo = (uint32_t)h, hi = (uint32_t)(h >> 32);
                memcpy(hc_out + o, in + run, off - run); o += off - run; run = mend;           // flush the untouched run before this message
                if (hc_known(h) && !hc_verify()) { uint32_t hd[2] = {16 + 4u * nw, 96}, ww[4] = {type, lo, hi, nb}; memcpy(hc_out + o, hd, 8); memcpy(hc_out + o + 8, ww, 16); memcpy(hc_out + o + 24, w, 4u * nw); o += 24 + 4u * nw;
                    wire = 24 + 4u * nw; hs_ref[t7] += 8 + plen - wire; hs_refn[t7]++; }
                else { uint32_t hd[2] = {plen + 12, 98}, ww[3] = {type, lo, hi}; memcpy(hc_out + o, hd, 8); memcpy(hc_out + o + 8, ww, 12); memcpy(hc_out + o + 20, pl, plen); o += 20 + plen; wire = 20 + plen; hs_tag[t7]++; }
            }
        }
        hs_wire[t7] += wire; off = mend;
    }
    (void)saved; hs_flushes++; hs_rawtot += n;
    if (!act) { hs_wiretot += n; return 0; }
    memcpy(hc_out + o, in + run, n - run); o += n - run; hs_wiretot += o; return o;
}
// BLUR_GFXSNAP="file:seconds:presents[,file:seconds:presents...]": scene capture for replay tools (e.g. the WebGPU replay page). `seconds` after the first streamed frame the host acts as if a NEW viewer had attached
// (g_net_epoch++, so every live resource is re-sent from guest memory: the file is self-contained) and records the raw message stream (same format as BLUR_GFXDUMP, before the hash-cache rewrite)
// until `presents` frames (type-73 messages) were written. Windows must not overlap.
typedef struct { char path[240]; double at; int presents, state, seen; FILE *f; } Snap;
static Snap snaps[8]; static int nsnaps = -1; static double snap_t0;
static void snap_flush(const uint8_t *b, size_t n) {
    if (nsnaps < 0) { nsnaps = 0; const char *e = getenv("BLUR_GFXSNAP");
        while (e && *e && nsnaps < 8) { Snap *s = &snaps[nsnaps]; char tmp[300]; int k = 0; while (*e && *e != ',' && k < 299) tmp[k++] = *e++; tmp[k] = 0; if (*e == ',') e++;
            char *c2 = strrchr(tmp, ':'); if (!c2) continue; *c2 = 0; char *c1 = strrchr(tmp, ':'); if (!c1) continue; *c1 = 0;
            snprintf(s->path, sizeof s->path, "%s", tmp); s->at = atof(c1 + 1); s->presents = atoi(c2 + 1); nsnaps++; } }
    if (!nsnaps) return;
    double now = (double)st_now() / 1e9; if (!snap_t0) snap_t0 = now;
    for (int i = 0; i < nsnaps; i++) { Snap *s = &snaps[i];
        if (s->state == 0 && now - snap_t0 >= s->at) { s->f = fopen(s->path, "wb"); if (s->f) { s->state = 1; g_net_epoch++; fprintf(stderr, "[gfxsnap] %s: recording %d presents after a full resource replay\n", s->path, s->presents); } else s->state = 2; continue; }
        if (s->state != 1) continue;
        fwrite(b, 1, n, s->f);
        for (size_t o = 0; o + 8 <= n;) { uint32_t pl, ty; memcpy(&pl, b + o, 4); memcpy(&ty, b + o + 4, 4); if (ty == G_PRESENT) s->seen++; o += 8 + pl; }
        if (s->seen >= s->presents) { fclose(s->f); s->f = NULL; s->state = 2; fprintf(stderr, "[gfxsnap] %s: done (%d presents)\n", s->path, s->seen); } }
}
void gfx_flush(void) {

    if (st_every < 0) st_every = getenv("BLUR_GFXSTAT") ? atoi(getenv("BLUR_GFXSTAT")) : 0;
    if (st_every > 0) { st_tot += blen; if (++st_frames >= (uint64_t)st_every) st_report(); }
    if (blen) {
        if (fh_on == 1) { static unsigned long long n; uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < blen; i++) { h ^= buf[i]; h *= 1099511628211ull; } fprintf(stderr, "[fh] %llu %016llx %zu\n", n++, (unsigned long long)h, blen); }
        static FILE *dump; static int init;                                   // BLUR_GFXDUMP=file: record the exact byte stream sent to the browser (for deterministic replay tests)
        if (!init) { init = 1; const char *p = getenv("BLUR_GFXDUMP"); if (p) dump = fopen(p, "wb"); }
        if (dump) { fwrite(buf, 1, blen, dump); fflush(dump); }
        snap_flush(buf, blen);
        if (hc_stat_on < 0) hc_stat_on = getenv("BLUR_HCSTAT") != NULL;
        if (net_client_connected() && (hc_on || hc_stat_on)) { size_t m = hc_xform(buf, blen); if (hc_stat_on && hs_flushes % 600 == 0) hc_stat_report(); if (m) { net_send(hc_out, m); blen = 0; return; } }
        net_send(buf, blen); blen = 0;
    }
}
static void reserve(size_t n) { if (blen + n > bcap) { bcap = (blen + n) * 2; buf = realloc(buf, bcap); } }
// like gfx_msg, but returns a pointer to ndata bytes of message payload for the caller to fill in place (saves the malloc + extra copy of rdbuf-into-temp). The pointer is valid until
// the next gfx_* call. NULL when no viewer is attached.
static uint32_t replay_epoch; void d3d9_replay_resources(void);
static inline void replay_check(void) { if (replay_epoch != g_net_epoch + 1) { replay_epoch = g_net_epoch + 1; hc_wait_ready(); d3d9_replay_resources(); } }      // new viewer: it holds no resources
uint8_t *gfx_msg_alloc(uint32_t type, const uint32_t *w, int nw, uint32_t ndata) {
    if (!gfx_on()) return NULL;
    replay_check();
    if (CONSUMER(type)) d3d9_flush_state();
    if (blen > (8u << 20)) gfx_flush();
    uint64_t t0 = st_every > 0 ? st_now() : 0;
    uint32_t plen = 4u * (uint32_t)nw + ndata; reserve(8 + plen);
    uint32_t h[2] = {plen, type}; memcpy(buf + blen, h, 8); blen += 8;
    if (nw) { memcpy(buf + blen, w, 4u * (uint32_t)nw); blen += 4u * (uint32_t)nw; }
    uint8_t *d = buf + blen; blen += ndata;
    if (st_every > 0) { st_cnt[type & 127]++; st_bytes[type & 127] += 8 + plen; st_ns += st_now() - t0; }
    return d;
}
// header words (u32) followed by an optional byte blob
void gfx_msg(uint32_t type, const uint32_t *w, int nw, const void *data, uint32_t ndata) {
    if (!gfx_on()) return;
    replay_check();
    if (CONSUMER(type)) d3d9_flush_state();
    uint64_t t0 = st_every > 0 ? st_now() : 0;
    uint32_t plen = 4u * (uint32_t)nw + ndata; reserve(8 + plen);
    uint32_t h[2] = {plen, type}; memcpy(buf + blen, h, 8); blen += 8;
    if (nw) { memcpy(buf + blen, w, 4u * (uint32_t)nw); blen += 4u * (uint32_t)nw; }
    if (ndata) { memcpy(buf + blen, data, ndata); blen += ndata; }
    if (st_every > 0) { st_cnt[type & 127]++; st_bytes[type & 127] += 8 + plen; st_ns += st_now() - t0; }
    if (blen > (8u << 20)) gfx_flush();
}
