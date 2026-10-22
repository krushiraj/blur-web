// usage: node dumpcut.js <in.dump> <frameN> <out.dump>
// Cuts ONE frame (the N-th present, 1-based) out of a BLUR_GFXDUMP stream so it can be replayed without the multi-GB loading prefix:
//   out = [resource messages still alive and referenced by frame N] + [state snapshot at the end of frame N-1] + [frame N, verbatim up to and including its present]
// Resource messages keep the original order; uploads are reduced to the newest one per texture (face, level) / the non-overwritten ones per buffer.
// Messages of the same layout as renderer.js handle(): u32 len, u32 type, payload (len bytes).
const fs = require('fs');
const [inFile, frameArg, outFile] = process.argv.slice(2);
const N = +frameArg;
const fd = fs.openSync(inFile, 'r'), size = fs.fstatSync(fd).size;
const CH = 64 << 20;
let win = Buffer.alloc(0), winStart = 0;
function slice(pos, len) {
  if (pos >= winStart && pos + len <= winStart + win.length) return win.subarray(pos - winStart, pos - winStart + len);
  const n = Math.min(size - pos, Math.max(CH, len)); const b = Buffer.alloc(n); fs.readSync(fd, b, 0, n, pos); win = b; winStart = pos;
  return win.subarray(0, len);
}
const T = { TEX_CREATE: 20, TEX_UPLOAD: 21, SURF_CREATE: 22, BUF_CREATE: 30, BUF_UPLOAD: 31, SHADER: 40, VDECL: 41, SHADER_REF: 42, SET_RS: 50, SET_SS: 51, SET_TEX: 52, SET_VS: 53, SET_PS: 54, SET_VDECL: 55, SET_FVF: 56, SET_STREAM: 57, SET_INDICES: 58, SET_VSC: 59, SET_PSC: 60, SET_VIEWPORT: 61, SET_SCISSOR: 62, SET_RT: 63, SET_DS: 64, DRAW: 70, DRAW_UP: 71, CLEAR: 72, PRESENT: 73, STRETCH: 74, DESTROY: 95 };
// resource tables (prefix state)
const tex = new Map(), buf = new Map(), always = [];            // tex: id -> {create:[off,len], up:Map(key->[off,len])}; buf: id -> {create, ups:[{o,l,off,len}]}
const st = { rs: new Map(), ss: new Map(), tex: new Map(), vs: 0, ps: 0, vdecl: 0, fvf: 0, stream: new Map(), ib: 0, vsc: new Float32Array(256 * 4), psc: new Float32Array(224 * 4), vp: null, sc: null, rt: new Map(), ds: null };
const used = { tex: new Set(), buf: new Set() };
let present = 0, pos = 0, cutPos = -1, frameEnd = -1;
function applyState(type, p, len) {
  const dv = new DataView(p.buffer, p.byteOffset, p.length), u = (i) => dv.getUint32(4 * i, true);
  switch (type) {
    case T.SET_RS: st.rs.set(u(0), u(1)); break;
    case T.SET_SS: st.ss.set(u(0) + ',' + u(1), u(2)); break;
    case T.SET_TEX: st.tex.set(u(0), u(1)); break;
    case T.SET_VS: st.vs = u(0); break; case T.SET_PS: st.ps = u(0); break;
    case T.SET_VDECL: st.vdecl = u(0); break; case T.SET_FVF: st.fvf = u(0); break;
    case T.SET_STREAM: st.stream.set(u(0), [u(0), u(1), u(2), u(3)]); break;
    case T.SET_INDICES: st.ib = u(0); break;
    case T.SET_VSC: case T.SET_PSC: { const n = u(1), a = type === T.SET_VSC ? st.vsc : st.psc; const f = new Float32Array(p.buffer.slice(p.byteOffset + 8, p.byteOffset + 8 + 16 * n)); a.set(f.subarray(0, Math.min(f.length, a.length - u(0) * 4)), u(0) * 4); break; }
    case T.SET_VIEWPORT: st.vp = Buffer.from(p.subarray(0, 24)); break;
    case T.SET_SCISSOR: st.sc = Buffer.from(p.subarray(0, 16)); break;
    case T.SET_RT: st.rt.set(u(0), [u(0), u(1), u(2), u(3), u(4)]); break;
    case T.SET_DS: st.ds = [u(0), u(1), u(2), u(3)]; break;
  }
}
function markBound() {
  for (const [, id] of st.tex) if (id) used.tex.add(id);
  for (const [, s] of st.stream) if (s[1]) used.buf.add(s[1]);
  if (st.ib) used.buf.add(st.ib);
  for (const [, r] of st.rt) if (r[1] === 1) used.tex.add(r[2]);
  if (st.ds && st.ds[0] === 1) used.tex.add(st.ds[1]);
}
const frameMsgs = [];     // [off,len] of frame N messages (verbatim)
let inFrame = false;
const stateTypes = new Set([50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64]);
const t0 = Date.now();
while (pos + 8 <= size) {
  const h = slice(pos, 8), len = h.readUInt32LE(0), type = h.readUInt32LE(4), total = 8 + len;
  if (present === N - 1 && cutPos < 0) { cutPos = pos; inFrame = true; }
  if (inFrame) {
    frameMsgs.push([pos, total]);
    const need = type === T.STRETCH || stateTypes.has(type) || type === T.DRAW || type === T.DRAW_UP || type === T.CLEAR;
    if (need) {
      const p = slice(pos + 8, len);
      if (stateTypes.has(type)) applyState(type, p, len);
      else if (type === T.DRAW || type === T.DRAW_UP) markBound();
      else if (type === T.STRETCH) { const dv = new DataView(p.buffer, p.byteOffset, p.length); for (const [k, i] of [[0, 1], [9, 10]]) if (dv.getUint32(4 * k, true) === 1) used.tex.add(dv.getUint32(4 * i, true)); }
      else if (type === T.CLEAR) markBound();
    }
    if (type === T.PRESENT) { frameEnd = pos + total; break; }
  } else {
    switch (type) {
      case T.TEX_CREATE: { const p = slice(pos + 8, len); tex.set(p.readUInt32LE(0), { create: [pos, total], up: new Map() }); break; }
      case T.TEX_UPLOAD: { const p = slice(pos + 8, len), id = p.readUInt32LE(0), e = tex.get(id); if (e) e.up.set(p.readUInt32LE(4) + ',' + p.readUInt32LE(8), [pos, total]); break; }
      case T.SURF_CREATE: case T.SHADER: case T.SHADER_REF: case T.VDECL: always.push([pos, total]); break;
      case T.BUF_CREATE: { const p = slice(pos + 8, len); buf.set(p.readUInt32LE(0), { create: [pos, total], ups: [] }); break; }
      case T.BUF_UPLOAD: { const p = slice(pos + 8, len), id = p.readUInt32LE(0), o = p.readUInt32LE(4), l = len - 8, e = buf.get(id);
        if (e) { e.ups = e.ups.filter((x) => !(x.o >= o && x.o + x.l <= o + l)); e.ups.push({ o, l, off: pos, len: total }); } break; }
      case T.DESTROY: { const p = slice(pos + 8, len), kind = p.readUInt32LE(0), id = p.readUInt32LE(4); (kind === 0 ? tex : buf).delete(id); break; }
      case T.PRESENT: present++; break;
      default: if (stateTypes.has(type)) applyState(type, slice(pos + 8, len), len);
    }
  }
  pos += total;
  if (!inFrame && present > N - 1) break;
}
if (frameEnd < 0) { console.error('dump ends before frame', N); process.exit(1); }
// ---- write output
const out = fs.openSync(outFile, 'w');
const msg = (type, words, data) => { const dl = data ? data.length : 0, b = Buffer.alloc(8 + 4 * words.length + dl); b.writeUInt32LE(4 * words.length + dl, 0); b.writeUInt32LE(type, 4); words.forEach((w, i) => b.writeUInt32LE(w >>> 0, 8 + 4 * i)); if (data) Buffer.from(data.buffer, data.byteOffset, dl).copy(b, 8 + 4 * words.length); return b; };
const res = [...always];
let nt = 0, nb = 0, bytes = 0;
for (const id of used.tex) { const e = tex.get(id); if (!e) continue; nt++; res.push(e.create); for (const u of e.up.values()) res.push(u); }
for (const id of used.buf) { const e = buf.get(id); if (!e) continue; nb++; res.push(e.create); for (const u of e.ups) res.push([u.off, u.len]); }
res.sort((a, b) => a[0] - b[0]);
// NB: create must precede uploads, and offsets are in original order, so a plain sort keeps that.
const chunks = [];
for (const [off, len] of res) { const b = slice(off, len); fs.writeSync(out, b); bytes += len; }
// state snapshot
const snap = [];
for (const [k, v] of st.rs) snap.push(msg(T.SET_RS, [k, v]));
for (const [k, v] of st.ss) { const [a, b] = k.split(',').map(Number); snap.push(msg(T.SET_SS, [a, b, v])); }
for (const [k, v] of st.tex) snap.push(msg(T.SET_TEX, [k, v]));
snap.push(msg(T.SET_VS, [st.vs]), msg(T.SET_PS, [st.ps]), msg(T.SET_VDECL, [st.vdecl]), msg(T.SET_FVF, [st.fvf]), msg(T.SET_INDICES, [st.ib]));
for (const [, s] of st.stream) snap.push(msg(T.SET_STREAM, s));
snap.push(msg(T.SET_VSC, [0, 256], st.vsc), msg(T.SET_PSC, [0, 224], st.psc));
if (st.vp) snap.push(msg(T.SET_VIEWPORT, [], st.vp)); if (st.sc) snap.push(msg(T.SET_SCISSOR, [], st.sc));
for (const [, r] of st.rt) snap.push(msg(T.SET_RT, r)); if (st.ds) snap.push(msg(T.SET_DS, st.ds));
for (const b of snap) { fs.writeSync(out, b); bytes += b.length; }
let fb = 0; for (const [off, len] of frameMsgs) { fs.writeSync(out, slice(off, len)); fb += len; }
fs.closeSync(out);
fs.writeFileSync(outFile + '.json', JSON.stringify({ frame: N, frameStart: bytes, total: bytes + fb, frameMsgs: frameMsgs.length }));
console.log(JSON.stringify({ frame: N, usedTextures: nt, usedBuffers: nb, resourceMsgs: res.length, prefixBytes: bytes, frameBytes: fb, frameMsgs: frameMsgs.length, secs: (Date.now() - t0) / 1000 }));
