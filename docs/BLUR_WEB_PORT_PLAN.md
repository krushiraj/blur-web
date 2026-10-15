# Plan: Blur (2010, PC) in the browser with LAN/online lobbies

Researched 2026-10-15 (Firecrawl). Items marked **[inference]** are my design conclusions, not sourced facts.

## 1. What we are porting (facts)

| Fact | Source |
|---|---|
| Bizarre Creations, Activision, 2010. PC / PS3 / 360. Studio closed Feb 2011. | Wikipedia |
| Engine: in-house **"Horizon"**, written from scratch for multicore 360/PS3. Multithreaded renderer, deferred lighting (SPU-offloaded on PS3). | Digital Foundry tech interview |
| PC renderer: **Direct3D 9.0c**. | PCGamingWiki |
| Multiplayer: up to **20 players online or over LAN**; 4-player split screen. | Wikipedia |
| Official master server is **shut down**. Delisted from Steam. Depends on dead Games for Windows Live (GFWL); community guides exist to run without it; retail patch 1.2 exists. LAN is reported flaky. | PCGamingWiki, Steam guides |

Consequence: the multiplayer back end has to be rebuilt either way. The game logic and netcode above it can be kept.

## 2. Legal and ethics guardrails (decide these first)

- Blur is Activision IP. Any distribution of the game's code, assets, or a decompiled source tree is infringement. This is the main risk of the whole project.
- Design so we **ship no Blur bytes**: the user supplies their own legally owned install. The web app loads it locally (File System Access API / OPFS) and runs it. Only our runtime, translator, and lobby server are public.
- Prefer a translator that converts the user's own `.exe` on their machine over publishing pre-translated output. Do not publish recompiled C/wasm of Blur.
- US reverse-engineering law: *Sega v. Accolade* and *Sony v. Connectix* found intermediate copying for interoperability to be fair use (EFF FAQ). DMCA §1201(f) has a narrow interoperability exception. GFWL/SecuROM-style protection may fall outside it. Contract/EULA terms are separate. **Get a lawyer's read before any public release.** This is research, not legal advice.
- Do not use it to enable piracy. Do not host or link to ROM-style copies of the game.

## 3. Correcting the starting tool list

- **MinHook/Detours/Cheat Engine/REFramework/UE4SS**: native-Windows tools. They are not part of a browser port. Use them only for dynamic analysis, i.e. to run the real game, log its D3D9/Winsock/file calls, and record golden traces for later comparison.
- **Claude 3.5 Sonnet / Codex API scripts**: out of date. Use current models through Ghidra MCP integrations (GhidraMCP, GhidrAssist) or the REA MCP tools already loaded in this session. **LLM4Decompile** targets Linux x86_64 GCC binaries (O0-O3), so it won't help with an MSVC Windows binary. A Hacker News thread reports Ghidra is weak on C++ and that LLMs get confused; plan for manual vtable and class recovery.
- **SPIRV-Cross/Naga**: they consume SPIR-V (or WGSL/GLSL), not D3D9 shader bytecode. Blur uses SM3-era programmable shaders **[inference]**, so we need a D3D9 shader-bytecode to SPIR-V step first (DXVK's d3d9 front end is the reference design), then SPIRV-Cross to GLSL ES 3.00 (WebGL2) or Naga to WGSL (WebGPU).
- **Binaryen/wasm-opt**: valid, final-stage size and speed pass.
- **Emscripten**: valid. Limits are wasm32 at 4 GB max memory (`-sMAXIMUM_MEMORY=4GB`), and pthreads that need SharedArrayBuffer plus COOP/COEP headers.

## 4. Three porting strategies

| | A. Emulate x86 + Win32 in wasm | B. Static recompile x86 -> C/wasm + reimplemented Win32/D3D9 (**recommended**) | C. Full decompile to readable C++ source port |
|---|---|---|---|
| Precedent | wine-assembly, v86, CheerpX | Theseus (win32 x86 -> wasm, threads as web workers), psp-web-recomp / ps2-web-recomp (God of War from boot to gameplay, 60 fps on laptop) | GunZ browser port with d3d9-webgl (had source), re3-style projects |
| Speed to first pixel | Fastest | Medium | Slowest (years for an engine this size) |
| Runtime perf | Poor to marginal for a 2010 3D racer | Good | Best |
| Moddability ("game merges", new content) | Low | Medium (hook points in generated code and in the host API layer) | Highest |
| Legal surface | Smallest | Small if translation is done client-side | Largest if source is shared |

**Plan: B as the main line, A as a throwaway feasibility probe, C only incrementally** for subsystems we want to change (netcode, tweaks), by lifting those functions into hand-written C++ that replaces the translated version.

## 5. Target architecture (B)

```
User's own Blur install --(local file access)--> Browser tab
  Translator (runs locally or in a build step on user's machine)
     blur.exe + DLLs --> lifted IR --> C --> wasm (Emscripten/clang, wasm-opt)
  Host runtime (ours, open source)
     Win32 shim: files/OPFS, threads->Web Workers, timers, heap, TLS, SEH, registry stub
     D3D9 layer: device/state/textures/VB/IB; HLSL SM3 -> SPIR-V -> GLSL ES / WGSL
     Audio: XAudio/DirectSound/Bink-style streams -> AudioWorklet (check actual middleware)
     Input: Gamepad API, keyboard; drops XInput/GFWL
     Net: Winsock/XNet shim -> WebRTC DataChannel (unordered, maxRetransmits 0) or WebTransport relay
Lobby service (ours): WebSocket signalling, room codes, matchmaking, TURN/relay fallback
```

**[inference]** Because a 2010 PC game is 32-bit, it fits wasm32. The 4 GB limit is not a blocker. Horizon's multithreaded job system maps to pthreads/Web Workers, so the page must be cross-origin isolated.

## 6. Multiplayer design

- Browsers cannot send raw UDP or LAN broadcast. "LAN" becomes **lobby with room code**, optionally same-network hinting via the signalling server.
- Replace GFWL and the dead master server with our own lobby service. First job is to RE how Blur's session/matchmaking layer and the GFWL/XNet calls are structured.
- Cleanest hook point **[inference]**: because we reimplement Win32/Winsock ourselves, back the game's sockets with WebRTC data channels (unordered, no retransmits) and keep the game's own netcode untouched. This avoids rewriting netcode if the game is peer or host-authoritative.
- Scale: WebRTC mesh scales poorly past a handful of peers. For 20 players, expect a host-relay topology or a server relay. Start with 2-4 players and measure.
- Need to determine early: is the sim deterministic lockstep, or host-authoritative state replication? That decides latency tolerance and whether rollback is even an option.

## 7. Phased roadmap with exit criteria

| Phase | Work | Exit criterion |
|---|---|---|
| 0 Legal and setup | Lawyer review, clean repo rules (no game bytes), `yarn dlx rea-agents@latest setup` (user asked for REA; run when binary work begins), Ghidra + GhidraMCP, a Windows box/VM with owned Blur 1.2 | Written policy; tools open the exe |
| 1 Baseline and recon | Run Blur on a Windows VM without GFWL. Load exe in Ghidra/REA. Inventory imports (d3d9, d3dx, ws2_32, dsound/xaudio, dinput/xinput, GFWL `xlive.dll`), packers/DRM, middleware, file formats | Import map plus DRM verdict; list of every OS API used |
| 2 Trace capture | Hook D3D9, Winsock, and file I/O on native Windows (Detours/MinHook, Frida). Record per-frame D3D call streams and packet captures of a LAN match | Golden traces for rendering and net protocol |
| 3 Feasibility probe | Run the exe in an existing wasm x86 emulator to find which Win32 calls it needs | Reaches menu, or a precise blocker list |
| 4 Translator and Win32 shim | Static x86->C lifting (build on Theseus-style approach), CRT, threads, SEH, memory | Boots to first D3D9 call in the browser |
| 5 Graphics | D3D9->WebGL2 first (reuse ideas from d3d9-webgl; its FFP-only scope isn't enough), SM3 shader pipeline, then optional WebGPU | Menu and a single-player race render, compared against golden traces |
| 6 Audio/input/save | Audio backend, gamepad, OPFS saves | Full single-player race playable |
| 7 Netcode | Winsock/XNet shim over WebRTC, lobby server, TURN | 2-player race across two browsers |
| 8 Scale and polish | 8, then 20 players, relay topology, loading and streaming, wasm-opt, WebGPU path | 20-player lobby stable |
| 9 Mods (optional) | Replace chosen functions with hand-written C++ (new modes, tracks, cars) | A custom rule or mod runs |

## 8. Top risks

1. **Legal/DRM**: GFWL or other protection may block distribution of a working tool. Mitigation: no assets shipped, client-side translation, legal review.
2. **Renderer fidelity**: SM3 shaders and Horizon's deferred and post-process effects (the "blur" visuals) are the hard visual part. Mitigation: golden-trace diffing.
3. **Performance**: translated code plus layered D3D9 may miss 60 fps. Mitigation: profile early, wasm threads, OffscreenCanvas render worker (as psp-web-recomp does).
4. **Unknown netcode**: could be non-deterministic or tied to GFWL session objects. Mitigation: Phase 2 captures.
5. **Browser limits**: COOP/COEP hosting needs, memory caps on mobile, WebGPU availability.
6. **Scope**: engine-sized RE. Time-box each phase and keep the throwaway probe honest.

## 9. Key sources

- Blur: en.wikipedia.org/wiki/Blur_(video_game); pcgamingwiki.com/wiki/Blur; digitalfoundry.net/articles/digitalfoundry-blur-tech-interview
- Recomp to web: github.com/snuri00/psp-web-recomp; neugierig.org/software/blog/2026/05/theseus-wasm.html; github.com/vgrichina/wine-assembly; labs.leaningtech.com/blog/cx-10
- D3D9 to WebGL: github.com/LostMyCode/d3d9-webgl (GunZ port)
- Emscripten: emscripten.org pthreads docs; v8.dev/blog/4gb-wasm-memory
- Netcode: webrtchacks.com/datachannel-multiplayer-game; sitepoint WebRTC data channels vs WebSockets
- LLM-assisted RE: github.com/albertan017/LLM4Decompile; GhidraMCP / GhidrAssist
- Law: eff.org/issues/coders/reverse-engineering-faq
