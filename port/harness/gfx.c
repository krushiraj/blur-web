// Render command stream to the browser. Every message: [u32 payload_len][u32 type][payload]. Messages are batched and
// flushed on Present (or when large). The browser implements a Direct3D 9 device on WebGL2 (web/renderer.js).
#include "emu.h"

static uint8_t *buf; static size_t blen, bcap;
int gfx_on(void) { return net_client_connected(); }
void gfx_flush(void) {
    if (blen) {
        static FILE *dump; static int init;                                   // BLUR_GFXDUMP=file: record the exact byte stream sent to the browser (for deterministic replay tests)
        if (!init) { init = 1; const char *p = getenv("BLUR_GFXDUMP"); if (p) dump = fopen(p, "wb"); }
        if (dump) { fwrite(buf, 1, blen, dump); fflush(dump); }
        net_send(buf, blen); blen = 0;
    }
}
static void reserve(size_t n) { if (blen + n > bcap) { bcap = (blen + n) * 2; buf = realloc(buf, bcap); } }
// header words (u32) followed by an optional byte blob
void gfx_msg(uint32_t type, const uint32_t *w, int nw, const void *data, uint32_t ndata) {
    if (!gfx_on()) return;
    uint32_t plen = 4u * (uint32_t)nw + ndata; reserve(8 + plen);
    uint32_t h[2] = {plen, type}; memcpy(buf + blen, h, 8); blen += 8;
    if (nw) { memcpy(buf + blen, w, 4u * (uint32_t)nw); blen += 4u * (uint32_t)nw; }
    if (ndata) { memcpy(buf + blen, data, ndata); blen += ndata; }
    if (blen > (8u << 20)) gfx_flush();
}
