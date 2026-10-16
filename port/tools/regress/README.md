# port/tools/regress: regression safety net

One command before delivering or merging a patch:

```
port/tools/regress/run_all.sh            # ~8-10 min: determinism + smoke walk + (wasm if built)     -> PASS/FAIL table
port/tools/regress/run_all.sh --quick    # ~1 min: determinism only (1500 frames, two identical runs)
port/tools/regress/run_all.sh --bin /path/to/blurrt_copy --harness /path/to/harness_copy   # test a work-dir build
```

Prerequisite: the harness must contain `harness-regress.patch` (3 small hunks in `gfx.c` / `d3d9.c`; `cd port && patch -p1 < tools/regress/harness-regress.patch`):
`BLUR_FRAMEHASH=1` (hash of every flushed render-command batch, works without a viewer), `BLUR_MAXFRAMES=N` (stop after N frames),
`BLUR_CHECKPOINTS="7700:name,..."` (print `[checkpoint] <frame> <name>` and pause ~1.5 s when a viewer is attached, so it can screenshot exactly that frame).

## What is checked

| step | script | what |
|---|---|---|
| determinism | `det_check.sh` | Runs the native harness headless twice (`BLUR_DET=5`, frame-keyed input script, `BLUR_FRAMEHASH`), requires **bit-identical per-frame command-stream hashes**, then compares every 50th hash with `refs/golden_hashes.txt` (a change = the rendering command stream changed; intentional after d3d9/HLE/game-data changes: `det_check.sh --update-golden`) |
| smoke | `smoke.sh` -> `smoke.js` | Starts the harness (cwd `port/harness`, viewer port 8797) with `BLUR_DET` and the canonical walk in `scenario.json` (frame-keyed taps: front-end -> "Press ENTER" -> Online Account dialog -> Skip -> warning dialog -> main menu -> Right), a headless Chromium viewer screenshots the canvas at 5 checkpoints. Asserts: clean exit at the frame limit; no `not recompiled` / `UNIMPLEMENTED` / deadlock / watchdog / heap-canary overrun / heap exhausted / object table full; no unexpected `[strict]` findings; guest heap bounded and flat in steady state; textures populated (`BLUR_TEXAUDIT`: filled >= 600, all-zero <= 120); median draws/frame in range (CPU-load independent, replaces fps); each screenshot is not flat **and** matches `refs/<name>.png` (mean abs error <= 0.06, 8x8-block SSIM >= 0.80 at 320x180, tolerant of small shading changes, flags UI/geometry breakage) |
| wasm | `smoke.sh --wasm` -> `wasm_check.sh` | native vs wasm API-call sequence equivalence (300k calls) in a private scratch dir. SKIP (exit 3) if `port/web/out/blur.wasm` is not built. No screenshots: the wasm build has no WebGL back end yet |

`out/` holds logs, screenshots, `results.json`, `summary.txt` of the last run. Update references after an intentional rendering change: `smoke.sh --update-refs` (look at `refs/*.png` before committing them).
Other options: `smoke.js --frames N --port P --no-shots --out DIR`; environment `BLUR_EXE` (game exe), `CHROMIUM`.

## Determinism findings (BLUR_DET)

* With `BLUR_DET=<us per API call>` the guest clock (QPC, GetTickCount, timeGetTime, RDTSC, Sleep/wait timeouts) only advances with API calls, the baton scheduler hands over on API/instruction budgets only,
  and input is keyed on **game frames** (`Key@f8200`; `Key@12.5` is wall seconds and NOT deterministic). Under these conditions the render command stream is **bit-identical run to run**:
  4 runs of 10,504 frame hashes identical, including two simultaneous runs at low priority (`taskpolicy -b`) on a loaded machine.
* Without `BLUR_DET` two runs already differ at the 3rd batch (wall-clock dependent loading/streaming timing, thread interleaving): hashes of non-DET runs are meaningless.
* Remaining known hole (harmless so far): `GetSystemTimeAsFileTime`/`GetStartupInfoA` take their seed from `CLOCK_REALTIME` (`hle.c wall_us()`), not from the deterministic clock (noted by wasm-core).
* Things that change the hashes legitimately: any change of the d3d9 HLE (object ids, message order), shader cache on/off, `BLUR_HEAPCHK` (canary padding shifts guest addresses; the command stream does not
  carry pointers, but keep golden runs on the plain configuration, as `det_check.sh` does), different game data.
* Pitfalls: start the game with cwd = `harness/` (the web server serves `./web`); `tools/shot/shot.js` was fixed to tolerate pages without the `#status/#fps` elements; headless Chromium needs ~1 s per screenshot.

## Files
`scenario.json` (walk, checkpoints, thresholds) · `smoke.js` · `smoke.sh` · `det_check.sh` · `wasm_check.sh` · `run_all.sh` · `lib/browser.js` (puppeteer-core from `tools/shot`) · `lib/imgdiff.js` (PNG decode + MAE/SSIM inside Chromium, no native deps) ·
`refs/` (reference PNGs + golden hashes) · `harness-regress.patch` · `out/` (generated).

## Update after the coroutine-scheduler merge (what changed and why)
* **Frame numbers are now relative** (`BLUR_REL=1`, set by smoke.js/det_check.sh): `Key@fN`, `BLUR_CHECKPOINTS` and `BLUR_MAXFRAMES` count from the first frame that draws >= 150 primitives (`[rel] front-end visible at absolute frame N` is logged). Reason: with the new scheduler the loading screen needs ~17,800 frames in `BLUR_DET` mode instead of ~7,450 (blocked threads no longer spin on their waits: WaitForSingleObject calls per 3000 frames 979k -> 67k, so far fewer virtual ticks and loader slices per frame; it is a frame-count change, not a hang: the front-end appears at frame ~17,790 and in ~15 s of wall time). The old absolute-frame scenario therefore ended on the loading spinner.
* **DET + viewer was not deterministic** (command stream diverged at batch 165 when a viewer attached): the back-pressure wait in `pace_present` yielded and ticked the virtual clock for as long as the viewer's acks took. In `BLUR_DET` it now waits in real time without yielding (`harness-regress-v2.patch`, rt.c hunk). Result: harness streams with a viewer are identical run to run (20,946 batches), and the canvas screenshots are pixel-identical again (MAE 0.0000, SSIM 1.000).
* smoke.js loads the viewer with `#sync` (synchronous shader compile) to keep shader pop-in out of the screenshots (not the cause of the earlier mismatch, but removes a timing dependency).
* `realtime.js` (`run_all.sh --realtime`, needs a quiet machine): real-time run (no DET) with the viewer; timestamps the page's per-frame acks (`[9, presented]`, one per batch) and reports mean fps, p50/p90/p99/max frame interval and % > 20 ms / > 33 ms over the last 30 s, plus the load average. Currently 60.0 fps, p99 17.3 ms, 0% > 20 ms. Pass = front-end reached, mean >= 45 fps, p99 <= 100 ms, no log errors.
* The wasm step is a WARN (not a failure) when native and wasm API logs differ (`STRICT_WASM=1` to fail) and SKIP when `blur.wasm` is older than the harness sources. Currently WARN: first divergence at API call 48303 (TlsGetValue stack-word argument), the wasm build predates the harness changes.
* `harness-regress-v2.patch` is INCREMENTAL on top of the already merged `harness-regress.patch` (BLUR_REL in d3d9.c, DET back-pressure in rt.c); apply with `cd port && patch -p1 < tools/regress/harness-regress-v2.patch` (dry-run clean).
* Refs and golden hashes were re-recorded after looking at all five screenshots (front-end "Press ENTER", Online Account, warning dialog over the main menu, main menu, main menu with the cursor moved right). They show the renderer's current look (teal tint); re-record after colour fixes.
* Timings now: determinism ~40 s, smoke ~85 s, realtime ~100 s, wasm ~2 s.

## State
Works: determinism + golden hashes (relative frames, 400 frames after the front-end appears), smoke walk with 5 screenshot refs (reproducible pixel-for-pixel), log/heap/texture/draws assertions, real-time pacing percentiles, wasm equivalence (WARN while stale), run_all.sh table.
Missing / not verified: no deliberately injected regression has been used to prove the smoke test fails when it should; race/gameplay, audio and wasm-WebGL coverage; refs come from headless Chromium on ANGLE/Metal; golden hashes are for the plain config only and need `--update-golden` after any intentional d3d9/HLE change; the real-time check is load-sensitive.
