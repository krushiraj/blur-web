'use strict';
// A Direct3D 9 device implemented on WebGL2. It consumes the command stream produced by the emulation harness
// (port/harness/gfx.c) and mirrors D3D9 device state. Conventions:
//  * every render target (including the D3D backbuffer) is stored top-down: D3D row 0 == GL row 0, so vertex shaders
//    flip clip-space Y; the backbuffer is blitted (flipped) to the canvas on Present.
//  * D3D9 half-pixel offset is applied in the vertex shader; D3D z range [0,1] is remapped to GL's [-1,1].
//  * shaders arrive as GLSL ES 3.00 translated from D3D bytecode; varyings are linked by D3D semantic.
// Built-in vertex shader for draws that use no vertex shader and a POSITIONT (pre-transformed XYZRHW, pixel coordinates) + TEXCOORD0 declaration:
// this is how the game draws Bink movie quads (DrawPrimitiveUP, strip of 4 vertices, pixel shader does YUV->RGB). Output follows D3D clip conventions;
// buildProgram appends the usual depth remap / half-pixel / flip epilogue.
const FF_POSITIONT_VS = `#version 300 es
layout(location = 0) in vec4 v0;
layout(location = 1) in vec4 v1;
out highp vec4 o1;
uniform vec2 u_vpsz;
void main()
{
    o1 = v1;
    gl_Position = vec4(v0.x / u_vpsz.x * 2.0 - 1.0, 1.0 - v0.y / u_vpsz.y * 2.0, v0.z, 1.0);
}
`;
const G = { DESTROY: 95, TEX_CREATE: 20, TEX_UPLOAD: 21, SURF_CREATE: 22, BUF_CREATE: 30, BUF_UPLOAD: 31, SHADER: 40, VDECL: 41, SHADER_REF: 42,
  SET_RS: 50, SET_SS: 51, SET_TEX: 52, SET_VS: 53, SET_PS: 54, SET_VDECL: 55, SET_FVF: 56, SET_STREAM: 57, SET_INDICES: 58, SET_VSC: 59, SET_PSC: 60,
  SET_VIEWPORT: 61, SET_SCISSOR: 62, SET_RT: 63, SET_DS: 64, DRAW: 70, DRAW_UP: 71, CLEAR: 72, PRESENT: 73, STRETCH: 74 };
const FOURCC = (s) => s.charCodeAt(0) | s.charCodeAt(1) << 8 | s.charCodeAt(2) << 16 | s.charCodeAt(3) << 24;
const FMT_DXT1 = FOURCC('DXT1'), FMT_DXT3 = FOURCC('DXT3'), FMT_DXT5 = FOURCC('DXT5'), FMT_INTZ = FOURCC('INTZ'), FMT_DF24 = FOURCC('DF24'), FMT_DF16 = FOURCC('DF16');

class D3D9Renderer {
  constructor(canvas, log) {
    this.dbg = /dbg/.test(location.hash);
    // quality options from the URL hash, e.g. #scale=2&msaa=4&aniso=16&bias=-0.5&srgb=1&hud=1   (all optional; applied at page load because render targets are sized then)
    const hq = {}; for (const kv of location.hash.replace(/^#/, '').split('&')) { const [k, v] = kv.split('='); if (k) hq[k] = v === undefined ? '1' : v; }
    this.q = { scale: Math.min(3, Math.max(0.5, parseFloat(hq.scale || '1') || 1)), msaa: parseInt(hq.msaa || '0') || 0, aniso: parseInt(hq.aniso || '1') || 1, bias: parseFloat(hq.bias || '0') || 0,
      srgb: hq.srgb === undefined ? 1 : parseInt(hq.srgb), hud: hq.hud === undefined ? 0 : parseInt(hq.hud), tri: hq.tri === undefined ? 1 : parseInt(hq.tri) };
    this.canvas = canvas; this.log = log || console.log;
    const gl = this.gl = canvas.getContext('webgl2', { antialias: false, alpha: false, depth: false, stencil: false, preserveDrawingBuffer: false, powerPreference: 'high-performance' });
    if (!gl) throw new Error('WebGL2 not available');
    // Redundant-call filter: the draw path issues >100 GL calls per draw and Chrome's GPU process (ANGLE) pays for every one even when the state is
    // unchanged. All call sites go through these wrappers, so the shadow state is always exact. BLUR 'glcache=0' in the URL hash disables it.
    if (/diag=1/.test(location.hash)) {       // count the GL calls that are really issued (this wrapper sits below the redundant-call filter), per frame, by function
      const cnt = this.glCalls = {}; for (const k of Object.getOwnPropertyNames(WebGL2RenderingContext.prototype)) { if (k === 'constructor' || typeof gl[k] !== 'function') continue; const f = gl[k].bind(gl); gl[k] = (...a) => { cnt[k] = (cnt[k] || 0) + 1; return f(...a); }; }
    }
    if (!/glcache=0/.test(location.hash)) {
      gl.__glc = true; const o = {}; for (const k of ['activeTexture', 'bindTexture', 'bindSampler', 'useProgram', 'bindBuffer', 'bindBufferRange', 'samplerParameteri', 'samplerParameterf', 'uniform1i', 'uniform1f', 'uniform2f',
        'deleteTexture', 'deleteBuffer', 'deleteSampler', 'deleteProgram']) o[k] = gl[k].bind(gl);
      let unit = 0; const tex = new Map(), smp = new Array(32).fill(null); let prog = null, arr = null, ubo = null; const uboR = new Array(16).fill(null);   // tex: (unit<<16|target) -> texture
      // lazy active texture unit: activeTexture only records the wanted unit; it is issued when a bind really changes or before any call that acts on the bound texture
      let want = 0; const syncUnit = () => { if (unit !== want) { unit = want; o.activeTexture(gl.TEXTURE0 + want); } };
      gl.activeTexture = (u) => { want = u - gl.TEXTURE0; };
      gl.bindTexture = (t, x) => { const k = (want << 16) | t; if (tex.get(k) !== x) { tex.set(k, x); syncUnit(); o.bindTexture(t, x); } };
      for (const k of ['texImage2D', 'texImage3D', 'texSubImage2D', 'texSubImage3D', 'texStorage2D', 'texStorage3D', 'compressedTexImage2D', 'compressedTexImage3D', 'compressedTexSubImage2D', 'compressedTexSubImage3D',
        'copyTexImage2D', 'copyTexSubImage2D', 'copyTexSubImage3D', 'texParameteri', 'texParameterf', 'generateMipmap', 'getTexParameter']) { const f = gl[k].bind(gl); gl[k] = (...a) => { syncUnit(); return f(...a); }; }
      gl.bindSampler = (u, x) => { if (smp[u] !== x) { smp[u] = x; o.bindSampler(u, x); } };
      gl.useProgram = (x) => { if (prog !== x) { prog = x; o.useProgram(x); } };
      // element-array binding is vertex-array state: shadow it per VAO (the renderer binds the index buffer before every indexed draw)
      let curVao = null; const vaos = new Set(); let eabNull;
      const eabGet = () => (curVao ? curVao.__eab : eabNull), eabSet = (b) => { if (curVao) curVao.__eab = b; else eabNull = b; };
      const bvao = gl.bindVertexArray.bind(gl); gl.bindVertexArray = (v) => { if (curVao === v) return; curVao = v; if (v) vaos.add(v); bvao(v); };
      gl.bindBuffer = (t, b) => { if (t === gl.ARRAY_BUFFER) { if (arr === b) return; arr = b; } else if (t === gl.UNIFORM_BUFFER) { if (ubo === b) return; ubo = b; } else if (t === gl.ELEMENT_ARRAY_BUFFER) { if (eabGet() === b) return; eabSet(b); } o.bindBuffer(t, b); };
      gl.bindBufferRange = (t, i, b, off, sz) => { if (t === gl.UNIFORM_BUFFER) { const r = uboR[i]; if (r && r.b === b && r.off === off && r.sz === sz) { return; } uboR[i] = { b, off, sz }; ubo = b; } o.bindBufferRange(t, i, b, off, sz); };
      const sp = (name) => (s_, pn, v) => { const c = s_.__c || (s_.__c = new Map()); if (c.get(pn) === v) return; c.set(pn, v); o[name](s_, pn, v); };
      gl.samplerParameteri = sp('samplerParameteri'); gl.samplerParameterf = sp('samplerParameterf');
      gl.uniform1i = (l, v) => { if (l && l.__a === v) return; if (l) l.__a = v; o.uniform1i(l, v); };
      gl.uniform1f = (l, v) => { if (l && l.__a === v) return; if (l) l.__a = v; o.uniform1f(l, v); };
      gl.uniform2f = (l, a, b) => { if (l && l.__a === a && l.__b === b) return; if (l) { l.__a = a; l.__b = b; } o.uniform2f(l, a, b); };
      gl.deleteTexture = (x) => { for (const [k, v] of tex) if (v === x) tex.delete(k); o.deleteTexture(x); };
      gl.deleteBuffer = (b) => { if (arr === b) arr = null; if (ubo === b) ubo = null; if (eabNull === b) eabNull = undefined; for (const v of vaos) if (v.__eab === b) v.__eab = undefined; for (let i = 0; i < 16; i++) if (uboR[i] && uboR[i].b === b) uboR[i] = null; o.deleteBuffer(b); };
      gl.deleteSampler = (x) => { for (let i = 0; i < smp.length; i++) if (smp[i] === x) smp[i] = null; o.deleteSampler(x); };
      gl.deleteProgram = (x) => { if (prog === x) prog = null; o.deleteProgram(x); };
      // fixed-function state: exact shadows of what is set on the context (every call site goes through these wrappers)
      const q = {}; for (const k of ['enable', 'disable', 'viewport', 'scissor', 'depthRange', 'depthFunc', 'depthMask', 'colorMask', 'cullFace', 'frontFace', 'blendFunc', 'blendFuncSeparate', 'blendEquation', 'blendEquationSeparate', 'blendColor',
        'stencilFuncSeparate', 'stencilOpSeparate', 'stencilMaskSeparate', 'stencilMask', 'bindFramebuffer', 'deleteFramebuffer', 'drawBuffers']) q[k] = gl[k].bind(gl);
      const caps = new Map(); gl.enable = (c) => { if (caps.get(c) !== true) { caps.set(c, true); q.enable(c); } }; gl.disable = (c) => { if (caps.get(c) !== false) { caps.set(c, false); q.disable(c); } };
      let vx = NaN, vy, vw, vh; gl.viewport = (x, y, w, h) => { if (vx !== x || vy !== y || vw !== w || vh !== h) { vx = x; vy = y; vw = w; vh = h; q.viewport(x, y, w, h); } };
      let sx = NaN, sy, sw, sh; gl.scissor = (x, y, w, h) => { if (sx !== x || sy !== y || sw !== w || sh !== h) { sx = x; sy = y; sw = w; sh = h; q.scissor(x, y, w, h); } };
      let dn = NaN, df; gl.depthRange = (n, f) => { if (dn !== n || df !== f) { dn = n; df = f; q.depthRange(n, f); } };
      let dfn = -1; gl.depthFunc = (f) => { if (dfn !== f) { dfn = f; q.depthFunc(f); } };
      let dm = -1; gl.depthMask = (m) => { const v = m ? 1 : 0; if (dm !== v) { dm = v; q.depthMask(m); } };
      let cr = -1, cg, cb, ca; gl.colorMask = (r, g, b, a) => { const R = r ? 1 : 0, G = g ? 1 : 0, B = b ? 1 : 0, A = a ? 1 : 0; if (cr !== R || cg !== G || cb !== B || ca !== A) { cr = R; cg = G; cb = B; ca = A; q.colorMask(r, g, b, a); } };
      let cf = -1; gl.cullFace = (m) => { if (cf !== m) { cf = m; q.cullFace(m); } };
      let ff = -1; gl.frontFace = (m) => { if (ff !== m) { ff = m; q.frontFace(m); } };
      let bs = -1, bd, bsa, bda; gl.blendFuncSeparate = (a, b, c, d) => { if (bs !== a || bd !== b || bsa !== c || bda !== d) { bs = a; bd = b; bsa = c; bda = d; q.blendFuncSeparate(a, b, c, d); } };
      gl.blendFunc = (a, b) => gl.blendFuncSeparate(a, b, a, b);
      let be = -1, bea; gl.blendEquationSeparate = (a, b) => { if (be !== a || bea !== b) { be = a; bea = b; q.blendEquationSeparate(a, b); } };
      gl.blendEquation = (e) => gl.blendEquationSeparate(e, e);
      let bcr = NaN, bcg, bcb, bca; gl.blendColor = (r, g, b, a) => { if (bcr !== r || bcg !== g || bcb !== b || bca !== a) { bcr = r; bcg = g; bcb = b; bca = a; q.blendColor(r, g, b, a); } };
      const sf = { [gl.FRONT]: [-1, 0, 0], [gl.BACK]: [-1, 0, 0] }, so = { [gl.FRONT]: [-1, 0, 0], [gl.BACK]: [-1, 0, 0] }, sm = { [gl.FRONT]: [-1], [gl.BACK]: [-1] };
      gl.stencilFuncSeparate = (face, fn, ref, mask) => { const faces = face === gl.FRONT_AND_BACK ? [gl.FRONT, gl.BACK] : [face]; for (const f of faces) { const t = sf[f]; if (t[0] === fn && t[1] === ref && t[2] === mask) continue; t[0] = fn; t[1] = ref; t[2] = mask; q.stencilFuncSeparate(f, fn, ref, mask); } };
      gl.stencilOpSeparate = (face, a, b, c) => { const faces = face === gl.FRONT_AND_BACK ? [gl.FRONT, gl.BACK] : [face]; for (const f of faces) { const t = so[f]; if (t[0] === a && t[1] === b && t[2] === c) continue; t[0] = a; t[1] = b; t[2] = c; q.stencilOpSeparate(f, a, b, c); } };
      gl.stencilMaskSeparate = (face, m) => { const faces = face === gl.FRONT_AND_BACK ? [gl.FRONT, gl.BACK] : [face]; for (const f of faces) { const t = sm[f]; if (t[0] === m) continue; t[0] = m; q.stencilMaskSeparate(f, m); } };
      gl.stencilMask = (m) => gl.stencilMaskSeparate(gl.FRONT_AND_BACK, m);
      let drawFb = null, readFb = null;
      gl.bindFramebuffer = (t, f) => { if (t === gl.FRAMEBUFFER) { if (drawFb === f && readFb === f) return; drawFb = readFb = f; } else if (t === gl.DRAW_FRAMEBUFFER) { if (drawFb === f) return; drawFb = f; } else if (t === gl.READ_FRAMEBUFFER) { if (readFb === f) return; readFb = f; } q.bindFramebuffer(t, f); };
      gl.deleteFramebuffer = (f) => { if (drawFb === f) drawFb = null; if (readFb === f) readFb = null; q.deleteFramebuffer(f); };
      gl.drawBuffers = (bufs) => { const fb = drawFb; if (fb) { const key = bufs.join(); if (fb.__db === key) return; fb.__db = key; } q.drawBuffers(bufs); };
    }
    this.ext = {
      cbf: gl.getExtension('EXT_color_buffer_float'), cbh: gl.getExtension('EXT_color_buffer_half_float'),
      s3tc: gl.getExtension('WEBGL_compressed_texture_s3tc'), aniso: gl.getExtension('EXT_texture_filter_anisotropic'),
      flin: gl.getExtension('OES_texture_float_linear'), par: gl.getExtension('KHR_parallel_shader_compile'),
    };
    this.cache = new Map(); this.cacheShaders = new Map(); this.byHk = new Map(); this.warmByHk = new Map(); this.used = new Set(); this.asyncMode = /(^|[#;&])async\b/.test(location.hash); this.frameDeferred = false; this.holdRun = 0;     // async compile is OPT-IN (#async): in real Chrome and headless Chromium it left draws deferred forever (user run: skipped 19 / deferred 19 / held 17, models rendered wrong); sync is the default until fixed: a draw whose program is still compiling is skipped and the frame is not presented (last complete frame stays)
    this.textures = new Map(); this.surfaces = new Map(); this.buffers = new Map(); this.shaders = new Map(); this.shaders.set(-1, { id: -1, pk: 'i-1', stage: 0, ins: [{ reg: 0, usage: 9, idx: 0, tex: 0 }, { reg: 1, usage: 5, idx: 0, tex: 0 }], outs: [{ reg: 1, usage: 5, idx: 0, tex: 0 }], glsl: FF_POSITIONT_VS }); this.vdecls = new Map();
    this.programs = new Map(); this.fbos = new Map(); this.warned = new Set();
    this.rs = new Uint32Array(256); this.ss = Array.from({ length: 32 }, () => new Uint32Array(16));
    this.tex = new Array(20).fill(0); this.streams = Array.from({ length: 16 }, () => ({ buf: 0, offset: 0, stride: 0 }));
    this.ib = 0; this.vdecl = 0; this.fvf = 0; this.vs = 0; this.ps = 0;
    this.vsc = new Float32Array(256 * 4); this.psc = new Float32Array(224 * 4); this.vscDirty = this.pscDirty = true;
    this.vp = { x: 0, y: 0, w: 1280, h: 720, minz: 0, maxz: 1 }; this.scissor = { l: 0, t: 0, r: 1280, b: 720 };
    this.rt = [{ kind: 3 }, { kind: 3 }, { kind: 3 }, { kind: 3 }]; this.ds = { kind: 3 };
    this.stats = { draws: 0, frameDraws: 0, clears: 0, progs: 0, shaders: 0, skipped: 0, tex: 0, deferred: 0, refs: 0, warmed: 0, held: 0 };
    this.fbWarn = true; this.initDefaults(); this.initGL(); if (this.q.hud) this.initHud();
    // Rear-view mirror at half rate when the page cannot keep up: the mirror passes (576x96 target and the heavy 256x256 helper it is composed from) are ~28% of all draws.
    // #mirror=half always skips them on alternate frames, #mirror=auto does it only while the present-to-present interval stays above ~18 ms, #mirror=full never. Default: auto.
    this.mirrorMode = (/mirror=(half|auto|full)/.exec(location.hash) || [0, 'auto'])[1]; this.mirrorOn = false; this.mirrorTouched = []; this.mirrorIv = 16.7; this.mirrorLast = 0; this.mirrorFrame = 0;
    this.diag = /diag=1/.test(location.hash); this.diagFrames = []; this.rtCount = {};      // #diag=1: per-frame draw counts by render target (see tools/shot/diag.js)
    window.addEventListener('keydown', (e) => { if (e.code === 'F2') { const first = !this.hud; this.initHud(); if (!first) this.hud.c.style.display = this.hud.c.style.display === 'none' ? '' : 'none'; } });
  }
  initDefaults() {
    const r = this.rs; r[7] = 1; r[14] = 1; r[23] = 4; r[22] = 3; r[19] = 2; r[20] = 1; r[168] = r[190] = r[191] = r[192] = 15; r[171] = 1; r[209] = 1; r[207] = 2; r[208] = 1;
    r[56] = 8; r[53] = r[54] = r[55] = 1; r[58] = r[59] = 0xffffffff; r[25] = 8; r[193] = 0xffffffff; r[186] = r[187] = r[188] = 1; r[189] = 8;
    for (const s of this.ss) { s[1] = s[2] = s[3] = 1; s[5] = s[6] = 1; s[10] = 1; }
  }
  initGL() {
    const gl = this.gl;
    this.vao = gl.createVertexArray(); gl.bindVertexArray(this.vao);
    this.upVB = gl.createBuffer(); this.upIB = gl.createBuffer();
    this.vsUBO = gl.createBuffer(); gl.bindBuffer(gl.UNIFORM_BUFFER, this.vsUBO); gl.bufferData(gl.UNIFORM_BUFFER, 256 * 16, gl.DYNAMIC_DRAW);
    this.psUBO = gl.createBuffer(); gl.bindBuffer(gl.UNIFORM_BUFFER, this.psUBO); gl.bufferData(gl.UNIFORM_BUFFER, 256 * 16, gl.DYNAMIC_DRAW);
    this.samplers = Array.from({ length: 20 }, () => gl.createSampler());
    const mk = (target, face) => { const t = gl.createTexture(); gl.bindTexture(target, t); const px = new Uint8Array([0, 0, 0, 255]);
      if (target === gl.TEXTURE_2D) gl.texImage2D(target, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, px);
      else if (target === gl.TEXTURE_CUBE_MAP) for (let f = 0; f < 6; f++) gl.texImage2D(gl.TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, px);
      else gl.texImage3D(target, 0, gl.RGBA, 1, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, px);
      gl.texParameteri(target, gl.TEXTURE_MIN_FILTER, gl.NEAREST); gl.texParameteri(target, gl.TEXTURE_MAG_FILTER, gl.NEAREST); return t; };
    this.dummy = { [gl.TEXTURE_2D]: mk(gl.TEXTURE_2D), [gl.TEXTURE_CUBE_MAP]: mk(gl.TEXTURE_CUBE_MAP), [gl.TEXTURE_3D]: mk(gl.TEXTURE_3D) };
    this.bb = null;                                                  // backbuffer surface: created by SURF_CREATE kind 0
    this.blitFbo = gl.createFramebuffer(); this.blitFbo2 = gl.createFramebuffer();
    this.maxAniso = this.ext.aniso ? gl.getParameter(this.ext.aniso.MAX_TEXTURE_MAX_ANISOTROPY_EXT) : 1;
  }
  // Render scale: the backbuffer and every render target whose size is the backbuffer size divided by an integer (full-res scene, half/quarter-res post buffers)
  // are allocated S times larger; viewport/scissor/clear/stretch rectangles are multiplied by the scale of the target they hit. Everything else (shadow maps, 256x256
  // probes ...) keeps its size. D3D-space coordinates (game logic, shader constants) are unchanged.
  rtScale(w, h) {
    if (!this.bb || this.q.scale === 1) return 1; const bw = this.bb.lw, bh = this.bb.lh; if (w > bw || h > bh) return 1;
    const rx = Math.round(bw / w); return rx >= 1 && rx <= 16 && Math.abs(bw / rx - w) < 17 && Math.abs(bh / rx - h) < 17 && h * 3 > bh / rx * 2 ? this.q.scale : 1;
  }
  scaled(v, sc) { return sc === 1 ? v : Math.round(v * sc); }
  warn(k, msg) { if (!this.warned.has(k)) { this.warned.add(k); this.log('[renderer] ' + msg); } }
  // frame-time HUD: a 2D canvas stacked over the game canvas; #hud=1 (or press F2). Shows interval between presents, our JS handling time per frame, and GPU time if the timer query extension exists.
  initHud() {
    if (this.hud) return; const c = document.createElement('canvas'); c.width = 420; c.height = 150; c.style.cssText = 'position:fixed;right:10px;top:44px;pointer-events:none;z-index:9;background:rgba(0,0,0,.55);border:1px solid #333';
    document.body.appendChild(c); this.hud = { c, g: c.getContext('2d'), iv: new Float32Array(180), cpu: new Float32Array(180), n: 0, last: 0, busy: 0 };
    this.gpuExt = this.gl.getExtension('EXT_disjoint_timer_query_webgl2'); this.gpuMs = 0; this.gpuQ = [];
  }
  frameDone() {
    const now = performance.now(), gl = this.gl;
    if (!this.ivs) { this.ivs = new Float32Array(600); this.ivn = 0; this.ivLast = 0; this.gpuExt = this.gpuExt || gl.getExtension('EXT_disjoint_timer_query_webgl2'); this.gpuPend = []; this.gpuMs = 0; }
    if (this.ivLast) { this.ivs[this.ivn % 600] = now - this.ivLast; this.ivn++; } this.ivLast = now;
    const X = this.gpuExt;                                              // GPU frame time: one TIME_ELAPSED query per presented frame, read back a few frames later
    if (X) {
      if (this.gpuCur) { gl.endQuery(X.TIME_ELAPSED_EXT); this.gpuPend.push(this.gpuCur); this.gpuCur = null; }
      const dis = gl.getParameter(X.GPU_DISJOINT_EXT);
      while (this.gpuPend.length && gl.getQueryParameter(this.gpuPend[0], gl.QUERY_RESULT_AVAILABLE)) { const q = this.gpuPend.shift(); if (!dis) { const ms = gl.getQueryParameter(q, gl.QUERY_RESULT) / 1e6; this.gpuMs = this.gpuMs ? this.gpuMs * 0.9 + ms * 0.1 : ms; this.gpuMax = Math.max(this.gpuMax || 0, ms); } gl.deleteQuery(q); }
      if (this.gpuPend.length < 6) { this.gpuCur = gl.createQuery(); gl.beginQuery(X.TIME_ELAPSED_EXT, this.gpuCur); }
    }
    if (this.hud) { const h = this.hud; h.n = this.ivn; h.iv = this.ivs; h.busy = 0; if (this.ivn % 3 === 0) this.drawHud(); }
  }
  // summary of the last N presented frames (for benchmarking from puppeteer): fps, avg/p99/max interval, GPU ms
  perf(n = 120) {
    const N = Math.min(n, this.ivn || 0, 600), v = []; for (let i = 0; i < N; i++) v.push(this.ivs[(this.ivn - N + i) % 600]); const s = v.slice().sort((a, b) => a - b), avg = v.reduce((a, b) => a + b, 0) / (N || 1);
    return { frames: N, fps: N ? 1000 / avg : 0, avg, p50: s[Math.floor(N * 0.5)], p99: s[Math.floor(N * 0.99)], max: s[N - 1], gpuMs: this.gpuMs, gpuMax: this.gpuMax };
  }
  drawHud() {
    const h = this.hud, g = h.g, N = Math.min(h.n, 180); g.clearRect(0, 0, 420, 150); g.font = '11px ui-monospace,Menlo,monospace';
    let sum = 0, mx = 0; const v = []; for (let i = 0; i < N; i++) { const x = h.iv[(h.n - N + i) % 600]; v.push(x); sum += x; mx = Math.max(mx, x); }
    const avg = sum / (N || 1), s = v.slice().sort((a, b) => a - b), p99 = s[Math.floor(s.length * 0.99)] || 0, y0 = 150, k = 3;
    g.fillStyle = '#2a4'; g.fillRect(0, y0 - 16.67 * k, 420, 1); g.fillStyle = '#fff';
    for (let i = 0; i < N; i++) { const x = v[i]; g.fillStyle = x > 20 ? '#e55' : x > 17.5 ? '#ea3' : '#5c8'; g.fillRect(i * 2, y0 - Math.min(x, 48) * k, 2, Math.min(x, 48) * k); }
    g.fillStyle = '#fff'; g.fillText(`${(1000 / avg).toFixed(1)} fps  avg ${avg.toFixed(1)} ms  p99 ${p99.toFixed(1)}  max ${mx.toFixed(1)}`, 4, 12);
    g.fillText(`scale ${this.q.scale}x  msaa ${this.q.msaa || 'off'}  aniso ${this.q.aniso}x  bias ${this.q.bias}  draws ${this.stats.frameDraws}  gpu ${this.gpuMs ? this.gpuMs.toFixed(1) + ' ms' : 'n/a'}`, 4, 26);
  }
  statLine() { const s = this.stats, fr = this.jsFrames || 0, js = fr ? (this.jsBusy / fr).toFixed(1) : '-', mp = fr ? Math.round(this.jsMsgs / fr) : '-'; this.jsBusy = this.jsFrames = this.jsMsgs = 0;
    return `[b4 glcache ${this.gl.__glc ? 'on' : 'off'}] JS ${js} ms/frame  msgs/frame ${mp}  draws/frame ${s.frameDraws}  progs ${s.progs}  shaders ${s.shaders}  tex ${this.textures.size}  skipped ${s.skipped}  deferred ${s.deferred}  held ${s.held}`; }

  // ---- message pump ---------------------------------------------------------------------------------------------------------------------------
  feed(ab) {
    const t0 = performance.now(); let r; try { r = this.feed0(ab); return r; } finally { const d = performance.now() - t0; if (this.hud) this.hud.busy += d; this.jsBusy = (this.jsBusy || 0) + d; if (r) { this.jsFrames = (this.jsFrames || 0) + r.presents; this.jsMsgs = (this.jsMsgs || 0) + r.messages; } }
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
      case 98: if (this.hc) this.hc.tagged(this, ab, dv, p, len); break;       // content-hash cache (hcache.js): tagged upload / reference to a cached upload
      case 96: if (this.hc) this.hc.ref(this, ab, dv, p, len); break;
      case G.TEX_CREATE: this.rtDirty = true; this.texCreate(u(0), u(1), u(2), u(3), u(4), u(5), u(6), u(7)); break;
      case G.TEX_UPLOAD: this.texUpload(u(0), u(1), u(2), u(3), u(4), u(5), u(6), u(7), new Uint8Array(ab, p + 32, len - 32)); break;       // consumed synchronously by texSubImage: no copy
      case G.SURF_CREATE: this.rtDirty = true; this.surfCreate(u(0), u(1), u(2), u(3), u(4), u(5)); break;
      case G.BUF_CREATE: this.bufCreate(u(0), u(1), u(2), u(3), u(4)); break;
      case G.BUF_UPLOAD: { const b = this.buffers.get(u(0)); if (b) { const gl = this.gl, tg = b.kind === 1 ? gl.ELEMENT_ARRAY_BUFFER : gl.ARRAY_BUFFER; gl.bindBuffer(tg, b.buf); gl.bufferSubData(tg, u(1), new Uint8Array(ab, p + 8, len - 8)); } break; }
      case G.SHADER: this.shaderCreate(ab, p, len, u); break;
      case G.SHADER_REF: this.shaderRef(u(0), u(1), u(2), u(3), u(4)); break;
      case G.VDECL: this.vdeclCreate(u(0), u(1), new DataView(ab, p + 8, len - 8)); break;
      case G.SET_RS: if (u(0) < 256) this.rs[u(0)] = u(1); break;
      case G.SET_SS: if (u(0) < 32 && u(1) < 16) { this.ss[u(0)][u(1)] = u(2); this.samplerDirty = true; } break;
      case G.SET_TEX: this.tex[u(0)] = u(1); break;
      case G.SET_VS: this.vs = u(0); break; case G.SET_PS: this.ps = u(0); break;
      case G.SET_VDECL: this.vdecl = u(0); break; case G.SET_FVF: this.fvf = u(0); break;
      case G.SET_STREAM: this.streams[u(0)] = { buf: u(1), offset: u(2), stride: u(3) }; break;
      case G.SET_INDICES: this.ib = u(0); break;
      case G.SET_VSC: { const n = u(1); this.vsc.set(((p + 8) & 3) ? new Float32Array(ab.slice(p + 8, p + 8 + 16 * n)) : new Float32Array(ab, p + 8, 4 * n), u(0) * 4); this.vscDirty = true; break; }
      case G.SET_PSC: { const n = u(1); this.psc.set(((p + 8) & 3) ? new Float32Array(ab.slice(p + 8, p + 8 + 16 * n)) : new Float32Array(ab, p + 8, 4 * n), u(0) * 4); this.pscDirty = true; break; }
      case G.SET_VIEWPORT: this.vp = { x: u(0), y: u(1), w: u(2), h: u(3), minz: f(4), maxz: f(5) }; break;
      case G.SET_SCISSOR: this.scissor = { l: u(0), t: u(1), r: u(2), b: u(3) }; break;
      case G.SET_RT: this.rtDirty = true; this.rt[u(0)] = { kind: u(1), id: u(2), level: u(3), face: u(4) }; break;
      case G.SET_DS: this.rtDirty = true; this.ds = { kind: u(0), id: u(1), level: u(2), face: u(3) }; break;
      case G.DRAW: this.draw(u(0), u(1), u(2), u(3), u(4), u(5), u(6)); break;
      case G.DRAW_UP: this.drawUP(u(0), u(1), u(2), u(3), u(4), u(5), u(6), new Uint8Array(ab, p + 28, len - 28)); break;
      case G.CLEAR: this.clear(u(0), u(1), f(2), u(3), u(4), new Int32Array(ab.slice(p + 20, p + 20 + 16 * u(4)))); break;
      case 80: this.setGamma(new Uint16Array(ab.slice(p + 4, p + 4 + 1536))); break;
      case 81: this.readbackTexel(u(0), u(1), u(2), u(3), u(4)); break;
      case 85: this.auditMsg(u, new Uint8Array(ab, p + 44, len - 44)); break;
      case 86: this.auditRun(u(0)); break;
      case G.PRESENT: this.present(); return true;
      case G.STRETCH: this.stretch(Array.from({ length: 19 }, (_, i) => u(i))); break;
      case 95: this.rtDirty = true; this.destroyRes(u(0), u(1)); break;     // host freed the D3D object: release the GL texture / buffer (kind 0 texture, 1 vertex/index buffer)
      case 90: if (this.onAudio) this.onAudio(u(0), u(1), u(2), ab.slice(p + 12, p + len)); break;      // host-mixed PCM: rate, channels, frames, int16 data
      default: this.warn('type' + type, 'unknown message type ' + type);
    }
    return false;
  }

  // ---- formats ----------------------------------------------------------------------------------------------------------------------------------------
  fmtInfo(f) {
    const gl = this.gl, E = this.ext;
    switch (f) {
      case 21: case 22: return { internal: gl.RGBA8, format: gl.RGBA, type: gl.UNSIGNED_BYTE, conv: f === 21 ? 'bgra' : 'bgrx', rt: true };
      case 23: return { internal: gl.RGB565, format: gl.RGB, type: gl.UNSIGNED_SHORT_5_6_5, rt: true };
      case 25: return { internal: gl.RGBA8, format: gl.RGBA, type: gl.UNSIGNED_BYTE, conv: 'a1r5g5b5' };
      case 26: return { internal: gl.RGBA8, format: gl.RGBA, type: gl.UNSIGNED_BYTE, conv: 'a4r4g4b4' };
      case 28: return { internal: gl.RGBA8, format: gl.RGBA, type: gl.UNSIGNED_BYTE, conv: 'a8' };
      case 50: return { internal: gl.RGBA8, format: gl.RGBA, type: gl.UNSIGNED_BYTE, conv: 'l8' };
      case 51: return { internal: gl.RGBA8, format: gl.RGBA, type: gl.UNSIGNED_BYTE, conv: 'a8l8' };
      case 60: return { internal: gl.RGBA8_SNORM, format: gl.RGBA, type: gl.BYTE, conv: 'v8u8' };           // D3D samples V8U8 as (u, v, 1, 1); RG8_SNORM would give b = 0
      case 63: return { internal: gl.RGBA8_SNORM, format: gl.RGBA, type: gl.BYTE };
      case 34: return { internal: gl.RGBA16F, format: gl.RGBA, type: gl.HALF_FLOAT, conv: 'rg16' };           // G16R16 (unorm): converted to half floats, b = a = 1 like D3D
      case 36: return { internal: gl.RGBA8, format: gl.RGBA, type: gl.UNSIGNED_BYTE, conv: 'rgba16' };
      case 111: return { internal: gl.R16F, format: gl.RED, type: gl.HALF_FLOAT, rt: !!(E.cbh || E.cbf), float: true };
      case 112: return { internal: gl.RG16F, format: gl.RG, type: gl.HALF_FLOAT, rt: !!(E.cbh || E.cbf), float: true };
      case 113: return { internal: gl.RGBA16F, format: gl.RGBA, type: gl.HALF_FLOAT, rt: !!(E.cbh || E.cbf), float: true };
      case 114: return { internal: gl.R32F, format: gl.RED, type: gl.FLOAT, rt: !!E.cbf, float: true, f32: true };
      case 115: return { internal: gl.RG32F, format: gl.RG, type: gl.FLOAT, rt: !!E.cbf, float: true, f32: true };
      case 116: return { internal: gl.RGBA32F, format: gl.RGBA, type: gl.FLOAT, rt: !!E.cbf, float: true, f32: true };
      case 35: return { internal: gl.RGB10_A2, format: gl.RGBA, type: gl.UNSIGNED_INT_2_10_10_10_REV, conv: 'a2r10g10b10', rt: true };
      case FMT_DXT1: return { compressed: true, internal: E.s3tc && E.s3tc.COMPRESSED_RGBA_S3TC_DXT1_EXT, block: 8 };
      case FMT_DXT3: return { compressed: true, internal: E.s3tc && E.s3tc.COMPRESSED_RGBA_S3TC_DXT3_EXT, block: 16 };
      case FMT_DXT5: return { compressed: true, internal: E.s3tc && E.s3tc.COMPRESSED_RGBA_S3TC_DXT5_EXT, block: 16 };
      case 80: return { internal: gl.DEPTH_COMPONENT16, format: gl.DEPTH_COMPONENT, type: gl.UNSIGNED_SHORT, depth: true };
      case 77: return { internal: gl.DEPTH_COMPONENT24, format: gl.DEPTH_COMPONENT, type: gl.UNSIGNED_INT, depth: true };
      case 71: case 82: return { internal: gl.DEPTH_COMPONENT32F, format: gl.DEPTH_COMPONENT, type: gl.FLOAT, depth: true };
      case 75: case 83: case FMT_INTZ: case FMT_DF24: return { internal: gl.DEPTH24_STENCIL8, format: gl.DEPTH_STENCIL, type: gl.UNSIGNED_INT_24_8, depth: true, stencil: true };
      case FMT_DF16: return { internal: gl.DEPTH_COMPONENT16, format: gl.DEPTH_COMPONENT, type: gl.UNSIGNED_SHORT, depth: true };
    }
    this.warn('fmt' + f, `unsupported D3D format ${f} (0x${f.toString(16)}); using RGBA8`);
    return { internal: gl.RGBA8, format: gl.RGBA, type: gl.UNSIGNED_BYTE, conv: 'raw32', rt: true };
  }

  // ---- textures & surfaces -------------------------------------------------------------------------------------------------------------------------
  destroyRes(kind, id) {
    const gl = this.gl;
    if (kind === 0) { const t = this.textures.get(id); if (!t || t.isRT || t.isDS) return; gl.deleteTexture(t.tex); if (t.snap) gl.deleteTexture(t.snap); this.textures.delete(id); this.stats.freed = (this.stats.freed || 0) + 1; }
    else { const b = this.buffers.get(id); if (!b) return; gl.deleteBuffer(b.buf); this.buffers.delete(id); this.stats.freed = (this.stats.freed || 0) + 1; }
  }
  texCreate(id, kind, w, h, d, levels, fmt, usage) {
    const gl = this.gl, info = this.fmtInfo(fmt), target = kind === 1 ? gl.TEXTURE_CUBE_MAP : kind === 2 ? gl.TEXTURE_3D : gl.TEXTURE_2D;
    const t = { id, kind, w, h, d, levels, fmt, usage, info, target, tex: gl.createTexture(), isRT: !!(usage & 1), isDS: !!(usage & 2) };
    t.sc = kind === 0 && (usage & 3) ? this.rtScale(w, h) : 1; t.sw = this.scaled(w, t.sc); t.sh = this.scaled(h, t.sc);
    gl.bindTexture(target, t.tex);
    if (info.compressed && !info.internal) { this.warn('s3tc', 'S3TC unsupported by this browser: DXT textures will be black'); t.info = { internal: gl.RGBA8, format: gl.RGBA, type: gl.UNSIGNED_BYTE, conv: 'none' }; t.sw = true; }
    const ii = t.info;
    if (kind === 2) gl.texStorage3D(target, levels, ii.internal, w, h, d); else gl.texStorage2D(target, levels, ii.internal, t.sw, t.sh);
    gl.texParameteri(target, gl.TEXTURE_MAX_LEVEL, levels - 1);
    this.textures.set(id, t); this.stats.tex++;
  }
  convert(fmt, info, w, h, d, data) {            // D3D memory layout -> GL upload array
    const n = w * h * d;
    switch (info.conv) {
      case 'bgra': case 'bgrx': { const s = new Uint32Array(data.buffer, 0, n), o = new Uint32Array(n), x = info.conv === 'bgrx';
        for (let i = 0; i < n; i++) { const v = s[i]; o[i] = (v & 0xff00ff00) | ((v & 0xff) << 16) | ((v >>> 16) & 0xff) | (x ? 0xff000000 : 0); } return new Uint8Array(o.buffer); }
      case 'a1r5g5b5': { const s = new Uint16Array(data.buffer, 0, n), o = new Uint8Array(n * 4);
        for (let i = 0; i < n; i++) { const v = s[i]; o[4 * i] = ((v >> 10) & 31) * 255 / 31; o[4 * i + 1] = ((v >> 5) & 31) * 255 / 31; o[4 * i + 2] = (v & 31) * 255 / 31; o[4 * i + 3] = (v >> 15) ? 255 : 0; } return o; }
      case 'a4r4g4b4': { const s = new Uint16Array(data.buffer, 0, n), o = new Uint8Array(n * 4);
        for (let i = 0; i < n; i++) { const v = s[i]; o[4 * i] = ((v >> 8) & 15) * 17; o[4 * i + 1] = ((v >> 4) & 15) * 17; o[4 * i + 2] = (v & 15) * 17; o[4 * i + 3] = ((v >> 12) & 15) * 17; } return o; }
      case 'a8': { const o = new Uint8Array(n * 4); for (let i = 0; i < n; i++) o[4 * i + 3] = data[i]; return o; }
      case 'l8': { const o = new Uint8Array(n * 4); for (let i = 0; i < n; i++) { const l = data[i]; o[4 * i] = l; o[4 * i + 1] = l; o[4 * i + 2] = l; o[4 * i + 3] = 255; } return o; }
      case 'a8l8': { const o = new Uint8Array(n * 4); for (let i = 0; i < n; i++) { const l = data[2 * i], a = data[2 * i + 1]; o[4 * i] = l; o[4 * i + 1] = l; o[4 * i + 2] = l; o[4 * i + 3] = a; } return o; }
      case 'rgba16': { const s = new Uint16Array(data.buffer, 0, n * 4), o = new Uint8Array(n * 4); for (let i = 0; i < n; i++) { o[4 * i] = s[4 * i] >> 8; o[4 * i + 1] = s[4 * i + 1] >> 8; o[4 * i + 2] = s[4 * i + 2] >> 8; o[4 * i + 3] = s[4 * i + 3] >> 8; } return o; }
      case 'rg16': { const s = new Uint16Array(data.buffer, data.byteOffset, n * 2), o = new Uint16Array(n * 4), F = this.f2h || (this.f2h = new Float32Array(1)), U = new Uint32Array(F.buffer);
        const h = (x) => { F[0] = x; const b = U[0], e = ((b >> 23) & 255) - 112; return e <= 0 ? 0 : e >= 31 ? 0x7c00 : ((b >> 16) & 0x8000) | (e << 10) | ((b >> 13) & 1023); };
        const lut = this.u16h || (this.u16h = (() => { const t = new Uint16Array(65536); for (let i = 0; i < 65536; i++) t[i] = h(i / 65535); return t; })());
        for (let i = 0; i < n; i++) { o[4 * i] = lut[s[2 * i]]; o[4 * i + 1] = lut[s[2 * i + 1]]; o[4 * i + 2] = 0x3c00; o[4 * i + 3] = 0x3c00; } return o; }
      case 'v8u8': { const o = new Int8Array(n * 4), s = new Int8Array(data.buffer, data.byteOffset, n * 2); for (let i = 0; i < n; i++) { o[4 * i] = s[2 * i]; o[4 * i + 1] = s[2 * i + 1]; o[4 * i + 2] = 127; o[4 * i + 3] = 127; } return o; }
      case 'a2r10g10b10': { const s = new Uint32Array(data.buffer, 0, n), o = new Uint32Array(n);
        for (let i = 0; i < n; i++) { const v = s[i], a = v >>> 30, r = (v >>> 20) & 1023, g = (v >>> 10) & 1023, b = v & 1023; o[i] = (a << 30) | (b << 20) | (g << 10) | r; } return o; }
      case 'raw32': case 'none': return new Uint8Array(n * 4);
    }
    if (info.type === this.gl.HALF_FLOAT) return new Uint16Array(data.buffer, 0, data.byteLength >> 1);
    if (info.type === this.gl.FLOAT) return new Float32Array(data.buffer, 0, data.byteLength >> 2);
    if (info.type === this.gl.BYTE) return new Int8Array(data.buffer, 0, data.byteLength);
    if (info.type === this.gl.UNSIGNED_SHORT_5_6_5 || info.type === this.gl.UNSIGNED_SHORT) return new Uint16Array(data.buffer, 0, data.byteLength >> 1);
    return data;
  }
  texUpload(id, face, level, w, h, d, fmt, pitch, data) {
    const t = this.textures.get(id), gl = this.gl; if (!t || t.isRT || t.isDS) return;
    { let nz = 0; const st2 = Math.max(1, data.length >> 8 & ~0); for (let i = 0; i < data.length; i += st2) if (data[i]) nz++; t.up = (t.up || 0) + 1; t.nz = nz / Math.ceil(data.length / st2); t.upl = level; }
    gl.bindTexture(t.target, t.tex); gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    const tgt = t.kind === 1 ? gl.TEXTURE_CUBE_MAP_POSITIVE_X + face : t.target;
    if (t.info.compressed) { if (t.info.internal) { if (t.kind === 2) gl.compressedTexSubImage3D(t.target, level, 0, 0, 0, w, h, d, t.info.internal, data); else gl.compressedTexSubImage2D(tgt, level, 0, 0, w, h, t.info.internal, data); } return; }
    const arr = this.convert(fmt, t.info, w, h, d, data.byteOffset ? data.slice() : data);        // convert() views data.buffer from offset 0: a view into the message buffer (no-copy upload path) must be copied first
    if (t.kind === 2) gl.texSubImage3D(t.target, level, 0, 0, 0, w, h, d, t.info.format, t.info.type, arr);
    else gl.texSubImage2D(tgt, level, 0, 0, w, h, t.info.format, t.info.type, arr);
  }
  surfCreate(id, w, h, fmt, kind, ms) {               // kind 0 backbuffer, 1 render target, 2 depth-stencil
    const gl = this.gl, info = this.fmtInfo(fmt), s = { id, w, h, lw: w, lh: h, fmt, kind, ms, info, rb: gl.createRenderbuffer(), tex: null, sc: 1 };
    gl.bindRenderbuffer(gl.RENDERBUFFER, s.rb);
    if (kind === 0) {                                                     // backbuffer is a texture so it can be blitted/sampled; depth comes from the DS surface
      s.sc = this.q.scale; s.w = this.scaled(w, s.sc); s.h = this.scaled(h, s.sc);
      s.tex = gl.createTexture(); gl.bindTexture(gl.TEXTURE_2D, s.tex); gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA8, s.w, s.h); this.bb = s; this.canvas.width = s.w; this.canvas.height = s.h;
    } else {
      s.sc = this.rtScale(w, h); s.w = this.scaled(w, s.sc); s.h = this.scaled(h, s.sc);
      if (ms > 1) gl.renderbufferStorageMultisample(gl.RENDERBUFFER, Math.min(ms, gl.getParameter(gl.MAX_SAMPLES)), info.internal, s.w, s.h);
      else gl.renderbufferStorage(gl.RENDERBUFFER, info.internal, s.w, s.h);
    }
    this.surfaces.set(id, s);
  }

  // ---- buffers / declarations / shaders ---------------------------------------------------------------------------------------------------------------
  bufCreate(id, kind, size, fmt, usage) { const gl = this.gl, buf = gl.createBuffer(), tg = kind === 1 ? gl.ELEMENT_ARRAY_BUFFER : gl.ARRAY_BUFFER; gl.bindBuffer(tg, buf); gl.bufferData(tg, size, gl.DYNAMIC_DRAW); this.buffers.set(id, { id, kind, size, fmt, buf }); }
  vdeclCreate(id, n, dv) {
    const elems = []; for (let i = 0; i < n; i++) { const o = i * 8, stream = dv.getUint16(o, true); if (stream === 0xff) break;
      elems.push({ stream, offset: dv.getUint16(o + 2, true), type: dv.getUint8(o + 4), method: dv.getUint8(o + 5), usage: dv.getUint8(o + 6), idx: dv.getUint8(o + 7) }); }
    this.vdecls.set(id, { id, elems });
  }
  shaderCreate(ab, p, len, u) {
    const id = u(0), stage = u(1), glslLen = u(2), nin = u(3), nout = u(4), nsamp = u(5);
    this.shaders.set(id, this.decodeShader(id, stage, glslLen, nin, nout, nsamp, new Uint8Array(ab, p + 24, len - 24))); this.stats.shaders++;
  }
  decodeShader(id, stage, glslLen, nin, nout, nsamp, b) {              // b: IO tables followed by the GLSL text (same layout as a cache entry body)
    const tab = (o, n) => Array.from({ length: n }, (_, i) => ({ reg: b[o + 4 * i], usage: b[o + 4 * i + 1], idx: b[o + 4 * i + 2], tex: b[o + 4 * i + 3] }));
    const ins = tab(0, nin), outs = tab(4 * nin, nout), samps = tab(4 * (nin + nout), nsamp), gOff = 4 * (nin + nout + nsamp);
    return { id, pk: 'i' + id, stage, ins, outs, samps, glsl: new TextDecoder().decode(b.subarray(gOff, gOff + glslLen)) };
  }
  // ---- offline shader cache (shaders.bin, see shcache.h): D3D9 bytecode hash -> pre-translated GLSL + IO tables ------------------------------------
  async loadShaderCache(url) {
    let res = await fetch(url); if (!res.ok) throw new Error(`shader cache ${url}: HTTP ${res.status}`);
    const t0 = performance.now(); let ab;
    if (/\.gz$/.test(url) && res.headers.get('content-encoding') !== 'gzip') ab = await new Response(res.body.pipeThrough(new DecompressionStream('gzip'))).arrayBuffer(); else ab = await res.arrayBuffer();
    const dv = new DataView(ab), u8 = new Uint8Array(ab);
    if (dv.getUint32(0, true) !== 0x31435342) throw new Error('bad shader cache magic');
    const n = dv.getUint32(4, true); let p = 8;
    for (let i = 0; i < n; i++) {
      const lo = dv.getUint32(p, true), hi = dv.getUint32(p + 4, true), clen = dv.getUint32(p + 8, true), stage = u8[p + 12], nin = u8[p + 13], nout = u8[p + 14], nsamp = u8[p + 15], glen = dv.getUint32(p + 16, true);
      const body = p + 20, blen = 4 * (nin + nout + nsamp) + glen;
      this.cache.set(`${lo}_${hi}_${clen}`, { hk: `${lo}_${hi}_${clen}`, stage, nin, nout, nsamp, glen, off: body, len: blen });
      p = (body + blen + 3) & ~3;
    }
    this.cacheBuf = u8; this.log(`[renderer] shader cache: ${n} entries, ${(ab.byteLength / 1048576).toFixed(2)} MB, parsed in ${(performance.now() - t0).toFixed(0)} ms`);
  }
  async loadWarm(url) {                                               // list of [vsKey, psKey, colorKey] program variants seen in earlier runs: compiled as soon as both shaders exist
    const res = await fetch(url); if (!res.ok) return; const list = await res.json(); this.warmCount = list.length;
    for (const w of list) for (const k of [w[0], w[1]]) { let a = this.warmByHk.get(k); if (!a) this.warmByHk.set(k, a = []); a.push(w); }
  }
  shaderRef(id, stage, lo, hi, clen) {
    const hk = `${lo}_${hi}_${clen}`; let sh = this.cacheShaders.get(hk);
    if (!sh) {
      const e = this.cache.get(hk); if (!e) { this.warn('sref' + hk, `shader #${id}: key ${hk} not in cache`); return; }
      sh = this.decodeShader(id, e.stage, e.glen, e.nin, e.nout, e.nsamp, this.cacheBuf.subarray(e.off, e.off + e.len)); sh.pk = 'h' + hk; sh.hk = hk;
      this.cacheShaders.set(hk, sh); this.byHk.set(hk, sh); this.stats.shaders++;
    }
    this.shaders.set(id, sh); this.stats.refs++;
    const ws = this.warmByHk.get(hk);
    if (ws) for (const w of ws) { const vs = this.byHk.get(w[0]), ps = this.byHk.get(w[1]); if (vs && ps && !w.done) { w.done = true; this.programFor(vs, ps, w[2], false); this.stats.warmed++; } }
  }
  exportWarm() { return JSON.stringify([...this.used].map((k) => k.split('|')), null, 0); }
  vname(e) { return `vary_${e.usage}_${e.idx}`; }
  buildProgram(vs, ps, colorRegs) {
    const gl = this.gl, vsId = vs.id, psId = ps.id;
    let vsrc = vs.glsl, psrc = ps.glsl;
    const prec = (s) => s.replace('precision mediump float;', 'precision highp float;');
    vsrc = prec(vsrc); psrc = prec(psrc);
    vsrc = vsrc.replace(/SPIRV_Cross_Combinedt(\d+)s(\d+)/g, 'VTX_Combinedt$1s$2');          // vertex-stage samplers (D3D sampler 257..260 -> slots 16..19) must not alias pixel samplers of the same index
    // link varyings by semantic
    // component offsets of trimmed varyings (`//COMP in v2 2` / `//COMP out o6 0` lines emitted by shadercvt.c): vkd3d declares only the used components, e.g. a pixel input read as v2.zw becomes `vec2 v2` at component 2
    const compOf = (src, kind, pre, list) => { const m = new Map(); for (const mm of src.matchAll(new RegExp(`//COMP ${kind} ${pre}(\\d+) (\\d)`, 'g'))) { const e = list.find((x) => x.reg === +mm[1]); if (e) m.set(this.vname(e), +mm[2]); } return m; };
    const psComp = compOf(ps.glsl, 'in', 'v', ps.ins), vsComp = compOf(vs.glsl, 'out', 'o', vs.outs);
    const psIn = new Map(); for (const e of ps.ins) if (!(e.tex > 0 && e.usage === 0 && false)) psIn.set(this.vname(e), e);
    for (const e of vs.outs) if (e.usage !== 0 && e.usage !== 4) vsrc = vsrc.replace(new RegExp(`\\bo${e.reg}\\b`, 'g'), this.vname(e));
    for (const e of ps.ins) if (e.usage !== 0 || true) psrc = psrc.replace(new RegExp(`\\bv${e.reg}\\b`, 'g'), this.vname(e));
    // reconcile varying widths: SPIRV-Cross trims each stage's varying to the components it uses, GL needs identical types
    const tsize = (t) => ({ float: 1, vec2: 2, vec3: 3, vec4: 4 }[t] || 4), tname = (n) => (n === 1 ? 'float' : 'vec' + n);
    const vtypes = new Map(), reDecl = (kw) => new RegExp(`\\b${kw}\\s+(?:highp\\s+|mediump\\s+)?(float|vec[234])\\s+(vary_\\d+_\\d+)\\s*;`, 'g');
    for (const m of vsrc.matchAll(reDecl('out'))) vtypes.set(m[2], m[1]);
    let psFix = '';
    psrc = psrc.replace(reDecl('in'), (all, pt, name) => {
      const vt = vtypes.get(name), po = psComp.get(name) || 0, vo = vsComp.get(name) || 0; if (!vt || (vt === pt && po === vo)) return all;
      const vn = tsize(vt), pn = tsize(pt), off = po - vo, comps = []; for (let k = 0; k < pn; k++) { const ix = off + k; comps.push(ix >= 0 && ix < vn ? (vn === 1 ? name : `${name}.${'xyzw'[ix]}`) : '0.0'); }
      psFix += `    ${name}_loc = ${pn === 1 ? comps[0] : pt + '(' + comps.join(', ') + ')'};\n`;
      return `@@DECL ${name} ${vt} ${pt}@@`;                // keep the interface name/type identical to the vertex stage, alias the pixel-stage use
    });
    for (const m of [...psrc.matchAll(/@@DECL (\S+) (\S+) (\S+)@@/g)]) {
      const [all, name, vt, pt] = m; psrc = psrc.split(all).join('\u0001');                        // protect the declaration site
      psrc = psrc.replace(new RegExp(`\\b${name}\\b`, 'g'), `${name}_loc`);
      psrc = psrc.replace('\u0001', `in highp ${vt} ${name};\nhighp ${pt} ${name}_loc;`);
    }
    if (psFix) psrc = psrc.replace(/void main\(\)\s*\{/, `void main()\n{\n${psFix}`);
    const have = new Set(vs.outs.filter((e) => e.usage !== 0 && e.usage !== 4).map((e) => this.vname(e))); let extraDecl = '', extraAssign = '';
    for (const n of psIn.keys()) if (!have.has(n)) { extraDecl += `out highp vec4 ${n};\n`; extraAssign += `${n} = vec4(0.0);\n`; }
    // D3DCOLOR vertex attributes arrive as BGRA
    let attrFix = '';
    for (const r of colorRegs) { const re = new RegExp(`(layout\\(location = ${r}\\) in )(?:highp )?vec4 v${r};`); if (re.test(vsrc)) { vsrc = vsrc.replace(re, `$1vec4 v${r}_in;\nvec4 v${r};`); attrFix += `v${r} = v${r}_in.zyxw;\n`; } }
    const addAfterVersion = (s, decl) => s.replace(/(#version 300 es\n)/, `$1${decl}`);
    vsrc = addAfterVersion(vsrc, 'precision highp float;\nprecision highp int;\nuniform float u_flipY;\nuniform vec2 u_hp;\n' + extraDecl);
    vsrc = vsrc.replace(/void main\(\)\s*\{/, `void main()\n{\n${attrFix}${extraAssign}`);
    vsrc = vsrc.replace(/\}\s*$/, '    gl_Position.z = gl_Position.z * 2.0 - gl_Position.w;\n    gl_Position.xy += u_hp * gl_Position.w;\n    gl_Position.y *= u_flipY;\n}\n');
    psrc = this.patchSampling(psrc);
    psrc = addAfterVersion(psrc, 'precision highp float;\nprecision highp int;\nuniform int u_atest;\nuniform int u_afunc;\nuniform float u_aref;\nuniform int u_srgbw;\nuniform int u_srgbtex;\nuniform float u_lodbias;\n' +
      'vec3 srgb2lin(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); }\nvec4 d3s(int k, vec4 c) { return ((u_srgbtex >> k) & 1) != 0 ? vec4(srgb2lin(c.rgb), c.a) : c; }\n');
    const hasO0 = /out highp vec4 o0;/.test(psrc);
    psrc = psrc.replace(/\}\s*$/, hasO0 ? `    if (u_atest != 0) { float a = o0.a; bool ok = u_afunc == 1 ? false : u_afunc == 2 ? a < u_aref : u_afunc == 3 ? a == u_aref : u_afunc == 4 ? a <= u_aref : u_afunc == 5 ? a > u_aref : u_afunc == 6 ? a != u_aref : u_afunc == 7 ? a >= u_aref : true; if (!ok) discard; }\n    if (u_srgbw != 0) { vec3 l = clamp(o0.rgb, 0.0, 1.0); o0.rgb = mix(l * 12.92, 1.055 * pow(l, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), l)); }\n}\n` : '}\n');
    // compile + link are only submitted here; with KHR_parallel_shader_compile they run on driver threads and the status is read in finishProgram()
    const mk = (type, src) => { const sh = gl.createShader(type); gl.shaderSource(sh, src); gl.compileShader(sh); return sh; };
    const v = mk(gl.VERTEX_SHADER, vsrc), f = mk(gl.FRAGMENT_SHADER, psrc);
    const prog = gl.createProgram(); gl.attachShader(prog, v); gl.attachShader(prog, f); gl.linkProgram(prog);
    return { prog, v, f, vs, ps, colorRegs, ready: false };
  }
  finishProgram(P) {
    const gl = this.gl, { prog, v, f, vs, ps } = P, vsId = vs.id, psId = ps.id;
    if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) {
      if (!gl.getShaderParameter(v, gl.COMPILE_STATUS)) this.warn('csvs#' + vsId, `vs#${vsId} compile error: ${gl.getShaderInfoLog(v)}`);
      if (!gl.getShaderParameter(f, gl.COMPILE_STATUS)) this.warn('csps#' + psId, `ps#${psId} compile error: ${gl.getShaderInfoLog(f)}`);
      this.warn(`ln${vsId}_${psId}`, `link error vs#${vsId} ps#${psId}: ${gl.getProgramInfoLog(prog)}`); return null;
    }
    gl.detachShader(prog, v); gl.detachShader(prog, f); gl.deleteShader(v); gl.deleteShader(f);
    Object.assign(P, { u: {}, samplers: [], vsBlock: -1, psBlock: -1, ready: true });
    for (const n of ['u_flipY', 'u_hp', 'u_atest', 'u_afunc', 'u_aref', 'u_srgbw', 'u_vpsz', 'u_srgbtex', 'u_lodbias']) P.u[n] = gl.getUniformLocation(prog, n);
    const nu = gl.getProgramParameter(prog, gl.ACTIVE_UNIFORMS);
    for (let i = 0; i < nu; i++) { const info = gl.getActiveUniform(prog, i), m = /^SPIRV_Cross_Combinedt(\d+)s(\d+)$/.exec(info.name);
      const mv = /^VTX_Combinedt(\d+)s(\d+)$/.exec(info.name);
      if (mv) P.samplers.push({ unit: 16 + +mv[2], loc: gl.getUniformLocation(prog, info.name), type: info.type });
      else if (m) P.samplers.push({ unit: +m[2], loc: gl.getUniformLocation(prog, info.name), type: info.type });
      else if (info.type === gl.SAMPLER_2D || info.type === gl.SAMPLER_CUBE || info.type === gl.SAMPLER_3D || info.type === gl.SAMPLER_2D_SHADOW) console.log('renderer: unrecognised sampler uniform ' + info.name + ' type ' + info.type); }
    { const seen = new Map(); for (const sm of P.samplers) { const k = sm.unit; if (seen.has(k) && seen.get(k) !== sm.type) console.log('renderer: sampler unit ' + k + ' used with types ' + seen.get(k) + ' and ' + sm.type); seen.set(k, sm.type); } }
    const vb = /uniform (cb\d+_struct)/.exec(vs.glsl), pb = /uniform (cb\d+_struct)/.exec(ps.glsl);
    if (vb) { P.vsBlock = gl.getUniformBlockIndex(prog, vb[1]); if (P.vsBlock >= 0) { gl.uniformBlockBinding(prog, P.vsBlock, 0); P.vsSize = gl.getActiveUniformBlockParameter(prog, P.vsBlock, gl.UNIFORM_BLOCK_DATA_SIZE); } }
    if (pb) { P.psBlock = gl.getUniformBlockIndex(prog, pb[1]); if (P.psBlock >= 0) { gl.uniformBlockBinding(prog, P.psBlock, 1); P.psSize = gl.getActiveUniformBlockParameter(prog, P.psBlock, gl.UNIFORM_BLOCK_DATA_SIZE); } }
    this.stats.progs++; return P;
  }
  // rewrite `texture(sampler, uv)` in a pixel shader into d3s(unit, texture(sampler, uv, u_lodbias)): applies D3DSAMP_SRGBTEXTURE (per-draw mask) and the LOD bias option
  patchSampling(src) {
    const out = []; let i = 0; const re = /(?<![A-Za-z0-9_])texture\(/g; let m; const shadow = new Set(); for (const mm of src.matchAll(/sampler\w*Shadow\s+(\w+)/g)) shadow.add(mm[1]);
    while ((m = re.exec(src))) {
      let d = 1, j = re.lastIndex, comma = -1, commas = 0; for (; j < src.length && d > 0; j++) { const ch = src[j]; if (ch === '(') d++; else if (ch === ')') d--; else if (ch === ',' && d === 1) { if (comma < 0) comma = j; commas++; } }
      const args = src.slice(re.lastIndex, j - 1), name = (comma >= 0 ? src.slice(re.lastIndex, comma) : args).trim(), mu = /Combinedt\d+s(\d+)$/.exec(name);
      if (!mu || shadow.has(name) || commas !== 1) continue;
      out.push(src.slice(i, m.index), `d3s(${mu[1]}, texture(${args}, u_lodbias))`); i = j; re.lastIndex = j;
    }
    out.push(src.slice(i)); return out.join('');
  }
  getProgram() {
    const vsId = this._vsEff ?? this.vs, pc = this._pc || (this._pc = new Map()); let m1 = pc.get(vsId), m2 = m1 && m1.get(this.ps); const hit = m2 && m2.get(this.vdecl | 0); if (hit) return hit;       // (vs, ps, vdecl) -> ready program: no per-draw key strings
    const P = this.getProgramSlow(vsId);
    if (P && P.ready) { if (!m1) pc.set(vsId, m1 = new Map()); if (!m2) m1.set(this.ps, m2 = new Map()); m2.set(this.vdecl | 0, P); }
    return P;
  }
  getProgramSlow() {
    const vs = this.shaders.get(this._vsEff ?? this.vs), ps = this.shaders.get(this.ps); if (!vs || !ps) return null;      // _vsEff = -1: built-in POSITIONT vertex shader (Bink quads)
    const d = this.vdecl ? this.vdecls.get(this.vdecl) : null;
    return this.programFor(vs, ps, d ? this.colorKey(d, vs) : '', true);
  }
  // program for a (vs, ps, colour-attribute) variant; programs are shared between shader objects with the same bytecode hash
  programFor(vs, ps, ck, forDraw) {
    const key = `${vs.pk}|${ps.pk}|${ck}`; let P = this.programs.get(key);
    if (P === undefined) {
      const regs = []; ck.split(',').forEach((x, i) => { if (x !== '' && vs.ins[i]) regs.push(vs.ins[i].reg); });
      P = this.buildProgram(vs, ps, regs); this.programs.set(key, P);
    }
    if (P && !P.ready) {
      if (forDraw && this.asyncMode && this.ext.par && !this.gl.getProgramParameter(P.prog, this.ext.par.COMPLETION_STATUS_KHR)) { this.stats.deferred++; this.frameDeferred = true; return null; }
      if (!forDraw) return P;                                          // prewarm: leave it compiling
      if (!this.finishProgram(P)) { this.programs.set(key, null); return null; }
      if (vs.hk && ps.hk) this.used.add(`${vs.hk}|${ps.hk}|${ck}`);                    // programs actually drawn -> warm list
    }
    return P;
  }
  colorKey(d, vs) { return vs.ins.map((e) => { const el = d.elems.find((x) => x.usage === e.usage && x.idx === e.idx); return el && el.type === 4 ? e.reg : ''; }).join(','); }

  // ---- render targets --------------------------------------------------------------------------------------------------------------------------------
  surfaceTarget(desc) {                                       // -> {tex|rb, w, h, level, face}
    if (!desc || desc.kind === 3) return null;
    if (desc.kind === 0) return this.bb ? { tex: this.bb.tex, w: this.bb.w, h: this.bb.h, sc: this.bb.sc, level: 0, target: this.gl.TEXTURE_2D, fmt: 21, owner: this.bb, ownerSamples: true } : null;
    if (desc.kind === 1) { const t = this.textures.get(desc.id); if (!t) return null; return { tex: t.tex, w: Math.max(1, t.sw >> desc.level), h: Math.max(1, t.sh >> desc.level), sc: t.sc, owner: desc.level === 0 && t.kind === 0 && !t.isDS && t.target === this.gl.TEXTURE_2D ? t : null, level: desc.level, target: t.kind === 1 ? this.gl.TEXTURE_CUBE_MAP_POSITIVE_X + desc.face : this.gl.TEXTURE_2D, fmt: t.fmt, depth: t.info.depth, stencil: t.info.stencil }; }
    const s = this.surfaces.get(desc.id); return s ? { rb: s.rb, w: s.w, h: s.h, sc: s.sc, owner: s.ms > 1 ? null : s, fmt: s.fmt, depth: s.info.depth, stencil: s.info.stencil, ms: s.ms } : null;
  }
  attach(gl, att, tg) { if (tg.rb) gl.framebufferRenderbuffer(gl.FRAMEBUFFER, att, gl.RENDERBUFFER, tg.rb); else gl.framebufferTexture2D(gl.FRAMEBUFFER, att, tg.target, tg.tex, tg.level); }
  // MSAA: when the pass has a plain renderbuffer depth surface (the main scene passes) and all colour targets are level-0 2D textures / the backbuffer, colour and depth are
  // swapped for multisampled renderbuffers (one per texture/surface, sample count = min(requested, supported)); the texture is resolved by blit the first time it is sampled,
  // stretched, or presented (msDirty). Passes without such a depth surface (post-process, UI) run as before.
  msSamples(internal) {
    const gl = this.gl; if (!this.msCache) this.msCache = new Map(); let v = this.msCache.get(internal);
    if (v === undefined) { let list = []; try { list = Array.from(gl.getInternalformatParameter(gl.RENDERBUFFER, internal, gl.SAMPLES)); } catch (e) { } v = 0; for (const n of list) if (n <= this.q.msaa) { v = n; break; } this.msCache.set(internal, v); }
    return v;
  }
  msRbFor(o, internal) {                                          // lazily created multisampled twin of a colour owner (texture object or surface record)
    const gl = this.gl, n = this.msSamples(internal); if (!n) return null; const w = o.sw || o.w, h = o.sh || o.h;
    if (!o.msRb || o.msW !== w || o.msH !== h) { o.msRb = gl.createRenderbuffer(); gl.bindRenderbuffer(gl.RENDERBUFFER, o.msRb); gl.renderbufferStorageMultisample(gl.RENDERBUFFER, n, internal, w, h); o.msW = w; o.msH = h; o.msN = n; }
    return o.msRb;
  }
  resolveMS(o) {
    if (!o || !o.msDirty) return; const gl = this.gl; o.msDirty = false;
    if (!this.msR) { this.msR = gl.createFramebuffer(); this.msD = gl.createFramebuffer(); }
    gl.disable(gl.SCISSOR_TEST); gl.colorMask(true, true, true, true);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, this.msR); gl.framebufferTexture2D(gl.READ_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, null, 0); gl.framebufferRenderbuffer(gl.READ_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, o.msRb); gl.readBuffer(gl.COLOR_ATTACHMENT0);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, this.msD);
    if (o.tex) { gl.framebufferRenderbuffer(gl.DRAW_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, null); gl.framebufferTexture2D(gl.DRAW_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, o.tex, 0); }
    else { gl.framebufferTexture2D(gl.DRAW_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, null, 0); gl.framebufferRenderbuffer(gl.DRAW_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, o.rb); }
    gl.drawBuffers([gl.COLOR_ATTACHMENT0]);
    const w = o.msW, h = o.msH; gl.blitFramebuffer(0, 0, w, h, 0, 0, w, h, gl.COLOR_BUFFER_BIT, gl.NEAREST); this.stats.resolves = (this.stats.resolves || 0) + 1;
  }
  bindTargets() {
    if (!this.rtDirty && this._bt) { const gl = this.gl, b = this._bt; gl.bindFramebuffer(gl.FRAMEBUFFER, b.fbo.fbo); gl.drawBuffers(b.fbo.bufs); if (b.ms) for (const o of b.fbo.owners) o.msDirty = true; return b.res; }       // render targets unchanged since the last draw
    const gl = this.gl, c0 = this.surfaceTarget(this.rt[0]); let ds = this.surfaceTarget(this.ds);
    // D3D9 allows a depth surface larger than the colour target; GL needs identical attachment sizes -> use a private depth renderbuffer of the colour size (cached per size/format)
    if (ds && c0 && (ds.w !== c0.w || ds.h !== c0.h)) {
      const k = `${c0.w}x${c0.h}/${ds.stencil ? 1 : 0}`; if (!this.depthCache) this.depthCache = new Map(); let d = this.depthCache.get(k);
      if (!d) { const rb = gl.createRenderbuffer(); gl.bindRenderbuffer(gl.RENDERBUFFER, rb); gl.renderbufferStorage(gl.RENDERBUFFER, ds.stencil ? gl.DEPTH24_STENCIL8 : gl.DEPTH_COMPONENT24, c0.w, c0.h); d = { rb, w: c0.w, h: c0.h, sc: c0.sc, depth: true, stencil: ds.stencil, key: k }; this.depthCache.set(k, d); this.warn('dsfix' + k, `depth surface ${ds.w}x${ds.h} != colour target ${c0.w}x${c0.h}: using private depth buffer`); }
      ds = d;
    }
    let ms = false;
    if (this.q.msaa > 1 && ds && ds.rb && ds.owner && c0 && c0.owner) {
      ms = true; for (let i = 0; i < 4; i++) { const tg = this.surfaceTarget(this.rt[i]); if (tg && (!tg.owner || !this.msSamples(tg.owner.info ? tg.owner.info.internal : this.gl.RGBA8))) ms = false; }
      if (ms && !this.msSamples(ds.owner.info.internal)) ms = false;
    }
    const key = JSON.stringify([this.rt, this.ds, ms, ds && ds.key || 0]);
    let fbo = this.fbos.get(key);
    if (!fbo) {
      fbo = gl.createFramebuffer(); gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
      const bufs = [], owners = [];
      for (let i = 0; i < 4; i++) {
        const tg = this.surfaceTarget(this.rt[i]);
        if (tg) { if (ms) { const o = tg.owner; gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0 + i, gl.RENDERBUFFER, this.msRbFor(o, o.info ? o.info.internal : gl.RGBA8)); owners.push(o); } else this.attach(gl, gl.COLOR_ATTACHMENT0 + i, tg); bufs.push(gl.COLOR_ATTACHMENT0 + i); } else bufs.push(gl.NONE);
      }
      if (ds) { if (ms) { const o = ds.owner; gl.framebufferRenderbuffer(gl.FRAMEBUFFER, ds.stencil ? gl.DEPTH_STENCIL_ATTACHMENT : gl.DEPTH_ATTACHMENT, gl.RENDERBUFFER, this.msRbFor(o, o.info.internal)); } else this.attach(gl, ds.stencil ? gl.DEPTH_STENCIL_ATTACHMENT : gl.DEPTH_ATTACHMENT, ds); }
      fbo = { fbo, bufs, owners }; this.fbos.set(key, fbo);
      const st = gl.checkFramebufferStatus(gl.FRAMEBUFFER); if (st !== gl.FRAMEBUFFER_COMPLETE) this.warn('fb' + key, `framebuffer incomplete (0x${st.toString(16)}) for ${key}`);
    }
    gl.bindFramebuffer(gl.FRAMEBUFFER, fbo.fbo); gl.drawBuffers(fbo.bufs);
    if (ms) for (const o of fbo.owners) o.msDirty = true;
    const res = { w: c0 ? c0.w : 1, h: c0 ? c0.h : 1, sc: c0 ? c0.sc : 1 }; this._bt = { fbo, ms, res }; this.rtDirty = false;
    return res;
  }

  // ---- state application ----------------------------------------------------------------------------------------------------------------------------
  cmp(f) { const gl = this.gl; return (this._cmpT || (this._cmpT = [gl.NEVER, gl.NEVER, gl.LESS, gl.EQUAL, gl.LEQUAL, gl.GREATER, gl.NOTEQUAL, gl.GEQUAL, gl.ALWAYS]))[f] || gl.ALWAYS; }
  blendFactor(b) { const gl = this.gl; return (this._bfT || (this._bfT = [gl.ZERO, gl.ZERO, gl.ONE, gl.SRC_COLOR, gl.ONE_MINUS_SRC_COLOR, gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA, gl.DST_ALPHA, gl.ONE_MINUS_DST_ALPHA, gl.DST_COLOR, gl.ONE_MINUS_DST_COLOR, gl.SRC_ALPHA_SATURATE, gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA, gl.CONSTANT_COLOR, gl.ONE_MINUS_CONSTANT_COLOR]))[b] ?? gl.ONE; }
  blendOp(o) { const gl = this.gl; return (this._boT || (this._boT = [gl.FUNC_ADD, gl.FUNC_ADD, gl.FUNC_SUBTRACT, gl.FUNC_REVERSE_SUBTRACT, gl.MIN, gl.MAX]))[o] ?? gl.FUNC_ADD; }
  stencilOp(o) { const gl = this.gl; return (this._soT || (this._soT = [gl.KEEP, gl.KEEP, gl.ZERO, gl.REPLACE, gl.INCR, gl.DECR, gl.INVERT, gl.INCR_WRAP, gl.DECR_WRAP]))[o] ?? gl.KEEP; }
  applyState(rtw, rth, rtsc) {
    const gl = this.gl, r = this.rs, vp = this.vp;
    const sc = this.curSc = rtsc || 1; gl.viewport(this.scaled(vp.x, sc), this.scaled(vp.y, sc), this.scaled(vp.w, sc), this.scaled(vp.h, sc)); gl.depthRange(vp.minz, vp.maxz);
    const hasDS = this.ds.kind !== 3;
    if (hasDS && r[7]) { gl.enable(gl.DEPTH_TEST); gl.depthFunc(this.cmp(r[23])); } else gl.disable(gl.DEPTH_TEST);
    gl.depthMask(!!r[14] && hasDS);
    if (r[22] === 1) gl.disable(gl.CULL_FACE); else { gl.enable(gl.CULL_FACE); gl.cullFace(gl.BACK); gl.frontFace(r[22] === 2 ? gl.CW : gl.CCW); }
    if (r[27]) { gl.enable(gl.BLEND); const col = r[193]; gl.blendColor(((col >> 16) & 255) / 255, ((col >> 8) & 255) / 255, (col & 255) / 255, ((col >>> 24) & 255) / 255);
      if (r[206]) { gl.blendFuncSeparate(this.blendFactor(r[19]), this.blendFactor(r[20]), this.blendFactor(r[207]), this.blendFactor(r[208])); gl.blendEquationSeparate(this.blendOp(r[171]), this.blendOp(r[209])); }
      else { gl.blendFunc(this.blendFactor(r[19]), this.blendFactor(r[20])); gl.blendEquation(this.blendOp(r[171])); } } else gl.disable(gl.BLEND);
    const cw = r[168]; gl.colorMask(!!(cw & 1), !!(cw & 2), !!(cw & 4), !!(cw & 8));
    if (hasDS && r[52]) { gl.enable(gl.STENCIL_TEST);
      gl.stencilFuncSeparate(gl.FRONT, this.cmp(r[56]), r[57] & 255, r[58] & 255); gl.stencilOpSeparate(gl.FRONT, this.stencilOp(r[53]), this.stencilOp(r[54]), this.stencilOp(r[55])); gl.stencilMaskSeparate(gl.FRONT, r[59] & 255);
      if (r[185]) { gl.stencilFuncSeparate(gl.BACK, this.cmp(r[189]), r[57] & 255, r[58] & 255); gl.stencilOpSeparate(gl.BACK, this.stencilOp(r[186]), this.stencilOp(r[187]), this.stencilOp(r[188])); gl.stencilMaskSeparate(gl.BACK, r[59] & 255); }
      else { gl.stencilFuncSeparate(gl.BACK, this.cmp(r[56]), r[57] & 255, r[58] & 255); gl.stencilOpSeparate(gl.BACK, this.stencilOp(r[53]), this.stencilOp(r[54]), this.stencilOp(r[55])); gl.stencilMaskSeparate(gl.BACK, r[59] & 255); } }
    else gl.disable(gl.STENCIL_TEST);
    if (r[174]) { gl.enable(gl.SCISSOR_TEST); const s = this.scissor; gl.scissor(this.scaled(s.l, sc), this.scaled(s.t, sc), this.scaled(Math.max(0, s.r - s.l), sc), this.scaled(Math.max(0, s.b - s.t), sc)); } else gl.disable(gl.SCISSOR_TEST);
    const bias = new Float32Array(1), slope = new Float32Array(1); new Uint32Array(bias.buffer)[0] = r[195]; new Uint32Array(slope.buffer)[0] = r[175];
    if (bias[0] !== 0 || slope[0] !== 0) { gl.enable(gl.POLYGON_OFFSET_FILL); gl.polygonOffset(slope[0], bias[0] * 16777216); } else gl.disable(gl.POLYGON_OFFSET_FILL);
  }
  filterFor(tx, s) {
    const gl = this.gl, mag = s[5], min = s[6], mip = s[7], multi = tx.levels > 1 && mip !== 0;
    let minF; if (tx.info.float && !(tx.info.f32 ? this.ext.flin : true)) minF = gl.NEAREST;
    else if (!multi) minF = min === 1 ? gl.NEAREST : gl.LINEAR;
    else if (mip === 1) minF = min === 1 ? gl.NEAREST_MIPMAP_NEAREST : gl.LINEAR_MIPMAP_NEAREST; else minF = min === 1 ? gl.NEAREST_MIPMAP_LINEAR : gl.LINEAR_MIPMAP_LINEAR;
    let magF = mag === 1 ? gl.NEAREST : gl.LINEAR; if (tx.info.float && tx.info.f32 && !this.ext.flin) magF = gl.NEAREST;
    return [minF, magF];
  }
  addr(a) { const gl = this.gl; return [gl.REPEAT, gl.REPEAT, gl.MIRRORED_REPEAT, gl.CLAMP_TO_EDGE, gl.CLAMP_TO_EDGE, gl.MIRRORED_REPEAT][a] ?? gl.REPEAT; }
  snapshotFor(tx) {
    const gl = this.gl, sw = tx.sw || tx.w, sh = tx.sh || tx.h;
    if (!tx.snap) { tx.snap = gl.createTexture(); gl.bindTexture(gl.TEXTURE_2D, tx.snap); gl.texStorage2D(gl.TEXTURE_2D, 1, tx.info.internal, sw, sh); }
    if (!this.snapR) { this.snapR = gl.createFramebuffer(); this.snapD = gl.createFramebuffer(); }
    gl.disable(gl.SCISSOR_TEST); gl.colorMask(true, true, true, true);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, this.snapR); gl.framebufferTexture2D(gl.READ_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tx.tex, 0); gl.readBuffer(gl.COLOR_ATTACHMENT0);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, this.snapD); gl.framebufferTexture2D(gl.DRAW_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tx.snap, 0); gl.drawBuffers([gl.COLOR_ATTACHMENT0]);
    gl.blitFramebuffer(0, 0, sw, sh, 0, 0, sw, sh, gl.COLOR_BUFFER_BIT, gl.NEAREST);
    this.rebindFbo = true;
  }
  bindTextures(P) {
    const gl = this.gl;
    for (const sm of P.samplers) {
      const tx = this.textures.get(this.tex[sm.unit] || 0), unit = sm.unit, s = this.ss[unit];
      const want = sm.type === gl.SAMPLER_CUBE ? gl.TEXTURE_CUBE_MAP : sm.type === gl.SAMPLER_3D ? gl.TEXTURE_3D : gl.TEXTURE_2D;
      gl.activeTexture(gl.TEXTURE0 + unit); if (tx) tx.usedAt = this.auditPresent || 0;
      if (tx && tx.target === want && !tx.isDS) {
        gl.bindTexture(want, this.feedback && this.feedback.get(tx.id) || tx.tex); const [mn, mg] = this.filterFor(tx, s), sp = this.samplers[unit];
        gl.samplerParameteri(sp, gl.TEXTURE_MIN_FILTER, mn); gl.samplerParameteri(sp, gl.TEXTURE_MAG_FILTER, mg);
        gl.samplerParameteri(sp, gl.TEXTURE_WRAP_S, this.addr(s[1])); gl.samplerParameteri(sp, gl.TEXTURE_WRAP_T, this.addr(s[2])); gl.samplerParameteri(sp, gl.TEXTURE_WRAP_R, this.addr(s[3]));
        if (this.ext.aniso) { const lin = (s[6] >= 2 || s[5] >= 2) && !tx.info.float && !tx.isRT, a = this.q.aniso > 1 && lin && sm.type !== gl.SAMPLER_3D ? this.q.aniso : (s[6] === 3 || s[5] === 3 ? Math.max(1, s[10]) : 1);
          gl.samplerParameterf(sp, this.ext.aniso.TEXTURE_MAX_ANISOTROPY_EXT, Math.min(this.maxAniso, a));
          if (a > 1 && this.q.tri && tx.levels > 1 && s[6] >= 2) gl.samplerParameteri(sp, gl.TEXTURE_MIN_FILTER, gl.LINEAR_MIPMAP_LINEAR); }
        gl.bindSampler(unit, sp);
      } else { gl.bindTexture(want, this.dummy[want]); gl.bindSampler(unit, null); }
      gl.uniform1i(sm.loc, unit);
    }
  }
  setupAttribs(P, vsInfo, decl, base) {
    const gl = this.gl; let used = 0; const AC0 = this.attrC || (this.attrC = new Array(16).fill(null));
    for (const e of vsInfo.ins) {
      const el = decl.elems.find((x) => x.usage === e.usage && x.idx === e.idx), loc = e.reg;
      const AC = this.attrC || (this.attrC = new Array(16).fill(null));               // shadow of the default VAO's per-location state (skip identical enable/pointer calls)
      if (!el) { if (AC[loc] !== 'c') { AC[loc] = 'c'; gl.disableVertexAttribArray(loc); gl.vertexAttrib4f(loc, 0, 0, 0, 1); } continue; }
      const st = this.streams[el.stream], b = this.buffers.get(st.buf); if (!b) { if (AC[loc] !== 'd') { AC[loc] = 'd'; gl.disableVertexAttribArray(loc); } continue; }
      const T = el.type, F = gl.FLOAT, off = st.offset + el.offset + base * st.stride;
      let size = 4, type = F, norm = false;
      switch (T) { case 0: size = 1; break; case 1: size = 2; break; case 2: size = 3; break; case 3: size = 4; break; case 4: size = 4; type = gl.UNSIGNED_BYTE; norm = true; break;
        case 5: size = 4; type = gl.UNSIGNED_BYTE; break; case 6: size = 2; type = gl.SHORT; break; case 7: size = 4; type = gl.SHORT; break; case 8: size = 4; type = gl.UNSIGNED_BYTE; norm = true; break;
        case 9: size = 2; type = gl.SHORT; norm = true; break; case 10: size = 4; type = gl.SHORT; norm = true; break; case 11: size = 2; type = gl.UNSIGNED_SHORT; norm = true; break;
        case 12: size = 4; type = gl.UNSIGNED_SHORT; norm = true; break; case 13: size = 4; type = gl.UNSIGNED_INT_2_10_10_10_REV; break; case 14: size = 4; type = gl.INT_2_10_10_10_REV; norm = true; break;
        case 15: size = 2; type = gl.HALF_FLOAT; break; case 16: size = 4; type = gl.HALF_FLOAT; break; default: this.warn('vt' + T, 'unsupported vertex type ' + T); }
      const c = AC[loc]; used |= 1 << loc;
      if (!(c && c.b === b.buf && c.size === size && c.type === type && c.norm === norm && c.stride === st.stride && c.off === off)) {
        gl.bindBuffer(gl.ARRAY_BUFFER, b.buf); if (!c || !c.b) gl.enableVertexAttribArray(loc); gl.vertexAttribPointer(loc, size, type, norm, st.stride, off); AC[loc] = { b: b.buf, size, type, norm, stride: st.stride, off }; }
    }
    for (let i = 0; i < 16; i++) if (!(used & (1 << i)) && !vsInfo.ins.some((e) => e.reg === i)) { if (AC0[i] !== 'd') { AC0[i] = 'd'; gl.disableVertexAttribArray(i); } }
  }
  primCount(type, n) { switch (type) { case 1: return n; case 2: return 2 * n; case 3: return n + 1; case 4: return 3 * n; case 5: case 6: return n + 2; } return n; }
  glMode(type) { const gl = this.gl; return [gl.POINTS, gl.POINTS, gl.LINES, gl.LINE_STRIP, gl.TRIANGLES, gl.TRIANGLE_STRIP, gl.TRIANGLE_FAN][type] ?? gl.TRIANGLES; }
  prepareDraw() {
    const gl = this.gl;
    let vsId = this.vs;
    if (!vsId) { const dd = this.vdecls.get(this.vdecl); if (dd && dd.elems.some((e) => e.usage === 9) && this.ps) vsId = -1; }     // pre-transformed quad (Bink movie): built-in VS
    this._vsEff = vsId;
    if (!vsId || !this.ps) { this.warn('noshader', 'draw without vertex/pixel shader (fixed-function pipeline not implemented)'); this.stats.skipped++; return null; }
    const P = this.getProgram(); if (!P) { this.stats.skipped++; return null; }
    if (this.q.msaa > 1) for (const sm of P.samplers) { const tx = this.textures.get(this.tex[sm.unit] || 0); if (tx && tx.msDirty) this.resolveMS(tx); }
    const vsInfo = this.shaders.get(vsId), decl = this.vdecls.get(this.vdecl); if (!decl) { this.warn('nodecl', 'draw without vertex declaration'); this.stats.skipped++; return null; }
    const t = this.bindTargets(); this.applyState(t.w, t.h, t.sc);
    gl.useProgram(P.prog);
    gl.uniform1f(P.u.u_flipY, -1); gl.uniform2f(P.u.u_hp, 1 / this.vp.w, -1 / this.vp.h); if (P.u.u_vpsz) gl.uniform2f(P.u.u_vpsz, this.vp.w, this.vp.h);
    gl.uniform1i(P.u.u_atest, this.rs[15] ? 1 : 0); gl.uniform1i(P.u.u_afunc, this.rs[25]); gl.uniform1f(P.u.u_aref, (this.rs[24] & 255) / 255); gl.uniform1i(P.u.u_srgbw, this.rs[194] ? 1 : 0); if (P.u.u_lodbias) gl.uniform1f(P.u.u_lodbias, this.q.bias);
    if (P.u.u_srgbtex) { let mask = 0; if (this.q.srgb) for (const sm of P.samplers) if (sm.unit < 16 && this.ss[sm.unit][11]) { const tx = this.textures.get(this.tex[sm.unit] || 0); if (tx && !tx.info.float) mask |= 1 << sm.unit; } gl.uniform1i(P.u.u_srgbtex, mask); }
    if (P.vsBlock >= 0) { if (this.vscDirty) { gl.bindBuffer(gl.UNIFORM_BUFFER, this.vsUBO); gl.bufferSubData(gl.UNIFORM_BUFFER, 0, this.vsc); this.vscDirty = false; } gl.bindBufferRange(gl.UNIFORM_BUFFER, 0, this.vsUBO, 0, Math.min(P.vsSize, 4096)); }
    if (P.psBlock >= 0) { if (this.pscDirty) { gl.bindBuffer(gl.UNIFORM_BUFFER, this.psUBO); gl.bufferSubData(gl.UNIFORM_BUFFER, 0, this.psc); this.pscDirty = false; } gl.bindBufferRange(gl.UNIFORM_BUFFER, 1, this.psUBO, 0, Math.min(P.psSize, 3584)); }
    this.feedback = null;
    for (const sm of P.samplers) {                                  // D3D9 lets a pass read the texture it renders to; GL rejects the draw -> sample a snapshot copy taken now
      const id = this.tex[sm.unit] || 0, tx = id ? this.textures.get(id) : null;
      if (tx && tx.kind === 0 && !tx.isDS && this.rt.some((r) => r.kind === 1 && r.id === id && !r.level)) { this.snapshotFor(tx); (this.feedback || (this.feedback = new Map())).set(id, tx.snap); this.stats.feedback = (this.stats.feedback || 0) + 1; }
    }
    if (this.feedback) this.bindTargets();
    this.bindTextures(P);
    return { P, vsInfo, decl };
  }
  skipRT() {        // true while drawing into a mirror-pass target on a skipped frame; also counts draws per target so heavy mirror targets are recognised
    const r = this.rt[0]; if (r.kind !== 1) return false; const t = this.textures.get(r.id); if (!t) return false;
    if (this.mirrorMode === 'full') return false;
    if (t.mdc === undefined) { t.mdc = 0; t.mskip = false; } if (t.mdc++ === 0) this.mirrorTouched.push(t);
    return this.mirrorOn && t.mskip;
  }
  draw(type, indexed, base, minIdx, numVerts, start, primCount) {
    if (this.skipRT()) return;
    const gl = this.gl, ctx = this.prepareDraw(); if (!ctx) return;
    if (this.diag) { const r0 = this.rt[0], k = r0.kind + ':' + (r0.id || 0); this.rtCount[k] = (this.rtCount[k] || 0) + 1; }
    const count = this.primCount(type, primCount), mode = this.glMode(type);
    const dn = this.dn = (this.dn || 0) + 1 - 1; this.dn++;
    if (window.__only && !(dn >= window.__only[0] && dn <= window.__only[1]) && this.rt[0].kind !== 0 && this.rt[0].id === window.__only[2]) return;
    if (this.cap) this.capDraw(ctx, { type, indexed, base, minIdx, numVerts, start, count });
    this.setupAttribs(ctx.P, ctx.vsInfo, ctx.decl, indexed ? base : 0);
    if (indexed) { const ib = this.buffers.get(this.ib); if (!ib) { this.stats.skipped++; return; } const isz = ib.fmt === 101 ? 2 : 4;
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ib.buf); gl.drawElements(mode, count, isz === 2 ? gl.UNSIGNED_SHORT : gl.UNSIGNED_INT, start * isz); }
    else gl.drawArrays(mode, start, count);
    if (this.dbg) { const e = gl.getError(); if (e) this.warn('glerr' + e, 'GL error 0x' + e.toString(16) + ' after draw type=' + type + ' indexed=' + indexed + ' start=' + start + ' count=' + count + ' vs=' + this.vs + ' ps=' + this.ps);
      if (this.rt && this.rt[0] && this.rt[0].kind === 1 && this.rs[15] && (this.dbgF || 0) > 8200 && (this.dbgM = (this.dbgM || 0) + 1) < 30) { console.log('renderer: after atest draw vs=' + this.vs + ' ps=' + this.ps + ' count=' + count + ' rt=' + this.rt[0].id); this.dbgReadRT(this.rt[0].id); }
      if (this.rs[15] && (this.dbgN = (this.dbgN || 0) + 1) < 40) console.log('renderer: dbg draw vs=' + this.vs + ' ps=' + this.ps + ' type=' + type + ' start=' + start + ' count=' + count + ' blend=' + this.rs[27] + ' cull=' + this.rs[22] + ' ztest=' + this.rs[7] + ' atest=' + this.rs[15] + ' afunc=' + this.rs[25] + ' aref=' + (this.rs[24] & 255) + ' vp=' + JSON.stringify(this.vp) + ' target=' + JSON.stringify(this.rt && this.rt[0])); }
    this.stats.draws++; this.stats.frameDraws++;
  }
  drawUP(type, primCount, stride, nverts, indexed, ifmt, nidx, blob) {
    if (this.skipRT()) return;
    const gl = this.gl, ctx = this.prepareDraw(); if (!ctx) return;
    const isz = ifmt === 101 ? 2 : 4, ib = indexed ? nidx * isz : 0, mode = this.glMode(type), count = this.primCount(type, primCount);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.upVB); gl.bufferData(gl.ARRAY_BUFFER, blob.subarray(ib), gl.STREAM_DRAW);
    const saved = this.streams[0], savedBuf = this.buffers.get(-1); this.buffers.set(-1, { buf: this.upVB }); this.streams[0] = { buf: -1, offset: 0, stride };
    this.setupAttribs(ctx.P, ctx.vsInfo, ctx.decl, 0); this.streams[0] = saved; if (savedBuf) this.buffers.set(-1, savedBuf); else this.buffers.delete(-1);
    if (indexed) { gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.upIB); gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, blob.subarray(0, ib), gl.STREAM_DRAW); gl.drawElements(mode, count, isz === 2 ? gl.UNSIGNED_SHORT : gl.UNSIGNED_INT, 0); }
    else gl.drawArrays(mode, 0, count);
    this.stats.draws++; this.stats.frameDraws++;
  }
  clear(flags, color, z, stencil, nrects, rects) {
    if (this.skipRT()) return;
    const gl = this.gl, t = this.bindTargets(); this.stats.clears++;
    gl.disable(gl.SCISSOR_TEST); gl.colorMask(true, true, true, true); gl.depthMask(true); gl.stencilMask(0xff);
    let bits = 0;
    if (flags & 1) { gl.clearColor(((color >> 16) & 255) / 255, ((color >> 8) & 255) / 255, (color & 255) / 255, ((color >>> 24) & 255) / 255); bits |= gl.COLOR_BUFFER_BIT; }
    if ((flags & 2) && this.ds.kind !== 3) { gl.clearDepth(z); bits |= gl.DEPTH_BUFFER_BIT; }
    if ((flags & 4) && this.ds.kind !== 3) { gl.clearStencil(stencil); bits |= gl.STENCIL_BUFFER_BIT; }
    if (!bits) return;
    const vp = this.vp, sc = t.sc, full = vp.x === 0 && vp.y === 0 && this.scaled(vp.w, sc) >= t.w && this.scaled(vp.h, sc) >= t.h, S = (v) => this.scaled(v, sc);
    if (nrects) { gl.enable(gl.SCISSOR_TEST); for (let i = 0; i < nrects; i++) { const l = rects[4 * i], tp = rects[4 * i + 1], r = rects[4 * i + 2], b = rects[4 * i + 3]; gl.scissor(S(l), S(tp), S(r) - S(l), S(b) - S(tp)); gl.clear(bits); } gl.disable(gl.SCISSOR_TEST); }
    else { if (!full) { gl.enable(gl.SCISSOR_TEST); gl.scissor(S(vp.x), S(vp.y), S(vp.w), S(vp.h)); } gl.clear(bits); gl.disable(gl.SCISSOR_TEST); }
  }
  stretch(w) {                                                // [skind,sid,slevel,sface,hasS,sl,st,sr,sb, dkind,did,dlevel,dface,hasD,dl,dt,dr,db, filter]
    const gl = this.gl, dst = this.surfaceTarget({ kind: w[9], id: w[10], level: w[11], face: w[12] }); if (!dst) return;
    const dr = w[13] ? [w[14], w[15], w[16], w[17]].map((v) => this.scaled(v, dst.sc || 1)) : [0, 0, dst.w, dst.h];
    gl.disable(gl.SCISSOR_TEST); gl.colorMask(true, true, true, true);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, this.blitFbo2); gl.framebufferTexture2D(gl.DRAW_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, null, 0); gl.framebufferRenderbuffer(gl.DRAW_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, null);
    if (dst.rb) gl.framebufferRenderbuffer(gl.DRAW_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, dst.rb); else gl.framebufferTexture2D(gl.DRAW_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, dst.target, dst.tex, dst.level);
    gl.drawBuffers([gl.COLOR_ATTACHMENT0]);
    if (w[0] === 4) {                                          // ColorFill
      const c = w[1]; gl.clearBufferfv(gl.COLOR, 0, [((c >> 16) & 255) / 255, ((c >> 8) & 255) / 255, (c & 255) / 255, ((c >>> 24) & 255) / 255]); return; }
    const src = this.surfaceTarget({ kind: w[0], id: w[1], level: w[2], face: w[3] }); if (!src) return;
    if (src.owner && src.owner.msDirty) { this.resolveMS(src.owner); gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, this.blitFbo2); gl.drawBuffers([gl.COLOR_ATTACHMENT0]); }
    const sr = w[4] ? [w[5], w[6], w[7], w[8]].map((v) => this.scaled(v, src.sc || 1)) : [0, 0, src.w, src.h];
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, this.blitFbo); gl.framebufferTexture2D(gl.READ_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, null, 0); gl.framebufferRenderbuffer(gl.READ_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, null);
    if (src.rb) gl.framebufferRenderbuffer(gl.READ_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, src.rb); else gl.framebufferTexture2D(gl.READ_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, src.target, src.tex, src.level);
    gl.readBuffer(gl.COLOR_ATTACHMENT0);
    gl.blitFramebuffer(sr[0], sr[1], sr[2], sr[3], dr[0], dr[1], dr[2], dr[3], gl.COLOR_BUFFER_BIT, w[18] === 1 && !(src.ms > 1) ? gl.NEAREST : gl.LINEAR);
  }
  // diagnostic: read back texture `id` level 0 from the GL side -> {w,h,fmt,min,max,nzFrac,first:[...]}
  readVol(id) {                                                  // 3D texture level 0: per-slice summary (first texel of slices 0,1,mid,last) + overall stats
    const gl = this.gl, t = this.textures.get(id); if (!t || t.kind !== 2) return { err: 'not a volume' }; const w = t.w, h = t.h, d = t.d, fb = gl.createFramebuffer(), px = new Uint8Array(w * h * 4); let nz = 0, tot = 0; const first = [];
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, fb);
    for (let k = 0; k < d; k++) { gl.framebufferTextureLayer(gl.READ_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, t.tex, 0, k); if (gl.checkFramebufferStatus(gl.READ_FRAMEBUFFER) !== gl.FRAMEBUFFER_COMPLETE) { gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null); return { err: 'fb incomplete', w, h, d, fmt: t.fmt }; }
      gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, px); for (let i = 0; i < px.length; i++) { tot++; if (px[i]) nz++; } if ([0, 1, d >> 1, d - 1].includes(k)) first.push(Array.from(px.subarray(0, 4)).concat(Array.from(px.subarray(4 * (w * h - 1)))));
    }
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null); gl.deleteFramebuffer(fb); t.dump = t.dump || null; return { w, h, d, levels: t.levels, fmt: t.fmt, up: t.up || 0, nzFrac: +(nz / tot).toFixed(3), slices_first_last_texel: first };
  }
  volData(id) {                                                  // whole level 0 of a volume texture as a flat RGBA byte array [z][y][x]
    const gl = this.gl, t = this.textures.get(id), w = t.w, h = t.h, d = t.d, fb = gl.createFramebuffer(), px = new Uint8Array(w * h * 4), all = new Uint8Array(w * h * d * 4);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, fb);
    for (let k = 0; k < d; k++) { gl.framebufferTextureLayer(gl.READ_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, t.tex, 0, k); gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, px); all.set(px, k * w * h * 4); }
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null); gl.deleteFramebuffer(fb); return Array.from(all);
  }
  readTex(id, n = 4) {
    const gl = this.gl, t = this.textures.get(id); if (!t) return { err: 'no texture' }; const w = t.sw || t.w, h = t.sh || t.h, f32 = !!(t.info.float), px = f32 ? new Float32Array(w * h * 4) : new Uint8Array(w * h * 4);
    const fb = gl.createFramebuffer(); gl.bindFramebuffer(gl.READ_FRAMEBUFFER, fb); gl.framebufferTexture2D(gl.READ_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, t.target === gl.TEXTURE_CUBE_MAP ? gl.TEXTURE_CUBE_MAP_POSITIVE_X : gl.TEXTURE_2D, t.tex, 0);
    const st = gl.checkFramebufferStatus(gl.READ_FRAMEBUFFER); let ok = st === gl.FRAMEBUFFER_COMPLETE; if (ok) { gl.readPixels(0, 0, w, h, gl.RGBA, f32 ? gl.FLOAT : gl.UNSIGNED_BYTE, px); ok = !gl.getError(); }
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null); gl.deleteFramebuffer(fb); if (!ok) return { err: 'fb status 0x' + st.toString(16), w, h, fmt: t.fmt };
    let nz = 0, mn = 1e30, mx = -1e30; for (let i = 0; i < px.length; i++) { if (px[i]) nz++; if (px[i] < mn) mn = px[i]; if (px[i] > mx) mx = px[i]; }
    return { w, h, fmt: t.fmt, rt: t.isRT, nzFrac: +(nz / px.length).toFixed(3), min: mn, max: mx, first: Array.from(px.subarray(0, 4 * n)).map((v) => +v.toFixed(4)) };
  }
  dbgReadRT(id) {                                              // diagnostic: count non-zero pixels of a texture render target
    const gl = this.gl, t = this.surfaceTarget({ kind: 1, id, level: 0, face: 0 }); if (!t || !t.tex) { console.log('renderer: dbgReadRT no target ' + id); return; }
    const fb = gl.createFramebuffer(); gl.bindFramebuffer(gl.READ_FRAMEBUFFER, fb); gl.framebufferTexture2D(gl.READ_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, t.tex, 0);
    const px = new Uint8Array(t.w * t.h * 4); gl.readPixels(0, 0, t.w, t.h, gl.RGBA, gl.UNSIGNED_BYTE, px); gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null); gl.deleteFramebuffer(fb);
    let nz = 0, na = 0, mx = 0; for (let i = 0; i < px.length; i += 4) { if (px[i] | px[i + 1] | px[i + 2]) nz++; if (px[i + 3]) na++; mx = Math.max(mx, px[i], px[i + 1], px[i + 2], px[i + 3]); }
    console.log('renderer: dbgReadRT ' + id + ' ' + t.w + 'x' + t.h + ' nonzero-rgb=' + nz + ' nonzero-alpha=' + na + ' max=' + mx);
  }
  // ---- diagnostic capture of one frame's draws (window.__capNext = true -> records the next full frame; window.__cap -> result) --------------------
  capTex(tx) {                                                    // GL-side content summary of a sampled texture (readable formats only), once per captured frame
    if (!this.capTexMap) this.capTexMap = new Map(); let r = this.capTexMap.get(tx.id);
    if (!r && tx.kind === 2) { r = this.readVol(tx.id); this.capTexMap.set(tx.id, r); } else if (!r) { const q = tx.info.compressed || tx.isDS || tx.kind === 2 || (tx.sw || tx.w) * (tx.sh || tx.h) > 1500000 ? null : this.readTex(tx.id, 2); r = q ? (q.err ? q.err : { nz: q.nzFrac, min: +q.min.toFixed(3), max: +q.max.toFixed(3), f: q.first }) : 'n/a'; this.capTexMap.set(tx.id, r); }
    return r;
  }
  capDraw(ctx, d) {
    const gl = this.gl, P = ctx.P, r = this.rs, wants = window.__capConst;
    const smp = P.samplers.map((sm) => { const tx = this.textures.get(this.tex[sm.unit] || 0), s = this.ss[sm.unit];
      const want = sm.type === gl.SAMPLER_CUBE ? 'cube' : sm.type === gl.SAMPLER_3D ? '3d' : sm.type === gl.SAMPLER_2D_SHADOW ? '2dshadow' : '2d';
      return { u: sm.unit, want, tex: tx ? tx.id : 0, kind: tx ? tx.kind : -1, fmt: tx ? tx.fmt : -1, gl: tx ? this.capTex(tx) : null, up: tx ? tx.up || 0 : 0, nz: tx ? +(tx.nz || 0).toFixed(3) : 0, w: tx ? tx.w : 0, h: tx ? tx.h : 0, lv: tx ? tx.levels : 0, rt: tx ? +tx.isRT : 0, ds: tx ? +tx.isDS : 0,
        dummy: !(tx && ((want === 'cube' && tx.target === gl.TEXTURE_CUBE_MAP) || (want === '3d' && tx.target === gl.TEXTURE_3D) || (want.startsWith('2d') && tx.target === gl.TEXTURE_2D)) && !tx.isDS), ss: Array.from(s.slice(1, 12)) }; });
    const rec = { n: this.cap.length, vs: this.vs, ps: this.ps, type: d.type, count: d.count, idx: d.indexed, base: d.base, start: d.start, nv: d.numVerts, rt: this.rt.map((x) => x.kind === 3 ? '-' : x.kind + ':' + (x.id || 0)).join(','), ds: this.ds.kind === 3 ? '-' : this.ds.kind + ':' + (this.ds.id || 0),
      vp: [this.vp.x, this.vp.y, this.vp.w, this.vp.h], rs: { z: r[7], zw: r[14], zf: r[23], cull: r[22], blend: r[27], src: r[19], dst: r[20], at: r[15], srgbw: r[194], cw: r[168], sten: r[52] }, smp,
      decl: ctx.decl.elems.map((e) => `${e.stream}:${e.offset}:t${e.type}:u${e.usage}.${e.idx}`).join(' ') };
    if (wants && wants.has(rec.n)) { rec.psc = Array.from(this.psc.subarray(0, 4 * 224)); rec.vsc = Array.from(this.vsc.subarray(0, 4 * 256)); }
    this.cap.push(rec);
  }
  // D3D9 gamma ramp (SetGammaRamp): applied in the present pass through a 256x1 RGBA8 LUT; identity ramps keep the plain blit
  readbackTexel(kind, id, level, face, dstId) {                  // GetRenderTargetData answer: texel (0,0) of the source as a D3D-order 32-bit value (R32F: raw float bits)
    // Asynchronous: readPixels into a pixel-pack buffer + fence, answered a few frames later (pollReadbacks). The host never waits for the answer
    // (it just stores the value into the destination surface), and a synchronous readPixels + checkFramebufferStatus stalled the whole GPU pipeline
    // every frame (the game reads a 1x1 luminance target each frame): ~30% of the page's time in the race profile.
    const gl = this.gl, tg = this.surfaceTarget({ kind, id, level, face }); if (!tg || !this.sendInput) return;
    const o = tg.owner; if (o && o.msDirty) this.resolveMS(o);
    const fb = this.rbFbo || (this.rbFbo = gl.createFramebuffer()); gl.bindFramebuffer(gl.READ_FRAMEBUFFER, fb);
    if (tg.rb) gl.framebufferRenderbuffer(gl.READ_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, tg.rb); else gl.framebufferTexture2D(gl.READ_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, tg.target, tg.tex, tg.level);
    const t = tg.tex && this.textures.get(id), flt = !!(t && t.info && t.info.float);
    if (flt && t.fmt !== 114) this.warn('rb' + t.fmt, 'GetRenderTargetData of float format ' + t.fmt + ' only supports R32F');
    const pool = this.rbPool || (this.rbPool = []); let pbo = pool.pop();
    if (!pbo) { pbo = gl.createBuffer(); gl.bindBuffer(gl.PIXEL_PACK_BUFFER, pbo); gl.bufferData(gl.PIXEL_PACK_BUFFER, 16, gl.STREAM_READ); } else gl.bindBuffer(gl.PIXEL_PACK_BUFFER, pbo);
    if (flt) gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.FLOAT, 0); else gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, 0);
    gl.bindBuffer(gl.PIXEL_PACK_BUFFER, null); gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null);
    (this.rbPending || (this.rbPending = [])).push({ pbo, fence: gl.fenceSync(gl.SYNC_GPU_COMMANDS_COMPLETE, 0), flt, raw: flt && t.fmt === 114, dstId });
  }
  pollReadbacks() {
    const q = this.rbPending; if (!q || !q.length) return; const gl = this.gl;
    while (q.length) {
      const r = q[0], st = gl.clientWaitSync(r.fence, 0, 0); if (st === gl.TIMEOUT_EXPIRED) break; q.shift(); gl.deleteSync(r.fence);
      let bits = 0; gl.bindBuffer(gl.PIXEL_PACK_BUFFER, r.pbo);
      if (st !== gl.WAIT_FAILED) { if (r.flt) { const f = new Float32Array(4); gl.getBufferSubData(gl.PIXEL_PACK_BUFFER, 0, f); bits = r.raw ? new Uint32Array(f.buffer)[0] : 0; }
        else { const b = new Uint8Array(4); gl.getBufferSubData(gl.PIXEL_PACK_BUFFER, 0, b); bits = ((b[3] << 24) | (b[0] << 16) | (b[1] << 8) | b[2]) | 0; } }
      gl.bindBuffer(gl.PIXEL_PACK_BUFFER, null); this.rbPool.push(r.pbo); this.sendInput(10, r.dstId, 0, 0, bits | 0);
    }
  }
  setGamma(r) {
    const gl = this.gl; let id = true; for (let i = 0; i < 768; i++) if (Math.abs(r[i] - (i & 255) * 257) > 128) { id = false; break; }
    this.gammaId = id; if (id) return; const px = new Uint8Array(256 * 4); for (let i = 0; i < 256; i++) { px[4 * i] = r[i] >> 8; px[4 * i + 1] = r[256 + i] >> 8; px[4 * i + 2] = r[512 + i] >> 8; px[4 * i + 3] = 255; }
    if (!this.gammaTex) { this.gammaTex = gl.createTexture(); gl.bindTexture(gl.TEXTURE_2D, this.gammaTex); gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA8, 256, 1); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE); }
    gl.bindTexture(gl.TEXTURE_2D, this.gammaTex); gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 256, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
    if (!this.gammaProg) {
      const mk = (t, s) => { const sh = gl.createShader(t); gl.shaderSource(sh, s); gl.compileShader(sh); return sh; }, prog = gl.createProgram();
      gl.attachShader(prog, mk(gl.VERTEX_SHADER, '#version 300 es\nout vec2 uv;\nvoid main() { vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); uv = vec2(p.x, 1.0 - p.y); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }'));
      gl.attachShader(prog, mk(gl.FRAGMENT_SHADER, '#version 300 es\nprecision highp float;\nuniform sampler2D src; uniform sampler2D lut; in vec2 uv; out vec4 o;\nvoid main() { vec3 c = texture(src, uv).rgb; vec3 q = c * (255.0 / 256.0) + 0.5 / 256.0; o = vec4(texture(lut, vec2(q.r, 0.5)).r, texture(lut, vec2(q.g, 0.5)).g, texture(lut, vec2(q.b, 0.5)).b, 1.0); }'));
      gl.linkProgram(prog); this.gammaProg = prog; this.gammaVao = gl.createVertexArray();
    }
  }
  present() { this.auditPresent = (this.auditPresent || 0) + 1;
    const gl = this.gl;
    this.pollReadbacks();
    if (this.mirrorMode !== 'full') {
      const now = performance.now(); if (this.mirrorLast) { const iv = now - this.mirrorLast; this.mirrorIv += (Math.min(iv, 100) - this.mirrorIv) * 0.08; } this.mirrorLast = now;
      for (const t of this.mirrorTouched) { t.mskip = t.mdc >= 150 && ((t.w === 576 && t.h === 96) || (t.w === 256 && t.h === 256)); t.mdc = 0; } this.mirrorTouched.length = 0;
      this.mirrorFrame++; const was = !!this.stats.mirrorActive, active = this.mirrorMode === 'half' || (was ? this.mirrorIv > 17.2 : this.mirrorIv > 18.0);      // hysteresis for auto
      this.mirrorOn = active && (this.mirrorFrame & 1) === 1; this.stats.mirrorActive = active;
    }
    if (this.diag) { let d = 0; for (const k in this.rtCount) d += this.rtCount[k]; this.diagFrames.push({ d, t: performance.now(), rts: this.rtCount, gl: Object.assign({}, this.glCalls) }); for (const k in this.glCalls) this.glCalls[k] = 0; if (this.diagFrames.length > 4000) this.diagFrames.shift(); this.rtCount = {}; }
    this.dn = 0;
    { const fd = this.stats.frameDraws; if (this.cap) { window.__cap = this.cap; this.cap = null; window.__capNext = false; console.log('renderer: capture done'); }
      else if (!window.__cap && (window.__capNext || (window.__capAuto && fd >= 300))) this.cap = []; this.capTexMap = null; } if (this.dbg && (this.dbgF = (this.dbgF || 0) + 1) % 600 === 0) { for (const id of (window.DBG_RT || [2795, 2798])) this.dbgReadRT(id); } this.stats.frameDraws = this.stats.draws - (this.lastDraws || 0); this.lastDraws = this.stats.draws;
    if (!this.bb) return;
    if (this.frameDeferred) { this.frameDeferred = false; if (++this.holdRun <= 30) { this.stats.held++; return; } }     // some draw was skipped (program compiling): keep showing the last complete frame, but never hold longer than 30 frames
    this.holdRun = 0;
    gl.disable(gl.SCISSOR_TEST); gl.colorMask(true, true, true, true); if (this.bb.msDirty) this.resolveMS(this.bb);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, this.blitFbo); gl.framebufferTexture2D(gl.READ_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, this.bb.tex, 0); gl.readBuffer(gl.COLOR_ATTACHMENT0);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, null);
    if (this.gammaTex && !this.gammaId && this.gammaProg) {          // gamma-ramp present pass (the source is stored top-down, uv.y = 1 - p.y selects the flipped row)
      gl.bindFramebuffer(gl.FRAMEBUFFER, null); gl.viewport(0, 0, this.bb.w, this.bb.h); gl.disable(gl.DEPTH_TEST); gl.disable(gl.BLEND); gl.disable(gl.CULL_FACE); gl.useProgram(this.gammaProg);
      gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, this.bb.tex); gl.bindSampler(0, null); gl.uniform1i(gl.getUniformLocation(this.gammaProg, 'src'), 0);
      gl.activeTexture(gl.TEXTURE1); gl.bindTexture(gl.TEXTURE_2D, this.gammaTex); gl.bindSampler(1, null); gl.uniform1i(gl.getUniformLocation(this.gammaProg, 'lut'), 1);
      gl.bindVertexArray(this.gammaVao); gl.drawArrays(gl.TRIANGLES, 0, 3); gl.bindVertexArray(this.vao); this.frameDone(); return;
    }
    gl.blitFramebuffer(0, 0, this.bb.w, this.bb.h, 0, this.bb.h, this.bb.w, 0, gl.COLOR_BUFFER_BIT, gl.NEAREST);     // top-down storage -> canvas (flip)
    this.frameDone();
  }
}
// ---- texture data-integrity audit (host: BLUR_TEXREF=<frames>, d3d9.c texref_maybe) ------------------------------------------------------------------
// message 85 carries the CURRENT guest-memory bytes of one texture level/face, 86 ends a batch. For each level an independent reference decoder (this file, D3D9
// sampling semantics: V8U8 -> (u,v,1,1), A8 -> (0,0,0,a), L8 -> (l,l,l,1) ...) produces the expected RGBA floats, the real GL texture is read back through a
// NEAREST textureLod/texelFetch pass into an RGBA32F target, and the two are compared. Result in window.__audit (and console).
(function () {
  const P = D3D9Renderer.prototype, f16 = (h) => { const s = (h & 0x8000) ? -1 : 1, e = (h >> 10) & 31, m = h & 1023; return e === 0 ? s * m * 5.960464477539063e-8 : e === 31 ? (m ? NaN : s * Infinity) : s * Math.pow(2, e - 15) * (1 + m / 1024); };
  const r565 = (c) => [((c >> 11) & 31) * 255 / 31, ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31];
  function dxtColors(d, o, three) {
    const c0 = d[o] | d[o + 1] << 8, c1 = d[o + 2] | d[o + 3] << 8, a = r565(c0), b = r565(c1), t = [a, b];
    if (c0 > c1 || three) { t.push(a.map((v, i) => (2 * v + b[i]) / 3), a.map((v, i) => (v + 2 * b[i]) / 3)); } else { t.push(a.map((v, i) => (v + b[i]) / 2), [0, 0, 0]); }
    return { t, bits: d[o + 4] | d[o + 5] << 8 | d[o + 6] << 16 | d[o + 7] << 24, trans: !(c0 > c1) };
  }
  // expected RGBA (0..1 floats) of one level; data = guest bytes, pitch from the host (tight rows are used: the host sends rows at the D3D pitch)
  P.auditDecode = function (fmt, w, h, d, data, pitch) {
    const n = w * h * d, out = new Float32Array(n * 4), u8 = (i) => data[i];
    const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
    const rowPx = (fmt === FMT_DXT1 || fmt === FMT_DXT3 || fmt === FMT_DXT5);
    if (rowPx) {
      const bb = fmt === FMT_DXT1 ? 8 : 16, bw = (w + 3) >> 2, bh = (h + 3) >> 2;
      for (let z = 0; z < d; z++) for (let by = 0; by < bh; by++) for (let bx = 0; bx < bw; bx++) {
        const o = (z * bw * bh + by * bw + bx) * bb; if (o + bb > data.length) continue;
        const cc = dxtColors(data, o + (fmt === FMT_DXT1 ? 0 : 8), fmt !== FMT_DXT1);
        let alpha = null;
        if (fmt === FMT_DXT3) { alpha = (i) => ((data[o + (i >> 1)] >> ((i & 1) * 4)) & 15) * 17; }
        else if (fmt === FMT_DXT5) { const a0 = data[o], a1 = data[o + 1], t = [a0, a1]; if (a0 > a1) for (let i = 1; i <= 6; i++) t.push(((7 - i) * a0 + i * a1) / 7); else { for (let i = 1; i <= 4; i++) t.push(((5 - i) * a0 + i * a1) / 5); t.push(0, 255); }
          let bits = 0n; for (let i = 0; i < 6; i++) bits |= BigInt(data[o + 2 + i]) << BigInt(8 * i); alpha = (i) => t[Number((bits >> BigInt(3 * i)) & 7n)]; }
        for (let py = 0; py < 4; py++) for (let px = 0; px < 4; px++) { const x = bx * 4 + px, y = by * 4 + py; if (x >= w || y >= h) continue; const i = py * 4 + px, ci = (cc.bits >>> (2 * i)) & 3, c = cc.t[ci];
          const q = ((z * h + y) * w + x) * 4; out[q] = c[0] / 255; out[q + 1] = c[1] / 255; out[q + 2] = c[2] / 255; out[q + 3] = fmt === FMT_DXT1 ? (cc.trans && ci === 3 ? 0 : 1) : alpha(i) / 255; }
      }
      return out;
    }
    const bpp = { 21: 4, 22: 4, 23: 2, 25: 2, 26: 2, 28: 1, 50: 1, 51: 2, 60: 2, 63: 4, 34: 4, 36: 8, 35: 4, 31: 4, 111: 2, 112: 4, 113: 8, 114: 4, 115: 8, 116: 16, 81: 2 }[fmt];
    if (!bpp) return null;
    for (let z = 0; z < d; z++) for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      const o = z * pitch * h + y * pitch + x * bpp, q = ((z * h + y) * w + x) * 4; if (o + bpp > data.length) continue; let r = 0, g = 0, b = 0, a = 1;
      switch (fmt) {
        case 21: b = u8(o) / 255; g = u8(o + 1) / 255; r = u8(o + 2) / 255; a = u8(o + 3) / 255; break;
        case 22: b = u8(o) / 255; g = u8(o + 1) / 255; r = u8(o + 2) / 255; break;
        case 23: { const v = dv.getUint16(o, true); r = (v >> 11) / 31; g = ((v >> 5) & 63) / 63; b = (v & 31) / 31; break; }
        case 25: { const v = dv.getUint16(o, true); a = v >> 15; r = ((v >> 10) & 31) / 31; g = ((v >> 5) & 31) / 31; b = (v & 31) / 31; break; }
        case 26: { const v = dv.getUint16(o, true); a = (v >> 12) / 15; r = ((v >> 8) & 15) / 15; g = ((v >> 4) & 15) / 15; b = (v & 15) / 15; break; }
        case 28: r = g = b = 0; a = u8(o) / 255; break;
        case 50: r = g = b = u8(o) / 255; break;
        case 51: r = g = b = u8(o) / 255; a = u8(o + 1) / 255; break;
        case 60: r = Math.max(-1, dv.getInt8(o) / 127); g = Math.max(-1, dv.getInt8(o + 1) / 127); b = 1; a = 1; break;
        case 63: r = Math.max(-1, dv.getInt8(o) / 127); g = Math.max(-1, dv.getInt8(o + 1) / 127); b = Math.max(-1, dv.getInt8(o + 2) / 127); a = Math.max(-1, dv.getInt8(o + 3) / 127); break;
        case 34: r = dv.getUint16(o, true) / 65535; g = dv.getUint16(o + 2, true) / 65535; b = 1; break;
        case 81: r = dv.getUint16(o, true) / 65535; break;
        case 36: r = dv.getUint16(o, true) / 65535; g = dv.getUint16(o + 2, true) / 65535; b = dv.getUint16(o + 4, true) / 65535; a = dv.getUint16(o + 6, true) / 65535; break;
        case 35: { const v = dv.getUint32(o, true); a = (v >>> 30) / 3; r = ((v >>> 20) & 1023) / 1023; g = ((v >>> 10) & 1023) / 1023; b = (v & 1023) / 1023; break; }
        case 31: { const v = dv.getUint32(o, true); a = (v >>> 30) / 3; b = ((v >>> 20) & 1023) / 1023; g = ((v >>> 10) & 1023) / 1023; r = (v & 1023) / 1023; break; }
        case 111: r = f16(dv.getUint16(o, true)); break;
        case 112: r = f16(dv.getUint16(o, true)); g = f16(dv.getUint16(o + 2, true)); break;
        case 113: r = f16(dv.getUint16(o, true)); g = f16(dv.getUint16(o + 2, true)); b = f16(dv.getUint16(o + 4, true)); a = f16(dv.getUint16(o + 6, true)); break;
        case 114: r = dv.getFloat32(o, true); break;
        case 115: r = dv.getFloat32(o, true); g = dv.getFloat32(o + 4, true); break;
        case 116: r = dv.getFloat32(o, true); g = dv.getFloat32(o + 4, true); b = dv.getFloat32(o + 8, true); a = dv.getFloat32(o + 12, true); break;
      }
      out[q] = r; out[q + 1] = g; out[q + 2] = b; out[q + 3] = a;
    }
    return out;
  };
  // GL side: read the level back through the sampling path -> Float32Array RGBA (null if impossible)
  P.auditReadGL = function (t, face, level, w, h, d) {
    const gl = this.gl; if (!this.ext.cbf) return null;
    if (!this.auditProg) {
      const vs = '#version 300 es\nout vec2 uv; void main() { vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); uv = p; gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }';
      const mk = (body, smp) => { const p = gl.createProgram(), c = (ty, src) => { const s = gl.createShader(ty); gl.shaderSource(s, src); gl.compileShader(s); if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s)); gl.attachShader(p, s); };
        c(gl.VERTEX_SHADER, vs); c(gl.FRAGMENT_SHADER, '#version 300 es\nprecision highp float; precision highp int; precision highp sampler3D;\n' + smp + '\nuniform int lod; uniform ivec3 dim; uniform int face;\nout vec4 o;\nvoid main() { ivec2 px = ivec2(gl_FragCoord.xy);\n' + body + '\n}'); gl.linkProgram(p); if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p)); return p; };
      this.auditProg = {
        2: mk('o = texelFetch(s, px, lod);', 'uniform sampler2D s;'),
        3: mk('o = texelFetch(s, ivec3(px.x, px.y % dim.y, px.y / dim.y), lod);', 'uniform sampler3D s;'),
        1: mk('vec2 q = (vec2(px) + 0.5) / vec2(dim.xy) * 2.0 - 1.0; vec3 d; if (face == 0) d = vec3(1.0, -q.y, -q.x); else if (face == 1) d = vec3(-1.0, -q.y, q.x); else if (face == 2) d = vec3(q.x, 1.0, q.y); else if (face == 3) d = vec3(q.x, -1.0, -q.y); else if (face == 4) d = vec3(q.x, -q.y, 1.0); else d = vec3(-q.x, -q.y, -1.0); o = textureLod(s, d, float(lod));', 'uniform samplerCube s;') };
      this.auditSmp = gl.createSampler(); gl.samplerParameteri(this.auditSmp, gl.TEXTURE_MIN_FILTER, gl.NEAREST_MIPMAP_NEAREST); gl.samplerParameteri(this.auditSmp, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
      gl.samplerParameteri(this.auditSmp, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE); gl.samplerParameteri(this.auditSmp, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE); gl.samplerParameteri(this.auditSmp, gl.TEXTURE_WRAP_R, gl.CLAMP_TO_EDGE);
      this.auditVao = gl.createVertexArray();
    }
    const H = h * d, rt = gl.createTexture(); gl.bindTexture(gl.TEXTURE_2D, rt); gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA32F, w, H);
    const fb = gl.createFramebuffer(); gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, fb); gl.framebufferTexture2D(gl.DRAW_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, rt, 0);
    const kind = t.kind === 1 ? 1 : t.kind === 2 ? 3 : 2, prog = this.auditProg[kind]; gl.useProgram(prog); gl.activeTexture(gl.TEXTURE15); gl.bindTexture(t.target, t.tex); gl.bindSampler(15, this.auditSmp);
    gl.uniform1i(gl.getUniformLocation(prog, 's'), 15); gl.uniform1i(gl.getUniformLocation(prog, 'lod'), level); gl.uniform3i(gl.getUniformLocation(prog, 'dim'), w, h, d); gl.uniform1i(gl.getUniformLocation(prog, 'face'), face);
    gl.disable(gl.BLEND); gl.disable(gl.DEPTH_TEST); gl.disable(gl.SCISSOR_TEST); gl.disable(gl.CULL_FACE); gl.disable(gl.STENCIL_TEST); gl.colorMask(true, true, true, true); gl.viewport(0, 0, w, H);
    gl.bindVertexArray(this.auditVao); gl.drawArrays(gl.TRIANGLES, 0, 3);
    const px = new Float32Array(w * H * 4); gl.bindFramebuffer(gl.READ_FRAMEBUFFER, fb); gl.readBuffer(gl.COLOR_ATTACHMENT0); gl.readPixels(0, 0, w, H, gl.RGBA, gl.FLOAT, px);
    gl.bindSampler(15, null); gl.bindFramebuffer(gl.FRAMEBUFFER, null); gl.deleteFramebuffer(fb); gl.deleteTexture(rt); this.fbBound = null; this.stateDirty = true;
    return gl.getError() ? null : px;
  };
  P.auditMsg = function (u, data) {
    if (!this.auditRefs) this.auditRefs = [];
    this.auditRefs.push({ id: u(0), face: u(1), level: u(2), w: u(3), h: u(4), d: u(5), fmt: u(6), pitch: u(7), usage: u(8) & 255, pool: u(8) >> 8, kind: u(9), nlev: u(10), data: data.slice() });
  };
  P.auditRun = function (frame) {
    const refs = this.auditRefs || []; this.auditRefs = []; const res = [], byTex = new Map(), pc = this.auditPresent || 0;
    for (const r of refs) {
      const t = this.textures.get(r.id), key = r.id; let e = byTex.get(key);
      if (!e) { e = { id: r.id, fmt: r.fmt, fmtHex: r.fmt > 255 ? '0x' + r.fmt.toString(16) : r.fmt, kind: ['', '', '', '2d', 'cube', 'vol'][r.kind], size: `${r.w}x${r.h}${r.d > 1 ? 'x' + r.d : ''}`, nlev: r.nlev, pool: r.pool, usage: r.usage, used: t ? (t.usedAt || -1) : null, levels: [], bad: 0 }; byTex.set(key, e); res.push(e); }
      if (!t) { e.missing = true; continue; }
      const exp = this.auditDecode(r.fmt, r.w, r.h, r.d, r.data, r.pitch); if (!exp) { e.levels.push({ l: r.level, f: r.face, skip: 'no decoder' }); continue; }
      const got = this.auditReadGL(t, r.face, r.level, r.w, r.h, r.d); if (!got) { e.levels.push({ l: r.level, f: r.face, skip: 'no GL readback' }); continue; }
      const comp = r.fmt === FMT_DXT1 || r.fmt === FMT_DXT3 || r.fmt === FMT_DXT5, tol = comp ? 0.03 : r.fmt === 23 || r.fmt === 26 || r.fmt === 25 || r.fmt === 36 || r.fmt === 35 ? 0.02 : (r.fmt >= 111 && r.fmt <= 116 ? 0.003 : 0.012);
      let nbad = 0, first = null, maxd = 0, n = r.w * r.h * r.d; const sumE = [0, 0, 0, 0], sumG = [0, 0, 0, 0];
      for (let i = 0; i < n; i++) { let bad = false; for (let c = 0; c < 4; c++) { const ev = exp[4 * i + c], gv = got[4 * i + c]; sumE[c] += ev; sumG[c] += gv; const dd = Math.abs(ev - gv) / (r.fmt >= 111 && r.fmt <= 116 ? Math.max(1, Math.abs(ev)) : 1); if (!(dd <= tol)) bad = true; if (dd > maxd) maxd = dd; }
        if (bad) { nbad++; if (!first) first = { x: i % r.w, y: ((i / r.w) | 0) % r.h, z: (i / (r.w * r.h)) | 0, exp: Array.from(exp.subarray(4 * i, 4 * i + 4)).map((v) => +v.toFixed(3)), got: Array.from(got.subarray(4 * i, 4 * i + 4)).map((v) => +v.toFixed(3)) }; } }
      const lv = { l: r.level, f: r.face, frac: +(nbad / n).toFixed(4), maxd: +maxd.toFixed(3), mean: sumE.map((v) => +(v / n).toFixed(3)), meanGL: sumG.map((v) => +(v / n).toFixed(3)) }; if (first) lv.first = first;
      if (nbad / n > 0.001) e.bad++; e.levels.push(lv);
    }
    const bad = res.filter((e) => e.bad || e.missing), used = res.filter((e) => e.used != null && e.used >= pc - 3);
    window.__audit = { frame, nTex: res.length, nBad: bad.length, nUsed: used.length, nUsedBad: used.filter((e) => e.bad || e.missing).length, bad, all: res };
    console.log(`[audit] frame ${frame}: ${res.length} textures, ${bad.length} with mismatching levels, sampled recently ${used.length} (bad ${window.__audit.nUsedBad})`);
  };
})();

window.D3D9Renderer = D3D9Renderer;
