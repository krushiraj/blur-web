# Blur replication, race flow and lobby requirements (static RE)

Companion to `NETCODE.md`. Same labels: **[O]** observed in decompiled/disassembled code or data, **[I]** inferred, **[U]** unknown.
Addresses are guest VAs of the unpacked image. Evidence: `dec/rep_*.txt`, `dec/phys_7e.txt`, `dec/flow_lua*.txt`, `dec/vt_methods.txt`, helper `scripts/rdseq.py`, `scripts/thunks.py`.

## 0. Result in one paragraph
Race state is replicated by **three independent mechanisms**: (1) **vehicle state**: every car is captured into a **25-byte quantised record** (24-byte bit-packed rigid-body state + 1 feedback byte)
and rebuilt on the receiver by a physics-frame-indexed jitter buffer with Hermite interpolation/extrapolation and error-triggered "jump to predicted" (section 1, **[O] for codec and receiver, [U] for the
network send call-site**); (2) **ordered, sequence-numbered "race events"** (25 types: power-up fire/hit/pickups, wrecks, boost...) piggy-backed on the per-player stats message, with replay protection by a per-player u16 sequence
(section 2, [O]); (3) **lobby/race-flow messages** (roster, car/loadout, votes, race-start block, per-player state machine) that drive a distributed "everyone ready, then start locally" countdown (sections 3-5, [O/I]).
There is **no authoritative server and no shared clock**: each peer starts its own physics-frame counter when it has seen every peer reach "WaitingForOthers" (section 4).

## 1. Vehicle state

### 1.1 Codec [O]
`CarDynamics` (vtable at 0xeaaf80, string "RacingPhysics.Cars.LoadCarDynamics" follows it) has network virtuals:
* **#23 `GetNetworkState(out25, peer)`** 0x7ea310 -> `7e5df0` (capture rigid body) + `7e5f10` (pack) writes 24 bytes, then `out[0x18] = clamp(peer->vtbl[0x80](), 0, 255)` (0xff when no peer). 0x7ea3d0 is the sibling that packs an explicit state (`7e74f0` fills a 0x90-byte `NetState`, `7e7640` quantises it).
* **#25 `ApplyNetworkState(in25, isLatest)`** 0x7e6a70 -> decode (`7e66c0`), store, adapt jitter buffer. 0x7e6c60 interpolates two packed states.

`NetState` (0x90 B, in memory): pos +0x28..0x30 (float, double copies at +0x38..0x48), linear velocity +0x50..0x58, angular-ish scalars +0x5c/+0x60/+0x64, +0x68..+0x7c (zeroed on decode), +0x80 scalar, +0x84 3-bit value, +0x88 u32 physics frame, +0x8c/+0x8d/+0x8e valid flags.

**Packed record, 24 bytes = six little-endian u32, bit fields counted from bit 0 of the 192-bit string** (decoded from 0x7e66d1 (unpack) and 0x7e7640 + 0x7e6623..0x7e66b7 (pack)):

| bits | width | field | decode (value = q * scale - offset) |
|---|---|---|---|
| 0-21 | 22 | pos X | q * 100000/2^22 (0.0238419) - 50000 |
| 22-41 | 20 | pos Y | q * 20000/2^20 (0.0190735) - 10000 |
| 42-63 | 22 | pos Z | as X |
| 64-75, 76-87, 88-99 | 12 each | orientation quaternion x, y, z ("smallest three"; w = sqrt(1 - x^2 - y^2 - z^2), >= 0) | q * 0.00048852 - 1.0 |
| 100-111 | 12 | lin. velocity X | q * 0.097704 - 200 |
| 112-119 | 8 | scalar A (+0x5c) | q * 0.0787402 - 10 |
| 120-127 | 8 | scalar B (+0x64) | q * 0.0787402 - 10 |
| 128-140 | 13 | lin. velocity Y | q * 0.02442 - 100 |
| 141-152 | 12 | lin. velocity Z | q * 0.097704 - 200 |
| 153-164 | 12 | scalar C (+0x60) | q * 0.024426 - 50 |
| 165-172 | 8 | scalar D (+0x80) | q * 0.015748 - 2 |
| 173-188 | 16 | **physics frame number** (low 16 bits) | unwrapped against the local frame counter: while (cur - f > 30000) f += 65536 |
| 189-191 | 3 | small state (+0x84) | stored `(value + 1) & 7` |
| byte 24 | 8 | receiver feedback | `clamp(peer->vtbl[0x80](), 0..255)` = jitter-buffer depth the sender sees for us [I] |

Units are consistent with metres and m/s (position range +-50 km, resolution 2.4 cm; velocity +-200 m/s). A, B, C are very likely the angular velocity components (x, z at +-10 rad/s with 8 bits, y at +-50 with 12 bits) [I]; D is a normalised control value [I].
Size on the wire: **25 B payload** per car update (+10 B message header, +DTLS/HMAC, +28 B UDP/IP).

### 1.2 Receive side [O]
`ApplyNetworkState`: decode; `if (car.expectedFrame <= state.frame)` accept (else stale -> dropped); optional adaptive playout delay: `car+0x908/0x90c/0x910` track a target buffer length from the feedback byte
(`DAT_011a8394`/`DAT_011a83bc` switch it on); resets the predicted-state ring `car+0x870` if the sim ran more than 31 frames ahead or the previous packet is older. Physics frame counter: `DAT_010d8cd8 - DAT_010d8cdc` (current minus race-start origin, see 4).
`NetworkUpdateModule::update` (0x7dbd50) compares the simulated car with the newest network reference every frame (lateral error `> DAT_010d9e7c`, forward error `> |v| * k + eps`, orientation dot `< DAT_010db0ec`) and sets "re-sync" which jumps the car to the predicted state (`CarNetworkJumpToPredicted`). `RunCatchUp`, `ResetRoundTrip`, `PatchAroundDeath`, `UseShortestSlerp` are the cvars of this system (registered at 0x11a8320..0x11a8398; consumer 0x7e66d1-0x7e72ed).
Remote cars are therefore **simulated locally, steered by corrections** (not pure playback) - consistent with state-sync at a low rate.

### 1.3 Rate and bandwidth
`Multiplayer.VehicleUpdateFrequencyInMilliseconds` default **100.0** (float at 0xe638b4, holder 0x1224544) [O value, I that it paces this record]. At 10 Hz: 25 B x 10 = **250 B/s payload per car**; ~100 B/s on the wire per directed pair after headers/crypto. With full mesh and N players one peer uploads (N-1) x ~1 KB/s for state plus events (below) plus voice (200 B frames when talking): ~11 KB/s for 12 players, ~19 KB/s for 20.

### 1.4 What is NOT found [U]
The call that hands the 25-byte record to the network layer. The direct-call scan of every stream-writer and every message-send function (`scripts/thunks.py`, `scripts/callargs.py`) finds no writer outside the lobby/race-flow set, and the image contains almost no `call [reg+disp]` forms (the protector turns virtual calls into other sequences), so the vtable users of #23/#25 cannot be found statically.
Cheapest experiments (they need the game running a LAN race, i.e. after the UI works):
1. `BLUR_HOOK=0x7ea310` (and 0x7e6a70): log return addresses and call rate (expect ~10 Hz per local car / ~10 Hz per remote car) -> gives the sender/receiver functions.
2. `BLUR_HOOK=0xb2cc10` (queue message): log `(type, len, reliable, channel)`; the car record shows as a 25-35 byte unreliable message at ~10 Hz per peer. Wire inspection with `BLUR_NETLOG` is useless because DTLS data packets are **encrypted** (cipher block + HMAC, 0xd1dc30, `bdDTLSData::serialize`), so classify at 0xb2cc10 or at the plaintext after decrypt.

## 2. Race events (ordered gameplay stream) [O structure, I semantics]
Per-player block in the mode object (stride **0x540**, base `mode+0xf0`): seq at +0x6c6 (u16), local/remote flags +0x6c3/+0x6c4, state table `DAT_01224d7c` stride 0xd4.
Events travel inside type **0x18** (`bb0a00` writer / `bb2890` reader = stats snapshot with the sender's pending events appended) and are dispatched by `bb2100`: `u8 count`, per event `u16 seq` (accepted only if `seq == last + 1`, otherwise handlers run with `replay=false` and state is not applied) and `u8 type` 1..25.
The sender's event list is built by `bb0a00` -> `bbe310` (`u8 n; per event: u16 id, blob`), blobs are sized by `0xc0b2a0`. Receivers apply them in order; a lost fragment of the reliable channel is re-sent by the message layer, so the sequence check mostly guards duplicate/old data.

Field layouts per event (from `scripts/rdseq.py`; `slot` = u8 player slot translated by `ba58c0`):

| id | handler | payload | meaning [I] |
|---|---|---|---|
| 1 | bac7a0 | u32 id | remove/expire entity by id (`b90140`) |
| 2 | bad4f0 | u32 entityId, u8 owner, u8 slot, 48 B transform (3 x vec4 / 4x3 matrix) | spawn pickup/projectile type A at transform (`b94cd0`) |
| 3 | bad650 | same as 2 | spawn type B (`b94fe0`) |
| 4 | bac7d0 | u8 attackerSlot, u8 victimSlot, u32 amount | damage/hit; slot 0 = environment (0x14 = none) |
| 5 | bae7f0 | packed blob decoded by `bbb9f0` (5 x u8, 2 x u32, 2 x f32, 12 B, 2 x u16) | power-up activation with parameters (attack/shield/boost) |
| 6 | bac870 | u32 id, 8 B | entity state update |
| 7 | bad430 | u32 id, 48 B transform, f32 | entity transform update (mines/projectiles in flight) |
| 8 | bad7b0 | `bbbb40` blob: 4 x u8 (nibble pairs), u32, u16, 2 x 12 B (two vec3) | hit with position + normal |
| 9 | bac8f0 | u8 slot, f32 | boost/speed change |
| 10 | baf980 | u8 slot, u8 flag | flag (e.g. shield on/off) |
| 11 | bac960 | u8 slot, u32, f32, f32, f32 | position-like triple |
| 12 | bb17a0 | u8, u8 | simple notification |
| 13 | bada80 | u8 slot | notification |
| 14 | bac9b0 | u8 attacker, u8 victim (0 -> 0x14), u8 type, u32 amount | typed hit/score event on the vehicle component |
| 15 | badb10 | `bbbc80` blob: 3 x u8 (+5-bit and 3-bit subfields) | pickup collected/ability use |
| 16 | bac6a0 | u32 id, u8, u8 | enable/disable ability on entity (`b4e390`/`b51570`) |
| 17 | bae9c0 | u8, u8, u16 | notification with value |
| 18 | baea90 | u8, u8, u8 | notification |
| 19 | baca60 | u8, u8, u8, u32 | notification with amount |
| 20 | bacb20 | `bbbb40` blob (as 8) | second hit variant |
| 21 | bad920 | `bbbbf0` blob: 4 x u8, 2 x u32 | ability pair event |
| 22 | bac720 | u32 id, u8 slot, f32 | set value on entity |
| 23 | badc90 | u8, u8 | mode-specific (Destruction/Elimination) |
| 24 | badc20 | u8 | mode-specific |
| 25 | baebc0 | u8, u8 | mode-specific |

Event 8/20 helper (`bbbb40`): `u8 a (hi nibble = typeA, lo nibble = typeB), u8, u8, u8, u32, u16, vec3(12 B), vec3(12 B)`; semantic names are [U], layouts [O].
Stats snapshot (type 0x18 / 0x19): per player `u8 slot, 4 B id, 25 B name, u32 score, u32`; type 0x19 carries the full per-player **0xd4-byte statistics struct** (53 dwords: elapsed time, distance, speed averages, hit counters...), `baf3c0` fills it, `baf7f0` writes it (`u8 1, u8 slot, 0xd4 B`), `bb14b0` reads it. Stats go to scoreboards/results, not to simulation.

## 3. Lobby/race-flow messages
Two session objects [I: from which Lua binding uses which global] (script tables `NetParty` and `NetRace`): `DAT_011b31fc` = **NetParty** (flags +0x410 init, +0x411 active; dispatcher `0xb3cb40`) and `DAT_011b31f8` = **NetRace** (+0x410 init, +0x412 active/joined; dispatcher `0xb3c7c0`); both print `+8` session states (7 = in race session). Player records (`b3e810(i)`): +0x30/+0x34 u64 id, +0x41 local-slot (0xff = remote), +0x11c/+0xbc race slot, **+0xe8 player state**, +0x15c ready token, +0x160 flags, +0x138/+0x150 loadout blobs, +0x164 12-byte blob.
Player state enum (+0xe8) [O strings in order + O set sites]: 0 Unset, 1 Joining, 2 Disconnecting, 3 JoinComplete, 4 Idle, 5 ChangingSettings, 6 ChangingCar, 7 LoadingRace, 8 LoadingUi, 9 PartyBusy, 10 PartySyncJoinRaceStart, 11 PartyJoinRace, 12 PartySyncJoinRaceEnd, 13 PartyJoinRaceFailed, 14 PartyJoinRaceSuccessful, 15 WaitingForOthers, 16 Racing, 17 Finished, 18 ViewingResults, 19 ViewingResultsIdle (`EnterRaceLoad` sets 7, `EnterSyncAfterRaceLoad` 15, `StartPlay` 16, `EnterResults` 18).

Message types (type byte) and their roles [I from field shapes + handler names]:

| type | direction | payload | role |
|---|---|---|---|
| 3 | joiner -> host | `u32 0x0F000000` (version 15), `u8 slot`, `u64 sessionId`, `u64 playerId`, `char name[25]`, `u16 x2`, `u32`, `u8`, then per known player `0x38 B + u8 + 0xC B + u8` | join request / hello (0xb30c70, 0xb32120) |
| 4 | host -> joiner | `u8` | join accepted / ack |
| 5 | host -> joiner | `bytes[0x54]`, `u8 n`, per player `u64 id, u8, name[25], u16, u16, u32, u8, 12 B, 56 B, 3 x u8` | **roster** |
| 7 | both | `u8 n, u8, ... 12 B, 24 B, u8, u8` | player info / **car selection + loadout** (24 B car config, 12 B livery) |
| 8 | host -> client | `bytes[0x54]` | session/security info refresh |
| 10 (0xa) | both | `u32 epoch, u64 playerId, u8 state` | **player state change** broadcast (the +0xe8 enum) |
| 11 | both | `u64, u8, ...` | player removed / dropped |
| 12-14 | host-election | 24 B blob / empty | **host migration** (see 5) |
| 15 (0xf) | host -> clients | per player `u8 slot, 24 B (+0x138), 12 B (+0x150)` | initial vehicle/loadout table (sent once per player, flag +0x160 bit0) |
| 16 (0x10) | host -> clients | **`0x838`-byte race-start block** | race start descriptor (see 4) |
| 18 (0x12) | client -> host | `u64` | vote to skip next race |
| 19 (0x13) | host -> clients | `u64, u8` | vote results / next race id |
| 20 (0x14) | host -> clients | `u16` | vote counter / timer |
| 21 (0x15) | both | `u8 n, bytes` | **voice frame(s)** (200-byte codec frames) |
| 22 (0x16) | both | `u8, u8, 24 B` | per-player settings/car update |
| 23 (0x17) | host -> clients | `u8[0x102]` | blob (race/playlist info) |
| 24 (0x18) | all | stats + events | scoreboard snapshot + race events (section 2) |
| 25 (0x19) | all | `u8 1, u8 slot, 0xd4 B` | per-player statistics struct |
| 26/27 | host -> clients | `u8, 0x90 B, u8` / `u8` | profile block / small notification |
| 28 (0x1c) | host -> clients, every 0.5 s | `f32 + u8 n + n x 16 B` | playlist/lobby settings heartbeat |
| 29 (0x1d) | host | `u8 slot, 12 B` | per-player blob |
| 30 (0x1e) | host | race parameters (`u8 mode, u16 x3, u32, f32 ...`) | race parameters sync |
| 31 (0x1f) | host | `u8, u64...` | race flow command |
| 32 (0x20) | both | `u8 n, n x (u8 slot, u8 flag)` | per-player flags (e.g. ready / muted) |
| 33 (0x21) | all | `u8` | race-finished/ping-like event |
| 2 | each peer, periodic (timer in 0xb3fa80) | empty | **keep-alive / RTT probe** (round trip is measured at the message layer) |

## 4. Start-of-race sequence [O flow, I details]
1. Host picks race/settings (UI), calls **`LaunchCustomRace`** (0xc18c80): fills the global MP block (`FUN_0042b600()+8`, **0x838 bytes**, built by the mode's `vtbl+0x48`), sets `block[0]=1`, `+0x843=1`, sends **type 0x10 with the whole block** to every remote peer whose state is Idle(4) or ChangingCar(6); also applies it locally (0xbf8dd0).
   The block contains the race parameters and a **player table**: `u8 count` at +0x18, entries of **0x68 bytes** (`slot` at entry+0x38, car/loadout/team/colour fields, name), and the receivers re-order their local table by slot (`bf92f0` copies entries 0x68 B at a time, matching `player.+0x11c`).
2. Each peer loads the level: `EnterRaceLoad` (state 7 LoadingRace). Level assets are local (the same data files are required: error `ContentMismatch`; playlist version checked: `PlaylistVerHost/Client`).
3. `EnterSyncAfterRaceLoad` (c183b0): every remote player in a valid state gets state **15 WaitingForOthers**; flag +0x840 set; `ChangeLocalPlayerState` messages (type 10) tell everyone.
4. `RaceStartSynchronised` (c18eb0) is true when no remote player (with `+0x15c != -1`) is in a state `1..14`, i.e. everyone is at least WaitingForOthers.
5. `StartPlay` (c18460): all peers -> state **16 Racing**, `+0x842=1`, and **`DAT_010d8cdc = DAT_010d8cd8`**: the local physics-frame counter becomes frame 0 of the race. The 3-2-1 intro (`MP_E3_COUNTDOWN`, "LobbyCountDown03/10", rolling start `Amax.Multiplayer.RollingStartHandOfGod`) is run locally on each peer.
**There is no clock sync message and no NTP-like handshake**: frame numbers in the car record are relative to each peer's own start; offsets (= the difference between when peers see "everybody ready") are absorbed by the adaptive jitter buffer (feedback byte, 1.2) and by the correction logic. Start skew between peers is therefore roughly one network one-way delay plus polling jitter. For the browser port this means no host clock to emulate; the transport must only be low-jitter and must deliver the state-change messages reliably.

## 5. Leave, drop and host migration [I, low confidence]
* A peer is dropped by the message layer when its datagrams stop (router status != established; `bdSocketRouter` DTLS reset/"connection reset" lines, `LobbyConnectionLost`, `MP_TIMED_OUT`, idle-timeout cvars `Multiplayer.IdleTimeout.*`). Handlers type 11 / state 2 Disconnecting remove it from the table (0xb3ee90, 0xb3eba0 clears `+0x15c` and notifies UI).
* `IsPlayerMigrating` / `IsSessionMigrating` read NetRace `+0x413/+0x414`. The migration state machine is at `NetParty +0x668` (values 0..6): on host loss peers with the lowest rank [I] enter 4/5/6, exchange types 12/13/14 (a 24-byte blob = new host's `SessionInfo`), then the new host `FUN_00d6a530` **updates the online matchmaking session** (bdMatchMaking) - i.e. migration is designed around the Demonware lobby. In **LAN mode** the matchmaking service does not exist: expect the session to end (`MP_DISCONNECTED`) if the host leaves; this needs the runtime experiment "kill the host's instance mid-lobby".
* Race in progress when a non-host leaves: the car is removed, its events stop; the idle-timeout system ghosts inactive players (`Multiplayer.IdleTimeout.ForceIdleGhost`, `LeftRaceGhostTime`).

## 6. What the game needs from the lobby at race start (checklist for our backend/UI)
* **Player slots**: stable 0..19 index per player; `slot` appears in every event/stat; our virtual IP slot (`10.77.0.<n>`) is independent but should be stable for the whole session.
* **Peer addresses** for every player in the roster (the bdCommonAddr list): provided by `GetAdaptersInfo` virtual IPs - nothing else.
* **Identity**: each peer sends `playerId (u64)` and a 25-byte name; ids only need to be unique inside the session (the game derives them from the Demonware user id online; in LAN a locally generated id is used [I]).
* **Car selection / loadout**: types 7/22/15 (24-byte car config + 12-byte livery + 56-byte ability/perk block) travel peer-to-peer; the host's race-start block carries the final table. Car/mod data files must exist on every machine (same `gamedata.pak`).
* **Map vote**: types 18/19/20 (`VoteOnNextRace`, `SendVoteResults`, `ReceivedVotingResults`); timers are local (`MP_VOTE_TIMER`).
* **Ready states**: the player-state enum via type 10; race starts when all reach WaitingForOthers.
* **Version gate**: `CheckLanServerNetworkVersion` (protocol 15 in the discovery advert, build/content compare), playlist version checks (`PlaylistVer*`).
* **Team modes**: `AreTeamsValid/AreTeamsBalanced` evaluated locally from the player table.
* Nothing in LAN mode needs server time, accounts, stats upload or entitlements (those are `bdLobbyService` tasks, online only).

## 7. Confidence summary
| item | label |
|---|---|
| 24-byte car-state bit layout, ranges, frame unwrap | O |
| receive-side logic (jitter buffer, jump-to-predicted) | O (names I) |
| call site that sends/receives the car record, exact message type | **U** |
| 10 Hz cadence | O value, I binding |
| 25 race-event ids/field layouts | O; meanings I |
| lobby message roles | I (shapes O) |
| start sequence (no clock sync, local frame origin) | O |
| host migration behaviour in LAN | I, low |
