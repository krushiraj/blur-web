# NET_GATE: what the online-account steps of the front-end wait for (static findings, net-re)

Status: partial. Static decompilation only; my one dynamic probe run (copy of port/harness/blurrt, BLUR_LUATRACE + BLUR_HOOK) never left the idle state
(no [lua] lines), so nothing below is confirmed at runtime. Lua sources (Screens\Account\NetLogin.lua / NetRegister.lua) were NOT found in any pak
(cache/*.pak name tables, zlib streams of ui_frontend.bpr and heap strings were searched); the script logic is therefore unknown.

## Bindings (decompiled, addresses in script_bindings.txt)
* `ReadOnlineAccountDetails(idName, idPass)` 0xc19c40: pure local profile read. Takes the game profile (0xb9e8e0), copies the wide strings at profile+0xff9 (account
  name) and +0x1019 (password). Pushes `true` and fills two Lua string widgets only if BOTH are non-empty; otherwise pushes `false`. No network involved.
  A fresh profile has neither, so it returns false -> the script almost certainly branches to the registration screen (NetRegister.lua) which waits for text input.
* `NetworkConnectionActive()` 0xc15f70: pushes the byte at 0x11b23f1 (always returns 1 result). Only reader; no direct stores found in the image (set via struct/loop code).
  0x11b23f0 (neighbour) is the "net system enabled" flag tested in 0xb2f170 before the net manager is brought up.
* `StartDLC` 0xc14330 / `ContinueDLC` 0xc17060: DLC controller; ContinueDLC pushes (done, flag DAT_011348cc) and is polled until the DLC state machine (0x4e7f40) finishes; local, no network.
* Account network ops go through the object at `*DAT_011b31d4` (guarded by the lock-valid byte DAT_011b31d0, lock object 0x11b31c8):
  vtable +0xc `CreateAccount(name, pass, key, ...)` (CreateAccount binding 0xc15450), +0x10 `Connect(bool, user, pass)` (Connect 0xc15670), +0x14 `Disconnect`.
  `ConnectionStatus` 0xc15730 returns state=[obj+4], [obj+8], flag=[obj+0xc]; `ConnectionStatusIsOnline` true iff state not in {0,2} and flag set; the session is usable (0xb2c300)
  only when [obj+4]==3 and bytes +0xc,+0xd,+0x20 are set. So states: 0 idle, 2 failed/offline, 3 online. `OnlineConnectionPossible` 0xc15610 needs DAT_010e5874 != -1, 0x79b320 != 0 and non-empty account strings.
* `CheckAccountDetails(a,b,c,d)` 0xc1ddd0: local validation of the registration form (returns code>>16: 0 ok, 1 = empty field(s), 2 = bad format); on success copies name/pass/key into the profile (0xff9/0x1019/0x1039).
  `CreateAccountResultCode()` 0xc154d0 polls DAT_01101074 (result ready) / DAT_01101078 (code) and clears it: the only network-dependent wait in this group.

## Conclusion
The stall after ReadOnlineAccountDetails is consistent with a USER-INPUT gate (registration form, UI invisible) rather than a missing network error: the account call returns false on a
new profile and the next bindings (CheckAccountDetails / CreateAccount / CreateAccountResultCode) are only called after text entry. Cheapest tests, in order:
1. Pre-seed the profile fields (+0xff9, +0x1019 as UTF-16, e.g. via BLUR_POKE/WATCHW or in the saved profile) so ReadOnlineAccountDetails returns true, then watch whether Connect/ConnectionStatus are called and what state they poll.
2. Hook 0xc15730/0xc15670 (BLUR_HOOK) to see the state; if the script polls ConnectionStatus until state is 2 or 3, force [DAT_011b31d4+4]=2 (failed) with flag +0xc=0 to take the offline path (strings Connecting_Offline / Failed_Offline).
3. Send Escape/B on the invisible form (Back is the likely "play offline" exit).

## HLE check
The current port/harness/hle2.c already models an offline PC (connect -> WSAEWOULDBLOCK then SO_ERROR 10061, gethostbyname -> 11001, per-thread WSAGetLastError).
Remaining gaps vs a real offline PC: `sendto`/`recvfrom`/`send`/`recv` all fail with 10057 (WSAENOTCONN); a real unconnected UDP socket accepts `sendto` (returns length) and a non-blocking
`recvfrom` gives WSAEWOULDBLOCK (10035); `bind`/`setsockopt`/`getsockname` should succeed with a real local address; `select` with a timeout should wait (not spin). `port/work/net-re/patch/wsock_hle.c`
(+`integrate_wsock.diff`) implements these (UDP semantics, 10035, SO_ERROR, select timeouts, DNS failure for non-local names). Not re-tested against the newest hle2.c.
