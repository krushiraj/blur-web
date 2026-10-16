# Blur multiplayer backend prototype (virtual LAN)

Lobby + signalling + datagram relay for the browser port, built per `port/work/net-re/NETCODE.md` (sections 7-9) and `REPLICATION.md`. A **room is a virtual LAN** (`10.77.0.<slot>`,
up to 20 players). The game never talks to this server: a Winsock HLE (native/wasm: `port/work/net-re/patch/wsock_hle.c`, browser library: `client/virtual-lan.js`) turns the game's UDP sockets
into datagrams that travel peer-to-peer over WebRTC data channels, with the server's WebSocket/UDP relay as fallback. In-game `EnumerateLanGames` (UDP broadcast to 255.255.255.255:50001,
2.0 s window, <= 10 hosts) then works exactly as on a real LAN. Wiring into the wasm build: **`INTEGRATION.md`**.

```
 browser A --WebRTC (unordered, maxRetransmits 0)-- browser B          topology per room:  mesh (<= 8 players)  |  star (host = hub, > 8 players)
    |  \_______________ WebSocket /ws (JSON control + binary relay frames) ______/
    |                                    lobby server  (rooms, signalling, relay, moderation, metrics)
 native harness / wasm HLE ---- UDP gateway or Module.blurLan (same frames) ---------+
```
Everything is in memory; nothing is persisted. No external service is needed on localhost (host ICE candidates).

## Run
```
cd port/mp
yarn install
yarn start                      # http+ws on 127.0.0.1:8786, demo at http://127.0.0.1:8786/demo/
UDP_PORT=47800 yarn start       # additionally open the UDP gateway for native harness instances (BLUR_NET_RELAY=127.0.0.1:47800 BLUR_NET_ROOM=CODE)
yarn test                       # 31 unit/integration tests (frame, rooms, ICE, relay, heartbeat, UDP gateway, resume, kick/ban, abuse limits, admin API, metrics, shutdown)
yarn test:features              # headless Chromium: session resume, star topology (+ both fallbacks + host migration), kick/ban/unban
yarn test:browser               # 3 peers x 60 Hz x 60 s over WebRTC and relay + discovery emulation  (options: --peers 12 --topology star --seconds 60 --modes webrtc,relay --tag NAME)
yarn test:interop               # native wsock_hle process <-> browser client through the server's UDP gateway
yarn test:wasm                  # wsock_hle.c compiled with emcc (2 instances) <-> VirtualLan raw mode <-> server
yarn test:demo                  # smoke test of demo/index.html with two headless pages
```
Browser tests need `/Applications/Chromium.app` (override with `CHROMIUM=...`) and `emcc` for `test:wasm`.

### Configuration (environment)
| variable | default | meaning |
|---|---|---|
| `PORT` `HOST` | 8786, 127.0.0.1 | listen address (use `HOST=0.0.0.0` only behind a proxy/firewall) |
| `UDP_PORT` | unset | enable the UDP gateway for native instances (0 = ephemeral); keep it off or firewalled in production |
| `HEARTBEAT_MS` `TIMEOUT_MS` | 5000, 15000 | WS ping interval / silence timeout |
| `RESUME_GRACE_MS` | 30000 | how long a dropped websocket keeps its slot (0 = leave immediately) |
| `UDP_TIMEOUT_MS` | 5000 | native peers without any frame are dropped |
| `MAX_CONNS` `MAX_CONNS_PER_IP` `MAX_ROOMS_PER_IP` | 2000, 20, 3 | global / per-IP connection and room caps |
| `MSG_RATE` `MSG_BURST` `MSG_STRIKES` | 50/s, 100, 30 | per-connection control-message token bucket; after `MSG_STRIKES` rejected messages the connection is closed (1008) |
| `MAX_MSG_BYTES` | 16384 | max JSON control message |
| `ALLOW_ORIGIN` | '' | origin allow-list for `/ws` (`''` = no check, `*`, or comma list); also sent as CORS header |
| `SHARED_SECRET` | '' | if set, `/ws` requires `?key=<secret>` (constant-time compare) |
| `ADMIN_TOKEN` | '' | if set: enables `/admin/*` (Bearer token) and requires it for `/metrics` |
| `TRUST_PROXY` | 0 | `1` behind Caddy/nginx: client IP from `X-Forwarded-For` (needed for per-IP limits and bans) |
| `DRAIN_MS` | 10000 | grace for graceful shutdown |
| `SERVE_STATIC` | 1 | serve `/demo /client /test` (set 0 if the client is hosted elsewhere) |
| `STUN_URLS` `TURN_URLS` `TURN_SECRET` `TURN_TTL` / `TURN_USER` `TURN_PASS` / `ICE_SERVERS` | - | ICE servers handed to clients (coturn REST credentials: `<expiry>:<peerId>`, HMAC-SHA1) |
| `LOG_LEVEL` | info | JSON lines on stdout: `listening ws_open ws_close ws_timeout room_created peer_joined peer_left peer_offline peer_resumed host_changed room_closed ip_banned pw_block udp_peer draining shutdown ...` |

## Protocol

### HTTP
`GET /health` (503 while draining), `GET /rooms` (public rooms: `code,name,host,players,max,hasPassword,topology,age`), `GET /metrics` (Prometheus text) and `/metrics.json`, static `/demo/ /client/ /test/`,
and with `ADMIN_TOKEN`: `GET /admin/rooms` (peers with IP/slot/vip), `GET /admin/bans`, `POST /admin/kick {room,id,ban?,ttlMs?,reason?}`, `POST /admin/close-room {room}`,
`POST|DELETE /admin/ban-ip {ip,ttlMs?}`, `POST /admin/drain`. All admin calls need `Authorization: Bearer <ADMIN_TOKEN>`.

### WebSocket `/ws` (JSON control + binary frames; `rid` is echoed in replies)
| client -> server | reply / effect |
|---|---|
| `hello {name}` / `ping {ts}` / `list` | -, `pong`, `rooms` |
| `create {name?,max?,password?,public?,topology?:'mesh'\|'star',player?}` | `joined {room:{code,name,max,hasPassword,public,topology,hostId}, you:{id,name,slot,vip}, peers, hostId, iceServers, resume:{token,graceMs}}` |
| `join {code,password?,player?}` | `joined`, or `error {code}`: `no_such_room bad_password room_full already_in_room banned blocked too_many_rooms server_full draining` |
| `resume {token}` | `joined {resumed:true, id, ...}` with the **same id, slot and virtual IP**; errors `bad_token`, `not_offline` |
| `leave` | `left` (no grace period) |
| `signal {to,data}` | forwarded as `signal {from,data}` (`{sdp}` or `{cand}`); `no_such_peer` |
| `kick {id,ban?,reason?}` (host only) | `kick_ok`; target gets `kicked {reason,ban}`, others `peer_left {reason:'kicked'}`; `not_host`, `no_such_peer`, `cannot_kick_self` |
| `bans` / `unban {ip}` (host only) | `bans {bans:[ip]}` / `unban_ok` |

Pushed: `welcome {id, iceServers, maxPayload:1288, heartbeatMs, resumeGraceMs}`, `peer_joined`, `peer_left {id,slot,reason}`, `peer_offline {id}` / `peer_online {id}` (resume grace),
`host_changed {id}`, `kicked`, `room_closed`, `shutdown {inMs}`, `error {code}` (`rate`, `too_large`, `bad_json`, `unknown_type`, ...).

Rooms: 5-char code, lowest free slot reused, max 2..20, optional password, public rooms listed, private ones join by code. **Topology**: `auto` = mesh up to 8 players, **star** above (override per room).
Host flag moves to the lowest remaining *online* slot when the host leaves. A closed websocket keeps the slot for `RESUME_GRACE_MS` (others see `peer_offline`; datagrams to it are dropped; its WebRTC channels keep working);
`resume` rebinds a new websocket to the same peer. Empty rooms are deleted. Bans are per room by IP (host, in memory) or global (admin, with expiry).

### Datagram frame (WebSocket binary, WebRTC channel, UDP gateway; network order)
```
 0  'B' 'N'   2 ver=1   3 type (1 HELLO, 2 DATA, 3 BYE, 4 WELCOME, 5 ERROR, 6 FWD)
 4  src_ip[4]   8 src_port[2]   10 dst_port[2]   12 dst_ip[4]   16.. payload (<= 1288 bytes)
```
* DATA to `255.255.255.255` / `x.x.x.255` = broadcast to every *other* peer (the sender loops it back locally, like Windows); to `10.77.0.<n>` = unicast. The server **rewrites `src_ip`** with the sender's virtual IP; over WebRTC the receiver takes the channel owner's IP.
  Oversize, malformed, wrong-type and rate-limited (2000 frames/s, burst 4000 per peer) frames are dropped and counted.
* **FWD (6)** is the star-topology forward: a leaf asks the hub (over its data channel) to deliver a frame; the hub checks `src_ip == channel owner` and forwards it (keeping the source). If a hub<->peer channel is down the hub hands the frame to the
  server, which accepts FWD **only from the room's host**, only for a source slot that exists in the room, and delivers it as DATA with the original source.
* UDP gateway (native): `HELLO` payload `room=CODE;name=..;pw=..` (or `create=1;max=N;public=0|1`) -> `WELCOME` payload `vip[4] slot[1] code...`; `BYE` leaves. Env for `wsock_hle`: `BLUR_NET_RELAY=host:udpport BLUR_NET_ROOM=CODE [BLUR_NET_NAME] [BLUR_NET_PW]`.

### WebRTC negotiation and topologies (`client/virtual-lan.js`)
* **mesh**: per pair, the higher slot offers; one `RTCDataChannel` `udp` `{ordered:false, maxRetransmits:0}`; no channel within 4 s or channel closed => that pair uses the server relay (and switches back when the channel opens).
  Mixed-path broadcasts go out as one relay broadcast (reaches everybody once).
* **star**: channels only between the host (hub) and each leaf. Leaf -> leaf unicast and leaf broadcast travel as FWD frames through the hub (2 hops); hub -> leaf is direct; leaf -> hub direct. Fallbacks: leaf without a hub channel sends plain DATA through the server;
  hub without a channel to a leaf forwards through the server (host-only FWD). On `host_changed` links are rebuilt to the new hub [tested]. Peer paths are reported as `webrtc`, `viahub`, `relay`, `relay-only` (native peers).
* **Session resume**: after a websocket drop the library reconnects (200 ms .. 2 s backoff) and resumes with the token; events `offline` / `resumed`; `closed` if the token expired (53 ms to resume in the local test).

## Client API (ES module, no dependencies)
```js
import { VirtualLan } from '/client/virtual-lan.js';
const lan = new VirtualLan({ url: 'wss://host/ws?key=SECRET', name: 'me', forceRelay: false, autoResume: true });
await lan.connect();  await lan.listRooms();  await lan.createRoom({ name, max, password, public, topology });  await lan.joinRoom(code, pw);  await lan.leaveRoom();
await lan.kick(id, { ban: true, reason });  await lan.bans();  await lan.unban(ip);                        // host only
lan.on('room' | 'peer_joined' | 'peer_left' | 'peer_offline' | 'peer_online' | 'host_changed' | 'transport' | 'offline' | 'resumed' | 'kicked' | 'room_closed' | 'shutdown' | 'closed' | 'error', fn);
const s = lan.socket(50001, { reuse: true });          // UDP-like sockets (EADDRINUSE unless reuse)
lan.send(s, '255.255.255.255', 50001, bytes);          // Uint8Array <= 1288 B
lan.recv(s)  /  lan.recvInto(s, buf, meta)  /  lan.pending(s)     // non-blocking, 256-deep ring per socket
lan.enableRaw(); lan.rawSendFrame(frame); lan.rawRecvFrame(dst);  // frame pipe for the wasm Winsock HLE (INTEGRATION.md)
VirtualLan.local();                                    // offline instance (no server, loop-back only)
lan.vip, lan.slot, lan.isHost, lan.topology, lan.peerInfo(), lan.stats()
```

## Test results (this machine, headless Chromium, 1 process per peer, localhost, 60 Hz unicast to every other peer, 200 B per packet + one 1288 B per second, 60 s)
| | 3 peers WebRTC | 3 peers relay | **12 peers mesh** | **12 peers star** | **12 peers relay** |
|---|---|---|---|---|---|
| packets sent / received | 21,600 / 21,600 | 21,600 / 21,600 | 475,200 / 475,200 | 475,200 / 475,200 | 475,200 / 475,200 |
| loss / reordered / duplicates | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |
| latency p50 (median flow) | 2.3 ms | 2.4 ms | 2.7 ms | 4.1 ms | 3.5 ms |
| latency p99 (worst flow) / max | 6.2 / 8.9 ms | 5.5 / 13.1 ms | 6.1 / 13.3 ms | 12.3 / 21.4 ms | 8.6 / 56.5 ms |
| server CPU (of one core) | 0.1 % | 5.6 % | 0.3 % | 0.3 % | **28.2 %** (7,920 frames/s, 111 MB relayed) |
| page CPU (Chromium task time) | 2-3 % | 3-4 % | 5.0 % avg, 5.6 max | 5.4 % avg, **12.1 % hub** | 6.6 % avg |
| max receive-poll gap | 6.7 ms | 6.0 ms | 11.9 ms | 9.2 ms | 14.9 ms |
Latency is one-way (`performance.timeOrigin + now()` across processes) and includes the receiver's 2 ms poll loop, so loopback transport itself is well under 2 ms (star leaf-to-leaf = 2 hops).
It says nothing about WAN behaviour (no loss/jitter on loopback). Raw data: `test/browser/results-*.json`, logs `run-60s.log`, `load12-*.log`.
Star hub traffic in the 12-peer run: 435,600 frames over data channels (its own 39,600 + 396,000 forwarded), 0 via the server.

Discovery emulation (1 scanner + 12 hosts = 13 peers, 78 WebRTC pairs): the scanner's own broadcast loops back, 11 replies arrive within 4-11 ms, the list keeps **10** (game cap), a reply at 2.15 s is ignored by the 2.0 s window; same over relay.
Native interop (`yarn test:interop`): real `wsock_hle.c` (assigned `10.77.0.2`) <-> browser client: broadcast query -> 160-byte reply in ~5.5 ms, 20/20 unicast ping-pongs (RTT p50 5 ms), native broadcast received by the browser.
Wasm interop (`yarn test:wasm`): the same C file compiled with emcc, two instances through the server: discovery in 10 ms, 10/10 ping-pongs.

## Deployment
Files: `Dockerfile`, `docker-compose.yml` (lobby server + Caddy + coturn), `deploy/Caddyfile`, `deploy/coturn/turnserver.conf`, `deploy/mp.service` (systemd), `deploy/.env.example`.
**Written but not built or run** in this prototype (no container tooling used); review before use.

**Bare VPS (systemd + Caddy):** Node >= 20; `yarn install --production`; copy `deploy/mp.service`, put secrets in `/etc/blur-mp.env` (`ADMIN_TOKEN`, `TURN_SECRET`, `STUN_URLS`, `TURN_URLS`, `ALLOW_ORIGIN=https://your.site`, optional `SHARED_SECRET`);
`TRUST_PROXY=1`, `HOST=127.0.0.1`. Stop = SIGTERM: clients get `shutdown {inMs}`, new connections get 503, the process exits when empty or after `DRAIN_MS` (second signal = immediate).

**wss:// (mandatory for pages served over https).** Caddy terminates TLS (automatic certificates) and proxies HTTP + WebSocket (Upgrade headers are forwarded automatically). Minimal `Caddyfile`:
```
lan.example.com {
	encode zstd gzip
	@private path /admin/* /metrics /metrics.json
	handle @private {            # keep admin/metrics private (or remove this block and rely on ADMIN_TOKEN + an allow-list)
		@local remote_ip 127.0.0.1 ::1
		reverse_proxy @local 127.0.0.1:8786
		respond "forbidden" 403
	}
	handle { reverse_proxy 127.0.0.1:8786 }
}
```
Client URL: `wss://lan.example.com/ws` (+ `?key=SECRET` when `SHARED_SECRET` is set). nginx equivalent: `proxy_http_version 1.1; proxy_set_header Upgrade $http_upgrade; proxy_set_header Connection "upgrade"; proxy_set_header X-Forwarded-For $remote_addr; proxy_read_timeout 1h;`.

**coturn**: `use-auth-secret`, `static-auth-secret=$TURN_SECRET`, UDP 3478 + TLS 5349 and the relay range (49160-49200/udp) open; set `STUN_URLS=stun:host:3478 TURN_URLS=turn:host:3478?transport=udp,turns:host:5349?transport=tcp TURN_SECRET=...`.
Expect 10-15 % of sessions to need TURN. The config denies relaying to private ranges.

**Capacity** (measured on loopback): signalling and WebRTC rooms cost ~nothing (0.3 % of a core for 12 peers); the **relay fallback is the expensive part** (28 % of a core for 12 peers x 660 frames/s). A 1 vCPU box serves roughly 3-4 fully-relayed 12-player rooms or many WebRTC rooms. Per-peer relay limit 2000 frames/s.

**Security notes**: no accounts; room passwords are shared secrets in memory and travel in JSON (use wss); src IP is server-enforced; limits: per-IP connections (20) and rooms (3), control-message rate/size, password brute-force block (10 failures/min -> 5 min), relay rate;
admin API and metrics are token-protected; host kick/ban by IP (IPv4 NAT users share an IP - bans are blunt). Behind a proxy set `TRUST_PROXY=1` or every client appears as the proxy.

## Known limits
Passwords are compared in clear in memory; no persistence across restarts (rooms vanish; clients with a resume token get `bad_token`); single process (no clustering); the star hub is the host (a weak host hurts everybody);
no WAN/TURN testing yet; rate limits are per connection, not per IP; the demo page is a prototype UI.

## Layout
`server/src/{frame,rooms,ice,server,log}.js` core; `server/index.js` (signals, drain); `server/test/*.test.js` (31 tests); `client/virtual-lan.js`; `demo/index.html`;
`test/browser/{page.html,run.js,features.js,demo-smoke.js}`; `native/{native_peer.c,run_interop.js,wasm_hle_test.c,run_wasm_hle.js}`; `deploy/`, `Dockerfile`, `docker-compose.yml`; `INTEGRATION.md`.
