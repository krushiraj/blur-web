// A tiny "game" built on the REAL wsock_hle.c: joins a lobby room through the server's UDP gateway (BLUR_NET_ROOM) and behaves like the
// Blur LAN code paths we care about: binds 0.0.0.0:50001 with SO_BROADCAST + FIONBIO, answers broadcast queries (24 B "QUERY..." -> 160 B reply) and
// echoes PING datagrams. Used by native/run_interop.js against a browser client in the same room.
//   env: BLUR_NET_RELAY=127.0.0.1:<udp gateway port> BLUR_NET_ROOM=<code> [BLUR_NET_NAME] ; args: <seconds>
#include "emu.h"
#include <stdarg.h>
#include <sys/mman.h>
#include <unistd.h>

uint8_t *G; uint32_t g_hw_lo, g_hw_hi; const char *g_cur_api;
static Thread th; static uint32_t bump = 0x100000;
Thread *cur_thread(Ctx *c) { (void)c; return &th; }
uint32_t guest_alloc(Ctx *c, uint32_t n) { (void)c; uint32_t r = bump; bump += (n + 15) & ~15u; return r; }
int rd_cstr(Ctx *c, uint32_t a, char *out, int max) { int i = 0; for (; i < max - 1; i++) { char ch = (char)rd8(c, a + i); out[i] = ch; if (!ch) return i; } out[i] = 0; return i; }
const HleDef *wsock_lookup(const char *name);

static Ctx *C;
static uint32_t call(const char *n, int na, ...) {
    const HleDef *d = wsock_lookup(n); if (!d) { fprintf(stderr, "no handler %s\n", n); exit(2); }
    uint32_t a[8] = {0}; va_list ap; va_start(ap, na); for (int i = 0; i < na; i++) a[i] = va_arg(ap, uint32_t); va_end(ap);
    return d->fn(C, a);
}
#define P_SA 0x2000
#define P_BUF 0x3000
#define P_FROM 0x4000
#define P_LEN 0x4100
#define P_ONE 0x4200
static void put_sa(uint32_t p, uint32_t ip_n, int port) { uint8_t b[16] = {2, 0, (uint8_t)(port >> 8), (uint8_t)port}; memcpy(b + 4, &ip_n, 4); wrbuf(C, p, b, 16); }

int main(int argc, char **argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 10;
    G = mmap(NULL, 1 << 24, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    C = calloc(1, sizeof *C); uc_open(UC_ARCH_X86, UC_MODE_32, &C->uc); uc_mem_map_ptr(C->uc, 0, 1 << 24, UC_PROT_ALL, G);
    th.teb = 0x8000;
    call("WSAStartup", 2, 0x202u, (uint32_t)0x5000);
    wr32(C, 0x6000, 0x288); call("GetAdaptersInfo", 2, (uint32_t)0x6100, (uint32_t)0x6000);
    char ip[32]; rd_cstr(C, 0x6100 + 0x1ac + 4, ip, sizeof ip); fprintf(stderr, "[native] adapter ip %s\n", ip);
    uint32_t s = call("socket", 3, 2u, 2u, 17u);
    wr32(C, P_ONE, 1); call("setsockopt", 5, s, 0xffffu, 0x20u, (uint32_t)P_ONE, 4u); call("ioctlsocket", 3, s, 0x8004667eu, (uint32_t)P_ONE);
    put_sa(P_SA, 0, 50001); if (call("bind", 3, s, (uint32_t)P_SA, 16u) != 0) { fprintf(stderr, "bind failed\n"); return 3; }
    int queries = 0, pings = 0, other = 0, announced = 0; time_t end = time(NULL) + seconds; uint8_t buf[2048];
    while (time(NULL) < end) {
        wr32(C, P_LEN, 16);
        uint32_t r = call("recvfrom", 6, s, (uint32_t)P_BUF, 2000u, 0u, (uint32_t)P_FROM, (uint32_t)P_LEN);
        if ((int32_t)r < 0) { usleep(2000); continue; }
        rdbuf(C, P_BUF, buf, r); uint8_t sa[16]; rdbuf(C, P_FROM, sa, 16); uint32_t from_ip; memcpy(&from_ip, sa + 4, 4); int from_port = (sa[2] << 8) | sa[3];
        if (r == 24 && !memcmp(buf, "QUERY", 5)) {                         // discovery query -> 160 B reply (like b3e320)
            queries++; uint8_t rep[160] = {0}; memcpy(rep, "REPLY", 5); snprintf((char *)rep + 8, 40, "native@%s", ip);
            wrbuf(C, P_BUF, rep, 160); put_sa(P_SA, from_ip, from_port); call("sendto", 6, s, (uint32_t)P_BUF, 160u, 0u, (uint32_t)P_SA, 16u);
        } else if (r >= 4 && !memcmp(buf, "PING", 4)) {
            pings++; memcpy(buf, "PONG", 4); wrbuf(C, P_BUF, buf, r); put_sa(P_SA, from_ip, from_port); call("sendto", 6, s, (uint32_t)P_BUF, r, 0u, (uint32_t)P_SA, 16u);
        } else if (r >= 5 && !memcmp(buf, "START", 5)) {                    // browser says: now broadcast an announcement (tests native->browser broadcast)
            const char *m = "NATIVE-ANNOUNCE"; wrbuf(C, P_BUF, m, strlen(m)); put_sa(P_SA, 0xffffffffu, 50001); call("sendto", 6, s, (uint32_t)P_BUF, (uint32_t)strlen(m), 0u, (uint32_t)P_SA, 16u); announced++;
        } else other++;
    }
    printf("{\"native\":{\"ip\":\"%s\",\"queries\":%d,\"pings\":%d,\"announced\":%d,\"other\":%d}}\n", ip, queries, pings, announced, other);
    return 0;
}
