// usage: node featscan.js <dump> [fromByte=0]
// Counts which D3D9 features a stream uses (messages from fromByte on; state is tracked from the start): primitive types, vertex element types actually drawn,
// MSAA surfaces, readbacks (81), gamma ramps (80), ColorFill/Stretch variants, per-target write masks, point-size shaders (needs the pack), alpha test, sRGB.
const fs = require('fs');
const [file, from = '0'] = process.argv.slice(2);
const buf = fs.readFileSync(file), dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
const cnt = {}, inc = (k, n = 1) => { cnt[k] = (cnt[k] || 0) + n; };
const vdecls = new Map(), streams = Array.from({ length: 16 }, () => ({ buf: 0, stride: 0 })), rs = new Uint32Array(256); let vdecl = 0, vs = 0, ps = 0, nstart = +from;
const shaderKey = new Map();
for (let off = 0; off + 8 <= buf.length;) {
  const len = dv.getUint32(off, true), type = dv.getUint32(off + 4, true), p = off + 8, u = (i) => dv.getUint32(p + 4 * i, true), live = off >= nstart; off = p + len;
  switch (type) {
    case 41: { const n = u(1), el = []; for (let i = 0; i < n; i++) { const o = p + 8 + i * 8; if (dv.getUint16(o, true) === 0xff) break; el.push({ stream: dv.getUint16(o, true), type: dv.getUint8(o + 4), usage: dv.getUint8(o + 6), idx: dv.getUint8(o + 7) }); } vdecls.set(u(0), el); break; }
    case 22: if (live && u(5) > 1) inc('msaaSurface ms=' + u(5)); break;
    case 42: shaderKey.set(u(0), u(2) + '_' + u(3) + '_' + u(4)); break;
    case 50: rs[u(0)] = u(1); break;
    case 53: vs = u(0); break; case 54: ps = u(0); break; case 55: vdecl = u(0); break;
    case 57: streams[u(0)] = { buf: u(1), stride: u(3) }; break;
    case 70: case 71: if (live) { inc(type === 70 ? 'draw' : 'drawUP'); inc('prim' + u(0)); if (u(0) === 6) inc(type === 70 && u(1) ? 'fanIndexed' : 'fanPlain');
        for (const e of vdecls.get(vdecl) || []) inc('vtype' + e.type + (type === 71 ? '(UP)' : ''));
        if (rs[15]) inc('alphaTest'); if (rs[194]) inc('srgbWrite'); if (rs[168] !== 15) inc('colorMask=' + rs[168]); if ((rs[190] & 15) !== 15 || (rs[191] & 15) !== 15) inc('mrtMask'); if (rs[27]) inc('blend'); if (rs[52]) inc('stencil'); if (rs[174]) inc('scissor'); }
      break;
    case 72: if (live) { inc('clear'); if (u(4)) inc('clearRects'); } break;
    case 74: if (live) inc(u(0) === 4 ? (u(13) ? 'colorFillRect' : 'colorFill') : 'stretch' + (u(4) || u(13) ? 'Rect' : ''), 1); break;
    case 80: if (live) inc('gamma'); break; case 81: if (live) inc('readback81'); break; case 73: if (live) inc('present'); break;
    case 90: if (live) inc('audio'); break;
  }
}
console.log(JSON.stringify(Object.fromEntries(Object.entries(cnt).sort()), null, 0));
