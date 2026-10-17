// Render command stream to the browser. Every message: [u32 payload_len][u32 type][payload]. Messages are batched and
// flushed on Present (or when large). The browser implements a Direct3D 9 device on WebGL2 (web/renderer.js).
#include "emu.h"
#include <time.h>

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
void gfx_flush(void) {
    if (st_every < 0) st_every = getenv("BLUR_GFXSTAT") ? atoi(getenv("BLUR_GFXSTAT")) : 0;
    if (st_every > 0) { st_tot += blen; if (++st_frames >= (uint64_t)st_every) st_report(); }
    if (blen) {
        if (fh_on == 1) { static unsigned long long n; uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < blen; i++) { h ^= buf[i]; h *= 1099511628211ull; } fprintf(stderr, "[fh] %llu %016llx %zu\n", n++, (unsigned long long)h, blen); }
        static FILE *dump; static int init;                                   // BLUR_GFXDUMP=file: record the exact byte stream sent to the browser (for deterministic replay tests)
        if (!init) { init = 1; const char *p = getenv("BLUR_GFXDUMP"); if (p) dump = fopen(p, "wb"); }
        if (dump) { fwrite(buf, 1, blen, dump); fflush(dump); }
        net_send(buf, blen); blen = 0;
    }
}
static void reserve(size_t n) { if (blen + n > bcap) { bcap = (blen + n) * 2; buf = realloc(buf, bcap); } }
// like gfx_msg, but returns a pointer to ndata bytes of message payload for the caller to fill in place (saves the malloc + extra copy of rdbuf-into-temp). The pointer is valid until
// the next gfx_* call. NULL when no viewer is attached.
uint8_t *gfx_msg_alloc(uint32_t type, const uint32_t *w, int nw, uint32_t ndata) {
    if (!gfx_on()) return NULL;
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
    if (CONSUMER(type)) d3d9_flush_state();
    uint64_t t0 = st_every > 0 ? st_now() : 0;
    uint32_t plen = 4u * (uint32_t)nw + ndata; reserve(8 + plen);
    uint32_t h[2] = {plen, type}; memcpy(buf + blen, h, 8); blen += 8;
    if (nw) { memcpy(buf + blen, w, 4u * (uint32_t)nw); blen += 4u * (uint32_t)nw; }
    if (ndata) { memcpy(buf + blen, data, ndata); blen += ndata; }
    if (st_every > 0) { st_cnt[type & 127]++; st_bytes[type & 127] += 8 + plen; st_ns += st_now() - t0; }
    if (blen > (8u << 20)) gfx_flush();
}
