// wsock_hle.c compiled for WebAssembly (emscripten, wasm-core's emu.h: guest address == wasm address) driving a client/virtual-lan.js instance in raw mode
// through Module.blurLan. Used by native/run_wasm_hle.js: two wasm instances in one node process talk through the real lobby server (relay path).
//   run_test(mode, ms): mode 0 = responder (answers QUERY with a 160-byte REPLY, PING with PONG), mode 1 = querier (broadcast discovery + 10 ping-pongs)
#include "emu.h"
#include <emscripten.h>
#include <stdarg.h>

uint32_t g_hw_lo, g_hw_hi; const char *g_cur_api;
static Thread th;
Thread *cur_thread(Ctx *c) { (void)c; return &th; }
uint32_t guest_alloc(Ctx *c, uint32_t n) { (void)c; return (uint32_t)(uintptr_t)malloc(n + 16); }
int rd_cstr(Ctx *c, uint32_t a, char *out, int max) { int i = 0; for (; i < max - 1; i++) { char ch = (char)rd8(c, a + i); out[i] = ch; if (!ch) return i; } out[i] = 0; return i; }
const HleDef *wsock_lookup(const char *name);

static Ctx C0; static Ctx *C = &C0;
static uint32_t call(const char *n, int na, ...) {
    const HleDef *d = wsock_lookup(n); if (!d) { printf("no handler %s\n", n); exit(2); }
    uint32_t a[8] = {0}; va_list ap; va_start(ap, na); for (int i = 0; i < na; i++) a[i] = va_arg(ap, uint32_t); va_end(ap);
    return d->fn(C, a);
}
#define P(x) ((uint32_t)(uintptr_t)(x))
static uint8_t wsa[400], ainfo[1024], sa[16], from[16], buf[2048]; static uint32_t alen, flen, one;
static void put_sa(uint32_t ip_n, int port) { memset(sa, 0, 16); sa[0] = 2; sa[2] = port >> 8; sa[3] = port; memcpy(sa + 4, &ip_n, 4); }

EMSCRIPTEN_KEEPALIVE int run_test(int mode, int ms) {
    th.teb = P(malloc(256));
    call("WSAStartup", 2, 0x202u, P(wsa));
    alen = 0x288; if (call("GetAdaptersInfo", 2, P(ainfo), P(&alen)) != 0) { printf("GetAdaptersInfo failed\n"); return 1; }
    char ip[32]; snprintf(ip, sizeof ip, "%s", (char *)ainfo + 0x1ac + 4);
    uint32_t s = call("socket", 3, 2u, 2u, 17u);
    one = 1; call("setsockopt", 5, s, 0xffffu, 0x20u, P(&one), 4u); call("ioctlsocket", 3, s, 0x8004667eu, P(&one));
    put_sa(0, 50001); if (call("bind", 3, s, P(sa), 16u) != 0) { printf("bind failed\n"); return 2; }
    int queries = 0, pings = 0, pongs = 0, replies = 0; char reply_from[32] = "";
    double t0 = emscripten_get_now(), tq = -1; int sent_q = 0, sent_pings = 0; double last_ping = 0;
    while (emscripten_get_now() - t0 < ms) {
        double now = emscripten_get_now() - t0;
        if (mode == 1 && !sent_q && now > 400) { memset(buf, 0, 24); memcpy(buf, "QUERY", 5); put_sa(0xffffffffu, 50001); call("sendto", 6, s, P(buf), 24u, 0u, P(sa), 16u); sent_q = 1; tq = now; }
        if (mode == 1 && replies && sent_pings < 10 && now - last_ping > 30) {
            uint32_t rip; memcpy(&rip, from + 4, 4); memset(buf, 0, 40); memcpy(buf, "PING", 4); buf[4] = (uint8_t)sent_pings; put_sa(rip, 50001);
            call("sendto", 6, s, P(buf), 40u, 0u, P(sa), 16u); sent_pings++; last_ping = now;
        }
        flen = 16; uint32_t r = call("recvfrom", 6, s, P(buf), 2000u, 0u, P(from), P(&flen));
        if ((int32_t)r < 0) { emscripten_sleep(2); continue; }
        uint32_t fip; memcpy(&fip, from + 4, 4); int fport = (from[2] << 8) | from[3];
        if (mode == 0 && r == 24 && !memcmp(buf, "QUERY", 5)) { queries++; uint8_t rep[160] = {0}; memcpy(rep, "REPLY", 5); snprintf((char *)rep + 8, 40, "wasm@%s", ip); memcpy(buf, rep, 160); put_sa(fip, fport); call("sendto", 6, s, P(buf), 160u, 0u, P(sa), 16u); }
        else if (mode == 0 && r >= 4 && !memcmp(buf, "PING", 4)) { pings++; memcpy(buf, "PONG", 4); put_sa(fip, fport); call("sendto", 6, s, P(buf), r, 0u, P(sa), 16u); }
        else if (mode == 1 && r == 160 && !memcmp(buf, "REPLY", 5)) { replies++; snprintf(reply_from, sizeof reply_from, "%u.%u.%u.%u", fip & 255, (fip >> 8) & 255, (fip >> 16) & 255, fip >> 24); printf("{\"discoveryMs\":%.1f}\n", now - tq); }
        else if (mode == 1 && r == 40 && !memcmp(buf, "PONG", 4)) pongs++;
        if (mode == 1 && pongs >= 10) break;
    }
    printf("{\"mode\":%d,\"ip\":\"%s\",\"queries\":%d,\"pings\":%d,\"replies\":%d,\"replyFrom\":\"%s\",\"pongs\":%d}\n", mode, ip, queries, pings, replies, reply_from, pongs);
    return mode == 1 ? (replies == 1 && pongs == 10 ? 0 : 3) : 0;
}
