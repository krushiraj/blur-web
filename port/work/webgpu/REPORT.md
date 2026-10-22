# Track B spike: WebGPU renderer (2026-10-23)

**Verdict: GO, as a second backend behind a flag (keep WebGL2 as fallback). ~42-55 focused days.**

## Shader path (all 2331 shaders, not a sample)
D3D9 bytecode -> SPIR-V (vkd3d 2.1) -> WGSL (naga 30.0.1, `--keep-coordinate-space`).
- vkd3d -> SPIR-V 2331/2331; naga as-is 1811/2331; after fixes A+B 2331/2331; Chrome 141 Tint as-is 2307/2331, after fix C 2331/2331; 111/111 real (vs,ps) pairs from warm.json build with `createRenderPipelineAsync` after fixes D-F.
- A (520 VS): `PointSize` builtin unsupported -> patch SPIR-V (tools/patchpts.py). B (2 PS): OpSNegate on u32 const -> `spirv-opt --ccp --simplify-instructions`. C (24 PS): `textureSample` in non-uniform control flow -> prepend `diagnostic(off, derivative_uniformity);`.
- D (14 pairs): vkd3d numbers bindings per shader -> PS in group 1 (production: fixed binding map). E: remap `@location` from IO tables. F (71 pairs): vkd3d trims varyings, naga drops Component -> widen VS outputs/PS inputs to vec4 and apply the component offset in a wrapper.
- Output 12.6 MB WGSL (2.2 MB gz) vs 1.27 MB GLSL cache. No compare-samplers, depth textures or derivative builtins in the corpus (shadow maps use manual compare). GLSL->WGSL directly is a dead end (naga GLSL front end rejects `#version 300 es`).
- Pipeline creation, 111 pairs serial: p50 103 ms, p95 329 ms, 13.6 s total: needs async compile, a warm list including state, and a fallback while compiling.

## Draw-cost benchmark (3000 indexed draws/frame, trivial GPU work)
JS ms/frame, headless Chromium 141, M4 Max:
| config | cal mean | heavy mean |
|---|---|---|
| WebGL2 immediate | 5.72 | 9.67 |
| WebGPU, encoder each frame | 0.69 | 1.55 |
| bundles 0/50/90/100% chunks reused | 0.88/0.56/0.07/0.01 | 1.71/1.15/0.51/0.01 |
Real Chrome 157: GL 5.2 vs WebGPU 0.92 (cal); 9.34 vs 1.88 (heavy). GPU-process CPU per frame: GL 10.0/16.1 ms vs WebGPU 1.75/2.85. Bundles only win when 100-draw chunks are byte-identical frame to frame, which D3D9 streams rarely are: optional.

## What it does not prove
Trivial shaders/draws (real frames may be GPU-bound, WebGPU does not speed shading); renderer.js keeps its per-draw JS (program lookup, state diff, vdecl, RT binding), so expect the page to go from 11-28 ms to roughly 3-8 ms, not ~1 ms; one machine (M4 Max/Metal) under load; no Windows/Linux/Intel/iGPU; untested: real state mix, uploads, readbacks, RT switches (each a new pass), pipeline-state explosion, numeric equality naga-WGSL vs GLSL path. The planned single captured-frame spike was not done.

## Mapping D3D9 -> WebGPU
- Pipeline key: vs hash, ps hash, vertex layout, blend (incl. separate alpha), colour mask, depth test/func/write, stencil, cull, topology, target formats, sample count, depth bias. Dynamic: viewport, scissor, stencil ref, blend constant. Alpha test / sRGB / LOD bias in a small per-draw uniform block.
- Vertex decl -> GPUVertexBufferLayout per stream; missing element -> zero-stride dummy buffer (0,0,0,1).
- VS/PS constants -> frame-ring uniform buffer with dynamic offsets (~1.3 KB/draw, ~4 MB/frame; queue.writeBuffer is ordered before the pass).
- Samplers cached; textures per draw in group 1 (16 textures + 16 samplers, exactly the limit). RT/DS change = new pass; MSAA = sampleCount 4 + resolveTarget. Shadow maps: depth as unfilterable-float texture_2d + non-filtering sampler. Readbacks: copyTextureToBuffer + mapAsync.

## Hard parts (most dangerous first)
1. Sampling the bound render target is rejected in one pass: end pass, copyTextureToTexture, new pass with loadOp load. Check stats.feedback frequency (F148 diag) first.
2. Pipeline-state explosion / hitches. 3. Per-pair interface rewriting (do offline, not regex in JS). 4. Vertex formats: uint8x4/sint16x4 into float inputs rejected (UBYTE4/SHORT need rewrite), unorm8x4-bgra absent in Chrome 141, UDEC3/DEC3N manual unpack. 5. No triangle-fan (index rewrite), no point size (quad expansion). 6. DXT via texture-compression-bc (feature; other devices need CPU decode); exotic formats keep CPU conversion; R5G6B5 not renderable. 7. DrawUP/dynamic data needs a ring (no GL orphaning). 8. Fixed-function draws are already skipped by renderer.js. 9. SM3 limits fit. 10. Secure context needed (see Track A LAN serving).

## Effort (days)
Translator + WGSL pack + IO tables 4-5; device/buffers/textures/samplers 8-10; draw path 6-8; RT/pass/MSAA/feedback/present 6-8; readbacks/queries/DrawUP 3-4; async compile + warm list 3-4; correctness vs WebGL2 on BLUR_GFXDUMP replays (tools/replay.js) 8-10; tuning/bundles/cross-GPU 4-6. Total ~42-55. Best first step (5-7 days): offline translator + minimal single-pass WebGPU replay of one BLUR_GFXDUMP frame compared to WebGL2.

## Reproduce
Tools: vkd3d-compiler (third_party/vkd3d-2.1/.libs, DYLD_LIBRARY_PATH), naga-cli 30.0.1, spirv-opt/spirv-val (Homebrew), Chromium 141 at /Applications/Chromium.app, puppeteer-core from tools/shot. Source bytecode: port/work/shader-cache/out/src/{pak,exe,extra}/*.bin.
Pipeline: `vkd3d-compiler -x d3dbc -b spirv-binary` -> `python3 -I tools/patchpts.py spv spv2` -> `tools/conv.sh` (naga, retry with spirv-opt) -> `python3 -I tools/meta.py` (meta.json). Files here: bench.html, bench.js, matrix.sh, summarize.py, results.jsonl, validate.html, pairs.html, limits.html, run.js, tools/.

Tool versions: spirv-opt/spirv-val SPIRV-Tools v2026.4 (2026-09-10, Homebrew); naga-cli 30.0.1; vkd3d-shader 2.1. Saved data: data/pack.json.gz (name -> WGSL), data/meta.json, data/wgsl.tar.gz.
