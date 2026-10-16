// binkw32 replacement on top of FFmpeg (bink_ff.c). Handlers take the guest stack arguments (stdcall, a[0]..) and return eax.
// Guest memory is accessed through the flat mapping G (guest address A = G + A), so this file has no emulator (Unicorn/Ctx) dependency
// and can be driven by a test program (bink_test.c) or by the harness glue in hle2.c.
//
// BINK struct fields the game reads (offsets): Width 0, Height 4, Frames 8, FrameNum 12 (1-based), LastFrameNum 16, FrameRate 20,
// FrameRateDiv 24, ReadError 28, OpenFlags 32, FrameRects[8] 56, NumRects 184, NumTracks 224 (0xe0).
// BINKFRAMEBUFFERS: TotalFrames, YABufferWidth/Height, cRcBBufferWidth/Height, FrameNum, then Frames[2] of 4 planes {Allocate, Buffer, Pitch} (12 bytes
// each; Allocate != 0 tells the application to supply a buffer for that plane, A only for movies with alpha).
// BINKTRACK (returned by BinkOpenTrack): Frequency, Bits, Channels, MaxSize, bink, sndcomp, trackindex.
#include "bink_hle.h"
#include "bink_ff.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint8_t *G;
static BkHost H_;
void bk_set_host(const BkHost *h) { H_ = *h; }
static inline uint32_t R32(uint32_t a) { uint32_t v; memcpy(&v, G + a, 4); return v; }
static inline void W32(uint32_t a, uint32_t v) { memcpy(G + a, &v, 4); }

#define MAXBINK 16
#define MAXTR 128
#define TRACK_MAX 16384
typedef struct {
    int used; uint32_t g; BkFF *ff; BkInfo info; uint32_t flags;
    uint64_t t0, pause_t; int started, paused; uint32_t fb;   // fb: last registered BINKFRAMEBUFFERS
    uint64_t decoded; uint32_t tr_h[BKFF_MAXTRACKS]; int vol[BKFF_MAXTRACKS + 1];
    uint8_t *mem;                                              // BINKFROMMEMORY copy
    double decode_ms; uint64_t ndecode;
} Bk;
static Bk bks[MAXBINK];
static struct { uint32_t g; int bk, track; } trs[MAXTR]; static int ntr;
static int blog(void) { static int v = -1; if (v < 0) v = getenv("BLUR_BINKLOG") != NULL; return v; }
#define LOG(...) do { if (blog()) fprintf(stderr, "[binkhle] " __VA_ARGS__); } while (0)
static Bk *find(uint32_t g) { for (int i = 0; i < MAXBINK; i++) if (bks[i].used && bks[i].g == g) return &bks[i]; return NULL; }
static uint64_t frame_us(Bk *b) { return (uint64_t)1000000 * (uint64_t)b->info.fps_den / (uint64_t)(b->info.fps_num ? b->info.fps_num : 30); }
static uint32_t align16(uint32_t v) { return (v + 15) & ~15u; }

uint32_t bk_BinkOpen(const uint32_t *a) {
    uint32_t name = a[0], flags = a[1]; char host[1024]; BkInfo info; BkFF *ff = NULL; uint8_t *mem = NULL;
    if (flags & 0x04000000u) {                                  // BINKFROMMEMORY: a[0] is the address of the .bik image; its header holds the size
        if (!name) return 0; uint32_t sz = R32(name + 4) + 8; mem = malloc(sz); memcpy(mem, G + name, sz); ff = bkff_open_mem(mem, sz, &info);
        LOG("BinkOpen(memory %08x, %u bytes, flags %08x) -> %s\n", name, sz, flags, ff ? "ok" : "FAIL");
    } else {
        char n[512]; int i = 0; while (i < 511 && G[name + i]) { n[i] = (char)G[name + i]; i++; } n[i] = 0;
        if (!n[0]) { LOG("BinkOpen(\"\", flags %08x) -> NULL (empty name)\n", flags); return 0; }
        if (!H_.map_path(n, host)) { LOG("BinkOpen(\"%s\", %08x) -> NULL (path)\n", n, flags); return 0; }
        ff = bkff_open_file(host, &info);
        LOG("BinkOpen(\"%s\" -> %s, flags %08x) -> %s\n", n, host, flags, ff ? "ok" : "FAIL");
    }
    if (!ff) { free(mem); return 0; }
    int s = -1; for (int i = 0; i < MAXBINK; i++) if (!bks[i].used) { s = i; break; }
    if (s < 0) { bkff_close(ff); free(mem); return 0; }
    Bk *b = &bks[s]; memset(b, 0, sizeof *b); b->used = 1; b->ff = ff; b->info = info; b->flags = flags; b->mem = mem;
    b->g = H_.alloc(0x400); memset(G + b->g, 0, 0x400);
    W32(b->g, (uint32_t)info.width); W32(b->g + 4, (uint32_t)info.height); W32(b->g + 8, (uint32_t)info.frames); W32(b->g + 12, 1); W32(b->g + 16, 0);
    W32(b->g + 20, (uint32_t)info.fps_num); W32(b->g + 24, (uint32_t)info.fps_den); W32(b->g + 32, flags);
    W32(b->g + 224, (uint32_t)info.ntracks);
    for (int i = 0; i <= BKFF_MAXTRACKS; i++) b->vol[i] = 0x8000;
    LOG("  %dx%d %d frames %d/%d fps alpha=%d tracks=%d -> %08x\n", info.width, info.height, info.frames, info.fps_num, info.fps_den, info.has_alpha, info.ntracks, b->g);
    return b->g;
}

uint32_t bk_BinkClose(const uint32_t *a) {
    Bk *b = find(a[0]); if (!b) return 0;
    LOG("BinkClose(%08x) decoded=%llu avg decode %.2f ms\n", b->g, (unsigned long long)b->ndecode, b->ndecode ? b->decode_ms / (double)b->ndecode : 0.0);
    bkff_close(b->ff); free(b->mem); b->used = 0; return 0;
}

uint32_t bk_BinkGetFrameBuffersInfo(const uint32_t *a) {
    Bk *b = find(a[0]); if (!b || !a[1]) return 0; uint32_t fb = a[1];
    uint32_t w = align16((uint32_t)b->info.width), h = align16((uint32_t)b->info.height);
    memset(G + fb, 0, 24 + 2 * 48);
    W32(fb, 2); W32(fb + 4, w); W32(fb + 8, h); W32(fb + 12, w / 2); W32(fb + 16, h / 2); W32(fb + 20, 0);
    for (int s = 0; s < 2; s++) for (int p = 0; p < 4; p++) W32(fb + 24 + 48 * (uint32_t)s + 12 * (uint32_t)p, p < 3 || b->info.has_alpha ? 1u : 0u);   // Allocate flags
    LOG("GetFrameBuffersInfo(%08x) -> 2 sets, Y %ux%u c %ux%u alpha=%d\n", b->g, w, h, w / 2, h / 2, b->info.has_alpha);
    return 0;
}
uint32_t bk_BinkRegisterFrameBuffers(const uint32_t *a) { Bk *b = find(a[0]); if (b) b->fb = a[1]; return 0; }

static void copy_plane(uint8_t *dst, uint32_t pitch, uint32_t dw, uint32_t dh, const uint8_t *src, int sls, uint32_t sw, uint32_t sh) {
    for (uint32_t y = 0; y < dh; y++) {
        const uint8_t *s = src + (size_t)(y < sh ? y : sh - 1) * (size_t)sls; uint8_t *d = dst + (size_t)y * pitch;
        uint32_t n = dw < sw ? dw : sw; memcpy(d, s, n); if (dw > n) memset(d + n, s[sw - 1], dw - n);
    }
}

uint32_t bk_BinkDoFrame(const uint32_t *a) {
    Bk *b = find(a[0]); if (!b) return 0;
    if (!b->started) { b->started = 1; b->t0 = H_.now_us(); }
    uint32_t fn = R32(b->g + 12); if (fn < 1) fn = 1; uint64_t t = H_.now_us();
    if (bkff_frame(b->ff, (int)fn - 1) < 0) { LOG("DoFrame(%08x) frame %u: decode failed\n", b->g, fn); W32(b->g + 28, 1); return 0; }
    W32(b->g + 16, fn);
    if (b->fb) {
        uint32_t fb = b->fb, total = R32(fb), set = total ? (uint32_t)(b->decoded % total) : 0; if (set > 1) set = 1;
        uint32_t yw = R32(fb + 4), yh = R32(fb + 8), cw = R32(fb + 12), ch = R32(fb + 16);
        for (int p = 0; p < 4; p++) {
            uint32_t pl = fb + 24 + 48 * set + 12 * (uint32_t)p, buf = R32(pl + 4), pitch = R32(pl + 8); int sls;
            if (!buf || !pitch) continue; const uint8_t *src = bkff_plane(b->ff, p, &sls); if (!src) continue;
            uint32_t sw = (uint32_t)b->info.width, sh = (uint32_t)b->info.height;
            if (p == 1 || p == 2) { sw = (sw + 1) / 2; sh = (sh + 1) / 2; }
            copy_plane(G + buf, pitch, p == 0 || p == 3 ? yw : cw, p == 0 || p == 3 ? yh : ch, src, sls, sw, sh);
        }
        W32(fb + 20, set);
        if (blog() && b->decoded < 4) LOG("  DoFrame -> set %u of %u: Y=%08x/%u cR=%08x/%u cB=%08x/%u A=%08x\n", set, total, R32(fb + 24 + 48 * set + 4), R32(fb + 24 + 48 * set + 8), R32(fb + 24 + 48 * set + 16), R32(fb + 24 + 48 * set + 20), R32(fb + 24 + 48 * set + 28), R32(fb + 24 + 48 * set + 32), R32(fb + 24 + 48 * set + 40));
    }
    b->decoded++; b->ndecode++; b->decode_ms += (double)(H_.now_us() - t) / 1000.0;
    if (blog() && (fn <= 2 || fn % 60 == 0)) LOG("DoFrame(%08x) frame %u/%u\n", b->g, fn, R32(b->g + 8));
    return 0;
}

uint32_t bk_BinkNextFrame(const uint32_t *a) {
    Bk *b = find(a[0]); if (!b) return 0; uint32_t f = R32(b->g + 12), n = R32(b->g + 8);
    W32(b->g + 12, f < n ? f + 1 : 1); return 0;
}
uint32_t bk_BinkWait(const uint32_t *a) {            // nonzero = the next frame is not due yet
    Bk *b = find(a[0]); if (!b) return 0;
    if (b->paused) return 1;
    if (!b->started) return 0;                      // first frame is always due
    uint64_t due = b->t0 + (uint64_t)(R32(b->g + 12) - 1) * frame_us(b);
    return H_.now_us() < due ? 1 : 0;
}
uint32_t bk_BinkShouldSkip(const uint32_t *a) {     // behind by more than a frame (and not on the last frame) -> skip display
    Bk *b = find(a[0]); if (!b || !b->started || b->paused) return 0;
    uint32_t fn = R32(b->g + 12); if (fn >= R32(b->g + 8)) return 0;
    uint64_t due = b->t0 + (uint64_t)(fn - 1) * frame_us(b); return H_.now_us() > due + frame_us(b) ? 1 : 0;
}
uint32_t bk_BinkPause(const uint32_t *a) {
    Bk *b = find(a[0]); if (!b) return 0; int p = (int)a[1];
    if (p && !b->paused) { b->paused = 1; b->pause_t = H_.now_us(); }
    else if (!p && b->paused) { b->paused = 0; b->t0 += H_.now_us() - b->pause_t; }
    return b->paused;
}
uint32_t bk_BinkGetRects(const uint32_t *a) {
    Bk *b = find(a[0]); if (!b) return 0;
    W32(b->g + 56, 0); W32(b->g + 60, 0); W32(b->g + 64, (uint32_t)b->info.width); W32(b->g + 68, (uint32_t)b->info.height); W32(b->g + 184, 1); return 1;
}
uint32_t bk_BinkSetVolume(const uint32_t *a) {      // (bink, trackid, volume 0..0x8000)
    Bk *b = find(a[0]); if (!b) return 1; int t = (int)a[1]; if (t < 0 || t >= BKFF_MAXTRACKS) t = BKFF_MAXTRACKS; b->vol[t] = (int)a[2]; return 1;
}

uint32_t bk_BinkOpenTrack(const uint32_t *a) {      // (bink, trackindex) -> BINKTRACK*
    Bk *b = find(a[0]); if (!b || (int)a[1] < 0 || (int)a[1] >= b->info.ntracks) return 0;
    int t = (int)a[1]; if (b->tr_h[t]) return b->tr_h[t]; if (ntr >= MAXTR) return 0;
    uint32_t g = H_.alloc(32); memset(G + g, 0, 32);
    W32(g, (uint32_t)b->info.tracks[t].rate); W32(g + 4, 16); W32(g + 8, (uint32_t)b->info.tracks[t].channels); W32(g + 12, TRACK_MAX); W32(g + 16, b->g); W32(g + 24, (uint32_t)t);
    trs[ntr].g = g; trs[ntr].bk = (int)(b - bks); trs[ntr].track = t; ntr++; b->tr_h[t] = g;
    LOG("BinkOpenTrack(%08x, %d) -> %08x (%d Hz, %d ch)\n", b->g, t, g, b->info.tracks[t].rate, b->info.tracks[t].channels); return g;
}
static int tfind(uint32_t g) { for (int i = 0; i < ntr; i++) if (trs[i].g == g) return i; return -1; }
uint32_t bk_BinkGetTrackData(const uint32_t *a) {   // (track, dest) -> bytes written (<= MaxSize)
    int i = tfind(a[0]); if (i < 0 || !a[1]) return 0; Bk *b = &bks[trs[i].bk]; if (!b->used) return 0;
    return (uint32_t)bkff_track_read(b->ff, trs[i].track, G + a[1], TRACK_MAX);
}
uint32_t bk_BinkGetTrackID(const uint32_t *a) {     // (bink, trackindex) -> track id from the file header
    Bk *b = find(a[0]); if (!b || (int)a[1] < 0 || (int)a[1] >= b->info.ntracks) return 0; return (uint32_t)b->info.tracks[a[1]].id;
}
uint32_t bk_BinkCloseTrack(const uint32_t *a) {
    int i = tfind(a[0]); if (i < 0) return 0; Bk *b = &bks[trs[i].bk]; if (b->used) b->tr_h[trs[i].track] = 0;
    trs[i] = trs[--ntr]; return 0;
}
