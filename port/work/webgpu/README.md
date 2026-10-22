# WGSL shader toolchain (WebGPU renderer, stage 1)

Offline translator from the D3D9 shader-bytecode corpus to a single versioned pack the page can fetch once and cache by content hash.
The page never rewrites shaders: every shader has fixed binding slots, canonical `@location`s and an IO table in the pack.
(The spike's one-off scripts `tools/conv.sh`, `meta.py`, `patchpts.py` and `data/` are superseded; kept for reference.)

## Commands
```
export NAGA=<path to naga 30.0.1>        # cargo install naga-cli --version 30.0.1 --locked --root DIR  -> DIR/bin/naga
tools/make_wgsl_pack.sh                   # ~25 s, 4 jobs  -> pack/blur.wgslpack, .gz, .br, stats.json
tools/make_wgsl_pack.sh --validate        # + headless Chromium: 2325 shader modules, 111 real (vs,ps) pipelines
tools/make_wgsl_pack.sh --numeric         # + WebGL2(GLSL) vs WebGPU(WGSL) pixel comparison of the 111 pairs
tools/validate.sh validate|numeric        # only the browser part (starts its own static server on a free port >= 8911)
```
Env: `CORPUS` (default `port/work/shader-cache/<build dir>/src`, holds `pak/ exe/ extra/*.bin`), `VKD3D` (default `third_party/vkd3d-2.1`),
`NAGA`, `JOBS`, `PACK_DIR`, `CHROME`. Needs python3 >= 3.9 (stdlib only), spirv-opt/spirv-val, brotli, node + `tools/shot/node_modules/puppeteer-core`,
Chromium with `--enable-unsafe-webgpu` (done by `run.js`).

Tool versions used: vkd3d-shader 2.1 (third_party), naga-cli 30.0.1 (`--keep-coordinate-space`), SPIRV-Tools v2026.4 (spirv-opt, spirv-val),
Chromium 141.0.7371.0 (Tint), Apple M-series / Metal. **Output is deterministic**: two runs (JOBS=4 and JOBS=2) give byte-identical pack, .gz (mtime 0) and .br.

## Pipeline (tools/translate.py)
1. `vkd3d-compiler -x d3dbc -b spirv-binary` (one shader at a time).
2. SPIR-V rewrite (`canonicalise`, own minimal SPIR-V editor `spirv.py`; vkd3d has no CLI options for bindings):
   * debug info stripped; the `BuiltIn PointSize` output (vkd3d emits it for every VS) becomes a private variable, dropped from the interface (naga rejects it).
   * `DescriptorSet`/`Binding` replaced by the fixed map below.
   * every Input/Output interface variable is replaced by a `vec4<f32>` variable at its canonical location; the original variable becomes `Private`
     and is loaded from (inputs, at function entry) or copied to (outputs, before every return) the vec4 with the component offset applied
     (vkd3d declares only the used components, with a `Component` offset that naga would drop). Access chains on demoted variables are retyped.
   * the type/constant/variable section is topologically re-sorted (new pointer types are defined after the variables that use them).
3. `spirv-val`, then naga. If naga fails (2 PS: `OpSNegate` on u32 constants) it is retried after `spirv-opt --ccp --simplify-instructions --eliminate-dead-code-aggressive`.
4. Post-processing: `diagnostic(off, derivative_uniformity);` is prepended to every PS that calls `textureSample` (1753 PS; Tint otherwise rejects 24 of them),
   whitespace removed, naga temporaries `_e<N>` renumbered from 0 (identical shaders then dedupe and compress far better).
Semantics come from the bytecode (`tools/d3dbc.py` reads dcl tokens, constant reads, `def` constants) and are cross-checked against SPIR-V;
an unmapped semantic or sampler >= 16 raises (the build fails loudly instead of skipping).

## Fixed binding map (all shaders)
| | VS | PS |
|---|---|---|
| bind group | 0 | 1 |
| float constants `c#` | `@binding(0) var<uniform> cb0_0_: array<vec4<f32>, N+1>` (member `.member[i]`), N = highest non-`def` register read + 1 (<= 243); **always bound, even when N = 0**; element N is the per-draw parameter vec4 (below) | same, N <= 212 |
| texture `t#` (# = D3D sampler register, 0..15) | `@binding(16 + #)` | `@binding(16 + #)` |
| sampler `s#` | `@binding(32 + #)` | `@binding(32 + #)` |
Binding 1..15 are free (reserved: int/bool constants, per-draw block). Textures are `texture_2d|texture_cube|texture_3d<f32>` as the shader declares (IO table `texTypes`),
samplers are plain filtering `sampler`. Use `layout` = explicit pipeline layout built from the table (unused slots omitted); bind the cb with a
dynamic offset and a 4096-byte window (larger than any N). VS vertex textures use the VS group with their own register numbers (D3D 257..260 -> 0..3).
`def c#` constants are immediates inside the shader (they are not in the cb array and not in `ranges`).

## Fixed locations
* Interpolants (VS outputs and PS inputs, all `vec4<f32>`): `TEXCOORDn -> n` (0..9), `COLORn -> 10+n`, `FOG -> 12`. Unwritten components are 0.
  Position is `@builtin(position)`. VS `PSIZE` is dropped as an interpolant (flag `pointSize`: 4 shaders; needs quad expansion in the renderer).
* PS colour outputs: `@location(k)` = D3D render-target index k, `vec4<f32>`; `oDepth` is `@builtin(frag_depth)` (flag `depth`, 4 PS).
* VS inputs (`vec4<f32>`; any vertex format with <= 4 floats works, missing components read (0,0,0,1) like D3D):
  POSITION0 -> 0, BLENDWEIGHT0 -> 1, BLENDINDICES0 -> 2, NORMAL0 -> 3, COLOR0/1 -> 4/5, TANGENT0 -> 6, BINORMAL0 -> 7, TEXCOORD0..7 -> 8..15.
  This depends only on the vertex declaration, not on the shader, so vertex-buffer layouts can be shared across VS. `inMask` says which are consumed.
* Linking needs no per-pair rewrite: types are identical everywhere and locations are canonical. The one case that cannot be solved offline per shader is a PS that reads an
  interpolant its VS never writes (WebGPU: pipeline error, D3D9: undefined). It does not occur in the 111 real pairs. If it ever does, the page calls the pure function
  `BWGP.padVsOutputs(vsWgsl, ps.inMask & ~vs.outMask)` (tools/packreader.js; appends zero outputs; tested on all 520 VS).

## Pack format v1 (little endian; `blur.wgslpack`, shipped as `.gz`/`.br`)
Key of a shader = `fnv1a64(bytecode)` as 16 hex digits + `_` + bytecode length: the same key as `web/shaders.bin` and warm.json (decimal `lo_hi_len` there).
The key is computed from the bytes, not from the file name (the corpus has 8 `extra/shader_NNN_ps_2_0.bin` test shaders = 2 distinct keys, so 2325 distinct shaders, not 2331 files).
```
header (40 B): "BWGP", u16 version=1, u16 flags=0, u32 nShaders, u32 nBlobs, u32 offShaders, u32 offBlobs, u32 offText, u32 offMeta, u32 lenMeta
shader records (40 B each, sorted by (hi, lo, len)):
  u32 lo, u32 hi, u32 len     key
  u32 blob                    index into the blob table (identical WGSL is stored once: 2272 blobs for 2325 shaders)
  u8 stage (0 vs, 1 ps), u8 flags
  u16 cbVec                   float-constant array length N (0 = shader has no cb); the page must supply at least N vec4
  u16 inMask                  VS: canonical vertex-attribute locations read; PS: interpolant locations read
  u16 outMask                 VS: interpolant locations written; PS: colour targets written
  u16 texMask                 bit r = texture/sampler register r used
  u16 nRanges, u32 texTypes (2 bits per register: 0 2d, 1 cube, 2 3d), u32 rangeOff (index into range array), u32 pad
range array: u16 start, u16 count   (contiguous runs of statically read float constant registers, non-def; relative addressing -> use [0,cbVec))
blob table: u32 textOff, u32 textLen (relative to offText);  text: UTF-8 WGSL;  meta: JSON (binding map, location maps, flags, tool versions, counts)
flags: 1 constRel (relative c# addressing), 2 intUndef (reads i# constants it does not `def`: 39 shaders, vkd3d gives them no buffer, same as the GLSL path),
       4 depth, 8 pointSize, 16 derivDiag (diagnostic prepended), 32 boolUndef (none)
```
Reader: `tools/packreader.js` (`BWGP.load(url)` gunzips with DecompressionStream; `pk.get(hexkey)` -> `{stage, flags, cbVec, inMask, outMask, texMask, texTypes, ranges, wgsl}`;
`BWGP.warmKey('lo_hi_len')` converts renderer keys). Entry point of every module is `main`.

## Sizes (2325 shaders: 520 VS, 1805 PS)
| | bytes |
|---|---|
| raw pack | 11,474,826 (WGSL text 11,329,412; tables ~0.15 MB) |
| gzip -9 | 1,867,460 |
| brotli -q11 | 407,114 |
Shrinking tried on the concatenated text: spike output (gz 2.15 MB / br 587 KB) -> `_e<N>` renumbering (gz 1.84 / br 348 KB, adopted) -> `vec4<f32>` aliases (gz 1.82 / br 346 KB, not adopted:
<2% and breaks copy-paste debugging); dedupe of identical WGSL (adopted, 53 blobs); comments do not exist in naga output. Not done: dead private-variable copies, let-inlining (would need a WGSL-aware pass).
Serve `.br` when possible (page fetches once, stores the bytes under their content hash). Raw size matters only for Tint, which compiles a module lazily, so keep modules per shader as is.

## Validation (Chromium 141, Apple M-series; `tools/validate.sh`)
* `pack_validate.html`: **2325/2325** `createShaderModule` OK with 0 warnings; **111/111** real `(vs,ps)` pairs from `web/warm.json` create a `GPURenderPipeline` using only pack tables
  (explicit bind group layouts from `texTypes`/`cbVec`, vertex attributes from `inMask`, targets from `outMask`; no per-pair patching); 0 pairs with a PS input missing in the VS;
  `padVsOutputs` valid on all 520 VS. Pipeline creation time varies with Chrome's disk shader cache (spike: p50 104 ms cold; now p50 2 ms warm).
* `numeric.html`: all 111 pairs rendered through the production WebGL2 path (GLSL from `shaders.bin`, linked by `D3D9Renderer.buildProgram`, snapshot in `ref/renderer_gl.js`) and through the pack,
  64x64 RGBA8 MRT targets, identical vertex data (quad; TEXCOORD = quad uv, others pseudo-random), 10 constant sets (identity-like / random [0.05,1] / random [-1,1], different seeds) and smooth
  2D/cube/3D textures (linear, clamp). 1110 cases: **1082 bit-exact, 28 differ by 1/255, none by more than 1**. 785 cases (102 pairs) draw visible pixels, GL and WebGPU agree on coverage in every case.
  9 pairs never drew anything with any of the 10 input sets (VS matrices off screen or PS `texkill`), so their pixels are not exercised (their modules do compile and link).
  The input-location set derived from bytecode equals the one in `shaders.bin` for all pairs. Not covered: sRGB read/write and alpha test (renderer-side uniforms), half-pixel offset, derivative-heavy
  shaders with discard on screen-space data, real vertex formats.

## Known limits / open items
* 39 shaders (37 PS, 2 VS) read `i#` constants without `def` (loop counts); vkd3d provides no buffer for them in either path. Whether the game sets them (`SetPixelShaderConstantI`) was not checked.
* `PSIZE` (4 VS) needs point-sprite expansion by the renderer; fixed-function draws are not covered here (renderer.js skips them).
* Metal-only validation; Tint on other backends (D3D12/Vulkan) not run. Uniform-array bounds: indexing past N is clamped (D3D9 returns 0).

## Pack v1 additions (milestone 2)
* **Per-draw parameters in `cb[N]`** (the array is declared N+1 long; `meta.drawParams`): VS `.xy` = D3D9 half-pixel offset in clip space `(1/viewportW, -1/viewportH)`, applied as `pos.xy += hp * pos.w`;
  PS `.x` = alpha-test D3DCMP function (0 = off, 1..8), `.y` = reference 0..1 (`apass()` + `discard` before the return). The page writes these after the N real constants of each draw.
  A relative-addressed constant read past N now returns this vec4 instead of clamping to the last constant (only matters for invalid programs).
* **VS pipeline overrides** (`meta.overrides`): `cswzN` (N = 0/1, bool) swaps R/B of the attribute at location 4/5 (D3DCOLOR is BGRA bytes; Chrome 141 has no `unorm8x4-bgra`);
  `iscL` (f32, per input location L the shader reads, default 1) turns a normalised attribute into its integer value: `unorm8x4` * 255 = UBYTE4, `snorm16x2/4` * 32767 = SHORT2/4.
* Every VS now has the `struct VertexOutput` form (also position-only VS), which `padVsOutputs` relies on (`var vo = VertexOutput(...)`).
* `tools/install_web.sh` copies the pack (`blur.wgslpack.gz`) and the reader (`wgslpack.js`) next to `port/harness/web/index.html`.

## Live renderer: `port/harness/web/renderer_gpu.js` (class `D3D9GpuRenderer`)
Same message interface as `renderer.js` (`feed`/`handle`, `sendInput`, `onAudio`, `hc` for message 96/98, `statLine`, `perf`, `ivs/ivn`, F2 HUD, `bb.lw/lh`). `index.html#gpu=1` selects it; default stays WebGL2.
Fallback to WebGL2 (console warning + orange note in the status bar) when `navigator.gpu`, the adapter, `texture-compression-bc` or device init is missing; a lost device sets a session flag and reloads with `#gpu=0`
(the host re-sends all resources to the new connection). Debug switches `#gpudbg=nosrgb,noaniso,nomip,trimip,nohp,noatest`.
Implemented: textures (DXT via BC, bgra/rgba/float/depth formats, CPU conversion for A8/L8/A8L8/565/1555/4444/V8U8/G16R16/16:16:16:16), sRGB read/write through `-srgb` views, surfaces, vertex/index buffers, DrawUP, samplers,
render passes with clears folded into load ops (+ scissored clear quad for partial clears), StretchRect (copy or blit), feedback snapshots, readback of texel (0,0) (message 81, async), UBYTE4/SHORT2/SHORT4 (pack overrides),
hash-cache messages, audio message, pipeline recipes + warm list, frame hold (<= 30 frames) while a pipeline compiles, `skipped/frame` counter.
Not implemented (not used by the captured title/career/race streams, counted with `tools/featscan.js`): MSAA, point sprites (no draw with point primitives; 4 VS have PSIZE), triangle fans (indexed; plain fans work),
gamma ramp, ColorFill with a rect, per-target write masks (one mask for all targets), DEC3N/UDEC3 (approximated), the GL path's mirror-pass skipping.

### Warm list
`port/harness/web/gpu-warm.json` = pipeline recipes (shader keys, vertex layout, formats, state, texture signature) seen while replaying the captured streams; `loadWarm` compiles them asynchronously at connect.
```
W=port/harness/web/gpu-warm.json; rm -f $W
for f in title.gfx career.gfx race.gfx race_f100.dump; do node tools/replay_run.js dump=/frames/$f reps=1 warm=1 warmOut=$PWD/../../harness/web/gpu-warm.json; done
```
Without it the first race frames skip 14183 draws (6 frames of 2364); with it 0 (`prewarm=1` in replay.html).

### Test tools
`tools/replay_run.js` (headless; `dump=/frames/x.gfx reps=N prewarm=1 hash=...`) -> `replay.html`: both renderers on one dump, backbuffer diff + PNGs + JS ms/frame (+ `PROF=40` CPU profile).
`page=bisect.html rt=<texture id> ...` finds the first draw after which a render target differs. `tools/dumpcut.js` cuts one frame out of a multi-GB dump, `tools/featscan.js` counts features in a stream,
`tools/mockhost.js <file.gfx>` is a fake harness (http + websocket) for `tools/live_test.js` (live page checks, fallbacks), `tools/summarize_replay.py` prints one table line.

### Findings
* All remaining GL/WebGPU differences on title, career and race come from sRGB texture reads: with sRGB off on both sides the images agree to 99.9% (<= 8/255). The WebGL2 path decodes sRGB after filtering and only
  in `texture()` calls (`textureLod` fetches such as a 512x1 LUT are left linear); WebGPU decodes before filtering through the `-srgb` view for every sample, as D3D9 hardware does.
* The cut frame `race_f100.dump` keeps 98% agreement only: persistent render targets (the 1x1 luminance texture) are empty in a cut, so it is not used for accuracy claims.
