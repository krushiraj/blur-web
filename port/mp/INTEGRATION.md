# Wiring `virtual-lan.js` into the wasm Winsock HLE

Audience: whoever owns `port/web` (wasm-core, PLAN F19/F34-F38). Everything below marked **[tested]** is covered by an automated test in this repo
(`yarn test:wasm`, `yarn test:interop`, `yarn test:features`, `yarn test`); the rest is the design to follow.

## 1. The picture

```
 guest code (lifted x86)  --stdcall-->  rt_dispatch -> HleDef.fn(Ctx*, a[])          (single wasm thread, cooperative scheduler, F35b)
                                          |  wsock_hle.c  (socket table, errors, broadcast loop-back, GetAdaptersInfo, EADDRINUSE, TCP refused)
                                          |  host_send(frame) / host_recv(frame)      EM_JS imports -> Module.blurLan
                                          v
                              client/virtual-lan.js in RAW mode  (enableRaw / rawSendFrame / rawRecvFrame)
                                          |  routing: WebRTC data channel (mesh or hub), server relay, host-only FWD fallback
                                          v
                              lobby server (rooms, signalling, relay)  <->  other browsers / native harness instances
```
Design decision: **the socket semantics stay in C** (the same file runs native and wasm, so the 30-odd guest-visible behaviours are tested once, natively, by `patch/run_test.sh`), and
**JS only moves 16-byte-header frames** between the game and the network. The alternative (map each guest socket onto a `lan.socket()` handle) needs a JS call per Winsock call and duplicates
bind/broadcast/EADDRINUSE logic; the library supports it (non-raw mode) but it is not recommended.

## 2. What exists [tested]
* `port/work/net-re/patch/wsock_hle.c` compiles for native **and** emscripten (`emcc -c -I port/work/wasm-core/harness`). Under `__EMSCRIPTEN__` the relay UDP socket is replaced by three `EM_JS` imports:
  `js_lan_send(ptr,len)`, `js_lan_recv(ptr,cap)`, `js_lan_vip()` (+ `js_seed()` for `srand`), all operating on `Module.blurLan`.
* `client/virtual-lan.js`: `enableRaw()`, `rawSendFrame(Uint8Array)`, `rawRecvFrame(Uint8Array) -> length | -1`, `VirtualLan.local()` (offline instance, loop-back only), plus `vip`.
* `native/wasm_hle_test.c` + `native/run_wasm_hle.js`: two emscripten module instances (wsock_hle.c + `Module.blurLan = VirtualLan`) in node, through the real lobby server: broadcast discovery (QUERY -> 160-byte REPLY, 10 ms) and 10/10 unicast ping-pongs. `yarn test:wasm`.

## 3. Handler -> client mapping (`wsock_hle.c`)
| guest API | HLE behaviour | client / host call |
|---|---|---|
| `WSAStartup` (ordinal 115) | writes WSADATA v2.2, `net_init_locked()` | first call: `js_seed()` -> `srand`, `js_lan_vip()` -> virtual IP (0 => default `10.77.0.1`, local-only) |
| `GetAdaptersInfo` | one Ethernet adapter, IP = vip | none (uses cached vip) |
| `gethostname` / `gethostbyname` | `BLUR-PC-<n>`; only numeric, `localhost`, own name resolve, everything else `WSAHOST_NOT_FOUND` | none |
| `socket(AF_INET, SOCK_DGRAM)` | table entry (`SOCKET = 0x1c0 + 4*i`) | none |
| `socket(SOCK_STREAM)` + `connect` | `WSAECONNREFUSED` | none (online services stay unreachable) |
| `ioctlsocket(FIONBIO / FIONREAD)`, `setsockopt(SO_BROADCAST, SO_REUSEADDR)`, `getsockopt`, `getsockname` | table state | none |
| `bind` | `EADDRINUSE` inside the instance unless both sockets set `SO_REUSEADDR`; port 0 -> ephemeral | none |
| `sendto` / `send` | unicast to own vip and broadcasts are looped back **in C** (`local_deliver`); everything else -> `relay_send(FR_DATA, ...)` | `host_send` -> `js_lan_send` -> `lan.rawSendFrame(frame)` |
| `recvfrom` / `recv` / `select` / `ioctl` | `pump_locked()` first: drains `host_recv` until empty (max 4096 frames), demultiplexes into the per-socket rings in C | `js_lan_recv` -> `lan.rawRecvFrame(buf)` |
| `closesocket`, `shutdown` | frees the table entry | none |
| `WSAGetLastError` / `WSASetLastError` | per-thread value, mirrored into the TEB last-error slot | none |
Join/leave/kick/host-change are **page-level** events (`lan.on(...)`); the game never sees them, it sees datagrams stop (its own DTLS/peer timeouts drop the player).

## 4. Steps
1. **Build**: add `wsock_hle.c` to `HOST_SRC` in `port/web/Makefile` (source dir `port/work/net-re/patch/` or copy it next to `hle2.c`). No new link flags: `EM_JS` needs only `emscripten.h`; the pthread mutex is a no-op in the single-threaded build. Check: `emcc -c -I$(HARNESS) wsock_hle.c` succeeds [tested].
2. **Lookup**: apply the `hle2_lookup()` hunk of `patch/integrate_wsock.diff` to the wasm-core copy of `hle2.c` (call `wsock_lookup(name)` before `win_lookup`) and the `emu.h` prototype. `blur_init` re-resolves handlers **by API name** (`rt_wasm.c:190 hle_lookup`), so no snapshot-format change is needed for the function pointers.
3. **Regenerate the OEP snapshot** after steps 1-2: the snapshot stores each stub's `nargs` (`snap_ctx.c:20`), and `GetAdaptersInfo` currently has no handler/signature (`nargs = -1`), so stdcall cleanup would be wrong with an old snapshot. Also apply `patch/main_ordinals.diff` (ws2_32 ordinals 10/11/12) *before* regenerating, otherwise the stub names for `ioctlsocket/inet_addr/inet_ntoa` are swapped inside the snapshot.
4. **Glue** (browser, main thread or Worker, before the first `blur_run`):
   ```js
   import { VirtualLan } from './virtual-lan.js';
   const lan = new VirtualLan({ url: 'wss://host/ws', name });   // or VirtualLan.local() for offline play
   await lan.connect(); await lan.joinRoom(code);                // UI: lobby list (see demo/)
   lan.enableRaw();
   const M = await createBlurModule({ blurLan: lan, /* ...existing options */ });   // Module.blurLan is what the EM_JS imports read
   ```
   Do this **before the game's network start-up** (WSAStartup runs early in the front-end boot): the HLE caches the virtual IP at first use, and the game builds its `bdCommonAddr` from `GetAdaptersInfo` once.
   Therefore: join (or create) the room first, then start/resume the game. For a single-player session pass `VirtualLan.local()`.
5. **Frame loop**: network I/O only happens when JS gets control back. `blur_run` returns at every `Present` (F34d), so with the planned `requestAnimationFrame` loop the data-channel/WebSocket callbacks run between frames and `rawRecvFrame` finds what arrived. No extra pump is needed. If the run loop is ever made to run many frames without yielding, the lib's raw ring (1024 frames, drops oldest) overflows after ~0.13 s at 7,900 frames/s (12 players, 60 Hz) - keep yielding every frame.
6. **Clock**: do **not** run MP with `BLUR_DET` (deterministic clock). The game's net timers (retransmit, NAT-trav stage timeouts, 10 Hz vehicle updates, 90 s join timeout) run on the guest clock; it must follow real time. Likewise `Sleep`-based pacing must stay real.
7. **Randomness**: the HLE's `CryptGenRandom` is libc `rand()`; `wsock_hle.c` seeds it from `crypto.getRandomValues` (`js_seed`). The proper fix (task for hle2.c) is `CryptGenRandom` -> `crypto.getRandomValues` via an `EM_JS` helper; without a seed every browser would generate identical DTLS nonces/DH keys.
8. **Smoke test in the real game** (once the UI works): open the demo lobby, create room, start the game with `Module.blurLan`, in the game open Multiplayer -> LAN; `BLUR_LOG=1` should show `bind 0.0.0.0:50001` and the 24-byte broadcast (`0f 00 00 00 ...`); a second browser/native instance in the same room must see the host in `EnumerateLanGames` within the 2.0 s window.

## 5. Memory ownership and sizes
* Guest address == wasm address (F35a). JS never stores pointers: each `EM_JS` call receives `(ptr, len)`, builds `HEAPU8.subarray(ptr, ptr+len)` **inside the call** (the symbol `HEAPU8` is re-bound by emscripten when memory grows; the module is linked with a fixed 2.5 GB initial memory but growth to 4 GB is allowed, F35a), and copies:
  * `rawSendFrame(view)`: the library copies the payload into a fresh frame (`buildFrame`) before returning; the view may be reused immediately.
  * `rawRecvFrame(view)`: the library `set()`s a JS-owned frame into the view and drops its own copy.
* Buffers on the C side are on the C stack (`uint8_t f[FR_HDR + MAXDG + 64]` = 2.1 KB); the maximum frame is 16 + 1288 bytes (`BD_MAX_DATAGRAM_SIZE`), larger sends return `WSAEMSGSIZE`.
* The JS rings are bounded: 1024 raw frames; overflow drops the oldest (UDP semantics) and increments `lan.stats().dropped.queue`.

## 6. Worker vs main thread (open point, coordinate with wasm-core M2)
`blur_run` is planned to run in a Worker (F38c). `VirtualLan` needs `WebSocket` (available in workers everywhere) and `RTCPeerConnection`.
* If `RTCPeerConnection` exists in `DedicatedWorkerGlobalScope` on the target browsers [U: verify per browser; Chromium-based browsers expose it in recent versions, Safari/Firefox must be checked], run `VirtualLan` **in the worker**: nothing else to do.
* Otherwise use a bridge, still without SharedArrayBuffer (F19c): the main thread owns the real `VirtualLan` (raw mode); the worker owns a tiny proxy object with the same three methods used by the EM_JS imports:
  ```
  worker -> main  {t:'tx', frames:[ArrayBuffer...]}  posted once per Present (batch, transferable)      main: for each: lan.rawSendFrame(new Uint8Array(buf))
  main -> worker  {t:'rx', frames:[ArrayBuffer...]}  posted when lan.rawRecvFrame has data (rAF-aligned)  worker: push to proxy ring; rawRecvFrame pops
  main -> worker  {t:'vip', vip:'10.77.0.x'} on join/resume (worker reads it once at WSAStartup)
  ```
  Added latency: one message hop each way (< 1 ms) + the frame period; acceptable for a 100 ms update rate. Keep batches under 1 MB.

## 7. Failure handling the integration must provide
* **WebSocket drop** (`lan.on('offline')` / `'resumed'`): the library resumes the same slot/IP within the server grace period (30 s default), data channels keep carrying traffic meanwhile [tested: `features.js` resume]. The game needs no action.
* **`closed`, `kicked`, `room_closed`, `shutdown`** events: show UI; the game session cannot continue (all datagrams will be lost): leave to the menu. A `shutdown` notice gives `inMs` to finish the race.
* **Host leaves**: `host_changed`; in a star room the library re-links to the new hub automatically [tested]; the game's own host-migration logic (online matchmaking) is not expected to work in LAN mode (REPLICATION.md 5).
* Background tab: `requestAnimationFrame` stops, the game stops polling, peers time out the player. Use the Page Visibility API to warn or to pause the race for that player (not solvable in the transport).

## 8. Risks
* NAT/TURN (not tested beyond loopback); 12-peer mesh = 11 channels per browser (load-tested, 5 % CPU); star topology halves channel counts but doubles hub load and adds one hop (p99 12 ms vs 6 ms on loopback).
* The first real LAN game exercises code that only exists inside the recompiled image (NAT-traversal stage 1, DTLS handshake with `CryptGenRandom` + OpenSSL): if the handshake fails, log both sides' datagrams with `BLUR_NETLOG` (native) / `lan.debug = true`.
* Frame loop timing: if the wasm run loop yields only once per Present and the game renders at 30 fps under load, effective network polling is 30 Hz; DTLS retransmit timers tolerate that.
