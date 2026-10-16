# Blur netcode: reverse-engineering notes and multiplayer architecture for the browser port

Scope: personal RE of the user's own copy. Evidence tags: **[O]** observed directly (disassembly / decompilation / data dump / a test I ran),
**[I]** inferred from code structure or names, **[U]** unknown / not yet found. All addresses are guest VAs in the unpacked image
(`re/dump/blur_oep.bin`, base 0x400000). Generated decompilations that back every claim are in `dec/`, the function list is `functions.tsv`.

## 0. Summary (read this first)

1. **Game networking = Demonware "bd" SDK (old `mmp3` generation) + Bizarre's own session layer on top.** All game traffic is **plain UDP**
   through one class (`bdPlatformSocket`) that calls Winsock `socket/bind/sendto/recvfrom/ioctlsocket/setsockopt` on **non-blocking** sockets that are polled
   once per frame. No `select`, no `WSAEventSelect`, no overlapped I/O on the game path. [O]
2. **There is a real LAN mode that needs no backend**: Lua UI calls `EnumerateLanGames / CreateLanServer / JoinLanServer / CheckLanServerNetworkVersion`
   (binding table at 0xee3a08). Discovery is a **UDP broadcast to 255.255.255.255:50001** (24-byte query, 160-byte reply, 2.0 s window, max 10 hosts);
   the actual session is peer-to-peer UDP through `bdSocketRouter` (NAT-traversal stage 1 + a DTLS-style handshake with HMAC and DH, key delivered inside the LAN advert). [O]
3. **Online matchmaking** (accounts, lobbies, stats, content, STUN) goes to `blur-pc-live.{auth,lsg}.mmp3.demonware.net` (TCP, custom TLV protocol) and
   `stun.{us,eu,jp,au}.demonware.net`. Those services are dead; emulating them is a big, separate project (see 8.4). **Recommendation: build everything on the LAN path.** [O for hosts/strings, I for the recommendation]
4. The wire format above the socket is Bizarre's: a 10-byte message header, fragments <= 1024 bytes, 16-bit sequence numbers, reliable and unreliable
   channels, typed messages (type byte), raw little-endian field serialisation. Gameplay is **state replication + ordered reliable "race events"**,
   not lockstep. [O for framing/types, U for the vehicle-state message, see 5.6]
5. For our HLE this means: **a virtual LAN of UDP datagrams is enough**. I wrote and tested (2 processes + relay) `patch/wsock_hle.c`, a Winsock/GetAdaptersInfo HLE with a 16-byte relay framing
   and a node relay; the same framing maps 1:1 onto a WebSocket/WebRTC data channel (section 7/8). **Not yet tested inside the game** (menus are not reachable). [O for the unit test]
6. Found a **harness bug**: `port/harness/main.c` `ws_ordinal_()` maps ordinals 10/11/12 to inet_addr/inet_ntoa/ioctlsocket; the real ws2_32 order is
   10 ioctlsocket, 11 inet_addr, 12 inet_ntoa (proven by how the game uses them at 0xd207e0/0xd20660/0xd20680). Patch: `patch/main_ordinals.diff`. [O]

## 1. Inventory

### 1.1 Imports [O]
`Blur.exe` (unpacked) imports, besides D3D/DSound/etc.:

| DLL | what | IAT slots |
|---|---|---|
| WS2_32.DLL | 32 imports, **by ordinal** except `WSACloseEvent/WSAEventSelect/WSAEnumNetworkEvents` | 0xe525dc..0xe52658, order: recvfrom send recv WSAGetLastError connect closesocket WSACloseEvent ntohl accept inet_addr htons htonl sendto WSAEventSelect socket listen bind setsockopt gethostbyname ntohs gethostname __WSAFDIsSet select shutdown WSAEnumNetworkEvents WSACleanup inet_ntoa ioctlsocket getsockname getsockopt WSASetLastError WSAStartup |
| IPHLPAPI.DLL | `GetAdaptersInfo` | 0xe52074 |
| WSOCK32.DLL | send/recv/shutdown/closesocket/WSA*LastError | only in the packer (SecuROM `.8thSin` section, 0x1d872b9..), not game code |

Full static xref list: `scripts/iat_xrefs.py` (run it) -> every call site; grouped by function: `scripts/fnrefs.py`.

### 1.2 Who uses Winsock (clusters) [O]
| cluster (function range) | what it is | relevance |
|---|---|---|
| **0xd20660-0xd20d00, 0xd28d60** | `bdPlatformSocket` (UDP create/bind/sendTo/receiveFrom), WSAStartup(2.2), host resolve | **the game netcode** |
| 0xd33xxx-0xd3f4xx (BIO/socket, select, gethostbyname) | OpenSSL BIO sockets + libcurl-style HTTP (Demonware content streaming, `DemonwareSSL.pem`) | TCP, online only |
| 0xd8b2xx-0xd8b9xx, thunks 0xcc602e-0xcc604c | curl easy-socket helpers (I) | TCP, online only |
| 0xaf4a40-0xaf4c70 | `WSAStartup/WSACleanup` + GetAdaptersInfo "is a NIC up" monitor | startup, see 7.4 |
| 0x4f9160, 0x4f9600, 0x505fe0-0x51f5b0, 0x5c8d10 | small TCP/UDP tool layer using WSAEventSelect: a **TCP server** ("accepting connection") and a UDP **"datalog"** sender to port 0x18ad (6317) | developer telemetry/remote tool; expected inactive in retail (I) |
| packer `.8thSin` (WSOCK32) | SecuROM online activation (`pa01.sonyvfactory.com` strings) | DRM; out of scope, do not touch |

### 1.3 Libraries statically linked [O]
Demonware `bd*` SDK (bdCore, bdSocket, bdSocketRouter, bdDTLS, bdNAT, bdRelay, bdQoS, bdUPnP, bdLobby*, bdAuth*, bdMatchMaking, bdStats, bdStorage, bdContentStreaming,
bdFriends, bdMessaging, bdKeyArchive, bdTitleUtilities, bdTwitter/bdFacebook, bdBandwidthTest, bdGetHostByName), OpenSSL, libcurl, Tiger hash. Host names: see 8.4.

## 2. Layering

```
 Lua UI (Screens\Multiplayer\MpLan.lua, MpLobby.lua, NetLogin.lua ...)     script tables NetParty / NetRace / NetServices   [O]
        |   binding table 0xee3a08..0xee4228 (255 entries, script_bindings.txt)
 Game session layer ("NetworkManager" update 0xb2f3b0, session object DAT_011b31f8, player table, lobby state machine)   [O]
        |   typed messages (type byte), reliable + unreliable, fragmentation (0xb2cc10 / 0xb2ae20, hdr 10 B)
 LAN discovery (0xb3e320/0xb3f670/0xb3ff20)         bdMatchMaking (online lobbies) / bdLobbyService (TCP)
        |                                                    |
 bdSocketRouter 0xd03f20 -> bdNATTravClient (stage1 direct, stage2 introducers) -> bdDTLSAssociation (HMAC+DH) -> data packets   [O]
        |   (bdRelaySocketRouter variant exists, selected by a flag in bdNet)
 bdSocket / bdPlatformSocket 0xd207e0..0xd20ae0     UDP, nonblocking, SO_BROADCAST only on the discovery socket
        |
 Winsock (WS2_32 ordinals)  +  GetAdaptersInfo (local IPs for bdCommonAddr)
```

## 3. Transport [O]

* `bdPlatformSocket::create(blocking, broadcast)` (0xd207e0): `socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)`; if `broadcast` -> `setsockopt(SOL_SOCKET, SO_BROADCAST)`;
  `ioctlsocket(FIONBIO = !blocking)`. All game sockets are created non-blocking.
* `bind(sock, ip, port)` (0xd20870): fills a 16-byte `sockaddr_in`, closes the socket on failure. `sendTo` (0xd20930) / `receiveFrom` (0xd20ae0) are thin
  wrappers; they treat WSAECONNRESET-class errors (0x2742,0x2743,0x2745,0x2746,0x274c,0x2751) as "connection reset" log lines and keep going. `WSAEWOULDBLOCK` (0x2733) = no data.
* `bdNetImpl::start` (0xd21050): requires a local address list; `findFreePort` (0xd23b70) probes by bind+close (so **bind must report EADDRINUSE**); logs "Requested port %u, using port %u".
  The session descriptor builder (0xb35ab9) receives the local common address and two port words `0x2800, 0x2800` for LAN host/join (0xc178f0/0xc179e0). The meaning (base port 10240? fixed?) is [U];
  it will be visible in the first `bind()` the game issues (experiment 10.2).
* Datagram limits: **BD_MAX_DATAGRAM_SIZE = 0x508 (1288)**, application payload < 0x4f0 (1264). (0xd10220) -> WebRTC unreliable channels must carry 1288-byte messages (SCTP fragments; OK) [O]
* Per-peer connect (`bdSocketRouter::connect`, 0xd04700): always starts `bdNATTravClient::connect` (0xd14fa0): **stage 1** = send traversal packets directly to the peer's
  advertised local+public addresses (this is what makes LAN work without STUN); **stage 2** = ask "introducers" (the dead `stun.*.demonware.net`). Then a DTLS-like handshake:
  `init / init-ack(cookie, HMAC) / cookie-echo / cookie-ack`, DH shared key derived from the **security ID/key pair** held in `bdSecurityKeyMap`, per-packet HMAC and sequence-number window. [O strings+structure, I details]
* QoS probe (0xd10ce0), UPnP port mapping (0xd25160, "UPnP disabled." when off), bandwidth test: online-only features. [O]
* Names the harness must resolve: `gethostname`, `gethostbyname` (Demonware/STUN host names - **must fail fast**, they are dead), `inet_addr`, `inet_ntoa` (`255.255.255.255` is special-cased at 0xd20d00).

## 4. LAN mode [O unless noted]

### 4.1 UI entry points (script bindings)
`EnumerateLanGames 0xc170d0`, `IsEnumeratingLanGames 0xc14eb0`, `NumLanServersEnumerated 0xc14ef0`, `CheckLanServerNetworkVersion 0xc17150`, `CreateLanServer 0xc178f0`,
`JoinLanServer 0xc179e0`, `MonitorJoinLanServer 0xc14f70`, `GetLanLobbyList 0xc151d0`. Screen script: `Screens\Multiplayer\MpLan.lua` (inside the paks, compressed; not read).
Whether the menu lets you reach MpLan **without an online account** is [U] (the three LAN functions themselves check only `DAT_011b31d0`, `session+0x410`; no account state). Experiment 10.1.

### 4.2 Discovery protocol (UDP 50001)
* Init (0xb3ff20): `create(nonblock=1, broadcast=1)` then `bind(0.0.0.0:50001)` (0xc351). The word at `this+0xc` ("network id") is taken from the bind result, compared for equality on every received packet.
* **Query** (0xb3f670, sent to `255.255.255.255:50001`, 24 bytes): `u32 version = htonl(0x0F000000)` (bytes `0F 00 00 00`; "network protocol version 15" - the receiver stores `ntohl()` of it and `CheckLanServerNetworkVersion` compares it with `0x0F000000`; 0xb3e320 itself only checks `netId`), `u32 netId`, `u32 type = 1`, `u8[8] nonce` (random, `this+0xcc`), `u32 version` (arg of `EnumerateLanGames`). Sets "enumerating" and starts a timer.
* **Reply** (0xb3e320, sent back to the query's source ip:port, 160 bytes, only if this instance is hosting and `query.version <= our version`):
  `version (same constant), netId, type = 2, queryType, echoed nonce word, ` then the host's session advert copied from the session object: two ints (`+0x28,+0x2c`), a 56-byte block (`+0x58`, 14 dwords = common-address/security data),
  4 x u64 (`+0x38/+0x40/+0x48/+0x50` incl. the host common address hash), the 25-byte host name (`+0x90`), `maxPlayers`, `openSlots` (= max - current), 2 more ints. Layout names are [I]; the 160-byte size and the field copy sources are [O]. Our relay never needs to parse it.
* Receive side keeps at most **10** hosts (`count != 10`), `CheckLanServerNetworkVersion(i)` returns two enums (cmp of the reply's version dword against `0x0F000000` and of a u16 against `*(DAT_010fad00+8)`), matching the string enums `eNetVersionNewer/Older/Same`.
* The enumeration window is **2.0 s** (float const at 0xe62c60) after which `enumerating` clears. One broadcast per `EnumerateLanGames` call (the UI presumably calls it again to refresh).
* A broadcast query is also received by the sender's own socket on Windows; the reply code answers any query where hosting is active, so a host will list itself unless 0xb3d350 filters on the nonce. The HLE therefore loops broadcasts back (as Windows does). [I for the self-listing behaviour]

### 4.3 Hosting / joining
`CreateLanServer` (0xc178f0): checks a session-state==5 precondition, builds the local `SessionInfo` (0xb35ab9: local common addr from `getLocalCommonAddr` 0xaf4d80, ports 0x2800/0x2800), calls
`host` (0xb397c0): `bdSecurityKeyMap::initKey` (fresh random security ID+key, 0xd08d40), `registerKey`, creates the router address handle, sets session flags (`+0x412` joined, `+0x6ac`).
`JoinLanServer(i)` (0xc179e0): takes the i-th discovered entry (`0xb3e2d0`), `0xb399c0`: copies the host's common address and **security key from the advert**, `registerKey`, `bdSocketRouter::connect` -> NAT-trav stage 1 + DTLS to the host's advertised addresses, then message type 3 (join request) etc. (5.3).
The advertised addresses come from `GetAdaptersInfo` (see 7.4), so peers must be mutually reachable at those addresses: the virtual LAN gives every instance such an address.

### 4.4 Limits [O]
Player table: 20 slots (loops `< 0x14`); LAN list: 10 hosts; split-screen: separate path.

## 5. Session and message layer [O unless noted]

### 5.1 Framing (0xb2cc10 queue, 0xb2ae20 dequeue, 0xb2e020 receive loop, 0xb2e330/0xb2e450 send)
Every game message is split into fragments of at most **0x400** payload bytes, each with a **10-byte header** (little-endian):

| off | size | meaning |
|---|---|---|
| 0 | u16 | `payload + 8` (= fragment bytes after this u16; the reader adds 2 to get the full fragment size) |
| 2 | u8  | `bit0` always 1, `bit1` **reliable**, `bits2..7` channel (6 bit) |
| 3 | u8  | **message type** |
| 4 | u16 | `((payload + 10) << 2) \| (firstFragment << 1) \| unsequenced` |
| 6 | u16 | total message length + 10 |
| 8 | u16 | sequence number (separate counters for reliable `+0x28` and unreliable `+0x2a`; 0 if `unsequenced`) |

Reliable fragments stay in a per-channel queue and are removed by acks (`0xb2aff0/0xb2af10/0xb2b0b0`, ack matches `hdr+8`). The receive loop
(0xb2e020) pulls datagrams from the router (`router->receiveAll`, vtbl+0xc / receiveFrom +0x24) and offers them to registered interceptors (`registerInterceptor`) which classify
control/ack/data. The on-wire datagram is the DTLS data packet that wraps one or more of these fragments. [O for the header, I for the exact ack/ping packet shapes]

### 5.2 Stream encoding
Raw host-endian (little-endian x86) fields: u8, u16, u32, u64, f32, raw byte blocks. Reader funcs 0xb2a9b0 (u8) 0xb2aab0 (u16) 0xb2ab10/0xb2ab70 (u32) 0xb2abd0 (u64) 0xb2ac90 (f32) 0xb2ad50 (bytes);
writers 0xb2a650/0xb2a610 (u8) 0xb2a690/0xb2a6f0 (u16) 0xb2a750/0xb2a7b0 (u32) 0xb2a810 (u64) 0xb2a8d0 (f32) 0xb2ca60 (bytes). Messages are prefixed by `0xb2c880(type, channel, flags)` and closed by `0xb2ca30` (writes a u16 length prefix). No byte swapping, no compression. [O]

### 5.3 Message types seen (type byte)
The type numbers and the dispatch/sender sites are [O]; the column "meaning" is [I] (inferred from the fields and from the neighbouring script-binding names).
Dispatch tables: pre-session / lobby `0xb3cb40` handles **5,7,10,11,12,13,14,22,26,27,32**; in-session `0xb3c7c0` handles **5,7,8,10,11,12,13,14,22,32** and forwards any other type to the
race-mode object (`mode->vtbl+0x40`, which handles **0x18** and `+0x3c` which handles **0x19, 0x21**). Senders found by scanning call sites (`scripts/callargs.py`): types **2,3,4,5,7,9,0x0a,0x0f,0x10,0x12,0x13,0x14,0x15,0x18,0x19,0x1c,0x1e,0x1f**.

| type | sender / receiver | content (observed) | meaning |
|---|---|---|---|
| 3 | tx 0xb30c70 / 0xb32120 | `u32 0x0F000000`, `u8 slot`, `u64 sessionId`, `u64 playerId`, `char name[25]`, `u16`, `u16`, `u32`, `u8`, then per other player: `0x38 B`, `u8 flag`, `0xC B`, `u8` | join request / roster hello |
| 4 | 0xb30420, 0xb33120 | small | join accept / ack |
| 5 | rx 0xb3abc0 / 0xb3c1f0 | `u8 n`, per player `u64 id, u8, name[25], ...` | roster / player list |
| 7, 0x0a | 0xb345f0 / 0xb34a20 / 0xb3a2f0 / 0xb3b990 | `u32 n`, per entry `u64 playerId, u8 state` | ready/state sync (0x0a), lobby state (7) |
| 0x20 | rx 0xb2fa70 | `u8 n`, per entry `u32 slot, u8 flag` | per-player flags |
| 0x0f | tx 0xbf8370, rx 0xbf8810 | per player: slot + `0x18` bytes (`+0x138`) + `0x0C` bytes (`+0x150`/`+0x164`) | per-player loadout/appearance blob [I] |
| 0x10, 0x12, 0x13, 0x14, 0x1f | 0xc18bf0, 0xc1b830, 0xc18ff0, 0xc192f0 | custom race launch / voting / next race settings | lobby voting and race start (names `SendBeginVoteMessage`, `SendVoteResults`, `SendNewRaceSettings`) [I] |
| 0x18 | tx 0xbb0c30, rx 0xbb2890 | `u8 n`, per player: `u8 slot, 4 B, 25 B name?, u32 score, u32` | scoreboard / scoring stats, ~per event [I] |
| 0x19 | tx 0xbb13e0 | **batch of ordered race events** (below) | gameplay events |
| 0x1c | tx 0xbf63c0 every 0.5 s | `f32 + u8 + n x 16 B` | settings heartbeat [I] |
| 0x1e | 0xbf69b0 / 0xbf5be0 | race parameters (`u8 mode, u16, u32, ...`) | race parameters sync |

Plus voice chat frames (200-byte, 0xb3fa80 -> 0xb40a60). [I]

### 5.4 Race events (the ordered gameplay stream) [O structure, I semantics]
`0xbb2100` receives `u8 count`, then per event `u16 seq` (must equal the player's last seq+1, otherwise handlers are called with a "replay" flag and state is not applied) and `u8 eventType` **1..25** dispatched to:

| id | handler | id | handler | id | handler |
|---|---|---|---|---|---|
| 1 | 0xbac7a0 | 10 | 0xbaf980 (flag) | 19 | 0xbaca60 |
| 2 | 0xbad4f0 (48-byte payload) | 11 | 0xbac960 | 20 | 0xbacb20 |
| 3 | 0xbad650 | 12 | 0xbb17a0 | 21 | 0xbad920 |
| 4 | 0xbac7d0 (2 slots + u32: attacker/victim + amount) | 13 | 0xbada80 | 22 | 0xbac720 |
| 5 | 0xbae7f0 | 14 | 0xbac9b0 (attacker, victim, pickup type, amount) | 23 | 0xbadc90 |
| 6 | 0xbac870 (8-byte) | 15 | 0xbadb10 | 24 | 0xbadc20 |
| 7 | 0xbad430 | 16 | 0xbac6a0 | 25 | 0xbaebc0 |
| 8 | 0xbad7b0 | 17 | 0xbae9c0 | | |
| 9 | 0xbac8f0 (slot + f32) | 18 | 0xbaea90 | | |

These carry hits, pickups/power-ups, wrecks, scoring and similar (cvars `Amax.Multiplayer.PickupMpSync`, `Multiplayer.ReportWhenNetworkDamageMessageIsActedUpon`, `Multiplayer.ReportNetworkPickupAutopsyReports`, `Multiplayer.NetEventLog` exist). The semantic names above 4 are [U]; payload decoding of each handler is in `dec/race_handlers.txt`.

### 5.5 State machines and timers [O]
Lobby player states (strings): `Unset, Joining, Disconnecting, JoinComplete, Idle, ChangingSettings, ChangingCar, LoadingRace, LoadingUi, PartyBusy, PartySyncJoinRaceStart, PartyJoinRace, PartySyncJoinRaceEnd, PartyJoinRaceFailed, PartyJoinRaceSuccessful, WaitingForOthers, Racing, Finished, ViewingResults, ViewingResultsIdle`.
Join errors: `PlaylistVerHost/Client, ProfileProblem, PartyClosed, ChampionshipStarted, RankedMatchStarted, ContentMismatch, ToManyTeamMembers, ToManyTeams, NoCommunications, BannedFromSession, NotEnoughOpenSlots, HostDiffersOnPeer, NoConnectionToHost`.
Online connect timeout 90 s (0xb2efd0). Lobby settings heartbeat 0.5 s (0xbf63c0). LAN enumeration window 2 s. Session update is driven per frame from the main loop (call at 0xb80f96 -> 0xb2f3b0) with the game's time-scaled dt.

### 5.6 Vehicle replication [U for the wire message, partial for the model]
* Physics module **`NetworkUpdateModule`** (name at 0xeaba94, registered in the racing-physics module table at 0x10d9e90; update 0x7dbd50 compares the car state with a stored reference and raises a "re-sync" flag when position error / speed error / orientation dot exceed thresholds).
* cvars: `RacingPhysics.Cars.Network.{CarNetworkJumpToPredicted, RunCatchUp, ResetRoundTrip, PatchAroundDeath, UseShortestSlerp}`, `Vehicle.Debug.Network.{FakeDropOut, VehicleStatus}`, `Amax.Multiplayer.playerrubberbanding`,
  **`Multiplayer.VehicleUpdateFrequencyInMilliseconds` default = 100.0 ms** (registered at 0xe36bc7, float const 0xe638b4 = 100.0). => **~10 Hz vehicle state updates** with client-side prediction/catch-up. [O for names and default, I for the model]
* I did **not** locate the serializer of the vehicle state; it does not appear among the typed messages sent through `0xb2c880/0xb407d0` listed above, so it either rides in one of the generic types (3/4/9/0x15/0x1c...) or uses a path that I did not
  find statically. Resolve with experiment 10.3 (type histogram of live datagrams).
* The sim is 60 Hz and not lockstep; remote cars are simulated locally and corrected (JumpToPredicted/RunCatchUp). The wire carries no frame numbers at fixed rate. => latency compensation is entirely inside the game; our transport only needs low jitter.

## 6. Anti-cheat, encryption, accounts [O/I]
* **No anti-cheat** found. Integrity features are transport-level: DTLS-like handshake with HMAC + DH keyed by the security ID/key pair, sequence-number window, "invalid security ID" resets. In LAN mode the key travels in the reply advert in clear (host `initKey`), so it is *session* authentication, not secrecy. It runs inside the recompiled game, we only relay opaque datagrams. [O]
* Online: auth (`bdAuthService`, `Could not read error code from authentication server response`, a hard-coded title key string near 0xce9290, ticket `BD_AUTH_TICKET_SIZE`), license/unlock-code online check (`encrypted_drm_unlock_online.cpp`, "unlock code is blacklisted (already used)"), SecuROM PA activation in the packer. Not needed for LAN. [O strings]
* Platform accounts: Demonware user account (NetLogin.lua / NetRegister.lua, `CreateAccount`, `CheckAccountDetails`). LAN functions do not call these. [I]
* `GetNatType`, `PeerRoutingEnabled`, `EnableMatchingQoS` are online-only script entries.

## 7. Winsock HLE design (implemented in `patch/wsock_hle.c`)

### 7.1 Model
Guest sockets are table entries (SOCKET = 0x1c0 + 4*i). UDP datagrams are delivered through **one host UDP socket to a relay** with a 16-byte frame:
`'B' 'N' ver=1 type(1 HELLO, 2 DATA, 3 BYE) src_ip[4] src_port[2] dst_port[2] dst_ip[4] payload`.
* Each instance owns a virtual IPv4 `10.77.0.<BLUR_NET_ID>` (default 1). Everything the guest sees (`getsockname`, `recvfrom` source, `GetAdaptersInfo`, `bdCommonAddr`) is virtual, so two instances on one machine never collide on ports (the game's `findFreePort` bind probe still gets EADDRINUSE inside one instance).
* Broadcast (`255.255.255.255` and `x.x.x.255`) goes to every socket bound to the dst port in every instance, **including the sender** (Windows behaviour); requires `SO_BROADCAST` on the sender (else WSAEACCES) exactly like Windows.
* Unicast to the own vip is delivered locally; others via the relay. `MSG_PEEK`, `FIONREAD`, `getsockopt(SO_ERROR/SO_TYPE)`, `WSAGetLastError` (per host thread) are implemented. `select()` is a non-blocking poll (only curl/OpenSSL use it, on TCP that fails earlier).
* **TCP is not provided**: `connect()` fails with WSAECONNREFUSED, `gethostbyname` of anything except numeric/`localhost`/own name returns NULL + WSAHOST_NOT_FOUND. Demonware auth/lobby/STUN/curl therefore fail immediately and the game stays "offline", which is the state the LAN menu expects.
* `GetAdaptersInfo` returns one Ethernet adapter ("Blur Virtual LAN Adapter", ip = vip, mask 255.255.255.0, gateway 10.77.0.254); first call with a too-small buffer returns ERROR_BUFFER_OVERFLOW(111)+640 as the game expects (it passes 0x288 and re-calls on 111). The game only requires "some adapter whose first IP string != 0.0.0.0" for the network-up flag (0xaf4a90) and builds `bdCommonAddr` from the same list (0xb2e7c0).
* Env: `BLUR_NET_ID=n` (1..254), `BLUR_NET_RELAY=host:port|off` (default 127.0.0.1:47800), `BLUR_NETLOG=1` (one stderr line per datagram: tx/rx, endpoints, size, first 24 bytes).

### 7.2 Files
* `patch/wsock_hle.c` - the HLE (compiles with the harness' `emu.h`; uses only `rd*/wr*/rdbuf/wrbuf/guest_alloc/cur_thread/rd_cstr`; mutex-protected for the recompiled-mode host threads).
* `patch/integrate_wsock.diff` (apply in `port/`: `patch -p0 < work/net-re/patch/integrate_wsock.diff`) - `hle2_lookup()` tries `wsock_lookup()` first, prototype in `emu.h`, Makefile source list. `patch/main_ordinals.diff` - the ordinal fix.
* `relay/lan_relay.js` - node relay (no dependencies): `node relay/lan_relay.js [--port 47800] [-v] [--stats]`.
* `patch/test_wsock.c`, `patch/run_test.sh` - 2-process test with a real relay: GetAdaptersInfo layout + 111 path, EADDRINUSE, TCP refusal, dead DNS, numeric DNS, broadcast + own loop-back, unicast reply, 1288-byte datagrams both ways, select(), 4-byte self unicast. **Result: `wsock test: id1=ok id2=ok`** (re-run: `sh patch/run_test.sh`).

### 7.3 Running two game instances locally (plan, not yet possible: menus unreachable)
Per instance: own harness copy/port (`BLUR_WEB=<port>`), own save dir (profile is per user), `BLUR_NET_ID=1|2`, one shared `node relay/lan_relay.js`. Both instances need a viewer each (one browser tab per BLUR_WEB port).
Expected first observable: `[wsock] virtual ip ...` then `bind 0.0.0.0:50001` after the LAN menu is opened (`BLUR_NETLOG=1`).

### 7.4 Timing and determinism
No lockstep, so no determinism requirement across peers. The net layer runs from the main loop with the game's time-scaled dt (60 Hz baked sim). Datagrams are polled once per frame; with a 60 fps frame loop that quantises latency to <= 16.7 ms. The browser build must keep calling `recvfrom` every frame even while the tab is hidden/throttled, or peers will hit the 5 s relay timeout / DTLS retransmits (use a Worker timer, not rAF alone, to pump the network).
`QueryPerformanceCounter` in the harness is the guest clock; network timers inside the game use it, so wall-clock jitter from the transport does not break them as long as the guest clock tracks real time.

## 8. Browser architecture (proposal)

### 8.1 Signalling + lobby server (small, stateless-ish)
* **Rooms = virtual LANs.** A room is a /24 (`10.77.<room>.0/24`); the server assigns each joiner an address (`.1` first, up to `.20`), enough for 20 players. A room can be public (listed on our web page), private (code), or password protected.
* Control channel: WebSocket (`wss://.../signal`), JSON messages: `create{name, public, max}`, `list{}`, `join{room, name}` -> `joined{vip, peers[{vip,id,name}], iceServers}`, `peer_joined/peer_left`, `signal{to, sdp|ice}` (relayed), `kick`, `chat`. Server enforces one vip per connection, TTL, rate limits.
* Data channels: **one `RTCPeerConnection` per remote peer** (full mesh; max 19 per client), a single unordered, `maxRetransmits: 0` data channel labelled `udp` carrying exactly the relay frame's body (`src_port, dst_port, dst_ip, payload` - src ip is implied by the channel, dst ip is checked). Broadcast frames are sent to every open channel; unicast to the channel owning `dst_ip`.
* NAT traversal: public STUN (Google or self-hosted `coturn`); **TURN** (coturn, UDP/TCP/TLS 443) is required for symmetric NATs/corporate networks; budget ~10-15 % of sessions. Fallback of last resort: server-side relay over the same WebSocket (`DATA` frames), identical framing, higher latency. 
* Because the game does its own NAT-traversal "stage 1" over *virtual* addresses, nothing in the game must change; the WebRTC layer is transparent.
* Optional UX: a web lobby page (not the in-game menu) that creates/joins a room and then starts the game inside it; the in-game `EnumerateLanGames` then finds the host exactly as on a LAN. No server-side knowledge of the Blur protocol is needed.

### 8.2 Browser runtime mapping
* The wasm harness exports the same table as `wsock_hle.c`; the "relay socket" becomes a JS bridge: `net_send(frame)` / `net_poll(buf)` against a ring buffer filled by data-channel `onmessage` (Worker with `RTCPeerConnection` is available in Chrome/Firefox workers; otherwise main-thread postMessage with a SharedArrayBuffer/transferred ArrayBuffer ring; wasm is single-threaded cooperative per F19, so a simple `Atomics`-free queue handed over at `Present` is enough).
* `HELLO` becomes join; `BYE` leave. Keep the 1 s presence in the signalling layer and drop the `HELLO` frames.
* Back-pressure: data-channel `bufferedAmount` > 256 KB -> drop (unreliable semantics).

### 8.3 Latency/jitter budget
10 Hz vehicle updates with prediction (5.6) tolerate 100-150 ms RTT; reliable events retransmit inside the game. WebRTC adds < 5 ms over the path RTT on LAN-like paths. Voice chat frames (200 B) are in the same stream - fine.

### 8.4 If someone insists on "online" lobbies later
Reimplementing the Demonware mmp3 lobby: TLV-serialised tasks over TCP (`bdLobbyConnection`, `bdRemoteTask`), AES/HMAC-protected auth tickets (title key hard-coded), plus HTTPS content streaming (curl + `DemonwareSSL.pem`: the pem is a client pinned CA, we would have to patch it or serve our own CA, which is invasive) and STUN introducers. Estimated weeks, high risk, and unnecessary for create/join/lobby-list. Do not start.

## 9. Prioritised plan and risks

| # | step | cost | risk | test |
|---|---|---|---|---|
| 1 | Apply `main_ordinals.diff` + `integrate_wsock.diff`; run with `BLUR_NETLOG=1`; confirm startup sequence still works (WSAStartup, GetAdaptersInfo, no socket until menus) | S | low (placeholders today return -1) | game still reaches the front-end; `[wsock] virtual ip` printed |
| 2 | Reach the Multiplayer menu once UI works; open the LAN screen; see `bind 50001` + a 24-byte broadcast | S (blocked by UI work) | **may require "online/sign-in" gating** (4.1) | `[wsock tx] ... 255.255.255.255:50001 24B 0f000000...` |
| 3 | Two harness instances + relay on one machine: host creates LAN game, second lists it, joins, both reach lobby | M | NAT-trav stage 1 or DTLS handshake may need timing (`now_us` guest clock vs wall) | host/joiner lobby shows 2 players |
| 4 | Load the same level on both; race starts; check vehicle updates arrive (histogram, 10.3) | M | unknown vehicle-sync path (5.6); clock scaling (`BLUR_NOLIMIT`) | remote car moves |
| 5 | Signalling server + WebRTC bridge for the native harness (node `wrtc`-less: just WebSocket relay first), then for wasm | M | NAT/TURN ops cost | 2 browsers across NAT |
| 6 | Room list page, kick/leave, reconnect, TURN | M | | |
| 7 | Hardening: rate limits, frame validation (dst_ip must equal the peer's vip, size <= 1288) | S | | fuzz |

Risks: (a) account/online gating in the menu scripts (U); (b) hidden dependency of `bdNet::start` on STUN/NAT-type discovery even for LAN (b2e7c0 is the *online* start-up; LAN path (0xc178f0) does not call it but `bdNATTravClient` stage 1 -> 2 fallback needs a short timeout, otherwise "connect" takes longer than the UI tolerates); (c) the DTLS handshake relies on the game's OpenSSL/HMAC/DH code and on randomness: the harness `h_CryptGenRandom` uses libc `rand()` and **nothing in the harness calls `srand`** [O: grep], so two instances produce the identical byte stream -> identical nonces/DH private keys/cookies/LAN query nonce. **`wsock_hle.c` therefore calls `srand(time ^ pid ^ id)` when `BLUR_NET_ID` is set**; a cleaner fix is to make `h_CryptGenRandom` read /dev/urandom. (The game may also use its own RNG/rdtsc; `rt_rdtsc` is real-time based so it differs.); (d) clock: guest `QueryPerformanceCounter` runs on `now_us()` (virtual); if it ever runs faster/slower than real time, DTLS retransmit/timeout logic will misbehave against a peer on a different scale; (e) legal/ToS: Demonware endpoints are not contacted at all.

## 10. Cheap runtime experiments for the main harness owner
1. **Is LAN reachable without a login?** Once the UI works: Main menu -> Multiplayer -> LAN. Record whether a sign-in screen intervenes. (Static: none of the LAN script functions checks account state.)
2. **First bind/ports**: run with `BLUR_NETLOG=1` after the patch; log every `socket/bind/sendto` from the game (add `fprintf` in `h_bind` printing ip:port - 3 lines) to learn the real game port (`0x2800`=10240 expected) and whether NAT-type discovery sends to `stun.*` (they will fail via gethostbyname).
3. **Vehicle-sync message type**: during a race, histogram the first bytes of datagrams per (src,dst); the DTLS data packet hides the type (encrypted? or only MAC'd?). If only HMAC'd (likely: "bdDTLSData" with HMAC and no cipher in this old version [U]), the message type is `payload[...]` at the fragment header offset 3 and a histogram of `type` and size will reveal the vehicle updates (~10 Hz per car). If it is encrypted, set a breakpoint-style hook (`BLUR_HOOK`) at `0xb2cc10` (queue message) and log `type, len`.
4. **Call-site trace without UI**: `BLUR_HOOK` at 0xc170d0/0xc178f0 can force a LAN host from the command line if the Lua state were available - not feasible before the UI exists; alternative is to call `0xb3ff20` (discovery init) and `0xb3f670` directly with `call_guest` after boot to see the 50001 broadcast leave the process (needs `this` = the session object `DAT_011b31f8`).
5. **GetAdaptersInfo**: confirm the current harness result (`BLUR_STATS`): the call exists at startup and the returned value drives the "NetworkCableRemoved" UI state.

## 11. Reproducing / tools in this directory
| file | purpose |
|---|---|
| `scripts/iat_xrefs.py`, `fnrefs.py` | Winsock IAT call sites grouped by function |
| `scripts/dec.sh addr...` | headless Ghidra decompile of functions in a **copy** of `re/ghidra-project-rt` (`ghidra/`), patches the protector's `jmp [tbl]` trampolines in memory first (`DecompFn.java`), creates functions on demand |
| `scripts/callers.py`, `callargs.py` | direct-call xrefs (cached in `dec/callidx.pkl`) and immediate arguments at call sites (used to enumerate message types) |
| `scripts/strxref.py`, `fnstrings.py`, `dref.py`, `dd.py`, `bindtab.py`, `show.sh` | string/data xrefs, binding-table parser, dword dumper, extraction helper |
| `dec/*.txt` | raw decompilations backing the claims (`lan_c.txt` LAN, `sess_b2e_b42.txt` + `sess_b28_b2e.txt` session/stream layer, `race_handlers.txt` events, `bdplat.txt` platform socket, `router_connect.txt`) |
| `functions.tsv`, `script_bindings.txt` | function catalogue (addr, name, layer, confidence, evidence) and all 255 Lua bindings |
| `patch/`, `relay/` | HLE, tests, relay, harness patches |

Note on the decompiler: the packer's code scattering (`jmp [0x1e7xxxx]` trampolines, 2584 sites, 2345 into the 0x1e7 table) breaks a stock Ghidra analysis (bodies appear as "Could not recover jumptable"). `DecompFn.java` rewrites them in the in-memory copy before decompiling.

## 12. Update: backend prototype
The lobby/signalling/relay server, the browser `VirtualLan` library, the demo page and the end-to-end tests live in `port/mp/` (see `port/mp/README.md`). `wsock_hle.c` gained `BLUR_NET_ROOM`
(join a room through the server's UDP gateway, server-assigned virtual IP, HELLO payload `room=CODE;name=..;pw=..`, WELCOME/ERROR frames, BYE at exit); `port/mp/native/run_interop.js` proves
native `wsock_hle` <-> browser through one room.

## 13. Update: replication
Section 5.6 is superseded by `REPLICATION.md` (vehicle-state codec decoded, race events, lobby/race-start flow). `patch/wsock_hle.c` now also builds for wasm (see `port/mp/INTEGRATION.md`).
