'use strict';
// A Direct3D 9 device implemented on WebGPU (milestone 1). Same message interface as D3D9Renderer (renderer.js): feed()/handle() consume the harness->browser
// byte stream (port/harness/gfx.c) and mirror D3D9 device state. Shaders are NOT translated at runtime: they come from the offline WGSL pack
// (port/work/webgpu/pack/blur.wgslpack.gz, tools/packreader.js = globalThis.BWGP, format in port/work/webgpu/README.md) with fixed bindings and locations.
//  * render targets are stored in D3D orientation (row 0 = top): no Y flip anywhere; the pack's VS adds the D3D9 half-pixel offset from the per-draw parameters
//    (cb[cbVec]), PS alpha test is done in the shader from cb[cbVec] too. sRGB read/write use -srgb view formats.
//  * commands are recorded into one encoder per flush; queue.writeBuffer/writeTexture happen at call time, so an upload to a resource already used by the recorded
//    commands first submits the encoder (stats.flushes). One render pass per run of draws with identical targets; clears fold into the load op.
//  * pipelines are created with createRenderPipelineAsync; a draw whose pipeline is still compiling is skipped (stats.skipped; r.idle() waits for them).
// Not in this milestone: MSAA, readbacks (msg 81), gamma ramp, triangle fans on indexed draws, point sprites (PSIZE), UBYTE4/SHORT vertex types (need int->float
// conversion), DEC3N, fixed-function draws (except the POSITIONT quad used for Bink), audit messages, the host content-hash cache messages (96/98).
const GG = { DESTROY: 95, TEX_CREATE: 20, TEX_UPLOAD: 21, SURF_CREATE: 22, BUF_CREATE: 30, BUF_UPLOAD: 31, SHADER: 40, VDECL: 41, SHADER_REF: 42,
  SET_RS: 50, SET_SS: 51, SET_TEX: 52, SET_VS: 53, SET_PS: 54, SET_VDECL: 55, SET_FVF: 56, SET_STREAM: 57, SET_INDICES: 58, SET_VSC: 59, SET_PSC: 60,
  SET_VIEWPORT: 61, SET_SCISSOR: 62, SET_RT: 63, SET_DS: 64, DRAW: 70, DRAW_UP: 71, CLEAR: 72, PRESENT: 73, STRETCH: 74 };
const FCC = (s) => s.charCodeAt(0) | s.charCodeAt(1) << 8 | s.charCodeAt(2) << 16 | s.charCodeAt(3) << 24;
const F_DXT1 = FCC('DXT1'), F_DXT3 = FCC('DXT3'), F_DXT5 = FCC('DXT5'), F_INTZ = FCC('INTZ'), F_DF24 = FCC('DF24'), F_DF16 = FCC('DF16');
const USAGE_NAMES = ['POSITION', 'BLENDWEIGHT', 'BLENDINDICES', 'NORMAL', 'PSIZE', 'TEXCOORD', 'TANGENT', 'BINORMAL', 'TESSFACTOR', 'POSITIONT', 'COLOR', 'FOG'];
const CMP = ['never', 'never', 'less', 'equal', 'less-equal', 'greater', 'not-equal', 'greater-equal', 'always'];
const BF = ['zero', 'zero', 'one', 'src', 'one-minus-src', 'src-alpha', 'one-minus-src-alpha', 'dst-alpha', 'one-minus-dst-alpha', 'dst', 'one-minus-dst', 'src-alpha-saturated', 'src-alpha', 'one-minus-src-alpha', 'constant', 'one-minus-constant'];
const BOP = ['add', 'add', 'subtract', 'reverse-subtract', 'min', 'max'];
const SOP = ['keep', 'keep', 'zero', 'replace', 'increment-clamp', 'decrement-clamp', 'invert', 'increment-wrap', 'decrement-wrap'];
const ADDR = ['repeat', 'repeat', 'mirror-repeat', 'clamp-to-edge', 'clamp-to-edge', 'mirror-repeat'];
const RECIPE_VER = 1;
const RSIDX = [22, 27, 19, 20, 171, 206, 207, 208, 209, 168, 7, 14, 23, 52, 56, 53, 54, 55, 58, 59, 185, 189, 186, 187, 188, 195, 175];
const UBO_RING = 16 << 20, UP_RING = 32 << 20, CB_WINDOW = 4096;

const BLIT_WGSL = `
struct U { src: vec4<f32>, flags: vec4<f32> }   // src = uv rect (u0,v0,u1,v1); flags.x = 1: output is bgra->same (unused)
@group(0) @binding(0) var<uniform> u: U;
@group(0) @binding(1) var t: texture_2d<f32>;
@group(0) @binding(2) var s: sampler;
struct VO { @builtin(position) p: vec4<f32>, @location(0) uv: vec2<f32> }
@vertex fn vs(@builtin(vertex_index) i: u32) -> VO {
  var q = array<vec2<f32>, 3>(vec2<f32>(0.0, 0.0), vec2<f32>(2.0, 0.0), vec2<f32>(0.0, 2.0));
  let c = q[i]; var o: VO; o.p = vec4<f32>(c.x * 2.0 - 1.0, 1.0 - c.y * 2.0, 0.0, 1.0);
  o.uv = vec2<f32>(mix(u.src.x, u.src.z, c.x), mix(u.src.y, u.src.w, c.y)); return o; }
@fragment fn fs(v: VO) -> @location(0) vec4<f32> { return textureSampleLevel(t, s, v.uv, 0.0); }
`;
const GAMMA_WGSL = `
@group(0) @binding(0) var t: texture_2d<f32>;
@group(0) @binding(1) var lut: texture_2d<f32>;
struct VO { @builtin(position) p: vec4<f32> }
@vertex fn vs(@builtin(vertex_index) i: u32) -> VO {
  var q = array<vec2<f32>, 3>(vec2<f32>(0.0, 0.0), vec2<f32>(2.0, 0.0), vec2<f32>(0.0, 2.0));
  let c = q[i]; var o: VO; o.p = vec4<f32>(c.x * 2.0 - 1.0, 1.0 - c.y * 2.0, 0.0, 1.0); return o; }
@fragment fn fs(v: VO) -> @location(0) vec4<f32> {
  let c = textureLoad(t, vec2<i32>(v.p.xy), 0).rgb; let k = vec3<i32>(clamp(c * 255.0 + 0.5, vec3<f32>(0.0), vec3<f32>(255.0)));
  return vec4<f32>(textureLoad(lut, vec2<i32>(k.r, 0), 0).r, textureLoad(lut, vec2<i32>(k.g, 0), 0).g, textureLoad(lut, vec2<i32>(k.b, 0), 0).b, 1.0); }
`;
const CLEAR_WGSL = `
struct U { color: vec4<f32>, z: vec4<f32> }
@group(0) @binding(0) var<uniform> u: U;
struct VO { @builtin(position) p: vec4<f32> }
struct FO { @location(0) c0: vec4<f32>, @builtin(frag_depth) d: f32 }
@vertex fn vs(@builtin(vertex_index) i: u32) -> VO {
  var q = array<vec2<f32>, 3>(vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
  var o: VO; o.p = vec4<f32>(q[i], 0.0, 1.0); return o; }
@fragment fn fs() -> FO { var o: FO; o.c0 = u.color; o.d = u.z.x; return o; }
@fragment fn fsc() -> @location(0) vec4<f32> { return u.color; }
`;
const FF_VS_WGSL = `
struct P { v: array<vec4<f32>, 2> }
@group(0) @binding(0) var<uniform> cb0_0_: P;
struct VO { @builtin(position) pos: vec4<f32>, @location(0) t0: vec4<f32> }
@vertex fn main(@location(0) p: vec4<f32>, @location(8) t: vec4<f32>) -> VO {
  let vp = cb0_0_.v[0]; let hp = cb0_0_.v[1]; var o: VO;
  let q = vec4<f32>(p.x / vp.x * 2.0 - 1.0, 1.0 - p.y / vp.y * 2.0, p.z, 1.0);
  o.pos = vec4<f32>(q.xy + hp.xy * q.w, q.zw); o.t0 = t; return o; }
`;

class D3D9GpuRenderer {
  constructor(canvas, log) {
    this.canvas = canvas; this.log = log || console.log; this.warned = new Set();
    this.dbg = new Set(((/gpudbg=([\w,]+)/.exec(location.hash) || [0, ''])[1]).split(','));      // debug switches: #gpudbg=nosrgb,noaniso,nomip,trimip,noatest
    this.stats = { draws: 0, frameDraws: 0, clears: 0, skipped: 0, pipelines: 0, pipelineFail: 0, passes: 0, flushes: 0, tex: 0, bindGroups: 0, feedback: 0, fans: 0, shaders: 0, partialClears: 0, stretch: 0, uploads: 0 };
    this.textures = new Map(); this.surfaces = new Map(); this.buffers = new Map(); this.shaders = new Map(); this.vdecls = new Map();
    this.rs = new Uint32Array(256); this.ss = Array.from({ length: 32 }, () => new Uint32Array(16));
    this.tex = new Array(20).fill(0); this.streams = Array.from({ length: 16 }, () => ({ buf: 0, offset: 0, stride: 0 }));
    this.ib = 0; this.vdecl = 0; this.fvf = 0; this.vs = 0; this.ps = 0;
    this.vsc = new Float32Array(256 * 4); this.psc = new Float32Array(224 * 4); this.vscVer = 1; this.pscVer = 1;
    this.vp = { x: 0, y: 0, w: 1280, h: 720, minz: 0, maxz: 1 }; this.scissor = { l: 0, t: 0, r: 1280, b: 720 };
    this.rt = [{ kind: 3 }, { kind: 3 }, { kind: 3 }, { kind: 3 }]; this.ds = { kind: 3 };
    this.initDefaults(); this.bb = null; this.uid = 1; this.serial = 1; this.dead = [];
    this.pipes = new Map(); this.recipes = new Map(); this.pending = 0; this.demandQ = []; this.warmQ = []; this.inflight = 0; this.MAXFLIGHT = 6; this.warmLeft = 0; this.warmUntil = 0; this.bgls = new Map(); this.bgCache = new Map(); this.samplers = new Map(); this.views = new Map(); this.pls = new Map();
    this.ssVer = new Uint32Array(32); this.streamGen = 1; this.texLife = 1; this.holdRun = 0; this.frameDeferred = false; this.pass = null; this.pc = null; this.tgtKey = ''; this.tgt = null; this.rtDirty = true; this.feedbackSnaps = new Map();
    this.vscScratch = new Float32Array(260 * 4); this.ffConst = new Float32Array(8); this.lastCb = [{ ck: '', serial: 0, off: 0 }, { ck: '', serial: 0, off: 0 }];
  }
  initDefaults() {
    const r = this.rs; r[7] = 1; r[14] = 1; r[23] = 4; r[22] = 3; r[19] = 2; r[20] = 1; r[168] = r[190] = r[191] = r[192] = 15; r[171] = 1; r[209] = 1; r[207] = 2; r[208] = 1;
    r[56] = 8; r[53] = r[54] = r[55] = 1; r[58] = r[59] = 0xffffffff; r[25] = 8; r[193] = 0xffffffff; r[186] = r[187] = r[188] = 1; r[189] = 8;
    for (const s of this.ss) { s[1] = s[2] = s[3] = 1; s[5] = s[6] = 1; s[10] = 1; }
  }
  warn(k, msg) { if (!this.warned.has(k)) { this.warned.add(k); this.log('[gpu] ' + msg); } }
  // ---- setup ----------------------------------------------------------------------------------------------------------------------------------------
  // opts: { packUrl, bodyKey(u8)->'lo_hi_len'|null  (maps a full-GLSL G_SHADER body to its bytecode key; only needed for streams that carry GLSL) }
  async init(opts = {}) {
    if (!navigator.gpu) throw new Error('WebGPU not available');
    const ad = this.adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' }); if (!ad) throw new Error('no WebGPU adapter');
    const want = ['texture-compression-bc', 'float32-filterable', 'float32-blendable', 'depth32float-stencil8', 'unorm8x4-bgra', 'timestamp-query'];
    this.features = new Set(want.filter((f) => ad.features.has(f)));
    if (opts.requireBC && !this.features.has('texture-compression-bc')) throw new Error('adapter lacks texture-compression-bc (DXT textures)');
    const dev = this.dev = await ad.requestDevice({ requiredFeatures: [...this.features], requiredLimits: { maxBufferSize: Math.min(ad.limits.maxBufferSize, 1 << 30), maxUniformBufferBindingSize: 65536 } });
    this.q = dev.queue; this.bcOk = this.features.has('texture-compression-bc'); this.f32f = this.features.has('float32-filterable');
    dev.addEventListener('uncapturederror', (e) => this.warn('uncap' + e.error.message.slice(0, 80), 'uncaptured: ' + e.error.message.slice(0, 400)));
    dev.lost.then((i) => { this.lost = i; this.log('[gpu] device lost: ' + i.message); if (this.onLost) this.onLost(i); });
    this.learn = !/gpulearn=0/.test(location.hash);
    if (/gpudump=1/.test(location.hash)) { window.__gpuWarm = () => this.exportWarm(); setInterval(() => console.log(`[gpu] used pipelines ${this.recipes.size}, skipped total ${this.stats.skipped}, held ${this.stats.held || 0}`), 10000); }
    this.pack = await BWGP.load(opts.packUrl || 'pack/blur.wgslpack.gz'); this.bodyKey = opts.bodyKey || null;
    this.vin = this.pack.meta.vertexInput;
    this.ctx = this.canvas.getContext('webgpu'); this.cfmt = navigator.gpu.getPreferredCanvasFormat();
    this.ctx.configure({ device: dev, format: this.cfmt, usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_DST, alphaMode: 'opaque' });
    this.uboStage = new Uint8Array(UBO_RING); this.uboStageF = new Float32Array(this.uboStage.buffer);
    this.ubo = dev.createBuffer({ size: UBO_RING, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST, label: 'cbring' }); this.uboOff = 0;
    this.upBuf = dev.createBuffer({ size: UP_RING, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.INDEX | GPUBufferUsage.COPY_DST, label: 'upring' }); this.upOff = 0;
    this.dummyVB = dev.createBuffer({ size: 16, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST }); this.q.writeBuffer(this.dummyVB, 0, new Float32Array([0, 0, 0, 1]));
    this.smallUbo = dev.createBuffer({ size: 256 * 1024, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST }); this.smallOff = 0;
    // dummy textures for unbound / mismatching sampler slots
    const mk = (dim, layers, vd) => { const t = dev.createTexture({ size: dim === '3d' ? [1, 1, 1] : [1, 1, layers], dimension: dim === '3d' ? '3d' : '2d', format: 'rgba8unorm', usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
      this.q.writeTexture({ texture: t }, new Uint8Array([0, 0, 0, 255]), { bytesPerRow: 4 }, [1, 1, dim === '3d' ? 1 : 1]);
      if (layers === 6) for (let f = 1; f < 6; f++) this.q.writeTexture({ texture: t, origin: [0, 0, f] }, new Uint8Array([0, 0, 0, 255]), { bytesPerRow: 4 }, [1, 1, 1]);
      const v = t.createView({ dimension: vd }); v.uid = this.uid++; v.stype = 'float'; return v; };
    this.dummy = { '2d': mk('2d', 1, '2d'), cube: mk('2d', 6, 'cube'), '3d': mk('3d', 1, '3d') };
    this.blitMod = dev.createShaderModule({ code: BLIT_WGSL }); this.clearMod = dev.createShaderModule({ code: CLEAR_WGSL }); this.ffMod = dev.createShaderModule({ code: FF_VS_WGSL });
    this.nearest = dev.createSampler({ magFilter: 'nearest', minFilter: 'nearest' }); this.linear = dev.createSampler({ magFilter: 'linear', minFilter: 'linear' });
    this.enc = dev.createCommandEncoder();
    window.addEventListener('keydown', (e) => { if (e.code === 'F2') { const first = !this.hud; this.initHud(); if (!first) this.hud.c.style.display = this.hud.c.style.display === 'none' ? '' : 'none'; } });
    this.ffRec = { ff: true, stage: 'vs', cbVec: 1, inMask: 1 | 1 << 8, outMask: 1, texTypes: {}, texMask: 0, flags: 0, module: this.ffMod, key: 'ff', pid: 0, regs: [] };
    return this;
  }
  async idle() { while (this.pending > 0) await new Promise((r) => setTimeout(r, 5)); await this.dev.queue.onSubmittedWorkDone(); }
  // ---- message pump (same contract as D3D9Renderer.feed) --------------------------------------------------------------------------------------------------
  feed(ab) {
    const t0 = performance.now(); let r; try { r = this.feed0(ab); return r; } finally { const d = performance.now() - t0; this.jsBusy = (this.jsBusy || 0) + d; if (r && r.presents) { (this.jsh || (this.jsh = new Float32Array(180)))[(this.jsn = (this.jsn || 0) + 1) % 180] = d / r.presents; } if (r) { this.jsFrames = (this.jsFrames || 0) + r.presents; this.jsMsgs = (this.jsMsgs || 0) + r.messages; } }
  }
  feed0(ab) {
    const dv = new DataView(ab); let off = 0, messages = 0, presents = 0;
    while (off + 8 <= ab.byteLength) {
      const len = dv.getUint32(off, true), type = dv.getUint32(off + 4, true), p = off + 8; off = p + len; messages++;
      try { if (this.handle(type, ab, dv, p, len)) presents++; } catch (e) { this.warn('ex' + type + e.message, `message ${type} failed: ${e.stack || e}`); }
    }
    return { messages, presents };
  }
  handle(type, ab, dv, p, len) {
    const u = (i) => dv.getUint32(p + 4 * i, true), f = (i) => dv.getFloat32(p + 4 * i, true);
    switch (type) {
      case GG.TEX_CREATE: this.texCreate(u(0), u(1), u(2), u(3), u(4), u(5), u(6), u(7)); break;
      case GG.TEX_UPLOAD: this.texUpload(u(0), u(1), u(2), u(3), u(4), u(5), u(6), u(7), new Uint8Array(ab, p + 32, len - 32)); break;
      case GG.SURF_CREATE: this.surfCreate(u(0), u(1), u(2), u(3), u(4), u(5)); break;
      case GG.BUF_CREATE: this.bufCreate(u(0), u(1), u(2), u(3), u(4)); break;
      case GG.BUF_UPLOAD: this.bufUpload(u(0), u(1), new Uint8Array(ab, p + 8, len - 8)); break;
      case GG.SHADER: this.shaderCreate(ab, p, len, u); break;
      case GG.SHADER_REF: this.shaderRef(u(0), u(1), u(2), u(3), u(4)); break;
      case GG.VDECL: this.vdeclCreate(u(0), u(1), new DataView(ab, p + 8, len - 8)); break;
      case GG.SET_RS: if (u(0) < 256) { this.rs[u(0)] = u(1); this.stDirty = true; } break;
      case GG.SET_SS: if (u(0) < 32 && u(1) < 16) { if (this.ss[u(0)][u(1)] !== u(2)) this.ssVer[u(0)]++; this.ss[u(0)][u(1)] = u(2); } break;
      case GG.SET_TEX: this.tex[u(0)] = u(1); break;
      case GG.SET_VS: this.vs = u(0); break; case GG.SET_PS: this.ps = u(0); break;
      case GG.SET_VDECL: this.vdecl = u(0); break; case GG.SET_FVF: this.fvf = u(0); break;
      case GG.SET_STREAM: { const o = this.streams[u(0)]; if (o.stride !== u(3) || !o.buf !== !u(1)) this.streamGen++; this.streams[u(0)] = { buf: u(1), offset: u(2), stride: u(3) }; break; }
      case GG.SET_INDICES: this.ib = u(0); break;
      case GG.SET_VSC: { const n = u(1); this.vsc.set(((p + 8) & 3) ? new Float32Array(ab.slice(p + 8, p + 8 + 16 * n)) : new Float32Array(ab, p + 8, 4 * n), u(0) * 4); this.vscVer++; break; }
      case GG.SET_PSC: { const n = u(1); this.psc.set(((p + 8) & 3) ? new Float32Array(ab.slice(p + 8, p + 8 + 16 * n)) : new Float32Array(ab, p + 8, 4 * n), u(0) * 4); this.pscVer++; break; }
      case GG.SET_VIEWPORT: this.vp = { x: u(0), y: u(1), w: u(2), h: u(3), minz: f(4), maxz: f(5) }; break;
      case GG.SET_SCISSOR: this.scissor = { l: u(0), t: u(1), r: u(2), b: u(3) }; break;
      case GG.SET_RT: this.rt[u(0)] = { kind: u(1), id: u(2), level: u(3), face: u(4) }; this.rtDirty = true; break;
      case GG.SET_DS: this.ds = { kind: u(0), id: u(1), level: u(2), face: u(3) }; this.rtDirty = true; this.stDirty = true; break;
      case GG.DRAW: this.draw(u(0), u(1), u(2), u(3), u(4), u(5), u(6)); break;
      case GG.DRAW_UP: this.drawUP(u(0), u(1), u(2), u(3), u(4), u(5), u(6), new Uint8Array(ab, p + 28, len - 28)); break;
      case GG.CLEAR: this.clear(u(0), u(1), f(2), u(3), u(4), new Int32Array(ab.slice(p + 20, p + 20 + 16 * u(4)))); break;
      case 98: if (this.hc) this.hc.tagged(this, ab, dv, p, len); break;       // content-hash cache (hcache.js): tagged upload / reference to a cached upload
      case 96: if (this.hc) this.hc.ref(this, ab, dv, p, len); break;
      case 90: if (this.onAudio) this.onAudio(u(0), u(1), u(2), ab.slice(p + 12, p + len)); break;      // host-mixed PCM: rate, channels, frames, int16 data
      case 81: this.readbackTexel(u(0), u(1), u(2), u(3), u(4)); break;
      case 80: this.setGamma(new Uint16Array(ab.slice(p + 4, p + 4 + 1536))); break;
      case 85: case 86: this.stats['skipMsg' + type] = (this.stats['skipMsg' + type] || 0) + 1; break;     // texture audit: not implemented
      case GG.PRESENT: this.present(); return true;
      case GG.STRETCH: this.stretch(Array.from({ length: 19 }, (_, i) => u(i))); break;
      case GG.DESTROY: this.destroyRes(u(0), u(1)); break;
      default: this.warn('type' + type, 'unknown message type ' + type);
    }
    return false;
  }
  // ---- formats --------------------------------------------------------------------------------------------------------------------------------------
  fmtInfo(f) {
    const I = (format, o = {}) => Object.assign({ format }, o);
    switch (f) {
      case 21: return I('bgra8unorm', { bpp: 4, srgb: 'bgra8unorm-srgb', rt: true });
      case 22: return I('bgra8unorm', { bpp: 4, conv: 'x8', srgb: 'bgra8unorm-srgb', rt: true });
      case 23: return I('rgba8unorm', { bpp: 2, conv: 'r5g6b5', srgb: 'rgba8unorm-srgb', rt: true });
      case 25: return I('rgba8unorm', { bpp: 2, conv: 'a1r5g5b5', srgb: 'rgba8unorm-srgb' });
      case 26: return I('rgba8unorm', { bpp: 2, conv: 'a4r4g4b4', srgb: 'rgba8unorm-srgb' });
      case 28: return I('rgba8unorm', { bpp: 1, conv: 'a8', srgb: 'rgba8unorm-srgb' });
      case 50: return I('rgba8unorm', { bpp: 1, conv: 'l8', srgb: 'rgba8unorm-srgb' });
      case 51: return I('rgba8unorm', { bpp: 2, conv: 'a8l8', srgb: 'rgba8unorm-srgb' });
      case 60: return I('rgba8snorm', { bpp: 2, conv: 'v8u8' });
      case 63: return I('rgba8snorm', { bpp: 4 });
      case 34: return I('rgba16float', { bpp: 4, conv: 'rg16' });
      case 36: return I('rgba8unorm', { bpp: 8, conv: 'rgba16', srgb: 'rgba8unorm-srgb' });
      case 35: return I('rgb10a2unorm', { bpp: 4, conv: 'a2r10g10b10', rt: true });
      case 111: return I('r16float', { bpp: 2, rt: true, float: true });
      case 112: return I('rg16float', { bpp: 4, rt: true, float: true });
      case 113: return I('rgba16float', { bpp: 8, rt: true, float: true });
      case 114: return I('r32float', { bpp: 4, rt: true, float: true, f32: true });
      case 115: return I('rg32float', { bpp: 8, rt: true, float: true, f32: true });
      case 116: return I('rgba32float', { bpp: 16, rt: true, float: true, f32: true });
      case F_DXT1: return I(this.bcOk ? 'bc1-rgba-unorm' : 'rgba8unorm', { block: 8, srgb: 'bc1-rgba-unorm-srgb', bc: true });
      case F_DXT3: return I(this.bcOk ? 'bc2-rgba-unorm' : 'rgba8unorm', { block: 16, srgb: 'bc2-rgba-unorm-srgb', bc: true });
      case F_DXT5: return I(this.bcOk ? 'bc3-rgba-unorm' : 'rgba8unorm', { block: 16, srgb: 'bc3-rgba-unorm-srgb', bc: true });
      case 80: case F_DF16: return I('depth16unorm', { depth: true });
      case 77: return I('depth24plus', { depth: true });
      case 71: case 82: return I('depth32float', { depth: true });
      case 75: case 83: case F_INTZ: case F_DF24: return I('depth24plus-stencil8', { depth: true, stencil: true });
    }
    this.warn('fmt' + f, `unsupported D3D format ${f} (0x${f.toString(16)}); using RGBA8`);
    return I('rgba8unorm', { bpp: 4, conv: 'none', rt: true });
  }
  // ---- textures / surfaces ---------------------------------------------------------------------------------------------------------------------------
  destroyRes(kind, id) {
    if (kind === 0) { const t = this.textures.get(id); if (!t || t.isRT || t.isDS) return; this.dead.push(t.gpu); if (t.snap) this.dead.push(t.snap); this.textures.delete(id); this.texLife++; }
    else { const b = this.buffers.get(id); if (!b) return; this.dead.push(b.gpu); this.buffers.delete(id); }
  }
  texCreate(id, kind, w, h, d, levels, fmt, usage) {
    const info = this.fmtInfo(fmt), isRT = !!(usage & 1), isDS = !!(usage & 2);
    let format = info.format; const maxLv = Math.floor(Math.log2(Math.max(w, h, kind === 2 ? d : 1))) + 1; const lv = Math.max(1, Math.min(levels, maxLv));
    if (info.bc && this.bcOk && ((w & 3) || (h & 3))) { this.warn('bcsz' + id, `DXT texture ${id} ${w}x${h} not a multiple of 4: black RGBA8 fallback`); format = 'rgba8unorm'; info.bcFallback = true; }
    const t = { id, kind, w, h, d, levels: lv, fmt, usage, info: Object.assign({}, info, { format }), isRT, isDS, uid: this.uid++, views: new Map(), serial: 0 };
    const vf = []; if (info.srgb && !isDS && format === info.format) vf.push(info.srgb);
    t.gpu = this.dev.createTexture({ size: kind === 1 ? [w, h, 6] : kind === 2 ? [w, h, d] : [w, h], dimension: kind === 2 ? '3d' : '2d', format, mipLevelCount: lv,
      usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.COPY_SRC | (isRT || isDS ? GPUTextureUsage.RENDER_ATTACHMENT : 0), viewFormats: vf, label: 'tex' + id });
    t.srgbOk = vf.length > 0; t.stype = info.depth || (info.f32 && !this.f32f) ? 'unfilterable-float' : 'float';
    this.textures.set(id, t); this.stats.tex++; this.texLife++;
  }
  surfCreate(id, w, h, fmt, kind, ms) {
    const info = this.fmtInfo(fmt); if (ms > 1) this.warn('ms' + id, 'MSAA surface requested: rendering single-sampled');
    const s = { id, w, h, lw: w, lh: h, fmt, kind, ms, info, uid: this.uid++, views: new Map(), serial: 0, isSurf: true };
    const depth = info.depth, format = kind === 0 ? 'bgra8unorm' : info.format;
    s.info = Object.assign({}, info, { format });
    s.gpu = this.dev.createTexture({ size: [w, h], format, usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC | GPUTextureUsage.COPY_DST | GPUTextureUsage.TEXTURE_BINDING, viewFormats: !depth && info.srgb ? [kind === 0 ? 'bgra8unorm-srgb' : info.srgb] : [], label: 'surf' + id });
    s.stype = 'float';
    if (kind === 0) this.bb = s; this.surfaces.set(id, s); this.texLife++;
  }
  convert(info, w, h, d, data) {          // D3D memory layout -> GPU upload bytes (only formats that WebGPU lacks)
    const n = w * h * d; const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
    switch (info.conv) {
      case 'x8': { const o = new Uint8Array(data.subarray(0, n * 4)); for (let i = 3; i < o.length; i += 4) o[i] = 255; return o; }
      case 'r5g6b5': { const o = new Uint8Array(n * 4); for (let i = 0; i < n; i++) { const v = dv.getUint16(2 * i, true); o[4 * i] = (v >> 11) * 255 / 31; o[4 * i + 1] = ((v >> 5) & 63) * 255 / 63; o[4 * i + 2] = (v & 31) * 255 / 31; o[4 * i + 3] = 255; } return o; }
      case 'a1r5g5b5': { const o = new Uint8Array(n * 4); for (let i = 0; i < n; i++) { const v = dv.getUint16(2 * i, true); o[4 * i] = ((v >> 10) & 31) * 255 / 31; o[4 * i + 1] = ((v >> 5) & 31) * 255 / 31; o[4 * i + 2] = (v & 31) * 255 / 31; o[4 * i + 3] = (v >> 15) ? 255 : 0; } return o; }
      case 'a4r4g4b4': { const o = new Uint8Array(n * 4); for (let i = 0; i < n; i++) { const v = dv.getUint16(2 * i, true); o[4 * i] = ((v >> 8) & 15) * 17; o[4 * i + 1] = ((v >> 4) & 15) * 17; o[4 * i + 2] = (v & 15) * 17; o[4 * i + 3] = ((v >> 12) & 15) * 17; } return o; }
      case 'a8': { const o = new Uint8Array(n * 4); for (let i = 0; i < n; i++) o[4 * i + 3] = data[i]; return o; }
      case 'l8': { const o = new Uint8Array(n * 4); for (let i = 0; i < n; i++) { const l = data[i]; o[4 * i] = l; o[4 * i + 1] = l; o[4 * i + 2] = l; o[4 * i + 3] = 255; } return o; }
      case 'a8l8': { const o = new Uint8Array(n * 4); for (let i = 0; i < n; i++) { const l = data[2 * i], a = data[2 * i + 1]; o[4 * i] = l; o[4 * i + 1] = l; o[4 * i + 2] = l; o[4 * i + 3] = a; } return o; }
      case 'rgba16': { const o = new Uint8Array(n * 4); for (let i = 0; i < n * 4; i++) o[i] = dv.getUint16(2 * i, true) >> 8; return o; }
      case 'rg16': { const o = new Uint16Array(n * 4), lut = this.u16h || (this.u16h = this.makeHalfLut()); for (let i = 0; i < n; i++) { o[4 * i] = lut[dv.getUint16(4 * i, true)]; o[4 * i + 1] = lut[dv.getUint16(4 * i + 2, true)]; o[4 * i + 2] = 0x3c00; o[4 * i + 3] = 0x3c00; } return new Uint8Array(o.buffer); }
      case 'v8u8': { const o = new Int8Array(n * 4), s = new Int8Array(data.buffer, data.byteOffset, n * 2); for (let i = 0; i < n; i++) { o[4 * i] = s[2 * i]; o[4 * i + 1] = s[2 * i + 1]; o[4 * i + 2] = 127; o[4 * i + 3] = 127; } return new Uint8Array(o.buffer); }
      case 'a2r10g10b10': { const o = new Uint32Array(n); for (let i = 0; i < n; i++) { const v = dv.getUint32(4 * i, true), a = v >>> 30, r = (v >>> 20) & 1023, g = (v >>> 10) & 1023, b = v & 1023; o[i] = (a << 30) | (b << 20) | (g << 10) | r; } return new Uint8Array(o.buffer); }
      case 'none': return new Uint8Array(n * 4);
    }
    return data;
  }
  makeHalfLut() {
    const F = new Float32Array(1), U = new Uint32Array(F.buffer), t = new Uint16Array(65536);
    for (let i = 0; i < 65536; i++) { F[0] = i / 65535; const b = U[0], e = ((b >> 23) & 255) - 112; t[i] = e <= 0 ? 0 : e >= 31 ? 0x7c00 : ((b >> 16) & 0x8000) | (e << 10) | ((b >> 13) & 1023); }
    return t;
  }
  texUpload(id, face, level, w, h, d, fmt, pitch, data) {
    const t = this.textures.get(id); if (!t || t.isRT || t.isDS) return;
    if (t.serial === this.serial) { this.stats.flushTex = (this.stats.flushTex || 0) + 1; this.flush(); }
    this.stats.uploads++; const info = t.info, lw = Math.max(1, t.w >> level), lh = Math.max(1, t.h >> level);
    let bytes = data, bpr, rows, ew = w, eh = h;
    if (info.bc && this.bcOk && !info.bcFallback) { const bw = (w + 3) >> 2, bh = (h + 3) >> 2; bpr = bw * info.block; rows = bh; ew = bw * 4; eh = bh * 4; }
    else if (info.bc) return;                                  // CPU fallback not implemented
    else { bytes = this.convert(info, w, h, d, data); bpr = (info.conv ? 4 : info.bpp) * w; rows = h; if (info.conv === 'rg16') bpr = 8 * w; }
    const origin = [0, 0, t.kind === 1 ? face : 0];
    this.q.writeTexture({ texture: t.gpu, mipLevel: level, origin }, bytes, { bytesPerRow: bpr, rowsPerImage: rows }, [ew, eh, t.kind === 2 ? d : 1]);
  }
  // ---- buffers / decls / shaders ---------------------------------------------------------------------------------------------------------------------
  bufCreate(id, kind, size, fmt, usage) {
    const sz = (size + 3) & ~3;
    const gpu = this.dev.createBuffer({ size: Math.max(4, sz), usage: GPUBufferUsage.COPY_DST | (kind === 1 ? GPUBufferUsage.INDEX : GPUBufferUsage.VERTEX), label: 'buf' + id });
    this.buffers.set(id, { id, kind, size, fmt, gpu, uid: this.uid++, serial: 0 });
  }
  bufUpload(id, off, data) {
    const b = this.buffers.get(id); if (!b) return;
    if (b.serial === this.serial && this.bufBusy(b, off, off + data.length)) { this.stats.flushBuf = (this.stats.flushBuf || 0) + 1; if (this.dbgFlush && this.dbgFlush.length < 40) this.dbgFlush.push({ id, kind: b.kind, size: b.size, off, len: data.length, used: JSON.stringify(b.used) }); this.flush(); }
    this.stats.uploads++;
    let src = data;
    if ((off & 3) || (data.length & 3)) { const a = off & ~3, end = (off + data.length + 3) & ~3; const t = new Uint8Array(end - a); t.set(data, off - a); src = t; off = a; this.warn('bufal', 'unaligned buffer upload (padded)'); }
    if (off + src.length > b.gpu.size) src = src.subarray(0, Math.max(0, b.gpu.size - off));
    if (src.length) this.q.writeBuffer(b.gpu, off, src.buffer, src.byteOffset, src.length);
  }
  // byte ranges of a buffer read by commands recorded since the last submit: an upload that overlaps one of them must wait for the submit
  noteBufUse(b, lo, hi) {
    if (b.serial !== this.serial) { b.serial = this.serial; b.used = []; }
    const u = b.used, n = u.length;
    if (n && u[n - 1][1] >= lo && u[n - 1][0] <= hi) { if (lo < u[n - 1][0]) u[n - 1][0] = lo; if (hi > u[n - 1][1]) u[n - 1][1] = hi; }
    else if (n < 64) u.push([lo, hi]); else { u[0] = [Math.min(u[0][0], lo), Math.max(u[n - 1][1], hi)]; u.length = 1; }
  }
  bufBusy(b, lo, hi) { if (!b.used) return true; for (const r of b.used) if (r[0] < hi && lo < r[1]) return true; return false; }
  vdeclCreate(id, n, dv) {
    const elems = []; for (let i = 0; i < n; i++) { const o = i * 8, stream = dv.getUint16(o, true); if (stream === 0xff) break;
      elems.push({ stream, offset: dv.getUint16(o + 2, true), type: dv.getUint8(o + 4), method: dv.getUint8(o + 5), usage: dv.getUint8(o + 6), idx: dv.getUint8(o + 7) }); }
    const locs = new Map(); let hasPT = false;
    for (const e of elems) { if (e.usage === 9) { hasPT = true; locs.set(0, e); continue; } const m = this.vin[USAGE_NAMES[e.usage]]; if (m && e.idx < m[1]) locs.set(m[0] + e.idx, e); }
    this.vdecls.set(id, { id, elems, locs, hasPT });
  }
  bindShader(id, stage, lo, hi, clen) {
    const key = (BigInt(hi) << 32n | BigInt(lo)).toString(16).padStart(16, '0') + '_' + clen, rec = this.recFor(key);
    if (!rec) { this.warn('nopack' + key, `shader ${key} not in the WGSL pack`); return; }
    this.shaders.set(id, rec); this.stats.shaders++;
  }
  shaderRef(id, stage, lo, hi, clen) { this.bindShader(id, stage, lo, hi, clen); }
  shaderCreate(ab, p, len, u) {
    const id = u(0), stage = u(1);
    const key = this.bodyKeyFor(new Uint8Array(ab, p + 24, len - 25));
    if (!key) { this.warn('nobody' + id, `shader #${id}: GLSL body not found in shaders.bin (no bytecode key)`); return; }
    const [lo, hi, cl] = key.split('_').map(Number); this.bindShader(id, stage, lo, hi, cl);
  }
  // ---- samplers / views / bind groups ------------------------------------------------------------------------------------------------------------------
  samplerFor(s, tx) {
    const multi = tx && tx.levels > 1 && s[7] !== 0, mag = s[5], min = s[6], mip = s[7];
    const aniso = (s[6] === 3 || s[5] === 3) && !this.dbg.has('noaniso') ? Math.max(1, Math.min(16, s[10])) : 1;
    const nonfilt = tx && tx.stype === 'unfilterable-float';
    const key = `${s[1]}${s[2]}${s[3]}${mag}${min}${multi ? mip : 0}${aniso}${nonfilt ? 1 : 0}`;
    let sm = this.samplers.get(key);
    if (!sm) {
      const lin = (x) => (nonfilt ? 'nearest' : (x === 1 ? 'nearest' : 'linear'));
      const an = aniso > 1 && !nonfilt;       // WebGPU rejects maxAnisotropy > 1 unless min, mag and mipmap filters are all linear (an invalid sampler invalidates the whole frame's command buffer)
      sm = this.dev.createSampler({ addressModeU: ADDR[s[1]] || 'repeat', addressModeV: ADDR[s[2]] || 'repeat', addressModeW: ADDR[s[3]] || 'repeat', magFilter: an ? 'linear' : lin(mag), minFilter: an ? 'linear' : lin(min),
        mipmapFilter: an || (multi && !nonfilt && mip === 2 && !this.dbg.has('trimip')) ? 'linear' : 'nearest', lodMinClamp: 0, lodMaxClamp: multi && !this.dbg.has('nomip') ? 32 : 0, maxAnisotropy: an ? aniso : 1 });
      sm.uid = this.uid++; sm.nonfilt = nonfilt; this.samplers.set(key, sm);
    }
    return sm;
  }
  viewFor(tx, srgb) {
    const k = srgb ? 1 : 0; let v = tx.views.get(k);
    if (!v) {
      const dim = tx.kind === 1 ? 'cube' : tx.kind === 2 ? '3d' : '2d', d = { dimension: dim };
      if (tx.info.depth) d.aspect = 'depth-only';
      if (srgb && tx.srgbOk) d.format = tx.info.srgb;
      v = tx.gpu.createView(d); v.uid = this.uid++; v.stype = tx.stype; tx.views.set(k, v);
    }
    return v;
  }
  rtView(tx, level, face, srgb) {
    const k = `r${level}.${face}.${srgb ? 1 : 0}`; let v = tx.views.get(k); const cubeT = !tx.isSurf && tx.kind === 1;
    if (!v) {
      const d = { dimension: '2d', baseMipLevel: level, mipLevelCount: 1, baseArrayLayer: cubeT ? face : 0, arrayLayerCount: 1 };
      if (srgb && (tx.srgbOk || tx.isSurf && tx.info.srgb)) d.format = tx.isSurf && tx.fmt !== undefined && tx.kind === 0 ? 'bgra8unorm-srgb' : tx.info.srgb;
      v = tx.gpu.createView(d); v.uid = this.uid++; tx.views.set(k, v);
    }
    return v;
  }
  // ---- render targets ---------------------------------------------------------------------------------------------------------------------------------
  resolveTarget(desc, srgb) {
    if (!desc || desc.kind === 3) return null;
    if (desc.kind === 0) { const s = this.bb; if (!s) return null; return { view: this.rtView(s, 0, 0, srgb), w: s.w, h: s.h, fmt: srgb ? 'bgra8unorm-srgb' : s.info.format, ref: s, key: 'b' + (srgb ? 1 : 0) }; }
    if (desc.kind === 1) { const t = this.textures.get(desc.id); if (!t) return null; const w = Math.max(1, t.w >> desc.level), h = Math.max(1, t.h >> desc.level);
      const isd = t.info.depth; return { view: isd ? this.depthView(t, desc.level) : this.rtView(t, desc.level, desc.face, srgb), w, h, fmt: srgb && !isd && t.srgbOk ? t.info.srgb : t.info.format, ref: t, depth: isd, stencil: t.info.stencil, key: `t${t.id}.${desc.level}.${desc.face}.${srgb ? 1 : 0}` }; }
    const s = this.surfaces.get(desc.id); if (!s) return null; const isd = s.info.depth;
    return { view: isd ? this.depthView(s, 0) : this.rtView(s, 0, 0, srgb), w: s.w, h: s.h, fmt: srgb && !isd && s.info.srgb ? s.info.srgb : s.info.format, ref: s, depth: isd, stencil: s.info.stencil, key: `s${s.id}.${srgb ? 1 : 0}` };
  }
  depthView(t, level) {
    const k = 'd' + level; let v = t.views.get(k);
    if (!v) { v = t.gpu.createView({ dimension: '2d', baseMipLevel: level, mipLevelCount: 1, aspect: 'all' }); v.uid = this.uid++; t.views.set(k, v); }
    return v;
  }
  privateDepth(w, h, stencil) {
    const key = `${w}x${h}/${stencil ? 1 : 0}`; if (!this.pdepth) this.pdepth = new Map(); let d = this.pdepth.get(key);
    if (!d) { const format = stencil ? 'depth24plus-stencil8' : 'depth24plus'; const t = this.dev.createTexture({ size: [w, h], format, usage: GPUTextureUsage.RENDER_ATTACHMENT, label: 'pdepth' + key });
      d = { view: t.createView(), w, h, fmt: format, depth: true, stencil, key: 'p' + key, ref: { serial: 0 } }; this.pdepth.set(key, d); this.warn('dsfix' + key, `depth surface larger than colour target: private depth buffer ${key}`); }
    return d;
  }
  bindTargets(srgbw) {
    const key0 = srgbw ? 1 : 0;
    if (!this.rtDirty && this.tgt && this.tgt.srgb === key0) return this.tgt;
    const cols = [0, 1, 2, 3].map((i) => this.resolveTarget(this.rt[i], !!srgbw));
    let ds = this.resolveTarget(this.ds, false), c0 = cols[0] || cols.find((c) => c);
    if (ds && c0 && (ds.w !== c0.w || ds.h !== c0.h)) ds = this.privateDepth(c0.w, c0.h, ds.stencil);
    const key = cols.map((c) => (c ? c.key : '-')).join(',') + '/' + (ds ? ds.key : '-');
    this.rtDirty = false;
    if (this.tgt && this.tgt.key === key) { return this.tgt; }
    this.endPass();                                         // different targets: close the open pass (applies a pending clear first)
    this.tgt = { key, cols, ds, w: c0 ? c0.w : 1, h: c0 ? c0.h : 1, srgb: key0, fmts: cols.map((c) => (c ? c.fmt : null)), dfmt: ds ? ds.fmt : null, hasStencil: !!(ds && ds.stencil) };
    this.passState = null;
    return this.tgt;
  }
  beginPass() {
    const t = this.tgt, pc = this.pc; this.pc = null;
    const ca = t.cols.map((c) => (c ? { view: c.view, loadOp: pc && pc.c ? 'clear' : 'load', storeOp: 'store', clearValue: pc && pc.c ? pc.c : undefined } : null));
    const d = { colorAttachments: ca };
    if (t.ds) {
      d.depthStencilAttachment = { view: t.ds.view, depthLoadOp: pc && pc.z !== undefined ? 'clear' : 'load', depthStoreOp: 'store', depthClearValue: pc && pc.z !== undefined ? pc.z : 1 };
      if (t.hasStencil) { d.depthStencilAttachment.stencilLoadOp = pc && pc.s !== undefined ? 'clear' : 'load'; d.depthStencilAttachment.stencilStoreOp = 'store'; d.depthStencilAttachment.stencilClearValue = pc && pc.s !== undefined ? pc.s : 0; }
    }
    this.pass = this.enc.beginRenderPass(d); this.stats.passes++; this.passState = { vx: -1, sx: -1, pipe: null, stencilRef: -1, bc: null, bg0: null, bg1: null, off0: -1, off1: -1, ib: null, vbs: [] };
    for (const c of t.cols) if (c && c.ref) c.ref.serial = this.serial; if (t.ds && t.ds.ref) t.ds.ref.serial = this.serial;
  }
  endPass() {
    if (!this.pass && this.pc && this.tgt) this.beginPass();       // pending clear without draws still has to happen
    if (this.pass) { this.pass.end(); this.pass = null; }
    this.pc = null;
  }
  flush() {
    this.endPass(); this.stats.flushes++;
    if (this.uboOff) this.q.writeBuffer(this.ubo, 0, this.uboStage.buffer, 0, this.uboOff);          // all constant blocks of this encoder in one write (ordered before the submit)
    this.q.submit([this.enc.finish()]); this.pumpReadbacks(); this.enc = this.dev.createCommandEncoder(); this.serial++; this.uboOff = 0; this.upOff = 0; this.smallOff = 0;
    for (const d of this.dead) d.destroy(); this.dead.length = 0; this.bgCacheClear();
  }
  bgCacheClear() { if (this.bgCache.size > 4000) this.bgCache.clear(); }
  // ---- clears / stretch ----------------------------------------------------------------------------------------------------------------------------
  clear(flags, color, z, stencil, nrects, rects) {
    const t = this.bindTargets(!!this.rs[194]); this.stats.clears++;
    const doC = (flags & 1) && t.cols.some((c) => c), doZ = (flags & 2) && t.ds, doS = (flags & 4) && t.hasStencil; if (!doC && !doZ && !doS) return;
    const vp = this.vp, full = vp.x === 0 && vp.y === 0 && vp.w >= t.w && vp.h >= t.h;
    const cv = [((color >> 16) & 255) / 255, ((color >> 8) & 255) / 255, (color & 255) / 255, ((color >>> 24) & 255) / 255];
    if (!nrects && full || (nrects === 1 && rects[0] <= 0 && rects[1] <= 0 && rects[2] >= t.w && rects[3] >= t.h)) {
      if (this.pass) { this.pass.end(); this.pass = null; }
      const pc = this.pc || (this.pc = {});
      if (doC) pc.c = cv; if (doZ) pc.z = z; if (doS) pc.s = stencil;
      return;
    }
    this.stats.partialClears++;
    const list = nrects ? Array.from({ length: nrects }, (_, i) => [rects[4 * i], rects[4 * i + 1], rects[4 * i + 2], rects[4 * i + 3]]) : [[vp.x, vp.y, vp.x + vp.w, vp.y + vp.h]];
    if (this.smallOff + 256 > this.smallUbo.size) this.flush();
    if (!this.pass) this.beginPass();
    const pipe = this.clearPipe(t, doC, doZ, doS); if (!pipe) return;
    const data = new Float32Array([...cv, z, 0, 0, 0]); this.q.writeBuffer(this.smallUbo, this.smallOff, data); const off = this.smallOff; this.smallOff += 256;
    const bg = this.clearBG || (this.clearBG = this.dev.createBindGroup({ layout: this.clearBGL, entries: [{ binding: 0, resource: { buffer: this.smallUbo, size: 32 } }] }));
    this.pass.setPipeline(pipe); this.pass.setBindGroup(0, bg, [off]); this.pass.setStencilReference(stencil);
    for (const [l, tp, r, b] of list) {
      const x = Math.max(0, l), y = Math.max(0, tp), w = Math.min(t.w, r) - x, h = Math.min(t.h, b) - y; if (w <= 0 || h <= 0) continue;
      this.pass.setViewport(0, 0, t.w, t.h, 0, 1); this.pass.setScissorRect(x, y, w, h); this.pass.draw(3);
    }
    this.passState.vx = -1; this.passState.sx = -1; this.passState.pipe = null;
  }
  clearPipe(t, doC, doZ, doS) {
    const key = `clr|${t.fmts.join()}|${t.dfmt}|${+doC}${+doZ}${+doS}`; let p = this.pipes.get(key);
    if (p) return p.pipe;
    if (!this.clearBGL) this.clearBGL = this.dev.createBindGroupLayout({ entries: [{ binding: 0, visibility: GPUShaderStage.FRAGMENT | GPUShaderStage.VERTEX, buffer: { type: 'uniform', hasDynamicOffset: true } }] });
    const targets = t.fmts.map((f, i) => (f ? { format: f, writeMask: doC && i === 0 ? 15 : 0 } : null));
    const hasD = !!t.dfmt;
    const desc = { layout: this.dev.createPipelineLayout({ bindGroupLayouts: [this.clearBGL] }), vertex: { module: this.clearMod, entryPoint: 'vs' },
      fragment: { module: this.clearMod, entryPoint: hasD ? 'fs' : 'fsc', targets: hasD ? targets.map((x, i) => x) : targets }, primitive: { topology: 'triangle-list', cullMode: 'none' } };
    if (hasD) { const stencil = t.hasStencil ? { compare: 'always', failOp: 'replace', depthFailOp: 'replace', passOp: 'replace' } : undefined;
      desc.depthStencil = { format: t.dfmt, depthWriteEnabled: !!doZ, depthCompare: 'always', stencilFront: stencil, stencilBack: stencil, stencilReadMask: 0xff, stencilWriteMask: doS ? 0xff : 0 }; }
    else desc.fragment.entryPoint = 'fsc';
    p = { pipe: this.dev.createRenderPipeline(desc) }; this.pipes.set(key, p); return p.pipe;
  }
  // D3D9 SetGammaRamp (3x256 uint16): identity ramps keep the plain copy, otherwise the present pass maps each channel through a 256x1 LUT
  setGamma(r) {
    let id = true; for (let i = 0; i < 768; i++) if (Math.abs(r[i] - (i & 255) * 257) > 128) { id = false; break; }
    this.gammaId = id; if (id) return;
    const px = new Uint8Array(256 * 4); for (let i = 0; i < 256; i++) { px[4 * i] = r[i] >> 8; px[4 * i + 1] = r[256 + i] >> 8; px[4 * i + 2] = r[512 + i] >> 8; px[4 * i + 3] = 255; }
    if (!this.gammaTex) this.gammaTex = this.dev.createTexture({ size: [256, 1], format: 'rgba8unorm', usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
    this.q.writeTexture({ texture: this.gammaTex }, px, { bytesPerRow: 1024 }, [256, 1]);
  }
  gammaPresent(srcView, dstView, dstFmt) {
    this.endPass();
    if (!this.gammaMod) { this.gammaMod = this.dev.createShaderModule({ code: GAMMA_WGSL });
      this.gammaBGL = this.dev.createBindGroupLayout({ entries: [{ binding: 0, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'unfilterable-float' } }, { binding: 1, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'unfilterable-float' } }] }); }
    const key = 'gamma|' + dstFmt; let pipe = this.pipes.get(key);
    if (!pipe) { pipe = this.dev.createRenderPipeline({ layout: this.dev.createPipelineLayout({ bindGroupLayouts: [this.gammaBGL] }), vertex: { module: this.gammaMod, entryPoint: 'vs' }, fragment: { module: this.gammaMod, entryPoint: 'fs', targets: [{ format: dstFmt }] }, primitive: { topology: 'triangle-list' } }); this.pipes.set(key, pipe); }
    const bg = this.dev.createBindGroup({ layout: this.gammaBGL, entries: [{ binding: 0, resource: srcView }, { binding: 1, resource: this.gammaTex.createView() }] });
    const ps = this.enc.beginRenderPass({ colorAttachments: [{ view: dstView, loadOp: 'load', storeOp: 'store' }] });
    ps.setPipeline(pipe); ps.setBindGroup(0, bg); ps.draw(3); ps.end();
  }
  blitPipe(fmt) {
    const key = 'blit|' + fmt; let p = this.pipes.get(key);
    if (!p) {
      if (!this.blitBGL) this.blitBGL = this.dev.createBindGroupLayout({ entries: [{ binding: 0, visibility: GPUShaderStage.VERTEX, buffer: { type: 'uniform', hasDynamicOffset: true } },
        { binding: 1, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float' } }, { binding: 2, visibility: GPUShaderStage.FRAGMENT, sampler: { type: 'filtering' } }] });
      p = { pipe: this.dev.createRenderPipeline({ layout: this.dev.createPipelineLayout({ bindGroupLayouts: [this.blitBGL] }), vertex: { module: this.blitMod, entryPoint: 'vs' },
        fragment: { module: this.blitMod, entryPoint: 'fs', targets: [{ format: fmt }] }, primitive: { topology: 'triangle-list' } }) }; this.pipes.set(key, p);
    }
    return p.pipe;
  }
  // render srcView (uv rect) into dstView with a full-screen triangle; dst rect via viewport
  blit(srcView, uv, dstView, dstFmt, vpRect, linear, clearFirst) {
    this.endPass();
    if (this.smallOff + 256 > this.smallUbo.size) this.flush();
    this.q.writeBuffer(this.smallUbo, this.smallOff, new Float32Array([uv[0], uv[1], uv[2], uv[3], 0, 0, 0, 0])); const off = this.smallOff; this.smallOff += 256;
    const bg = this.dev.createBindGroup({ layout: this.blitBGL || (this.blitPipe(dstFmt), this.blitBGL), entries: [{ binding: 0, resource: { buffer: this.smallUbo, size: 32 } }, { binding: 1, resource: srcView }, { binding: 2, resource: linear ? this.linear : this.nearest }] });
    const ps = this.enc.beginRenderPass({ colorAttachments: [{ view: dstView, loadOp: 'load', storeOp: 'store' }] });
    ps.setPipeline(this.blitPipe(dstFmt)); ps.setBindGroup(0, bg, [off]); if (vpRect) { ps.setViewport(vpRect[0], vpRect[1], vpRect[2], vpRect[3], 0, 1); ps.setScissorRect(vpRect[0], vpRect[1], vpRect[2], vpRect[3]); } ps.draw(3); ps.end();
  }
  stretch(w) {                                                // [skind,sid,slevel,sface,hasS,sl,st,sr,sb, dkind,did,dlevel,dface,hasD,dl,dt,dr,db, filter]
    this.stats.stretch++;
    const dst = this.resolveTarget({ kind: w[9], id: w[10], level: w[11], face: w[12] }, false); if (!dst) return;
    if (dst.ref.serial === this.serial) { /* in-flight use: ordering is preserved by the encoder */ }
    const dr = w[13] ? [w[14], w[15], w[16] - w[14], w[17] - w[15]] : [0, 0, dst.w, dst.h];
    this.endPass(); this.tgt = null; this.rtDirty = true;
    if (w[0] === 4) {                                          // ColorFill: render a clear
      const c = w[1], cv = { r: ((c >> 16) & 255) / 255, g: ((c >> 8) & 255) / 255, b: (c & 255) / 255, a: ((c >>> 24) & 255) / 255 };
      if (!w[13]) { const ps = this.enc.beginRenderPass({ colorAttachments: [{ view: dst.view, loadOp: 'clear', storeOp: 'store', clearValue: cv }] }); ps.end(); return; }
      this.warn('cfrect', 'ColorFill with rect: not implemented'); return;
    }
    const src = this.resolveTarget({ kind: w[0], id: w[1], level: w[2], face: w[3] }, false); if (!src || src.depth) return;
    const sr = w[4] ? [w[5], w[6], w[7], w[8]] : [0, 0, src.w, src.h];
    const same = !w[4] && !w[13] && src.w === dst.w && src.h === dst.h && src.fmt === dst.fmt && src.ref !== dst.ref;
    if (same && src.ref.gpu && dst.ref.gpu) { this.enc.copyTextureToTexture({ texture: src.ref.gpu, mipLevel: w[2] }, { texture: dst.ref.gpu, mipLevel: w[11], origin: [0, 0, !dst.ref.isSurf && dst.ref.kind === 1 ? w[12] : 0] }, [dst.w, dst.h, 1]); return; }
    const sv = src.ref.gpu.createView({ dimension: '2d', baseMipLevel: w[2], mipLevelCount: 1, baseArrayLayer: !src.ref.isSurf && src.ref.kind === 1 ? w[3] : 0, arrayLayerCount: 1, format: src.ref.info.format });
    this.blit(sv, [sr[0] / src.w, sr[1] / src.h, sr[2] / src.w, sr[3] / src.h], dst.view, dst.fmt, dr, w[18] !== 1);
  }
  // ---- readback of texel (0,0) (message 81): the game reads its 1x1 luminance target every frame; answered asynchronously like the GL path (sendInput type 10) ----
  readbackTexel(kind, id, level, face, dstId) {
    if (!this.sendInput) return;
    const tg = this.resolveTarget({ kind, id, level, face }, false); if (!tg || tg.depth || !tg.ref.gpu) return;
    const ref = tg.ref, cube = !ref.isSurf && ref.kind === 1; this.endPass();
    const pool = this.rbPool || (this.rbPool = []), buf = pool.pop() || this.dev.createBuffer({ size: 256, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    this.enc.copyTextureToBuffer({ texture: ref.gpu, mipLevel: ref.isSurf ? 0 : level, origin: [0, 0, cube ? face : 0] }, { buffer: buf, bytesPerRow: 256 }, [1, 1, 1]);
    (this.rbPending || (this.rbPending = [])).push({ buf, fmt: ref.info.format, dstId });
  }
  pumpReadbacks() {      // after a submit: map the copies and answer
    const q = this.rbPending; if (!q || !q.length) return; this.rbPending = [];
    for (const r of q) r.buf.mapAsync(GPUMapMode.READ).then(() => {
      const u = new Uint32Array(r.buf.getMappedRange().slice(0, 4))[0]; r.buf.unmap(); this.rbPool.push(r.buf);
      let bits = 0; if (r.fmt === 'bgra8unorm' || r.fmt === 'r32float') bits = u; else if (/8unorm/.test(r.fmt)) bits = ((u >>> 24) << 24) | ((u & 255) << 16) | (((u >> 8) & 255) << 8) | ((u >> 16) & 255);
      this.sendInput(10, r.dstId, 0, 0, bits | 0);
    }, () => { this.rbPool.push(r.buf); });
  }
  // ---- offline shader cache hook (index.html calls loadShaderCache/loadWarm for the WebGL2 renderer): shaders.bin is only needed to map type-40 GLSL bodies to bytecode keys ----
  async loadShaderCache(url) { const res = await fetch(url); if (!res.ok) throw new Error(`shader cache ${url}: HTTP ${res.status}`); this.scBuf = await res.arrayBuffer(); }
  bodyKeyFor(u8) {
    if (this.bodyKey) return this.bodyKey(u8);
    if (!this.bodyMap) {
      if (!this.scBuf) return null; const dv = new DataView(this.scBuf), b = new Uint8Array(this.scBuf); this.bodyMap = new Map();
      for (let i = 0, n = dv.getUint32(4, true), p = 8; i < n; i++) { const lo = dv.getUint32(p, true), hi = dv.getUint32(p + 4, true), clen = dv.getUint32(p + 8, true), nin = b[p + 13], nout = b[p + 14], nsamp = b[p + 15], glen = dv.getUint32(p + 16, true), body = p + 20, blen = 4 * (nin + nout + nsamp) + glen;
        this.bodyMap.set(this.decStr(b.subarray(body, body + blen)), `${lo}_${hi}_${clen}`); p = (body + blen + 3) & ~3; }
    }
    return this.bodyMap.get(this.decStr(u8)) || null;
  }
  decStr(u8) { let s = ''; for (let i = 0; i < u8.length; i += 8192) s += String.fromCharCode.apply(null, u8.subarray(i, i + 8192)); return s; }
  // ---- frame timing / HUD (same contract as the WebGL2 renderer: ivs/ivn ring, perf(), statLine(), F2) ----
  frameDone() {
    const now = performance.now();
    if (!this.ivs) { this.ivs = new Float32Array(600); this.ivn = 0; this.ivLast = 0; }
    if (this.ivLast) { this.ivs[this.ivn % 600] = now - this.ivLast; this.ivn++; } this.ivLast = now;
    if (this.hud && this.ivn % 3 === 0) this.drawHud();
  }
  perf(n = 120) {
    const N = Math.min(n, this.ivn || 0, 600), v = []; for (let i = 0; i < N; i++) v.push(this.ivs[(this.ivn - N + i) % 600]); const s = v.slice().sort((a, b) => a - b), avg = v.reduce((a, b) => a + b, 0) / (N || 1);
    return { frames: N, fps: N ? 1000 / avg : 0, avg, p50: s[Math.floor(N * 0.5)], p99: s[Math.floor(N * 0.99)], max: s[N - 1] };
  }
  initHud() {
    if (this.hud) return; const c = document.createElement('canvas'); c.width = 420; c.height = 150; c.style.cssText = 'position:fixed;right:10px;top:44px;pointer-events:none;z-index:9;background:rgba(0,0,0,.55);border:1px solid #333';
    document.body.appendChild(c); this.hud = { c, g: c.getContext('2d') };
  }
  drawHud() {
    const h = this.hud, g = h.g, N = Math.min(this.ivn, 180); g.clearRect(0, 0, 420, 150); g.font = '11px ui-monospace,Menlo,monospace';
    let sum = 0, mx = 0; const v = []; for (let i = 0; i < N; i++) { const x = this.ivs[(this.ivn - N + i) % 600]; v.push(x); sum += x; mx = Math.max(mx, x); }
    const avg = sum / (N || 1), s = v.slice().sort((a, b) => a - b), p99 = s[Math.floor(s.length * 0.99)] || 0, y0 = 150, k = 3;
    g.fillStyle = '#2a4'; g.fillRect(0, y0 - 16.67 * k, 420, 1);
    for (let i = 0; i < N; i++) { const x = v[i]; g.fillStyle = x > 20 ? '#e55' : x > 17.5 ? '#ea3' : '#5c8'; g.fillRect(i * 2, y0 - Math.min(x, 48) * k, 2, Math.min(x, 48) * k); }
    g.fillStyle = '#fff'; g.fillText(`${(1000 / avg).toFixed(1)} fps  avg ${avg.toFixed(1)} ms  p99 ${p99.toFixed(1)}  max ${mx.toFixed(1)}`, 4, 12);
    if (this.jsh) { let a = 0, m = 0; const n = Math.min(this.jsn, 180); for (let i = 0; i < n; i++) { a += this.jsh[i]; m = Math.max(m, this.jsh[i]); } g.fillText(`page JS per frame: avg ${(a / n).toFixed(1)} ms  max ${m.toFixed(1)} ms   (intervals above = time between game Presents)`, 4, 40); }
    g.fillText(`WebGPU  draws ${this.stats.frameDraws}  pipelines ${this.stats.pipelines} (pending ${this.pending})  skipped/frame ${this.lastFrameSkip || 0}  held ${this.stats.held || 0}`, 4, 26);
  }
  statLine() { const s = this.stats, fr = this.jsFrames || 0, js = fr ? (this.jsBusy / fr).toFixed(1) : '-', mp = fr ? Math.round(this.jsMsgs / fr) : '-'; this.jsBusy = this.jsFrames = this.jsMsgs = 0;
    return `[WebGPU] JS ${js} ms/frame  msgs/frame ${mp}  draws/frame ${s.frameDraws}  pipelines ${s.pipelines} pending ${this.pending}  skipped/frame ${this.lastFrameSkip || 0}  held ${s.held || 0}  passes ${s.passes}  flushes ${s.flushes}  fb ${s.feedback}`; }
  // ---- present -----------------------------------------------------------------------------------------------------------------------------------------
  present() {
    this.endPass(); this.tgt = null; this.rtDirty = true;
    this.stats.frameDraws = this.stats.draws - (this.lastDraws || 0); this.lastDraws = this.stats.draws; this.lastFrameSkip = this.fSkip || 0; this.fSkip = 0;
    let hold = false;
    if (this.warmLeft > 0 && performance.now() < this.warmUntil) hold = true;       // warm set still compiling right after connect: do not show half-drawn frames
    if (this.frameDeferred) { this.frameDeferred = false; if (hold) { /* already held */ } else if (++this.holdRun <= 30) { hold = true; this.stats.held = (this.stats.held || 0) + 1; } }   // a draw was skipped while its pipeline compiled: keep showing the last complete frame (at most 30 frames)
    if (!hold) this.holdRun = 0;
    if (this.bb && !hold) {
      const ct = this.ctx.getCurrentTexture();
      if (this.canvas.width !== this.bb.w || this.canvas.height !== this.bb.h) { this.canvas.width = this.bb.w; this.canvas.height = this.bb.h; }
      if (this.gammaTex && !this.gammaId && ct.width === this.bb.w && ct.height === this.bb.h) this.gammaPresent(this.bb.gpu.createView(), ct.createView(), ct.format);
      else if (this.bb.info.format === ct.format && ct.width === this.bb.w && ct.height === this.bb.h) this.enc.copyTextureToTexture({ texture: this.bb.gpu }, { texture: ct }, [this.bb.w, this.bb.h, 1]);
      else this.blit(this.bb.gpu.createView(), [0, 0, 1, 1], ct.createView(), ct.format, null, false);
    }
    this.flush(); this.frameDone();
  }
  // ---- draw --------------------------------------------------------------------------------------------------------------------------------------------
  primCount(type, n) { switch (type) { case 1: return n; case 2: return 2 * n; case 3: return n + 1; case 4: return 3 * n; case 5: case 6: return n + 2; } return n; }
  vtxFormat(e, loc, ov) {
    const I = (fmt, scale) => { if (scale) ov['isc' + loc] = scale; return fmt; };       // iscL: the pack's VS turns a unorm/snorm attribute into the integer value
    switch (e.type) {
      case 0: return 'float32'; case 1: return 'float32x2'; case 2: return 'float32x3'; case 3: return 'float32x4';
      case 4: if (this.features.has('unorm8x4-bgra')) return 'unorm8x4-bgra'; if (loc === 4 || loc === 5) ov['cswz' + (loc - 4)] = 1; else this.warn('vt4' + loc, 'D3DCOLOR at location ' + loc + ': R/B not swapped'); return 'unorm8x4';
      case 5: return I('unorm8x4', 255);
      case 6: return I('snorm16x2', 32767); case 7: return I('snorm16x4', 32767);
      case 8: return 'unorm8x4'; case 9: return 'snorm16x2'; case 10: return 'snorm16x4'; case 11: return 'unorm16x2'; case 12: return 'unorm16x4';
      case 13: this.warn('vt13', 'UDEC3 approximated by unorm10-10-10-2'); return 'unorm10-10-10-2';
      case 14: this.warn('vt14', 'DEC3N not supported'); return 'unorm10-10-10-2';
      case 15: return 'float16x2'; case 16: return 'float16x4';
    }
    this.warn('vt' + e.type, 'unsupported vertex type ' + e.type); return 'float32x4';
  }
  // fixed-function state that is baked into a pipeline: the render-state indices below, in this order (stateKey = their values; recipes store the same array)
  stateKey() {
    if (!this.stDirty && this._stKey) return this._stKey;
    const r = this.rs, hasDS = this.ds.kind !== 3, v = RSIDX.map((i) => r[i]);
    if (!hasDS) { v[RSIDX.indexOf(7)] = 0; v[RSIDX.indexOf(14)] = 0; v[RSIDX.indexOf(52)] = 0; }
    this._stVals = v; this._stKey = v.join(','); this.stDirty = false; return this._stKey;
  }
  blendDesc(fmt, r) {
    if (!r[27] || /32float/.test(fmt) && !this.features.has('float32-blendable')) return undefined;
    const color = { srcFactor: BF[r[19]] || 'one', dstFactor: BF[r[20]] || 'zero', operation: BOP[r[171]] || 'add' };
    const alpha = r[206] ? { srcFactor: BF[r[207]] || 'one', dstFactor: BF[r[208]] || 'zero', operation: BOP[r[209]] || 'add' } : Object.assign({}, color);
    for (const b of [color, alpha]) { if (b.operation === 'min' || b.operation === 'max') { b.srcFactor = 'one'; b.dstFactor = 'one'; } }
    return { color, alpha };
  }
  getPipeline(vs, ps, vl, t, topo, stripFmt, sk) {
    const L = this.lastPipe;       // same (vs, ps, layout, targets, state, topology, texture signature) as the previous draw: skip the key string
    if (L && L.vs === vs && L.ps === ps && L.vl === vl && L.t === t && L.sk === sk && L.topo === topo && L.sf === stripFmt && L.sv === this.layoutSigVs && L.sp === this.layoutSigPs && L.p.state === 'ready') return L.p;
    this.stateKey();
    const recipe = { vs: vs.key, ps: ps.key, vl: { key: vl.key, layouts: vl.layouts, constants: vl.constants }, fm: t.fmts, df: t.dfmt, hs: t.hasStencil, tp: topo, sf: stripFmt || null, sv: this.layoutSigVs, sp: this.layoutSigPs, rs: this._stVals.slice() };
    const p = this.pipelineFor(recipe, vs, ps); this.lastPipe = { vs, ps, vl, t, sk, topo, sf: stripFmt, sv: this.layoutSigVs, sp: this.layoutSigPs, p }; return p;
  }
  recipeKey(c) { return `${c.vs}|${c.ps}|${c.vl.key}|${c.fm.join()}|${c.df}|${c.rs.join()}|${c.tp}|${c.sf || ''}|${c.sv}|${c.sp}`; }
  // warm list: recipes of every pipeline created so far (export), and re-creation of recipes at connect (precompile)
  exportWarm() { return JSON.stringify([...this.recipes.values()].filter((c) => c.vs !== 'ff')); }
  // warm set = static list (gpu-warm.json, from replays) + what earlier sessions on this browser actually used (IndexedDB, versioned by pack id and recipe format)
  async loadWarm(url) {
    if (!/gpu-warm/.test(url)) return;       // index.html also passes the WebGL2 warm.json: not ours
    this.warmUntil = performance.now() + 5000; let list = [];
    try { if (/gpustatic=0/.test(location.hash)) throw new Error('static list disabled by #gpustatic=0'); const res = await fetch(url); if (res.ok) list = await res.json(); } catch (e) { this.log('[gpu] no static warm list: ' + e.message); }
    let learned = []; try { learned = await this.loadLearned(); } catch (e) { this.log('[gpu] learned warm list unavailable: ' + e); }
    let n = 0, nl = learned.length; const seen = new Set();
    for (const c of list.concat(learned)) { if (!c.vl) continue; const k = this.recipeKey(c); if (seen.has(k)) continue; seen.add(k); const vs = this.recFor(c.vs), ps = this.recFor(c.ps); if (vs && ps) { this.warmLeft++; this.pipelineFor(c, vs, ps, true); n++; } }
    this.warmCount = n; this.log(`[gpu] warm set: ${n} pipelines (${nl} learned from earlier sessions) compiling`);
    if (!n) this.warmUntil = 0;
  }
  idbOpen() { return new Promise((res, rej) => { const rq = indexedDB.open('blur-gpuwarm', 1); rq.onupgradeneeded = () => rq.result.createObjectStore('kv'); rq.onsuccess = () => res(rq.result); rq.onerror = () => rej(rq.error); }); }
  async loadLearned() {
    const db = await this.idbOpen(), v = await new Promise((res, rej) => { const r = db.transaction('kv').objectStore('kv').get('recipes'); r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error); }); db.close();
    return v && v.ver === RECIPE_VER && v.packId === this.pack.id ? v.list : [];
  }
  scheduleSave() { if (this.saveTimer || !this.learn) return; this.saveTimer = setTimeout(() => { this.saveTimer = 0; this.saveLearned(); }, 5000); }
  async saveLearned() {
    if (!this.warmDirty) return; this.warmDirty = false;
    try { const list = [...this.recipes.values()].filter((c) => c.vs !== 'ff').slice(-3000), db = await this.idbOpen();
      await new Promise((res, rej) => { const tx = db.transaction('kv', 'readwrite'); tx.objectStore('kv').put({ ver: RECIPE_VER, packId: this.pack.id, list }, 'recipes'); tx.oncomplete = res; tx.onerror = () => rej(tx.error); }); db.close();
    } catch (e) { this.log('[gpu] cannot persist warm set: ' + e); }
  }
  recFor(key) {
    let rec = this.recs && this.recs.get(key);
    if (!rec) { const s = this.pack.get(key); if (!s) return null; rec = s; rec.key = key; rec.pid = this.uid++; rec.module = null; rec.regs = Object.keys(s.texTypes).map(Number); (this.recs || (this.recs = new Map())).set(key, rec); }
    return rec;
  }
  // Compiles are queued: demand (a draw needs it) before warm-list entries, at most MAXFLIGHT creations in flight so a burst of new effects does not hitch the frame
  pipelineFor(c, vs, ps, warm) {
    const key = this.recipeKey(c); let p = this.pipes.get(key); if (p) return p;
    p = { state: 'pending', pipe: null, warm: !!warm }; this.pipes.set(key, p); this.pending++; this.stats.pipelines++;
    if (!this.recipes.has(key)) { this.recipes.set(key, c); if (!warm) { this.warmDirty = true; this.scheduleSave(); } }
    (warm ? this.warmQ : this.demandQ).push({ c, vs, ps, key, p }); this.pumpCompile(); return p;
  }
  pumpCompile() {
    while (this.inflight < (this.demandQ.length ? this.MAXFLIGHT : Math.min(3, this.MAXFLIGHT)) && (this.demandQ.length || this.warmQ.length)) {
      const j = this.demandQ.length ? this.demandQ.shift() : this.warmQ.shift(); this.inflight++; this.startCompile(j);
    }
  }
  startCompile({ c, vs, ps, key, p }) {
    const done = () => { this.inflight--; this.pending--; if (p.warm) this.warmLeft--; this.pumpCompile(); };
    const r = c.rs.reduce((o, v, i) => { o[RSIDX[i]] = v; return o; }, {});
    const targets = c.fm.map((f) => (f ? { format: f, blend: this.blendDesc(f, r), writeMask: r[168] & 15 } : null));
    while (targets.length && !targets[targets.length - 1]) targets.pop();
    const sigs = [c.sv, c.sp];
    const desc = { layout: this.pipelineLayoutSig(vs, ps, c.sv, c.sp), vertex: { module: this.moduleFor(vs), entryPoint: 'main', buffers: c.vl.layouts, constants: c.vl.constants },
      fragment: { module: this.moduleFor(ps), entryPoint: 'main', targets }, primitive: { topology: c.tp, cullMode: r[22] === 1 ? 'none' : 'back', frontFace: r[22] === 2 ? 'ccw' : 'cw' } };
    if (c.sf) desc.primitive.stripIndexFormat = c.sf;
    if (c.df) {
      desc.depthStencil = { format: c.df, depthWriteEnabled: !!(r[7] && r[14]), depthCompare: r[7] ? CMP[r[23]] || 'always' : 'always' };
      const bias = new Float32Array(1), slope = new Float32Array(1); new Uint32Array(bias.buffer)[0] = r[195]; new Uint32Array(slope.buffer)[0] = r[175];
      if (bias[0] || slope[0]) { desc.depthStencil.depthBias = Math.round(bias[0] * 16777216); desc.depthStencil.depthBiasSlopeScale = slope[0]; }
      if (c.hs && r[52]) {
        const f = { compare: CMP[r[56]] || 'always', failOp: SOP[r[53]] || 'keep', depthFailOp: SOP[r[54]] || 'keep', passOp: SOP[r[55]] || 'keep' };
        const b = r[185] ? { compare: CMP[r[189]] || 'always', failOp: SOP[r[186]] || 'keep', depthFailOp: SOP[r[187]] || 'keep', passOp: SOP[r[188]] || 'keep' } : f;
        desc.depthStencil.stencilFront = f; desc.depthStencil.stencilBack = b; desc.depthStencil.stencilReadMask = r[58] & 255; desc.depthStencil.stencilWriteMask = r[59] & 255;
      }
    }
    this.dev.createRenderPipelineAsync(desc).then((pipe) => { p.pipe = pipe; p.state = 'ready'; done(); }, (e) => { p.state = 'failed'; done(); this.stats.pipelineFail++; this.warn('pf' + key, `pipeline failed (vs ${vs.key} ps ${ps.key}): ${String(e.message).slice(0, 300)}`); });
    return p;
  }
  moduleFor(rec) { if (!rec.module) { rec.module = this.dev.createShaderModule({ code: rec.wgsl, label: rec.key }); } return rec.module; }
  bgl(rec, sig, cbCount) {
    const key = rec.stage + sig; let l = this.bgls.get(key);
    if (!l) {
      const vis = rec.stage === 'vs' ? GPUShaderStage.VERTEX : GPUShaderStage.FRAGMENT, B = this.pack.meta.bindings, entries = [{ binding: B.cbFloat, visibility: vis, buffer: { type: 'uniform', hasDynamicOffset: true } }];
      for (const part of sig.split(';')) { if (!part) continue; const [r, type, st] = part.split(':'); entries.push({ binding: B.tex0 + +r, visibility: vis, texture: { sampleType: st, viewDimension: type } }, { binding: B.samp0 + +r, visibility: vis, sampler: { type: st === 'float' ? 'filtering' : 'non-filtering' } }); }
      l = this.dev.createBindGroupLayout({ entries, label: key }); l.uid = this.uid++; this.bgls.set(key, l);
    }
    return l;
  }
  pipelineLayout(vs, ps) { return this.pipelineLayoutSig(vs, ps, this.layoutSigVs, this.layoutSigPs); }
  pipelineLayoutSig(vs, ps, sv, sp) {
    const a = this.bgl(vs, sv), b = this.bgl(ps, sp), k = a.uid + ',' + b.uid; let pl = this.pls.get(k);
    if (!pl) { pl = this.dev.createPipelineLayout({ bindGroupLayouts: [a, b] }); this.pls.set(k, pl); }
    return pl;
  }
  // layout signature of a stage for the textures bound right now: "reg:dim:sampleType;..."
  stageSig(rec, unitBase) {
    const regs = rec.regs, e = rec._ss;
    if (e && e.life === this.texLife && !e.fb) {          // same textures and sampler states as the last time this shader was drawn: reuse signature, views and bind group
      let ok = true; for (let i = 0; i < regs.length; i++) { const u = unitBase + regs[i]; if (this.tex[u] !== e.ids[i] || this.ssVer[u] !== e.vers[i]) { ok = false; break; } }
      if (ok) { for (let i = 0; i < e.txs.length; i++) if (e.txs[i]) e.txs[i].serial = this.serial; return e; }
    }
    let sig = ''; const views = [], ids = [], vers = [], txs = [];
    for (const r of regs) {
      const unit = unitBase + r, id = this.tex[unit] || 0, tx = id ? this.textures.get(id) : null, dim = rec.texTypes[r];
      const want = dim === 'cube' ? 1 : dim === '3d' ? 2 : 0; ids.push(this.tex[unit]); vers.push(this.ssVer[unit]);
      if (tx && tx.kind === want) {
        const s = this.ss[unit], srgb = !!s[11] && !tx.info.float && !tx.info.depth && !this.dbg.has('nosrgb');
        views.push(this.viewFor(tx, srgb), tx, s); sig += `${r}:${dim}:${tx.stype};`; tx.serial = this.serial; txs.push(tx);
      } else { views.push(this.dummy[dim], null, this.ss[unit]); sig += `${r}:${dim}:float;`; txs.push(null); }
    }
    return (rec._ss = { sig, views, ids, vers, txs, life: this.texLife, fb: false, bg: null });
  }
  constBlock(rec, isPs, params) {
    // write cb[0..cbVec) + the params vec4 at cb[cbVec] into the ring (room is guaranteed by setupDraw); returns the dynamic offset
    const n = rec.cbVec, src = rec.ff ? this.ffConst : (isPs ? this.psc : this.vsc), ver = rec.ff ? this.vp.w * 4096 + this.vp.h : (isPs ? this.pscVer : this.vsVerOf());
    const ck = ver + '|' + n + '|' + params[0] + '|' + params[1] + '|' + (rec.ff ? 1 : 0), L = this.lastCb[isPs ? 1 : 0];
    if (L.ck === ck && L.serial === this.serial) return L.off;
    const bytes = (n + 1) * 16, sc = this.vscScratch; sc.set(src.subarray(0, n * 4)); sc[n * 4] = params[0]; sc[n * 4 + 1] = params[1]; sc[n * 4 + 2] = 0; sc[n * 4 + 3] = 0;
    const off = this.uboOff; this.uboStageF.set(sc.subarray(0, (n + 1) * 4), off >> 2); this.uboOff += (bytes + 255) & ~255;
    L.ck = ck; L.serial = this.serial; L.off = off; return off;
  }
  vsVerOf() { return this.vscVer; }
  prepareDraw(topoType, indexed, ibFmt) {
    let vsRec = this.shaders.get(this.vs), vdecl = this.vdecls.get(this.vdecl);
    if (!this.vs && vdecl && vdecl.hasPT && this.ps) vsRec = this.ffRec;
    const psRec = this.shaders.get(this.ps);
    if (!vsRec || !psRec) { this.stats.skipped++; this.warn('noshader', 'draw without vertex/pixel shader (fixed-function pipeline not implemented)'); return null; }
    if (!vdecl) { this.stats.skipped++; this.warn('nodecl', 'draw without vertex declaration'); return null; }
    const t = this.bindTargets(!!this.rs[194]);
    if (!t.cols.some((c) => c) && !t.ds) { this.stats.skipped++; return null; }
    // textures: feedback (sampled texture is also an attachment of the open pass) -> sample a snapshot
    const vsSig = this.stageSig(vsRec, 16), psSig = this.stageSig(psRec, 0);
    this.layoutSigVs = vsSig.sig; this.layoutSigPs = psSig.sig;
    this.feedbackCheck(psSig, psRec, 0); this.feedbackCheck(vsSig, vsRec, 16);
    // vertex layout
    const vl = this.vertexLayout(vsRec, vdecl); if (!vl) { this.stats.skipped++; return null; }
    const topo = ['point-list', 'point-list', 'line-list', 'line-strip', 'triangle-list', 'triangle-strip', 'triangle-list'][topoType] || 'triangle-list';
    const stripFmt = indexed && (topo === 'triangle-strip' || topo === 'line-strip') ? ((ibFmt !== undefined ? ibFmt : (this.buffers.get(this.ib) || {}).fmt) === 101 ? 'uint16' : 'uint32') : undefined;
    const sk = this.stateKey();
    const p = this.getPipeline(vsRec, psRec, vl, t, topo, stripFmt, sk);
    if (p.state !== 'ready') {
      if (this.warmLeft > 0 && performance.now() < this.warmUntil) { this.stats.skippedWarmup = (this.stats.skippedWarmup || 0) + 1; return null; }      // startup replay while the warm set compiles: frames are held anyway, not counted as skips
      this.stats.skipped++; this.fSkip = (this.fSkip || 0) + 1; if (p.state === 'pending') this.frameDeferred = true; return null;
    }
    return { vsRec, psRec, vsSig, psSig, vl, t, p, topo };
  }
  feedbackCheck(sig, rec, unitBase) {
    const t = this.tgt; if (!t) return;
    for (let i = 0; i < rec.regs.length; i++) {
      const tx = sig.views[i * 3 + 1]; if (!tx || !tx.isRT && !tx.isDS && !tx.isSurf) continue;
      if (t.cols.some((c) => c && c.ref === tx) || (t.ds && t.ds.ref === tx)) {
        this.stats.feedback++; this.endPass();
        if (!tx.snap) tx.snap = this.dev.createTexture({ size: [tx.w, tx.h], format: tx.info.format, usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST, label: 'snap' + tx.id });
        this.enc.copyTextureToTexture({ texture: tx.gpu }, { texture: tx.snap }, [tx.w, tx.h, 1]);
        if (!tx.snapView) { tx.snapView = tx.snap.createView(); tx.snapView.uid = this.uid++; tx.snapView.stype = tx.stype; }
        sig.views[i * 3] = tx.snapView; sig.fb = true; sig.bg = null;
      }
    }
  }
  vertexLayout(vsRec, vdecl) {
    const c0 = vsRec._vl; if (c0 && c0.did === vdecl.id && c0.gen === this.streamGen && c0.dd === vdecl) return c0.vl;
    const vl0 = this.vertexLayout0(vsRec, vdecl); vsRec._vl = { did: vdecl.id, gen: this.streamGen, dd: vdecl, vl: vl0 }; return vl0;
  }
  vertexLayout0(vsRec, vdecl) {
    // key on (vs, decl, streams strides, swizzle)
    let ck = vsRec.pid + '/' + vdecl.id + '/'; for (const e of vdecl.elems) ck += this.streams[e.stream].stride + ',';
    let vl = this.vlCache && this.vlCache.get(ck); if (vl) return vl;
    const ov = {}, bufs = new Map(), attrsDummy = [];
    for (let L = 0; L < 16; L++) {
      if (!(vsRec.inMask >> L & 1)) continue;
      const e = vdecl.locs.get(L);
      if (!e) { attrsDummy.push({ shaderLocation: L, offset: 0, format: 'float32x4' }); continue; }
      const st = this.streams[e.stream]; if (!st.buf) { attrsDummy.push({ shaderLocation: L, offset: 0, format: 'float32x4' }); continue; }
      if (!bufs.has(e.stream)) bufs.set(e.stream, { stride: st.stride, attributes: [], stream: e.stream });
      bufs.get(e.stream).attributes.push({ shaderLocation: L, offset: e.offset, format: this.vtxFormat(e, L, ov) });
    }
    const layouts = [], slots = [];
    for (const b of bufs.values()) { layouts.push({ arrayStride: b.stride, attributes: b.attributes }); slots.push(b.stream); }
    if (attrsDummy.length) { layouts.push({ arrayStride: 0, attributes: attrsDummy }); slots.push(-1); }
    const constants = ov;
    vl = { key: layouts.map((l) => l.arrayStride + ':' + l.attributes.map((a) => a.shaderLocation + a.format + a.offset).join('.')).join('|') + JSON.stringify(constants), layouts, slots, constants: vsRec.ff ? undefined : constants };
    (this.vlCache || (this.vlCache = new Map())).set(ck, vl);
    return vl;
  }
  bindGroupFor(rec, sig, cbOffsetNeeded, unitBase) {
    if (sig.bg && !sig.fb) return sig.bg;
    const layout = this.bgl(rec, sig.sig); let key = layout.uid + ':';
    for (let i = 0; i < sig.views.length; i += 3) { const s = sig.views[i + 2], tx = sig.views[i + 1]; const sm = this.samplerFor(s, tx); sig.views[i + 2] = sm; key += sig.views[i].uid + '.' + sm.uid + ','; }
    let bg = this.bgCache.get(key);
    if (!bg) {
      const B = this.pack.meta.bindings, entries = [{ binding: B.cbFloat, resource: { buffer: this.ubo, size: CB_WINDOW } }];
      rec.regs.forEach((r, i) => { entries.push({ binding: B.tex0 + r, resource: sig.views[i * 3] }, { binding: B.samp0 + r, resource: sig.views[i * 3 + 2] }); });
      bg = this.dev.createBindGroup({ layout, entries }); this.bgCache.set(key, bg); this.stats.bindGroups++;
    }
    if (!sig.fb) sig.bg = bg;
    return bg;
  }
  applyDynamic(ctx) {
    const t = ctx.t, ps = this.passState, vp = this.vp, r = this.rs;
    const x = Math.min(Math.max(0, vp.x), t.w - 1), y = Math.min(Math.max(0, vp.y), t.h - 1), w = Math.max(1, Math.min(vp.w, t.w - x)), h = Math.max(1, Math.min(vp.h, t.h - y));
    const mn = Math.min(Math.max(vp.minz, 0), 1), mx = Math.min(Math.max(vp.maxz, 0), 1);
    if (ps.vx !== x || ps.vy !== y || ps.vw !== w || ps.vh !== h || ps.vmn !== mn || ps.vmx !== mx) { this.pass.setViewport(x, y, w, h, mn, mx); ps.vx = x; ps.vy = y; ps.vw = w; ps.vh = h; ps.vmn = mn; ps.vmx = mx; }
    let sx = 0, sy = 0, sw = t.w, sh = t.h;
    if (r[174]) { const s = this.scissor; sx = Math.min(Math.max(0, s.l), t.w); sy = Math.min(Math.max(0, s.t), t.h); sw = Math.max(0, Math.min(s.r, t.w) - sx); sh = Math.max(0, Math.min(s.b, t.h) - sy); }
    if (ps.sx !== sx || ps.sy !== sy || ps.sw !== sw || ps.sh !== sh) { this.pass.setScissorRect(sx, sy, sw, sh); ps.sx = sx; ps.sy = sy; ps.sw = sw; ps.sh = sh; }
    if (r[52] && ps.stencilRef !== r[57] && t.hasStencil) { this.pass.setStencilReference(r[57] & 255); ps.stencilRef = r[57]; }
    if (r[27] && ps.bc !== r[193]) { const c = r[193]; this.pass.setBlendConstant({ r: ((c >> 16) & 255) / 255, g: ((c >> 8) & 255) / 255, b: (c & 255) / 255, a: ((c >>> 24) & 255) / 255 }); ps.bc = c; }
    return sw > 0 && sh > 0;
  }
  setupDraw(ctx) {
    if (this.uboOff + 2 * CB_WINDOW > UBO_RING) this.flush();
    if (!this.pass) this.beginPass();
    const ps = this.passState, { vsRec, psRec } = ctx;
    if (ps.pipe !== ctx.p.pipe) { this.pass.setPipeline(ctx.p.pipe); ps.pipe = ctx.p.pipe; }
    if (!this.applyDynamic(ctx)) return false;
    // constants
    const hp = this.dbg.has('nohp') ? [0, 0] : [1 / this.vp.w, -1 / this.vp.h], at = this.rs[15] && !this.dbg.has('noatest') ? [this.rs[25], (this.rs[24] & 255) / 255] : [0, 0];
    if (vsRec.ff) { this.ffConst[0] = this.vp.w; this.ffConst[1] = this.vp.h; }
    const off0 = this.constBlock(vsRec, false, hp), off1 = this.constBlock(psRec, true, at);
    const bg0 = this.bindGroupFor(vsRec, ctx.vsSig, 0, 16), bg1 = this.bindGroupFor(psRec, ctx.psSig, 0, 0);
    if (ps.bg0 !== bg0 || ps.off0 !== off0) { this.pass.setBindGroup(0, bg0, [off0]); ps.bg0 = bg0; ps.off0 = off0; }
    if (ps.bg1 !== bg1 || ps.off1 !== off1) { this.pass.setBindGroup(1, bg1, [off1]); ps.bg1 = bg1; ps.off1 = off1; }
    // textures/targets in use
    for (let i = 1; i < ctx.vsSig.views.length; i += 3) if (ctx.vsSig.views[i]) ctx.vsSig.views[i].serial = this.serial;
    for (let i = 1; i < ctx.psSig.views.length; i += 3) if (ctx.psSig.views[i]) ctx.psSig.views[i].serial = this.serial;
    return true;
  }
  setVB(i, buf, off) { const vb = this.passState.vbs[i]; if (!vb || vb.b !== buf || vb.o !== off) { this.pass.setVertexBuffer(i, buf, off); this.passState.vbs[i] = { b: buf, o: off }; } }
  setVertexBuffers(ctx, vlo, vhi) {      // vlo/vhi: first vertex (relative to the stream start) and one past the last that this draw can fetch
    const vl = ctx.vl;
    for (let i = 0; i < vl.slots.length; i++) {
      const s = vl.slots[i];
      if (s < 0) { this.setVB(i, this.dummyVB, 0); continue; }
      const st = this.streams[s], b = this.buffers.get(st.buf); if (!b) { this.setVB(i, this.dummyVB, 0); continue; }
      const vb = this.passState.vbs[i]; if (!vb || vb.b !== b.gpu || vb.o !== st.offset) { this.pass.setVertexBuffer(i, b.gpu, st.offset); this.passState.vbs[i] = { b: b.gpu, o: st.offset }; }
      this.noteBufUse(b, st.offset + vlo * st.stride, st.offset + vhi * st.stride);
    }
  }
  draw(type, indexed, base, minIdx, numVerts, start, primCount) {
    const ctx = this.prepareDraw(type, indexed); if (!ctx) return;
    const count = this.primCount(type, primCount);
    if (!this.setupDraw(ctx)) return;
    if (indexed) this.setVertexBuffers(ctx, Math.max(0, base + minIdx), base + minIdx + numVerts); else this.setVertexBuffers(ctx, start, start + count);
    if (type === 6 && !indexed) {            // triangle fan -> generated list indices
      this.stats.fans++; const n = primCount, idx = new Uint32Array(n * 3); for (let i = 0; i < n; i++) { idx[3 * i] = 0; idx[3 * i + 1] = i + 1; idx[3 * i + 2] = i + 2; }
      if (this.upOff + idx.byteLength > UP_RING) this.flush();
      this.q.writeBuffer(this.upBuf, this.upOff, idx); this.pass.setIndexBuffer(this.upBuf, 'uint32', this.upOff, idx.byteLength); this.passState.ib = null; this.upOff += (idx.byteLength + 3) & ~3;
      this.pass.drawIndexed(n * 3, 1, 0, start, 0); this.stats.draws++; return;
    }
    if (type === 6) { this.warn('fanidx', 'indexed triangle fan not supported'); this.stats.skipped++; return; }
    if (indexed) {
      const ib = this.buffers.get(this.ib); if (!ib) { this.stats.skipped++; return; }
      if (this.passState.ib !== ib.gpu) { this.pass.setIndexBuffer(ib.gpu, ib.fmt === 101 ? 'uint16' : 'uint32'); this.passState.ib = ib.gpu; } this.noteBufUse(ib, start * (ib.fmt === 101 ? 2 : 4), (start + count) * (ib.fmt === 101 ? 2 : 4));
      this.pass.drawIndexed(count, 1, start, base, 0);
    } else this.pass.draw(count, 1, start, 0);
    this.stats.draws++;
  }
  drawUP(type, primCount, stride, nverts, indexed, ifmt, nidx, blob) {
    // stream 0 comes from the blob: present it as a stream with a non-null buffer id and the right stride while the vertex layout is built
    const saved = this.streams[0]; this.streams[0] = { buf: -1, offset: 0, stride }; this.streamGen++;
    const ctx = this.prepareDraw(type, !!indexed, ifmt); this.streams[0] = saved; this.streamGen++; if (!ctx) return;
    const isz = ifmt === 101 ? 2 : 4, ibBytes = indexed ? nidx * isz : 0, count = this.primCount(type, primCount);
    const vb = blob.subarray(ibBytes), vbLen = (vb.length + 3) & ~3, ibLen = (ibBytes + 3) & ~3;
    if (this.upOff + vbLen + ibLen + 8 > UP_RING) this.flush();
    if (!this.setupDraw(ctx)) return;
    const vOff = this.upOff; this.upOff += vbLen;
    if (vb.length & 3) { const t = new Uint8Array(vbLen); t.set(vb); this.q.writeBuffer(this.upBuf, vOff, t); } else this.q.writeBuffer(this.upBuf, vOff, vb.buffer, vb.byteOffset, vb.length);
    const slots = ctx.vl.slots;
    for (let i = 0; i < slots.length; i++) { if (slots[i] < 0) this.setVB(i, this.dummyVB, 0); else this.setVB(i, this.upBuf, vOff); }
    if (indexed) {
      const iOff = this.upOff; const t = new Uint8Array(ibLen); t.set(blob.subarray(0, ibBytes)); this.q.writeBuffer(this.upBuf, iOff, t); this.upOff += ibLen;
      this.pass.setIndexBuffer(this.upBuf, isz === 2 ? 'uint16' : 'uint32', iOff, ibBytes); this.passState.ib = null; this.pass.drawIndexed(count, 1, 0, 0, 0);
    } else this.pass.draw(count, 1, 0, 0);
    this.stats.draws++;
  }
}
window.D3D9GpuRenderer = D3D9GpuRenderer;
