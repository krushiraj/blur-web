# Blur-in-browser: progress log

Personal reverse-engineering / porting study. Game files are the user's own copy and never leave this machine.
See `BLUR_WEB_PORT_PLAN.md` for the overall plan and research.

## Status (2026-10-16)

**The game runs, statically recompiled, and renders in a browser.** The decrypted x86 image (plus `msvcr90.dll`) is lifted to C
(`port/recomp`), linked with our Win32/D3D9 host layer (`port/harness`), and streams D3D9 commands to a WebGL2 page. Verified:
the front-end 3D scene (car + night city with bloom/flare) at ~60 fps, 378 draws/frame, 90 shader programs, 1961 textures
(screenshot sent to the user; `/tmp/claude-501/shots2/a3.png`). Recompiled code sustains ~180 frames/s of command generation
(the Unicorn emulator could not get past the loading screen).

Known visual bug: the car body is black (paint/environment-map path in the renderer).

## Layout

| Path | What |
|---|---|
| `game/` | `archive/` original zip, `extracted/BLUR/` game files |
| `re/` | Ghidra projects/scripts, `dump/blur_runtime.bin` (decrypted image, exit-time), `blur_oep.bin` (image at the entry point), `msvcr90.bin` |
| `port/recomp/` | **static recompiler**: `lift.py` (x86-32 → C via Capstone), `lift_par.py` (parallel generator, discovery, shared-block extraction, pruning), `lift_extra.py` (incremental lifting of run-time-learned targets), `rt.h`, `rt_vec.c` (SSE2/MMX integer helpers), `oracle.c` (differential tester vs Unicorn), `overrides.txt`, `learned*.txt`, `blocks.txt` (emulator-executed block starts), `extra_loop.sh` (run → learn → relift → relink loop) |
| `port/harness/` | emulator (Unicorn) boot to OEP + host runtime: `rt.c` (dispatcher, API calls, guest threads, CPUID), Win32/D3D9/D3DX/DirectInput HLE, `web/` WebGL2 renderer |
| `tools/shot` | headless-Chromium screenshot tool |

## How it works

1. `blurhle` (Unicorn) loads the packed `Blur.exe`; the packer stub decrypts and jumps to the real entry point 0xd43105.
2. With `BLUR_RECOMP=1` the harness stops emulating there and runs the recompiled code (`blurrt`): guest memory is one flat 4 GB mapping shared with Unicorn.
3. Recompiler model: every call, return and indirect jump is a `MUSTTAIL` jump through `rt_dispatch` (the guest stack alone defines control flow, which is what the packer's return-address tricks need); call-return addresses are entry points; flags travel in `c->fl`; host APIs are stubs the dispatcher recognises.
4. Discovery: Ghidra function list, call/jump targets, code pointers in data, thunk cells, **blocks the emulator executed**, plus targets the recompiled game reached at run time (`learned_x.txt`). Shared blocks become their own functions (dedups 3x code), unreachable code is pruned (373k functions, ~170 MB binary).
5. Validation: `oracle` runs random-state differential tests of lifted functions against Unicorn (integer set: 0 mismatches in 5.5k trials; float set: only NaN-sign differences). The function-entry-sequence diff against the emulator located the first divergences.

Build/run (see memory for gotchas): `cd port/recomp && python lift_par.py gen_p2 && make -j14 GEN=gen_p2 OPT=-O1 OBJDIR=obj_p2`; `cd ../harness && make blurrt OBJDIR=obj_p2`;
`BLUR_WEB=8765 BLUR_RECOMP=1 ./blurrt ../../game/extracted/BLUR/Blur.exe` then open `http://127.0.0.1:8765/`. Incremental loop: `port/recomp/extra_loop.sh`.

## Findings worth keeping

- `Blur.exe`: PE32 x86, D3D9, ~15 threads, Winsock by ordinal, no xlive. The `.8thSin` section is the packer: decrypt stub + "call gates" that derive jump targets from **CPUID** (our CPUID must equal Unicorn's), rewrite return addresses, and run small routines reached through data cells.
- The 8 GB-free design constraint: generated C is ~520 MB; a native arm64 link needs `-O1` and a pruned function set.
- WebGL2 specifics fixed: no LUMINANCE/ALPHA storage formats (converted to RGBA8), buffers keep one binding target, SPIRV-Cross combined samplers must be named via the C API.

## Next

1. Input (keyboard/mouse/gamepad → DirectInput/XInput state) to drive the menus; fix the black car paint.
2. Bink movies (FFmpeg), audio (DirectSound/XACT → WebAudio).
3. Browser build: Emscripten/wasm (musttail needs `-mtail-call`), in-process renderer instead of the WebSocket, asset streaming of the 1.1 GB paks.
4. Multiplayer: Winsock layer → WebRTC data channels + lobby server; hand-decompile the netcode for modding.
