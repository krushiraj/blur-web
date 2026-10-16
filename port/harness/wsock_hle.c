// Winsock + IP Helper HLE for the Blur harness: emulated UDP sockets on a "virtual LAN".
//
// Design (see NETCODE.md section 6):
//   * every guest socket is an entry in a small table; nothing touches a real network except ONE host UDP socket that talks to a relay
//     (BLUR_NET_RELAY=host:port, default 127.0.0.1:47800) using the 16-byte framing below. The same framing is what the browser build will
//     carry over a WebSocket / WebRTC data channel.
//   * each instance has a virtual IPv4 address 10.77.0.<BLUR_NET_ID> (default 1). Guest-visible addresses are always virtual, so two instances
//     on one machine never collide on ports and the game's GetAdaptersInfo / bdCommonAddr logic sees a normal "LAN".
//   * 255.255.255.255 and 10.77.0.255 are broadcast: delivered to every socket bound to the destination port in every instance, INCLUDING the
//     sender (Windows loops broadcasts back; Blur's LAN discovery relies on hosts also answering themselves / ignoring their own nonce).
//   * TCP is not provided: socket(SOCK_STREAM) succeeds, connect() fails with WSAECONNREFUSED, so Demonware auth/lobby/curl fail fast and the
//     game stays in offline/LAN mode.
//   * Blur only uses non-blocking UDP (ioctlsocket FIONBIO) polled once per frame by bdSocket; no select()/WSAEventSelect on that path.
//
// Integration: in hle2.c, `hle2_lookup()` should first try `wsock_lookup(name)`; remove the placeholder winsock rows from `real[]`.
// Handlers use only rd*/wr*/rdbuf/wrbuf/guest_alloc/cur_thread from emu.h.
// Two host back-ends behind host_send()/host_recv() (a "frame pipe" of 16-byte-header datagrams, see FR_* below):
//   native  : one UDP socket to the relay / the lobby server's UDP gateway (default)
//   wasm    : __EMSCRIPTEN__ -> JS imports on `Module.blurLan`, a client/virtual-lan.js instance in raw mode (enableRaw, rawSendFrame, rawRecvFrame)
// Everything guest-visible (socket table, errors, broadcast loop-back, GetAdaptersInfo) is identical in both builds.
#include "emu.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(int, js_lan_send, (const uint8_t *p, int n), { const l = Module.blurLan; return l && l.rawSendFrame(HEAPU8.subarray(p, p + n)) ? 1 : 0; });
EM_JS(int, js_lan_recv, (uint8_t *p, int cap), { const l = Module.blurLan; return l ? l.rawRecvFrame(HEAPU8.subarray(p, p + cap)) : -1; });
EM_JS(unsigned, js_seed, (), { const a = new Uint32Array(1); globalThis.crypto.getRandomValues(a); return a[0]; });   // the harness' CryptGenRandom is libc rand(): without this every browser would produce the same nonces / DH keys
// virtual IP of the browser peer as the 4 network-order bytes read as a little-endian u32 (0 = no room / library not attached)
EM_JS(unsigned, js_lan_vip, (), { const l = Module.blurLan; if (!l || !l.vip) return 0; const q = l.vip.split('.').map(Number); return (q[0] | (q[1] << 8) | (q[2] << 16) | (q[3] << 24)) >>> 0; });
#endif

#define H(n) static uint32_t h_##n(Ctx *c, uint32_t *a)
#define UNUSED (void)c; (void)a

enum {  // winsock error codes
    WSAEINTR_ = 10004, WSAEBADF_ = 10009, WSAEINVAL_ = 10022, WSAEWOULDBLOCK_ = 10035, WSAENOTSOCK_ = 10038, WSAEMSGSIZE_ = 10040,
    WSAEOPNOTSUPP_ = 10045, WSAEAFNOSUPPORT_ = 10047, WSAEADDRINUSE_ = 10048, WSAENOTCONN_ = 10057, WSAECONNREFUSED_ = 10061,
    WSAEDESTADDRREQ_ = 10039, WSAHOST_NOT_FOUND_ = 11001,
};
#define SOCKET_ERROR_ 0xffffffffu
#define INVALID_SOCKET_ 0xffffffffu
#define MAX_SOCK 256
#define RXQ 256
#define MAXDG 2048
#define SOCK_BASE 0x1c0u               // guest SOCKET value = SOCK_BASE + 4 * index (never 0 / -1)
#define PORT_DEFAULT_RELAY 47800

// ---- relay framing (all multi-byte fields are network order) ---------------------------------------------------------------------------
//  0  'B','N'  2 ver=1  3 type (1 HELLO, 2 DATA, 3 BYE)  4 src_ip[4]  8 src_port  10 dst_port  12 dst_ip[4]  16.. payload
#define FR_HDR 16
enum { FR_HELLO = 1, FR_DATA = 2, FR_BYE = 3, FR_WELCOME = 4, FR_ERROR = 5 };

typedef struct { uint32_t ip; uint16_t port; uint16_t len; uint8_t data[MAXDG]; } Dgram;       // ip/port: sender (network order ip, host order port)
typedef struct {
    int used, type, nonblock, bcast, reuse, listening;
    uint32_t ip; uint16_t port; int bound;    // ip network order (0 = INADDR_ANY), port host order
    Dgram *q; int qh, qn;                     // ring buffer, allocated lazily
    uint32_t peer_ip; uint16_t peer_port;     // connect() on UDP
} Sock;

static struct {
    pthread_mutex_t mu; int init;
    Sock s[MAX_SOCK];
    uint32_t vip;                             // network order
    int rfd; struct sockaddr_in raddr; int relay_ok; uint64_t last_hello;
    uint16_t next_eph;
    uint32_t hostent, ntoa_buf; int started;
    uint64_t tx_dg, rx_dg, drop_dg;
    char hello[300]; int joined;               // BLUR_NET_ROOM: payload of the HELLO frames (room=CODE;name=..;pw=..) once joined via the lobby server's UDP gateway
} W = { .mu = PTHREAD_MUTEX_INITIALIZER };

static ssize_t host_send(const void *f, size_t n) {
#ifdef __EMSCRIPTEN__
    return js_lan_send((const uint8_t *)f, (int)n) ? (ssize_t)n : -1;
#else
    return sendto(W.rfd, f, n, 0, (struct sockaddr *)&W.raddr, sizeof W.raddr);
#endif
}
static ssize_t host_recv(void *f, size_t cap) {
#ifdef __EMSCRIPTEN__
    return js_lan_recv((uint8_t *)f, (int)cap);
#else
    return recv(W.rfd, f, cap, 0);
#endif
}
static __thread uint32_t t_err;
static void set_wsa(Ctx *c, uint32_t e) { t_err = e; wr32(c, cur_thread(c)->teb + 0x34, e); }
static int bcast_addr(uint32_t ip_n) { return ip_n == 0xffffffffu || (ip_n & 0xff000000u) == 0xff000000u; }   // 255.255.255.255 or x.x.x.255 (LE view of network order: top byte last)

static uint64_t mono_us(void) { struct timeval tv; gettimeofday(&tv, NULL); return (uint64_t)tv.tv_sec * 1000000ull + tv.tv_usec; }

static void relay_send(int type, uint32_t sip, uint16_t sport, uint16_t dport, uint32_t dip, const void *p, size_t n);
static void bye_at_exit(void);
static void join_room_locked(void);
static void net_init_locked(void) {
    if (W.init) return;
    W.init = 1; W.rfd = -1; W.next_eph = 49152;
#ifdef __EMSCRIPTEN__
    { srand(js_seed()); unsigned v = js_lan_vip(); W.vip = v ? v : htonl((10u << 24) | (77u << 16) | 1u); W.relay_ok = 1;
      fprintf(stderr, "[wsock] browser host, virtual ip %u.%u.%u.%u\n", W.vip & 255, (W.vip >> 8) & 255, (W.vip >> 16) & 255, W.vip >> 24); return; }
#endif
    int id = getenv("BLUR_NET_ID") ? atoi(getenv("BLUR_NET_ID")) : 1;
    if (id < 1 || id > 254) id = 1;
    W.vip = htonl((10u << 24) | (77u << 16) | (0u << 8) | (uint32_t)id);
    if (getenv("BLUR_NET_ID")) srand((unsigned)(time(NULL) * 2654435761u) ^ ((unsigned)getpid() << 8) ^ (unsigned)id);   // multi-instance: the harness' CryptGenRandom uses libc rand(), never seeded -> identical nonces/DH keys otherwise
    const char *r = getenv("BLUR_NET_RELAY");           // "host:port"; "off" disables the relay (in-process loopback only)
    if (r && !strcmp(r, "off")) return;
    char host[128] = "127.0.0.1"; int port = PORT_DEFAULT_RELAY;
    if (r) { const char *col = strrchr(r, ':'); if (col) { snprintf(host, sizeof host, "%.*s", (int)(col - r), r); port = atoi(col + 1); } else snprintf(host, sizeof host, "%s", r); }
    W.rfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (W.rfd < 0) return;
    fcntl(W.rfd, F_SETFL, fcntl(W.rfd, F_GETFL) | O_NONBLOCK);
    memset(&W.raddr, 0, sizeof W.raddr); W.raddr.sin_family = AF_INET; W.raddr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &W.raddr.sin_addr) != 1) { close(W.rfd); W.rfd = -1; return; }
    W.relay_ok = 1;
    if (getenv("BLUR_NET_ROOM") && *getenv("BLUR_NET_ROOM")) join_room_locked();
    fprintf(stderr, "[wsock] virtual ip %u.%u.%u.%u relay %s:%d%s\n", W.vip & 255, (W.vip >> 8) & 255, (W.vip >> 16) & 255, W.vip >> 24, host, port, W.joined ? " (joined lobby room)" : "");
}

static void netlog(const char *dir, uint32_t sip, uint16_t sport, uint32_t dip, uint16_t dport, const uint8_t *p, size_t n) {
    static int on = -1; if (on < 0) on = getenv("BLUR_NETLOG") != NULL;       // BLUR_NETLOG=1: one line per datagram (tx/rx/loop), first 24 bytes in hex
    if (!on) return;
    fprintf(stderr, "[wsock %s] %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u %zuB ", dir, sip & 255, (sip >> 8) & 255, (sip >> 16) & 255, sip >> 24, sport,
            dip & 255, (dip >> 8) & 255, (dip >> 16) & 255, dip >> 24, dport, n);
    for (size_t i = 0; i < n && i < 24; i++) fprintf(stderr, "%02x", p[i]);
    fputc('\n', stderr);
}

static void relay_send(int type, uint32_t sip, uint16_t sport, uint16_t dport, uint32_t dip, const void *p, size_t n) {
    if (!W.relay_ok || n > MAXDG) return;
    uint8_t f[FR_HDR + MAXDG];
    f[0] = 'B'; f[1] = 'N'; f[2] = 1; f[3] = (uint8_t)type;
    memcpy(f + 4, &sip, 4); f[8] = sport >> 8; f[9] = sport & 255; f[10] = dport >> 8; f[11] = dport & 255; memcpy(f + 12, &dip, 4);
    if (n) memcpy(f + FR_HDR, p, n);
#ifdef __EMSCRIPTEN__
    if (type != FR_DATA) return;                          // HELLO/BYE are lobby-server business; the page joins rooms through the library
#endif
    host_send(f, FR_HDR + n);
}

// Join a lobby-server room through its UDP gateway: HELLO(payload "room=CODE;name=..;pw=..[;create=1;max=N;public=0|1]") -> WELCOME(vip[4], slot[1], code...).
// The server assigns the virtual IP (10.77.0.<slot>); it overrides BLUR_NET_ID. Waits up to 2 s; on failure we stay on the local-only vip.
static void bye_at_exit(void) { if (W.joined) relay_send(FR_BYE, W.vip, 0, 0, 0, NULL, 0); }
static void join_room_locked(void) {
    const char *room = getenv("BLUR_NET_ROOM"), *name = getenv("BLUR_NET_NAME"), *pw = getenv("BLUR_NET_PW"), *create = getenv("BLUR_NET_CREATE");
    if (create && *create == '1') snprintf(W.hello, sizeof W.hello, "create=1;name=%s;pw=%s;max=%s", name ? name : "native", pw ? pw : "", getenv("BLUR_NET_MAX") ? getenv("BLUR_NET_MAX") : "20");
    else snprintf(W.hello, sizeof W.hello, "room=%s;name=%s;pw=%s", room, name ? name : "native", pw ? pw : "");
    relay_send(FR_HELLO, W.vip, 0, 0, 0, W.hello, strlen(W.hello));
    uint8_t f[FR_HDR + 64];
    for (int i = 0; i < 100; i++) {                  // 100 x 20 ms
        ssize_t n = host_recv(f, sizeof f);
        if (n >= FR_HDR && f[0] == 'B' && f[1] == 'N' && f[2] == 1) {
            if (f[3] == FR_WELCOME && n >= FR_HDR + 5) { memcpy(&W.vip, f + FR_HDR, 4); W.joined = 1; atexit(bye_at_exit); fprintf(stderr, "[wsock] lobby welcome: slot %d room %.*s\n", f[FR_HDR + 4], (int)(n - FR_HDR - 5), f + FR_HDR + 5); return; }
            if (f[3] == FR_ERROR) { fprintf(stderr, "[wsock] lobby join refused: %.*s\n", (int)(n - FR_HDR), f + FR_HDR); W.hello[0] = 0; return; }
        }
        usleep(20000);
    }
    fprintf(stderr, "[wsock] lobby join timed out (is UDP_PORT enabled on the server?)\n"); W.hello[0] = 0;
}

static void enqueue(Sock *s, uint32_t sip, uint16_t sport, const uint8_t *p, size_t n) {
    if (!s->q) s->q = calloc(RXQ, sizeof(Dgram));
    if (s->qn == RXQ) { s->qh = (s->qh + 1) % RXQ; s->qn--; W.drop_dg++; }           // overflow: drop the oldest
    Dgram *d = &s->q[(s->qh + s->qn) % RXQ];
    d->ip = sip; d->port = sport; d->len = (uint16_t)n; memcpy(d->data, p, n); s->qn++;
}
// deliver one datagram to local sockets (broadcast: every matching socket incl. the sender's; unicast: the most recently created match)
static int local_deliver(uint32_t sip, uint16_t sport, uint16_t dport, uint32_t dip, const uint8_t *p, size_t n) {
    int hits = 0, pick = -1, bc = bcast_addr(dip);
    for (int i = 0; i < MAX_SOCK; i++) {
        Sock *s = &W.s[i];
        if (!s->used || s->type != 2 || !s->bound || s->port != dport) continue;
        if (!bc && s->ip != 0 && s->ip != dip) continue;
        if (bc) { enqueue(s, sip, sport, p, n); hits++; } else pick = i;
    }
    if (!bc && pick >= 0) { enqueue(&W.s[pick], sip, sport, p, n); hits = 1; }
    return hits;
}

static void pump_locked(void) {
    net_init_locked();
    if (!W.relay_ok) return;
    uint64_t t = mono_us();
    if (t - W.last_hello > 1000000) { W.last_hello = t; relay_send(FR_HELLO, W.vip, 0, 0, 0, W.hello, strlen(W.hello)); }
    uint8_t f[FR_HDR + MAXDG + 64];
    for (int k = 0; k < 4096; k++) {
        ssize_t n = host_recv(f, sizeof f);
        if (n < FR_HDR) break;
        if (f[0] != 'B' || f[1] != 'N' || f[2] != 1 || f[3] != FR_DATA) continue;
        uint32_t sip, dip; memcpy(&sip, f + 4, 4); memcpy(&dip, f + 12, 4);
        uint16_t sport = (f[8] << 8) | f[9], dport = (f[10] << 8) | f[11];
        W.rx_dg++; netlog("rx", sip, sport, dip, dport, f + FR_HDR, (size_t)n - FR_HDR);
        local_deliver(sip, sport, dport, dip, f + FR_HDR, (size_t)n - FR_HDR);
    }
}

static Sock *getsock(Ctx *c, uint32_t h) {
    if (h < SOCK_BASE || (h - SOCK_BASE) % 4 || (h - SOCK_BASE) / 4 >= MAX_SOCK) { set_wsa(c, WSAENOTSOCK_); return NULL; }
    Sock *s = &W.s[(h - SOCK_BASE) / 4];
    if (!s->used) { set_wsa(c, WSAENOTSOCK_); return NULL; }
    return s;
}
static void sock_free(Sock *s) { free(s->q); memset(s, 0, sizeof *s); }

static void rd_sockaddr(Ctx *c, uint32_t p, uint32_t *ip, uint16_t *port) {
    uint16_t fam = rd16(c, p); (void)fam;
    uint16_t be = rd16(c, p + 2); *port = (uint16_t)((be << 8) | (be >> 8));
    *ip = rd32(c, p + 4);                    // already network order in memory
}
static void wr_sockaddr(Ctx *c, uint32_t p, uint32_t ip, uint16_t port) {
    uint8_t b[16] = {0}; b[0] = 2; b[2] = port >> 8; b[3] = port & 255; memcpy(b + 4, &ip, 4); wrbuf(c, p, b, 16);
}
static int ephemeral_locked(void) {
    for (int k = 0; k < 16384; k++) {
        uint16_t p = W.next_eph++; if (W.next_eph == 0) W.next_eph = 49152; if (p < 49152) p = 49152;
        int used = 0; for (int i = 0; i < MAX_SOCK; i++) if (W.s[i].used && W.s[i].bound && W.s[i].port == p) used = 1;
        if (!used) return p;
    }
    return 0;
}

// ---- API ----------------------------------------------------------------------------------------------------------------------------------
H(WSAStartup) { uint8_t z[400] = {0}; wrbuf(c, a[1], z, sizeof z); wr16(c, a[1], 0x0202); wr16(c, a[1] + 2, 0x0202);
    pthread_mutex_lock(&W.mu); net_init_locked(); W.started++; pthread_mutex_unlock(&W.mu); return 0; }
H(WSACleanup) { UNUSED; pthread_mutex_lock(&W.mu); if (W.started > 0) W.started--; pthread_mutex_unlock(&W.mu); return 0; }
H(WSAGetLastError) { UNUSED; return t_err; }
H(WSASetLastError) { set_wsa(c, a[0]); return 0; }
H(htons) { UNUSED; return ((a[0] & 0xff) << 8) | ((a[0] >> 8) & 0xff); }
H(htonl) { UNUSED; uint32_t v = a[0]; return (v << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24); }

H(socket) {
    pthread_mutex_lock(&W.mu); net_init_locked();
    uint32_t r = INVALID_SOCKET_;
    if (a[0] != 2 && a[0] != 0) { set_wsa(c, WSAEAFNOSUPPORT_); goto out; }
    if (a[1] != 1 && a[1] != 2) { set_wsa(c, WSAEOPNOTSUPP_); goto out; }
    for (int i = 0; i < MAX_SOCK; i++) if (!W.s[i].used) { W.s[i].used = 1; W.s[i].type = (int)a[1]; r = SOCK_BASE + 4u * (uint32_t)i; break; }
    if (r == INVALID_SOCKET_) set_wsa(c, 10024);   // WSAEMFILE
out: pthread_mutex_unlock(&W.mu); return r;
}
H(closesocket) {
    pthread_mutex_lock(&W.mu); Sock *s = getsock(c, a[0]); uint32_t r = SOCKET_ERROR_;
    if (s) { sock_free(s); r = 0; }
    pthread_mutex_unlock(&W.mu); return r;
}
H(shutdown) { pthread_mutex_lock(&W.mu); Sock *s = getsock(c, a[0]); uint32_t r = s ? 0 : SOCKET_ERROR_; pthread_mutex_unlock(&W.mu); return r; }

H(bind) {
    pthread_mutex_lock(&W.mu); uint32_t r = SOCKET_ERROR_; Sock *s = getsock(c, a[0]);
    if (!s) goto out;
    uint32_t ip; uint16_t port; rd_sockaddr(c, a[1], &ip, &port);
    if (s->bound) { set_wsa(c, WSAEINVAL_); goto out; }
    if (ip != 0 && ip != W.vip && !bcast_addr(ip) && ip != htonl(0x7f000001)) { set_wsa(c, 10049); goto out; }    // WSAEADDRNOTAVAIL
    if (ip == htonl(0x7f000001)) ip = W.vip;
    if (port == 0) port = (uint16_t)ephemeral_locked();
    else for (int i = 0; i < MAX_SOCK; i++) { Sock *o = &W.s[i]; if (o != s && o->used && o->type == s->type && o->bound && o->port == port && (o->ip == ip || o->ip == 0 || ip == 0) && !(o->reuse && s->reuse)) { set_wsa(c, WSAEADDRINUSE_); goto out; } }
    s->ip = ip; s->port = port; s->bound = 1; r = 0;
out: pthread_mutex_unlock(&W.mu); return r;
}
H(connect) {
    pthread_mutex_lock(&W.mu); uint32_t r = SOCKET_ERROR_; Sock *s = getsock(c, a[0]);
    if (!s) goto out;
    if (s->type == 1) { set_wsa(c, WSAECONNREFUSED_); goto out; }       // TCP: no backend reachable
    uint32_t ip; uint16_t port; rd_sockaddr(c, a[1], &ip, &port); s->peer_ip = ip; s->peer_port = port; r = 0;
out: pthread_mutex_unlock(&W.mu); return r;
}
H(listen) { UNUSED; set_wsa(c, WSAEOPNOTSUPP_); return SOCKET_ERROR_; }
H(accept) { UNUSED; set_wsa(c, WSAEOPNOTSUPP_); return INVALID_SOCKET_; }
H(getpeername) { pthread_mutex_lock(&W.mu); Sock *s = getsock(c, a[0]); uint32_t r = SOCKET_ERROR_;
    if (s && s->peer_port) { wr_sockaddr(c, a[1], s->peer_ip, s->peer_port); wr32(c, a[2], 16); r = 0; } else if (s) set_wsa(c, WSAENOTCONN_);
    pthread_mutex_unlock(&W.mu); return r; }
H(getsockname) { pthread_mutex_lock(&W.mu); Sock *s = getsock(c, a[0]); uint32_t r = SOCKET_ERROR_;
    if (s) { wr_sockaddr(c, a[1], s->bound ? s->ip : 0, s->bound ? s->port : 0); wr32(c, a[2], 16); r = 0; }
    pthread_mutex_unlock(&W.mu); return r; }

static int do_sendto(Ctx *c, Sock *s, uint32_t buf, uint32_t len, uint32_t dip, uint16_t dport) {
    if (len > MAXDG - 1) { set_wsa(c, WSAEMSGSIZE_); return -1; }
    if (!s->bound) { s->ip = 0; s->port = (uint16_t)ephemeral_locked(); s->bound = 1; }
    uint8_t tmp[MAXDG]; rdbuf(c, buf, tmp, len);
    uint32_t sip = W.vip;
    if (dip == htonl(0x7f000001)) dip = W.vip;
    W.tx_dg++; netlog("tx", sip, s->port, dip, dport, tmp, len);
    if (bcast_addr(dip)) { local_deliver(sip, s->port, dport, dip, tmp, len); relay_send(FR_DATA, sip, s->port, dport, dip, tmp, len); }
    else if (dip == W.vip) local_deliver(sip, s->port, dport, dip, tmp, len);
    else relay_send(FR_DATA, sip, s->port, dport, dip, tmp, len);
    return (int)len;
}
H(sendto) {         // sendto(s, buf, len, flags, to, tolen)
    pthread_mutex_lock(&W.mu); pump_locked(); uint32_t r = SOCKET_ERROR_; Sock *s = getsock(c, a[0]);
    if (!s) goto out;
    if (s->type != 2) { set_wsa(c, WSAENOTCONN_); goto out; }
    uint32_t ip; uint16_t port; rd_sockaddr(c, a[4], &ip, &port);
    if (bcast_addr(ip) && !s->bcast) { set_wsa(c, 10013); goto out; }   // WSAEACCES, as on Windows without SO_BROADCAST
    { int n = do_sendto(c, s, a[1], a[2], ip, port); if (n >= 0) r = (uint32_t)n; }
out: pthread_mutex_unlock(&W.mu); return r;
}
H(send) {
    pthread_mutex_lock(&W.mu); uint32_t r = SOCKET_ERROR_; Sock *s = getsock(c, a[0]);
    if (!s) goto out;
    if (s->type != 2 || !s->peer_port) { set_wsa(c, WSAENOTCONN_); goto out; }
    { int n = do_sendto(c, s, a[1], a[2], s->peer_ip, s->peer_port); if (n >= 0) r = (uint32_t)n; }
out: pthread_mutex_unlock(&W.mu); return r;
}
H(recvfrom) {       // recvfrom(s, buf, len, flags, from, fromlen)
    pthread_mutex_lock(&W.mu); pump_locked(); uint32_t r = SOCKET_ERROR_; Sock *s = getsock(c, a[0]);
    if (!s) goto out;
    if (s->type != 2) { set_wsa(c, WSAENOTCONN_); goto out; }
    if (!s->bound) { set_wsa(c, WSAEINVAL_); goto out; }
    if (!s->qn) {
        if (s->nonblock) { set_wsa(c, WSAEWOULDBLOCK_); goto out; }
        c->block = 1; r = 0; goto out;                                          // blocking socket: retry after a thread switch
    }
    { Dgram *d = &s->q[s->qh]; uint32_t n = d->len, copy = n < a[2] ? n : a[2];
      wrbuf(c, a[1], d->data, copy);
      if (a[4]) { wr_sockaddr(c, a[4], d->ip, d->port); if (a[5]) wr32(c, a[5], 16); }
      if (!(a[3] & 2)) { s->qh = (s->qh + 1) % RXQ; s->qn--; }                  // MSG_PEEK = 2
      if (copy < n) { set_wsa(c, WSAEMSGSIZE_); r = SOCKET_ERROR_; } else r = copy; }
out: pthread_mutex_unlock(&W.mu); return r;
}
H(recv) { uint32_t b[6] = { a[0], a[1], a[2], a[3], 0, 0 }; return h_recvfrom(c, b); }

H(ioctlsocket) {    // FIONBIO 0x8004667e, FIONREAD 0x4004667f
    pthread_mutex_lock(&W.mu); pump_locked(); uint32_t r = SOCKET_ERROR_; Sock *s = getsock(c, a[0]);
    if (!s) goto out;
    if (a[1] == 0x8004667eu) { s->nonblock = rd32(c, a[2]) != 0; r = 0; }
    else if (a[1] == 0x4004667fu) { wr32(c, a[2], s->qn ? s->q[s->qh].len : 0); r = 0; }
    else set_wsa(c, WSAEINVAL_);
out: pthread_mutex_unlock(&W.mu); return r;
}
H(setsockopt) {     // setsockopt(s, level, opt, val*, len)
    pthread_mutex_lock(&W.mu); Sock *s = getsock(c, a[0]); uint32_t r = SOCKET_ERROR_;
    if (s) { r = 0; if (a[1] == 0xffff) { uint32_t v = a[4] >= 4 ? rd32(c, a[3]) : (a[4] ? rd8(c, a[3]) : 0); if (a[2] == 0x20) s->bcast = v != 0; else if (a[2] == 4) s->reuse = v != 0; } }
    pthread_mutex_unlock(&W.mu); return r;
}
H(getsockopt) {     // getsockopt(s, level, opt, val*, len*)
    pthread_mutex_lock(&W.mu); Sock *s = getsock(c, a[0]); uint32_t r = SOCKET_ERROR_;
    if (s) { uint32_t v = 0; if (a[1] == 0xffff) { if (a[2] == 0x1008) v = (uint32_t)s->type; else if (a[2] == 0x20) v = s->bcast; else if (a[2] == 4) v = s->reuse; else if (a[2] == 0x1001 || a[2] == 0x1002) v = 65536; }
      if (a[3]) wr32(c, a[3], v); if (a[4]) wr32(c, a[4], 4); r = 0; }
    pthread_mutex_unlock(&W.mu); return r;
}

// fd_set (Win32): u_int fd_count; SOCKET fd_array[64]. select() here is a non-blocking poll (timeout ignored): readable == datagram queued.
H(select) {         // select(nfds, readfds, writefds, exceptfds, timeout)
    pthread_mutex_lock(&W.mu); pump_locked(); uint32_t ready = 0;
    uint32_t sets[3] = { a[1], a[2], a[3] };
    for (int k = 0; k < 3; k++) {
        if (!sets[k]) continue;
        uint32_t n = rd32(c, sets[k]), keep[64], nk = 0; if (n > 64) n = 64;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t h = rd32(c, sets[k] + 4 + 4 * i); Sock *s = (h >= SOCK_BASE && (h - SOCK_BASE) % 4 == 0 && (h - SOCK_BASE) / 4 < MAX_SOCK) ? &W.s[(h - SOCK_BASE) / 4] : NULL;
            if (!s || !s->used) continue;
            int rdy = (k == 0) ? (s->qn > 0) : (k == 1) ? (s->type == 2) : 0;
            if (rdy) keep[nk++] = h;
        }
        wr32(c, sets[k], nk); for (uint32_t i = 0; i < nk; i++) wr32(c, sets[k] + 4 + 4 * i, keep[i]);
        ready += nk;
    }
    pthread_mutex_unlock(&W.mu); return ready;
}
H(__WSAFDIsSet) { uint32_t n = rd32(c, a[1]); if (n > 64) n = 64; for (uint32_t i = 0; i < n; i++) if (rd32(c, a[1] + 4 + 4 * i) == a[0]) return 1; return 0; }
H(WSAEventSelect) { UNUSED; return SOCKET_ERROR_; }
H(WSAEnumNetworkEvents) { UNUSED; return SOCKET_ERROR_; }
H(WSACloseEvent) { UNUSED; return 1; }

// ---- addresses / names ------------------------------------------------------------------------------------------------------------------
static uint32_t parse_ip(const char *s) {   // dotted quad -> network order, 0xffffffff if invalid (inet_addr semantics, incl. 255.255.255.255 -> -1)
    unsigned b[4]; char x;
    if (sscanf(s, "%u.%u.%u.%u%c", &b[0], &b[1], &b[2], &b[3], &x) != 4) return 0xffffffffu;
    for (int i = 0; i < 4; i++) if (b[i] > 255) return 0xffffffffu;
    return (uint32_t)(b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24));
}
H(inet_addr) { char s[64]; rd_cstr(c, a[0], s, sizeof s); return parse_ip(s); }
H(inet_ntoa) {      // a[0] = in_addr by value (network order)
    pthread_mutex_lock(&W.mu); if (!W.ntoa_buf) W.ntoa_buf = guest_alloc(c, 32);
    char s[20]; snprintf(s, sizeof s, "%u.%u.%u.%u", a[0] & 255, (a[0] >> 8) & 255, (a[0] >> 16) & 255, a[0] >> 24);
    wrbuf(c, W.ntoa_buf, s, strlen(s) + 1); uint32_t r = W.ntoa_buf; pthread_mutex_unlock(&W.mu); return r;
}
H(gethostname) { pthread_mutex_lock(&W.mu); net_init_locked(); char n[32]; snprintf(n, sizeof n, "BLUR-PC-%u", W.vip >> 24); pthread_mutex_unlock(&W.mu);
    wrbuf(c, a[0], n, strlen(n) + 1); return 0; }
H(gethostbyname) {  // only numeric hosts, "localhost" and our own name resolve; everything else (Demonware, STUN) fails like a dead DNS
    char n[256]; rd_cstr(c, a[0], n, sizeof n);
    pthread_mutex_lock(&W.mu); net_init_locked();
    uint32_t ip = parse_ip(n); char mine[32]; snprintf(mine, sizeof mine, "BLUR-PC-%u", W.vip >> 24);
    if (ip == 0xffffffffu && (!strcasecmp(n, "localhost") || !strcasecmp(n, mine))) ip = W.vip;
    uint32_t r = 0;
    if (ip == 0xffffffffu) set_wsa(c, WSAHOST_NOT_FOUND_);
    else {
        if (!W.hostent) W.hostent = guest_alloc(c, 128);
        uint32_t he = W.hostent;               // hostent{name,aliases,addrtype,length,addr_list} + storage
        wrbuf(c, he + 32, n, strlen(n) + 1); wr32(c, he + 96, ip); wr32(c, he + 104, he + 96); wr32(c, he + 108, 0);
        wr32(c, he, he + 32); wr32(c, he + 4, he + 108); wr16(c, he + 8, 2); wr16(c, he + 10, 4); wr32(c, he + 12, he + 104); r = he;
    }
    pthread_mutex_unlock(&W.mu); return r;
}

// GetAdaptersInfo(IP_ADAPTER_INFO*, ULONG*): one fake ethernet adapter carrying the virtual IP. IP_ADAPTER_INFO is 640 bytes (x86); IpAddressList at +0x1ac.
H(GetAdaptersInfo) {
    pthread_mutex_lock(&W.mu); net_init_locked(); uint32_t vip = W.vip; pthread_mutex_unlock(&W.mu);
    uint32_t have = a[1] ? rd32(c, a[1]) : 0;
    if (!a[0] || have < 640) { if (a[1]) wr32(c, a[1], 640); return 111; }              // ERROR_BUFFER_OVERFLOW
    uint8_t b[640] = {0}; char ip[16], nm[16] = "255.255.255.0"; snprintf(ip, sizeof ip, "%u.%u.%u.%u", vip & 255, (vip >> 8) & 255, (vip >> 16) & 255, vip >> 24);
    snprintf((char *)b + 8, 260, "{B1A2C3D4-0000-4000-8000-0000000000%02X}", vip >> 24);          // AdapterName
    snprintf((char *)b + 268, 132, "Blur Virtual LAN Adapter");                                        // Description
    uint32_t v;
    v = 6; memcpy(b + 400, &v, 4);                                                                     // AddressLength
    b[404] = 0x02; b[405] = 0x42; b[406] = 0x0a; b[407] = 0x4d; b[408] = 0; b[409] = (uint8_t)(vip >> 24);   // MAC
    v = 1; memcpy(b + 412, &v, 4);                                                                     // Index
    v = 6; memcpy(b + 416, &v, 4);                                                                     // Type = MIB_IF_TYPE_ETHERNET
    // DhcpEnabled(420)=0, CurrentIpAddress(424)=0
    memcpy(b + 0x1ac + 4, ip, strlen(ip) + 1); memcpy(b + 0x1ac + 20, nm, strlen(nm) + 1); v = 1; memcpy(b + 0x1ac + 36, &v, 4);   // IpAddressList{Next,IpAddress,IpMask,Context}
    memcpy(b + 0x1d4 + 4, "10.77.0.254", 12); memcpy(b + 0x1d4 + 20, "0.0.0.0", 8);               // GatewayList
    wrbuf(c, a[0], b, sizeof b); wr32(c, a[1], 640); return 0;
}

static const HleDef wtab[] = {
    { "WSAStartup", 2, 0, h_WSAStartup }, { "WSACleanup", 0, 0, h_WSACleanup }, { "WSAGetLastError", 0, 0, h_WSAGetLastError }, { "WSASetLastError", 1, 0, h_WSASetLastError },
    { "htons", 1, 0, h_htons }, { "ntohs", 1, 0, h_htons }, { "htonl", 1, 0, h_htonl }, { "ntohl", 1, 0, h_htonl },
    { "socket", 3, 0, h_socket }, { "closesocket", 1, 0, h_closesocket }, { "shutdown", 2, 0, h_shutdown }, { "bind", 3, 0, h_bind }, { "connect", 3, 0, h_connect },
    { "listen", 2, 0, h_listen }, { "accept", 3, 0, h_accept }, { "getpeername", 3, 0, h_getpeername }, { "getsockname", 3, 0, h_getsockname },
    { "sendto", 6, 0, h_sendto }, { "send", 4, 0, h_send }, { "recvfrom", 6, 0, h_recvfrom }, { "recv", 4, 0, h_recv },
    { "ioctlsocket", 3, 0, h_ioctlsocket }, { "setsockopt", 5, 0, h_setsockopt }, { "getsockopt", 5, 0, h_getsockopt },
    { "select", 5, 0, h_select }, { "__WSAFDIsSet", 2, 0, h___WSAFDIsSet }, { "WSAEventSelect", 3, 0, h_WSAEventSelect },
    { "WSAEnumNetworkEvents", 3, 0, h_WSAEnumNetworkEvents }, { "WSACloseEvent", 1, 0, h_WSACloseEvent },
    { "inet_addr", 1, 0, h_inet_addr }, { "inet_ntoa", 1, 0, h_inet_ntoa }, { "gethostname", 2, 0, h_gethostname }, { "gethostbyname", 1, 0, h_gethostbyname },
    { "GetAdaptersInfo", 2, 0, h_GetAdaptersInfo },
};
// Off unless BLUR_NET_ROOM or BLUR_NET_ID is set (integration policy): the default build keeps the old placeholder winsock rows (no sockets, no relay).
static int wsock_enabled(void) { static int on = -1; if (on < 0) on = (getenv("BLUR_NET_ROOM") && *getenv("BLUR_NET_ROOM")) || getenv("BLUR_NET_ID") || getenv("BLUR_NET_RELAY"); return on; }
const HleDef *wsock_lookup(const char *name) {
    if (!wsock_enabled()) return NULL;
    for (size_t i = 0; i < sizeof wtab / sizeof wtab[0]; i++) if (!strcmp(wtab[i].name, name)) return &wtab[i];
    return NULL;
}
