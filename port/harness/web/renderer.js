'use strict';
// A Direct3D 9 device implemented on WebGL2. It consumes the command stream produced by the emulation harness
// (port/harness/gfx.c) and mirrors D3D9 device state. Conventions:
//  * every render target (including the D3D backbuffer) is stored top-down: D3D row 0 == GL row 0, so vertex shaders
//    flip clip-space Y; the backbuffer is blitted (flipped) to the canvas on Present.
//  * D3D9 half-pixel offset is applied in the vertex shader; D3D z range [0,1] is remapped to GL's [-1,1].
//  * shaders arrive as GLSL ES 3.00 translated from D3D bytecode; varyings are linked by D3D semantic.
const G = { TEX_CREATE: 20, TEX_UPLOAD: 21, SURF_CREATE: 22, BUF_CREATE: 30, BUF_UPLOAD: 31, SHADER: 40, VDECL: 41,
  SET_RS: 50, SET_SS: 51, SET_TEX: 52, SET_VS: 53, SET_PS: 54, SET_VDECL: 55, SET_FVF: 56, SET_STREAM: 57, SET_INDICES: 58, SET_VSC: 59, SET_PSC: 60,
  SET_VIEWPORT: 61, SET_SCISSOR: 62, SET_RT: 63, SET_DS: 64, DRAW: 70, DRAW_UP: 71, CLEAR: 72, PRESENT: 73, STRETCH: 74 };
const FOURCC = (s) => s.charCodeAt(0) | s.charCodeAt(1) << 8 | s.charCodeAt(2) << 16 | s.charCodeAt(3) << 24;
const FMT_DXT1 = FOURCC('DXT1'), FMT_DXT3 = FOURCC('DXT3'), FMT_DXT5 = FOURCC('DXT5'), FMT_INTZ = FOURCC('INTZ'), FMT_DF24 = FOURCC('DF24'), FMT_DF16 = FOURCC('DF16');

class D3D9Renderer {
  constructor(canvas, log) {
    this.dbg = /[?&]dbg/.test(location.search);
    this.canvas = canvas; this.log = log || console.log;
    const gl = this.gl = canvas.getContext('webgl2', { antialias: false, alpha: false, depth: false, stencil: false, preserveDrawingBuffer: false, powerPreference: 'high-performance' });
    if (!gl) throw new Error('WebGL2 not available');
    this.ext = {
      cbf: gl.getExtension('EXT_color_buffer_float'), cbh: gl.getExtension('EXT_color_buffer_half_float'),
      s3tc: gl.getExtension('WEBGL_compressed_texture_s3tc'), aniso: gl.getExtension('EXT_texture_filter_anisotropic'),
      flin: gl.getExtension('OES_texture_float_linear'),
    };
    this.textures = new Map(); this.surfaces = new Map(); this.buffers = new Map(); this.shaders = new Map(); this.vdecls = new Map();
    this.programs = new Map(); this.fbos = new Map(); this.warned = new Set();
    this.rs = new Uint32Array(256); this.ss = Array.from({ length: 32 }, () => new Uint32Array(16));
    this.tex = new Array(16).fill(0); this.streams = Array.from({ length: 16 }, () => ({ buf: 0, offset: 0, stride: 0 }));
    this.ib = 0; this.vdecl = 0; this.fvf = 0; this.vs = 0; this.ps = 0;
    this.vsc = new Float32Array(256 * 4); this.psc = new Float32Array(224 * 4); this.vscDirty = this.pscDirty = true;
    this.vp = { x: 0, y: 0, w: 1280, h: 720, minz: 0, maxz: 1 }; this.scissor = { l: 0, t: 0, r: 1280, b: 720 };
    this.rt = [{ kind: 3 }, { kind: 3 }, { kind: 3 }, { kind: 3 }]; this.ds = { kind: 3 };
    this.stats = { draws: 0, frameDraws: 0, clears: 0, progs: 0, shaders: 0, skipped: 0, tex: 0 };
    this.initDefaults(); this.initGL();
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
    this.samplers = Array.from({ length: 16 }, () => gl.createSampler());
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
  warn(k, msg) { if (!this.warned.has(k)) { this.warned.add(k); this.log('[renderer] ' + msg); } }
  statLine() { const s = this.stats; return `draws/frame ${s.frameDraws}  progs ${s.progs}  shaders ${s.shaders}  tex ${this.textures.size}  skipped ${s.skipped}`; }

  // ---- message pump ---------------------------------------------------------------------------------------------------------------------------
  feed(ab) {
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
      case G.TEX_CREATE: this.texCreate(u(0), u(1), u(2), u(3), u(4), u(5), u(6), u(7)); break;
      case G.TEX_UPLOAD: this.texUpload(u(0), u(1), u(2), u(3), u(4), u(5), u(6), u(7), new Uint8Array(ab, p + 32, len - 32).slice()); break;
      case G.SURF_CREATE: this.surfCreate(u(0), u(1), u(2), u(3), u(4), u(5)); break;
      case G.BUF_CREATE: this.bufCreate(u(0), u(1), u(2), u(3), u(4)); break;
      case G.BUF_UPLOAD: { const b = this.buffers.get(u(0)); if (b) { const gl = this.gl, tg = b.kind === 1 ? gl.ELEMENT_ARRAY_BUFFER : gl.ARRAY_BUFFER; gl.bindBuffer(tg, b.buf); gl.bufferSubData(tg, u(1), new Uint8Array(ab, p + 8, len - 8)); } break; }
      case G.SHADER: this.shaderCreate(ab, p, len, u); break;
      case G.VDECL: this.vdeclCreate(u(0), u(1), new DataView(ab, p + 8, len - 8)); break;
      case G.SET_RS: if (u(0) < 256) this.rs[u(0)] = u(1); break;
      case G.SET_SS: if (u(0) < 32 && u(1) < 16) { this.ss[u(0)][u(1)] = u(2); this.samplerDirty = true; } break;
      case G.SET_TEX: this.tex[u(0)] = u(1); break;
      case G.SET_VS: this.vs = u(0); break; case G.SET_PS: this.ps = u(0); break;
      case G.SET_VDECL: this.vdecl = u(0); break; case G.SET_FVF: this.fvf = u(0); break;
      case G.SET_STREAM: this.streams[u(0)] = { buf: u(1), offset: u(2), stride: u(3) }; break;
      case G.SET_INDICES: this.ib = u(0); break;
      case G.SET_VSC: { const n = u(1); this.vsc.set(new Float32Array(ab.slice(p + 8, p + 8 + 16 * n)), u(0) * 4); this.vscDirty = true; break; }
      case G.SET_PSC: { const n = u(1); this.psc.set(new Float32Array(ab.slice(p + 8, p + 8 + 16 * n)), u(0) * 4); this.pscDirty = true; break; }
      case G.SET_VIEWPORT: this.vp = { x: u(0), y: u(1), w: u(2), h: u(3), minz: f(4), maxz: f(5) }; break;
      case G.SET_SCISSOR: this.scissor = { l: u(0), t: u(1), r: u(2), b: u(3) }; break;
      case G.SET_RT: this.rt[u(0)] = { kind: u(1), id: u(2), level: u(3), face: u(4) }; break;
      case G.SET_DS: this.ds = { kind: u(0), id: u(1), level: u(2), face: u(3) }; break;
      case G.DRAW: this.draw(u(0), u(1), u(2), u(3), u(4), u(5), u(6)); break;
      case G.DRAW_UP: this.drawUP(u(0), u(1), u(2), u(3), u(4), u(5), u(6), new Uint8Array(ab, p + 28, len - 28).slice()); break;
      case G.CLEAR: this.clear(u(0), u(1), f(2), u(3), u(4), new Int32Array(ab.slice(p + 20, p + 20 + 16 * u(4)))); break;
      case G.PRESENT: this.present(); return true;
      case G.STRETCH: this.stretch(Array.from({ length: 19 }, (_, i) => u(i))); break;
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
      case 60: return { internal: gl.RG8_SNORM, format: gl.RG, type: gl.BYTE };
      case 63: return { internal: gl.RGBA8_SNORM, format: gl.RGBA, type: gl.BYTE };
      case 34: return { internal: gl.RG16F, format: gl.RG, type: gl.UNSIGNED_SHORT, conv: 'rg16' };
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
  texCreate(id, kind, w, h, d, levels, fmt, usage) {
    const gl = this.gl, info = this.fmtInfo(fmt), target = kind === 1 ? gl.TEXTURE_CUBE_MAP : kind === 2 ? gl.TEXTURE_3D : gl.TEXTURE_2D;
    const t = { id, kind, w, h, d, levels, fmt, usage, info, target, tex: gl.createTexture(), isRT: !!(usage & 1), isDS: !!(usage & 2) };
    gl.bindTexture(target, t.tex);
    if (info.compressed && !info.internal) { this.warn('s3tc', 'S3TC unsupported by this browser: DXT textures will be black'); t.info = { internal: gl.RGBA8, format: gl.RGBA, type: gl.UNSIGNED_BYTE, conv: 'none' }; t.sw = true; }
    const ii = t.info;
    if (kind === 2) gl.texStorage3D(target, levels, ii.internal, w, h, d); else gl.texStorage2D(target, levels, ii.internal, w, h);
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
      case 'rg16': { const s = new Uint16Array(data.buffer, 0, n * 2), o = new Uint16Array(n * 2); const h2f = (x) => x; for (let i = 0; i < n * 2; i++) o[i] = s[i]; return o; }
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
    gl.bindTexture(t.target, t.tex); gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    const tgt = t.kind === 1 ? gl.TEXTURE_CUBE_MAP_POSITIVE_X + face : t.target;
    if (t.info.compressed) { if (t.info.internal) { if (t.kind === 2) gl.compressedTexSubImage3D(t.target, level, 0, 0, 0, w, h, d, t.info.internal, data); else gl.compressedTexSubImage2D(tgt, level, 0, 0, w, h, t.info.internal, data); } return; }
    const arr = this.convert(fmt, t.info, w, h, d, data);
    if (t.kind === 2) gl.texSubImage3D(t.target, level, 0, 0, 0, w, h, d, t.info.format, t.info.type, arr);
    else gl.texSubImage2D(tgt, level, 0, 0, w, h, t.info.format, t.info.type, arr);
  }
  surfCreate(id, w, h, fmt, kind, ms) {               // kind 0 backbuffer, 1 render target, 2 depth-stencil
    const gl = this.gl, info = this.fmtInfo(fmt), s = { id, w, h, fmt, kind, ms, info, rb: gl.createRenderbuffer(), tex: null };
    gl.bindRenderbuffer(gl.RENDERBUFFER, s.rb);
    if (kind === 0) {                                                     // backbuffer is a texture so it can be blitted/sampled; depth comes from the DS surface
      s.tex = gl.createTexture(); gl.bindTexture(gl.TEXTURE_2D, s.tex); gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA8, w, h); this.bb = s; this.canvas.width = w; this.canvas.height = h;
    } else if (ms > 1) gl.renderbufferStorageMultisample(gl.RENDERBUFFER, Math.min(ms, gl.getParameter(gl.MAX_SAMPLES)), info.internal, w, h);
    else gl.renderbufferStorage(gl.RENDERBUFFER, info.internal, w, h);
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
    const id = u(0), stage = u(1), glslLen = u(2), nin = u(3), nout = u(4), nsamp = u(5), b = new Uint8Array(ab, p + 24, len - 24);
    const tab = (o, n) => Array.from({ length: n }, (_, i) => ({ reg: b[o + 4 * i], usage: b[o + 4 * i + 1], idx: b[o + 4 * i + 2], tex: b[o + 4 * i + 3] }));
    const ins = tab(0, nin), outs = tab(4 * nin, nout), samps = tab(4 * (nin + nout), nsamp), gOff = 4 * (nin + nout + nsamp);
    this.shaders.set(id, { id, stage, ins, outs, samps, glsl: new TextDecoder().decode(b.subarray(gOff, gOff + glslLen)) }); this.stats.shaders++;
  }
  vname(e) { return `vary_${e.usage}_${e.idx}`; }
  buildProgram(vsId, psId, decl) {
    const gl = this.gl, vs = this.shaders.get(vsId), ps = this.shaders.get(psId); if (!vs || !ps) return null;
    let vsrc = vs.glsl, psrc = ps.glsl;
    const prec = (s) => s.replace('precision mediump float;', 'precision highp float;');
    vsrc = prec(vsrc); psrc = prec(psrc);
    // link varyings by semantic
    const psIn = new Map(); for (const e of ps.ins) if (!(e.tex > 0 && e.usage === 0 && false)) psIn.set(this.vname(e), e);
    for (const e of vs.outs) if (e.usage !== 0 && e.usage !== 4) vsrc = vsrc.replace(new RegExp(`\\bo${e.reg}\\b`, 'g'), this.vname(e));
    for (const e of ps.ins) if (e.usage !== 0 || true) psrc = psrc.replace(new RegExp(`\\bv${e.reg}\\b`, 'g'), this.vname(e));
    // reconcile varying widths: SPIRV-Cross trims each stage's varying to the components it uses, GL needs identical types
    const tsize = (t) => ({ float: 1, vec2: 2, vec3: 3, vec4: 4 }[t] || 4), tname = (n) => (n === 1 ? 'float' : 'vec' + n);
    const vtypes = new Map(), reDecl = (kw) => new RegExp(`\\b${kw}\\s+(?:highp\\s+|mediump\\s+)?(float|vec[234])\\s+(vary_\\d+_\\d+)\\s*;`, 'g');
    for (const m of vsrc.matchAll(reDecl('out'))) vtypes.set(m[2], m[1]);
    let psFix = '';
    psrc = psrc.replace(reDecl('in'), (all, pt, name) => {
      const vt = vtypes.get(name); if (!vt || vt === pt) return all;
      const vn = tsize(vt), pn = tsize(pt), sw = 'xyzw'.slice(0, pn);
      psFix += pn <= vn ? `    ${name}_loc = ${name}${pn === vn ? '' : '.' + sw};\n` : `    ${name}_loc = ${pt}(${name}${Array(pn - vn).fill(', 0.0').join('')});\n`;
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
    let colorRegs = [];
    if (decl) for (const e of vs.ins) { const el = decl.elems.find((x) => x.usage === e.usage && x.idx === e.idx); if (el && el.type === 4) colorRegs.push(e.reg); }
    let attrFix = '';
    for (const r of colorRegs) { const re = new RegExp(`(layout\\(location = ${r}\\) in )(?:highp )?vec4 v${r};`); if (re.test(vsrc)) { vsrc = vsrc.replace(re, `$1vec4 v${r}_in;\nvec4 v${r};`); attrFix += `v${r} = v${r}_in.zyxw;\n`; } }
    const addAfterVersion = (s, decl) => s.replace(/(#version 300 es\n)/, `$1${decl}`);
    vsrc = addAfterVersion(vsrc, 'precision highp float;\nprecision highp int;\nuniform float u_flipY;\nuniform vec2 u_hp;\n' + extraDecl);
    vsrc = vsrc.replace(/void main\(\)\s*\{/, `void main()\n{\n${attrFix}${extraAssign}`);
    vsrc = vsrc.replace(/\}\s*$/, '    gl_Position.z = gl_Position.z * 2.0 - gl_Position.w;\n    gl_Position.xy += u_hp * gl_Position.w;\n    gl_Position.y *= u_flipY;\n}\n');
    psrc = addAfterVersion(psrc, 'precision highp float;\nprecision highp int;\nuniform int u_atest;\nuniform int u_afunc;\nuniform float u_aref;\nuniform int u_srgbw;\n');
    const hasO0 = /out highp vec4 o0;/.test(psrc);
    psrc = psrc.replace(/\}\s*$/, hasO0 ? `    if (u_atest != 0) { float a = o0.a; bool ok = u_afunc == 1 ? false : u_afunc == 2 ? a < u_aref : u_afunc == 3 ? a == u_aref : u_afunc == 4 ? a <= u_aref : u_afunc == 5 ? a > u_aref : u_afunc == 6 ? a != u_aref : u_afunc == 7 ? a >= u_aref : true; if (!ok) discard; }\n    if (u_srgbw != 0) o0.rgb = pow(max(o0.rgb, vec3(0.0)), vec3(1.0 / 2.2));\n}\n` : '}\n');
    const mk = (type, src, label) => { const sh = gl.createShader(type); gl.shaderSource(sh, src); gl.compileShader(sh);
      if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) { this.warn('cs' + label, `${label} compile error: ${gl.getShaderInfoLog(sh)}`); return null; } return sh; };
    const v = mk(gl.VERTEX_SHADER, vsrc, `vs#${vsId}`), f = mk(gl.FRAGMENT_SHADER, psrc, `ps#${psId}`); if (!v || !f) return null;
    const prog = gl.createProgram(); gl.attachShader(prog, v); gl.attachShader(prog, f); gl.linkProgram(prog);
    if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) { this.warn(`ln${vsId}_${psId}`, `link error vs#${vsId} ps#${psId}: ${gl.getProgramInfoLog(prog)}`); return null; }
    const P = { prog, vs, ps, colorRegs, u: {}, samplers: [], vsBlock: -1, psBlock: -1 };
    for (const n of ['u_flipY', 'u_hp', 'u_atest', 'u_afunc', 'u_aref', 'u_srgbw']) P.u[n] = gl.getUniformLocation(prog, n);
    const nu = gl.getProgramParameter(prog, gl.ACTIVE_UNIFORMS);
    for (let i = 0; i < nu; i++) { const info = gl.getActiveUniform(prog, i), m = /^SPIRV_Cross_Combinedt(\d+)s(\d+)$/.exec(info.name);
      if (m) P.samplers.push({ unit: +m[2], loc: gl.getUniformLocation(prog, info.name), type: info.type });
      else if (info.type === gl.SAMPLER_2D || info.type === gl.SAMPLER_CUBE || info.type === gl.SAMPLER_3D || info.type === gl.SAMPLER_2D_SHADOW) console.log('renderer: unrecognised sampler uniform ' + info.name + ' type ' + info.type); }
    { const seen = new Map(); for (const sm of P.samplers) { const k = sm.unit; if (seen.has(k) && seen.get(k) !== sm.type) console.log('renderer: sampler unit ' + k + ' used with types ' + seen.get(k) + ' and ' + sm.type); seen.set(k, sm.type); } }
    const vb = /uniform (cb\d+_struct)/.exec(vs.glsl), pb = /uniform (cb\d+_struct)/.exec(ps.glsl);
    if (vb) { P.vsBlock = gl.getUniformBlockIndex(prog, vb[1]); if (P.vsBlock >= 0) { gl.uniformBlockBinding(prog, P.vsBlock, 0); P.vsSize = gl.getActiveUniformBlockParameter(prog, P.vsBlock, gl.UNIFORM_BLOCK_DATA_SIZE); } }
    if (pb) { P.psBlock = gl.getUniformBlockIndex(prog, pb[1]); if (P.psBlock >= 0) { gl.uniformBlockBinding(prog, P.psBlock, 1); P.psSize = gl.getActiveUniformBlockParameter(prog, P.psBlock, gl.UNIFORM_BLOCK_DATA_SIZE); } }
    this.stats.progs++; return P;
  }
  getProgram(decl) {
    const d = this.vdecl ? this.vdecls.get(this.vdecl) : null, key = `${this.vs}|${this.ps}|${d ? this.colorKey(d) : ''}`;
    let P = this.programs.get(key); if (P === undefined) { P = this.buildProgram(this.vs, this.ps, d); this.programs.set(key, P); } return P;
  }
  colorKey(d) { const vs = this.shaders.get(this.vs); if (!vs) return ''; return vs.ins.map((e) => { const el = d.elems.find((x) => x.usage === e.usage && x.idx === e.idx); return el && el.type === 4 ? e.reg : ''; }).join(','); }

  // ---- render targets --------------------------------------------------------------------------------------------------------------------------------
  surfaceTarget(desc) {                                       // -> {tex|rb, w, h, level, face}
    if (!desc || desc.kind === 3) return null;
    if (desc.kind === 0) return this.bb ? { tex: this.bb.tex, w: this.bb.w, h: this.bb.h, level: 0, target: this.gl.TEXTURE_2D, fmt: 21 } : null;
    if (desc.kind === 1) { const t = this.textures.get(desc.id); if (!t) return null; return { tex: t.tex, w: Math.max(1, t.w >> desc.level), h: Math.max(1, t.h >> desc.level), level: desc.level, target: t.kind === 1 ? this.gl.TEXTURE_CUBE_MAP_POSITIVE_X + desc.face : this.gl.TEXTURE_2D, fmt: t.fmt, depth: t.info.depth, stencil: t.info.stencil }; }
    const s = this.surfaces.get(desc.id); return s ? { rb: s.rb, w: s.w, h: s.h, fmt: s.fmt, depth: s.info.depth, stencil: s.info.stencil, ms: s.ms } : null;
  }
  attach(gl, att, tg) { if (tg.rb) gl.framebufferRenderbuffer(gl.FRAMEBUFFER, att, gl.RENDERBUFFER, tg.rb); else gl.framebufferTexture2D(gl.FRAMEBUFFER, att, tg.target, tg.tex, tg.level); }
  bindTargets() {
    const gl = this.gl, c0 = this.surfaceTarget(this.rt[0]), ds = this.surfaceTarget(this.ds), key = JSON.stringify([this.rt, this.ds]);
    let fbo = this.fbos.get(key);
    if (!fbo) {
      fbo = gl.createFramebuffer(); gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
      const bufs = [];
      for (let i = 0; i < 4; i++) { const tg = this.surfaceTarget(this.rt[i]); if (tg) { this.attach(gl, gl.COLOR_ATTACHMENT0 + i, tg); bufs.push(gl.COLOR_ATTACHMENT0 + i); } else bufs.push(gl.NONE); }
      if (ds) this.attach(gl, ds.stencil ? gl.DEPTH_STENCIL_ATTACHMENT : gl.DEPTH_ATTACHMENT, ds);
      fbo = { fbo, bufs }; this.fbos.set(key, fbo);
      const st = gl.checkFramebufferStatus(gl.FRAMEBUFFER); if (st !== gl.FRAMEBUFFER_COMPLETE) this.warn('fb' + key, `framebuffer incomplete (0x${st.toString(16)}) for ${key}`);
    }
    gl.bindFramebuffer(gl.FRAMEBUFFER, fbo.fbo); gl.drawBuffers(fbo.bufs);
    return { w: c0 ? c0.w : 1, h: c0 ? c0.h : 1 };
  }

  // ---- state application ----------------------------------------------------------------------------------------------------------------------------
  cmp(f) { const gl = this.gl; return [gl.NEVER, gl.NEVER, gl.LESS, gl.EQUAL, gl.LEQUAL, gl.GREATER, gl.NOTEQUAL, gl.GEQUAL, gl.ALWAYS][f] || gl.ALWAYS; }
  blendFactor(b) { const gl = this.gl; const c = this.rs[193]; return [gl.ZERO, gl.ZERO, gl.ONE, gl.SRC_COLOR, gl.ONE_MINUS_SRC_COLOR, gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA, gl.DST_ALPHA, gl.ONE_MINUS_DST_ALPHA, gl.DST_COLOR, gl.ONE_MINUS_DST_COLOR, gl.SRC_ALPHA_SATURATE, gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA, gl.CONSTANT_COLOR, gl.ONE_MINUS_CONSTANT_COLOR][b] ?? gl.ONE; }
  blendOp(o) { const gl = this.gl; return [gl.FUNC_ADD, gl.FUNC_ADD, gl.FUNC_SUBTRACT, gl.FUNC_REVERSE_SUBTRACT, gl.MIN, gl.MAX][o] ?? gl.FUNC_ADD; }
  stencilOp(o) { const gl = this.gl; return [gl.KEEP, gl.KEEP, gl.ZERO, gl.REPLACE, gl.INCR, gl.DECR, gl.INVERT, gl.INCR_WRAP, gl.DECR_WRAP][o] ?? gl.KEEP; }
  applyState(rtw, rth) {
    const gl = this.gl, r = this.rs, vp = this.vp;
    gl.viewport(vp.x, vp.y, vp.w, vp.h); gl.depthRange(vp.minz, vp.maxz);
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
    if (r[174]) { gl.enable(gl.SCISSOR_TEST); const s = this.scissor; gl.scissor(s.l, s.t, Math.max(0, s.r - s.l), Math.max(0, s.b - s.t)); } else gl.disable(gl.SCISSOR_TEST);
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
  bindTextures(P) {
    const gl = this.gl;
    for (const sm of P.samplers) {
      const tx = this.textures.get(this.tex[sm.unit] || 0), unit = sm.unit, s = this.ss[unit];
      const want = sm.type === gl.SAMPLER_CUBE ? gl.TEXTURE_CUBE_MAP : sm.type === gl.SAMPLER_3D ? gl.TEXTURE_3D : gl.TEXTURE_2D;
      gl.activeTexture(gl.TEXTURE0 + unit);
      if (tx && tx.target === want && !tx.isDS) {
        gl.bindTexture(want, tx.tex); const [mn, mg] = this.filterFor(tx, s), sp = this.samplers[unit];
        gl.samplerParameteri(sp, gl.TEXTURE_MIN_FILTER, mn); gl.samplerParameteri(sp, gl.TEXTURE_MAG_FILTER, mg);
        gl.samplerParameteri(sp, gl.TEXTURE_WRAP_S, this.addr(s[1])); gl.samplerParameteri(sp, gl.TEXTURE_WRAP_T, this.addr(s[2])); gl.samplerParameteri(sp, gl.TEXTURE_WRAP_R, this.addr(s[3]));
        if (this.ext.aniso) gl.samplerParameterf(sp, this.ext.aniso.TEXTURE_MAX_ANISOTROPY_EXT, Math.min(this.maxAniso, s[6] === 3 || s[5] === 3 ? Math.max(1, s[10]) : 1));
        gl.bindSampler(unit, sp);
      } else { gl.bindTexture(want, this.dummy[want]); gl.bindSampler(unit, null); }
      gl.uniform1i(sm.loc, unit);
    }
  }
  setupAttribs(P, vsInfo, decl, base) {
    const gl = this.gl; let used = 0;
    for (const e of vsInfo.ins) {
      const el = decl.elems.find((x) => x.usage === e.usage && x.idx === e.idx), loc = e.reg;
      if (!el) { gl.disableVertexAttribArray(loc); gl.vertexAttrib4f(loc, 0, 0, 0, 1); continue; }
      const st = this.streams[el.stream], b = this.buffers.get(st.buf); if (!b) { gl.disableVertexAttribArray(loc); continue; }
      const T = el.type, F = gl.FLOAT, off = st.offset + el.offset + base * st.stride;
      let size = 4, type = F, norm = false;
      switch (T) { case 0: size = 1; break; case 1: size = 2; break; case 2: size = 3; break; case 3: size = 4; break; case 4: size = 4; type = gl.UNSIGNED_BYTE; norm = true; break;
        case 5: size = 4; type = gl.UNSIGNED_BYTE; break; case 6: size = 2; type = gl.SHORT; break; case 7: size = 4; type = gl.SHORT; break; case 8: size = 4; type = gl.UNSIGNED_BYTE; norm = true; break;
        case 9: size = 2; type = gl.SHORT; norm = true; break; case 10: size = 4; type = gl.SHORT; norm = true; break; case 11: size = 2; type = gl.UNSIGNED_SHORT; norm = true; break;
        case 12: size = 4; type = gl.UNSIGNED_SHORT; norm = true; break; case 13: size = 4; type = gl.UNSIGNED_INT_2_10_10_10_REV; break; case 14: size = 4; type = gl.INT_2_10_10_10_REV; norm = true; break;
        case 15: size = 2; type = gl.HALF_FLOAT; break; case 16: size = 4; type = gl.HALF_FLOAT; break; default: this.warn('vt' + T, 'unsupported vertex type ' + T); }
      gl.bindBuffer(gl.ARRAY_BUFFER, b.buf); gl.enableVertexAttribArray(loc); gl.vertexAttribPointer(loc, size, type, norm, st.stride, off); used |= 1 << loc;
    }
    for (let i = 0; i < 16; i++) if (!(used & (1 << i)) && !vsInfo.ins.some((e) => e.reg === i)) gl.disableVertexAttribArray(i);
  }
  primCount(type, n) { switch (type) { case 1: return n; case 2: return 2 * n; case 3: return n + 1; case 4: return 3 * n; case 5: case 6: return n + 2; } return n; }
  glMode(type) { const gl = this.gl; return [gl.POINTS, gl.POINTS, gl.LINES, gl.LINE_STRIP, gl.TRIANGLES, gl.TRIANGLE_STRIP, gl.TRIANGLE_FAN][type] ?? gl.TRIANGLES; }
  prepareDraw() {
    const gl = this.gl;
    if (!this.vs || !this.ps) { this.warn('noshader', 'draw without vertex/pixel shader (fixed-function pipeline not implemented)'); this.stats.skipped++; return null; }
    const P = this.getProgram(); if (!P) { this.stats.skipped++; return null; }
    const vsInfo = this.shaders.get(this.vs), decl = this.vdecls.get(this.vdecl); if (!decl) { this.warn('nodecl', 'draw without vertex declaration'); this.stats.skipped++; return null; }
    const t = this.bindTargets(); this.applyState(t.w, t.h);
    gl.useProgram(P.prog);
    gl.uniform1f(P.u.u_flipY, -1); gl.uniform2f(P.u.u_hp, 1 / this.vp.w, -1 / this.vp.h);
    gl.uniform1i(P.u.u_atest, this.rs[15] ? 1 : 0); gl.uniform1i(P.u.u_afunc, this.rs[25]); gl.uniform1f(P.u.u_aref, (this.rs[24] & 255) / 255); gl.uniform1i(P.u.u_srgbw, this.rs[194] ? 1 : 0);
    if (P.vsBlock >= 0) { if (this.vscDirty) { gl.bindBuffer(gl.UNIFORM_BUFFER, this.vsUBO); gl.bufferSubData(gl.UNIFORM_BUFFER, 0, this.vsc); this.vscDirty = false; } gl.bindBufferRange(gl.UNIFORM_BUFFER, 0, this.vsUBO, 0, Math.min(P.vsSize, 4096)); }
    if (P.psBlock >= 0) { if (this.pscDirty) { gl.bindBuffer(gl.UNIFORM_BUFFER, this.psUBO); gl.bufferSubData(gl.UNIFORM_BUFFER, 0, this.psc); this.pscDirty = false; } gl.bindBufferRange(gl.UNIFORM_BUFFER, 1, this.psUBO, 0, Math.min(P.psSize, 3584)); }
    this.bindTextures(P);
    return { P, vsInfo, decl };
  }
  draw(type, indexed, base, minIdx, numVerts, start, primCount) {
    const gl = this.gl, ctx = this.prepareDraw(); if (!ctx) return;
    const count = this.primCount(type, primCount), mode = this.glMode(type);
    this.setupAttribs(ctx.P, ctx.vsInfo, ctx.decl, indexed ? base : 0);
    if (indexed) { const ib = this.buffers.get(this.ib); if (!ib) { this.stats.skipped++; return; } const isz = ib.fmt === 101 ? 2 : 4;
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ib.buf); gl.drawElements(mode, count, isz === 2 ? gl.UNSIGNED_SHORT : gl.UNSIGNED_INT, start * isz); }
    else gl.drawArrays(mode, start, count);
    if (this.dbg) { const e = gl.getError(); if (e) this.warn('glerr' + e, 'GL error 0x' + e.toString(16) + ' after draw type=' + type + ' indexed=' + indexed + ' start=' + start + ' count=' + count + ' vs=' + this.vs + ' ps=' + this.ps);
      if (this.rs[15] && (this.dbgN = (this.dbgN || 0) + 1) < 40) console.log('renderer: dbg draw vs=' + this.vs + ' ps=' + this.ps + ' type=' + type + ' start=' + start + ' count=' + count + ' blend=' + this.rs[27] + ' cull=' + this.rs[22] + ' ztest=' + this.rs[7] + ' atest=' + this.rs[15] + ' afunc=' + this.rs[25] + ' aref=' + (this.rs[24] & 255) + ' vp=' + JSON.stringify(this.vp) + ' target=' + JSON.stringify(this.rt && this.rt[0])); }
    this.stats.draws++; this.stats.frameDraws++;
  }
  drawUP(type, primCount, stride, nverts, indexed, ifmt, nidx, blob) {
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
    const gl = this.gl, t = this.bindTargets(); this.stats.clears++;
    gl.disable(gl.SCISSOR_TEST); gl.colorMask(true, true, true, true); gl.depthMask(true); gl.stencilMask(0xff);
    let bits = 0;
    if (flags & 1) { gl.clearColor(((color >> 16) & 255) / 255, ((color >> 8) & 255) / 255, (color & 255) / 255, ((color >>> 24) & 255) / 255); bits |= gl.COLOR_BUFFER_BIT; }
    if ((flags & 2) && this.ds.kind !== 3) { gl.clearDepth(z); bits |= gl.DEPTH_BUFFER_BIT; }
    if ((flags & 4) && this.ds.kind !== 3) { gl.clearStencil(stencil); bits |= gl.STENCIL_BUFFER_BIT; }
    if (!bits) return;
    const vp = this.vp, full = vp.x === 0 && vp.y === 0 && vp.w >= t.w && vp.h >= t.h;
    if (nrects) { gl.enable(gl.SCISSOR_TEST); for (let i = 0; i < nrects; i++) { const l = rects[4 * i], tp = rects[4 * i + 1], r = rects[4 * i + 2], b = rects[4 * i + 3]; gl.scissor(l, tp, r - l, b - tp); gl.clear(bits); } gl.disable(gl.SCISSOR_TEST); }
    else { if (!full) { gl.enable(gl.SCISSOR_TEST); gl.scissor(vp.x, vp.y, vp.w, vp.h); } gl.clear(bits); gl.disable(gl.SCISSOR_TEST); }
  }
  stretch(w) {                                                // [skind,sid,slevel,sface,hasS,sl,st,sr,sb, dkind,did,dlevel,dface,hasD,dl,dt,dr,db, filter]
    const gl = this.gl, dst = this.surfaceTarget({ kind: w[9], id: w[10], level: w[11], face: w[12] }); if (!dst) return;
    const dr = w[13] ? [w[14], w[15], w[16], w[17]] : [0, 0, dst.w, dst.h];
    gl.disable(gl.SCISSOR_TEST); gl.colorMask(true, true, true, true);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, this.blitFbo2); gl.framebufferTexture2D(gl.DRAW_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, null, 0); gl.framebufferRenderbuffer(gl.DRAW_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, null);
    if (dst.rb) gl.framebufferRenderbuffer(gl.DRAW_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, dst.rb); else gl.framebufferTexture2D(gl.DRAW_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, dst.target, dst.tex, dst.level);
    gl.drawBuffers([gl.COLOR_ATTACHMENT0]);
    if (w[0] === 4) {                                          // ColorFill
      const c = w[1]; gl.clearBufferfv(gl.COLOR, 0, [((c >> 16) & 255) / 255, ((c >> 8) & 255) / 255, (c & 255) / 255, ((c >>> 24) & 255) / 255]); return; }
    const src = this.surfaceTarget({ kind: w[0], id: w[1], level: w[2], face: w[3] }); if (!src) return;
    const sr = w[4] ? [w[5], w[6], w[7], w[8]] : [0, 0, src.w, src.h];
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, this.blitFbo); gl.framebufferTexture2D(gl.READ_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, null, 0); gl.framebufferRenderbuffer(gl.READ_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, null);
    if (src.rb) gl.framebufferRenderbuffer(gl.READ_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, src.rb); else gl.framebufferTexture2D(gl.READ_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, src.target, src.tex, src.level);
    gl.readBuffer(gl.COLOR_ATTACHMENT0);
    gl.blitFramebuffer(sr[0], sr[1], sr[2], sr[3], dr[0], dr[1], dr[2], dr[3], gl.COLOR_BUFFER_BIT, w[18] === 1 && !(src.ms > 1) ? gl.NEAREST : gl.LINEAR);
  }
  present() {
    const gl = this.gl; this.stats.frameDraws = this.stats.draws - (this.lastDraws || 0); this.lastDraws = this.stats.draws;
    if (!this.bb) return;
    gl.disable(gl.SCISSOR_TEST); gl.colorMask(true, true, true, true);
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, this.blitFbo); gl.framebufferTexture2D(gl.READ_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, this.bb.tex, 0); gl.readBuffer(gl.COLOR_ATTACHMENT0);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, null);
    gl.blitFramebuffer(0, 0, this.bb.w, this.bb.h, 0, this.bb.h, this.bb.w, 0, gl.COLOR_BUFFER_BIT, gl.NEAREST);     // top-down storage -> canvas (flip)
  }
}
window.D3D9Renderer = D3D9Renderer;
