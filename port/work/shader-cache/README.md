# Offline shader translation cache

Goal: remove vkd3d-shader + SPIRV-Cross from the run-time path (they cannot run in the browser build). D3D9 shader
bytecode (SM2/SM3 token streams as passed to `CreateVertexShader/CreatePixelShader`) is translated **offline** to GLSL ES 3.00
into a single file keyed by a hash of the bytecode; the browser renderer loads that file and the harness only sends the key.

Work dir: `port/work/shader-cache` (`harness.base` = untouched copy, `harness` = modified, game instances on port 8782).

## Pieces

| file | what |
|---|---|
| `tools/pakshaders.py` | standalone extractor: `shaders-speed.pak` (and optionally raw image dumps) -> `<fnv1a64>_<len>.bin` + `index.tsv` |
| `harness/shadertool.c` | translator: dirs/files of `.bin` -> `shaders.bin` (uses the *same* `shader_translate()` as the live path) |
| `harness/shcache.{h,c}` | cache format, FNV-1a 64, key-set loader for the harness |
| `harness/d3d9.c` (`send_shader`) | `BLUR_SHADER_CACHE=<shaders.bin>`: hit -> sends `G_SHADER_REF` (id, stage, hash lo/hi, len) instead of GLSL; miss -> live translation + log (+ `BLUR_SHADER_MISS=<dir>` saves the bytecode) |
| `harness/web/renderer.js` | `loadShaderCache()`, `loadWarm()`, `shaderRef()`, programs keyed by bytecode hash (shared between shader ids), compile/link split into submit + `finishProgram()` so `KHR_parallel_shader_compile` can be used; `#async` hash flag skips draws whose program is still compiling; `exportWarm()` |
| `harness/web/index.html` | fetches `shaders.bin` (+ optional `warm.json`) before opening the WebSocket; absent file = old behaviour |
| `harness/gfx.c` | `BLUR_GFXDUMP=<file>` records the exact harness->browser byte stream (used for the deterministic replay test) |
| `tools/build_cache.sh` | pak + exe scan + shadertool in one go |
| `tools/replay.{js,html}` | replay a `BLUR_GFXDUMP` stream through the renderer in headless Chromium, `live` vs `cache` mode, PNG of a chosen present |
| `tools/bench.{js,html}` | cache parse + warm-program compile timing + stand-alone compile of every GLSL in the cache |
| `tools/shotx.js` | like `tools/shot/shot.js` but prints renderer stats and exports the warm list |
| `tools/explore.py`, `cmpdump.py`, `findmiss.py` | pak format analysis / coverage checks against runtime dumps |

## `shaders-speed.pak` format (reverse engineered)

* `"2KAP"`, u32 version=1, u32 0x800 (alignment), u32 0x28; the first ~0x800+ bytes are an obfuscated table of contents (file names; not needed).
* Body = 1255 chunks, each at a 0x800-aligned file offset: `u32 compressed_size` + a zlib stream (`78 da`) of exactly that size, zero padded to 0x800.
  1255 chunks cover the whole file except the TOC; they inflate to 29.95 MB total.
* Each chunk is an effect container `"2BXFFXB\0"` (`PCFX` technique/pass tables) with the compiled shader token streams embedded
  verbatim (CTAB comment, ends with `0x0000FFFF`), **not** 4-byte aligned. The extractor finds `[00|01][02|03][fe|ff]ff` version tokens at every
  byte offset and validates by walking the token stream (comment skipping, instruction length field, END token).
* Result: 14474 embedded streams -> **1510 distinct** (356 vs, 1154 ps).
* Another **813 distinct** shaders (164 vs, 649 ps) are embedded in `Blur.exe` itself (found in `re/dump/blur_oep.bin`, the decrypted image). Together with 8 Bink ps_2_0 shaders captured from an earlier real-Bink run
  (`re/out/shaders/shader_00*`, in neither the pak nor the exe image) the union is **2325 distinct shaders (520 vs, 1805 ps)**, all translate (0 failures) and all compile standalone in Chromium (0 errors).
  The run-time harvest of the earlier harness (`re/out/gameshaders`, 635 distinct) is fully covered; the showroom run creates 899 shaders (852 distinct) and all hit the cache.

## Cache file `shaders.bin` (magic `BSC1`)

`u32 count`, then per entry: `u32 hash_lo, hash_hi, code_len; u8 stage, nin, nout, nsamp; u32 glsl_len; IOEnt in[nin], out[nout], samp[nsamp]` (4 bytes each: reg, usage, idx, tex) `; GLSL (no NUL); pad to 4`.
Key = FNV-1a 64 of the bytecode + its length. No translation options exist: the translation depends only on the bytecode (D3D-semantic linking, colour-attribute swizzle, depth/half-pixel/alpha-test patching and varying width reconciliation are still done by `renderer.js` per (vs, ps, colour-attribute) program, from the cached GLSL + IO tables, exactly as before).
The constant layout is still discovered from the GLSL (`cb<N>_struct` uniform block) and sampler naming `SPIRV_Cross_Combined<t><s>`, unchanged.
Size: 7.75 MB raw, **1.27 MB gzip** (`shaders.bin.gz` is also accepted: `loadShaderCache('shaders.bin.gz')` inflates with `DecompressionStream`).

## Usage

```
cd port/work/shader-cache
make -C harness shadertool V=../../../../third_party/vkd3d-2.1      # (blurrt: make blurrt OBJDIR=obj_p2 V=...)
./tools/build_cache.sh                                               # -> out/shaders.bin, out/shaders.bin.gz
ln -sf ../../out/shaders.bin harness/web/shaders.bin                  # served by the harness web server
cd harness && BLUR_SHADER_CACHE=../out/shaders.bin BLUR_WEB=8782 BLUR_RECOMP=1 BINK_SETS=1 BINK_FAKE=1 ./blurrt .../Blur.exe
# optional prewarm: out/warm.json (list of [vsKey, psKey, colourKey] actually drawn in the showroom) -> harness/web/warm.json
```
Page flags (location hash): `#async` = never block on a still-compiling program (draw is skipped, counted in `stats.deferred`).

## Verification

* Deterministic replay: `BLUR_GFXDUMP` recorded a showroom run (7513 presents, 1.34 GB). `node tools/replay.js live|cache <dump> harness/web at=7512 out.png`
  -> `out/replay_live.png` and `out/replay_cache.png` are **byte-identical** (cmp), framebuffer hash equal; all 899 shader messages resolved by body, 0 misses.
* Live game with the cache (`BLUR_SHADER_CACHE`): `refs 899`, 0 `[shader-cache] MISS`, same scene (`out/cache_live.png`).
* The refactored renderer without cache behaves as the baseline (live replay hash identical to cache replay).

## Numbers (headless Chromium/ANGLE-Metal, machine shared with other agents, so timings are noisy)

* cache fetch+parse: 25-50 ms (2325 entries, GLSL decoded lazily per used shader).
* 40 programs actually drawn by the showroom: synchronous compile+link 1.5-3.3 s (~40-80 ms each = the hitch size without prewarm);
  with `KHR_parallel_shader_compile` the main-thread cost is 3-6 ms for all 40 (submit) + ~1 ms to finish them, wall time until all are linked 3-4.5 s
  (no speed-up of the wall time was visible in this headless ANGLE/Metal setup; the gain is that the main thread never blocks).
* Compiling all 2325 shaders stand-alone: 147 s (~63 ms each) - so do NOT warm everything; warm only the program list (`warm.json`, 40 entries for the showroom).
* Showroom: 899 `Create*Shader` calls (852 distinct) but only 38-40 distinct programs ever drawn.

## What still depends on vkd3d / SPIRV-Cross at run time

* With `BLUR_SHADER_CACHE` set and the cache covering the shader: nothing in the render path. The native harness still links vkd3d-shader/SPIRV-Cross for (a) cache misses (fallback) and (b) **`D3DXCompileShader` (`d3dx.c`, HLSL -> SM3 via vkd3d-shader)** - not touched here; not touched here; whether the game calls it was not determined (failures are always logged, successes only under verbose; no failure in the showroom run) - check before the wasm build drops vkd3d.
* Shaders outside the cache (anything created at run time from other sources, e.g. a real Bink/`binkw32.dll` path if movies are enabled) are only covered if captured: use `BLUR_SHADER_MISS=<dir>` on a run, then add the dir to `build_cache.sh`.
* The warm list covers the showroom only; other scenes need a run with `r.exportWarm()` (or a pak-structure-derived pair list, not done: vs/ps pairing and the colour-attribute key come from the effect techniques + vertex declarations).
* Remaining cost per program at run time is renderer-side regex rewriting of the cached GLSL (varying linking etc.), a few ms in JS; it could also be moved offline per (vs, ps) pair if profiling shows it matters.

## Patch

`out/shader-cache.patch` = `diff -ruN harness.base harness` (excluding binaries/symlinks); apply with `patch -p1` from `port/harness` after stripping the first path component, e.g. `cd port/harness && patch -p1 < .../out/shader-cache.patch`
(paths read `harness.base/...` / `harness/...`). The Makefile adds `shcache.c` to the object lists and a `shadertool` target.
