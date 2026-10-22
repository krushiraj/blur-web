// Reader for the BWGP WGSL pack (format: see ../README.md).  Plain script: defines globalThis.BWGP.
// const pk = BWGP.parse(arrayBuffer)           // arrayBuffer already gunzipped
// const s = pk.get('001663f8d30df6db_2220')     // fnv1a64 hex + '_' + bytecode length, or pk.getLoHi(lo, hi, len)
// s = { stage:'vs'|'ps', flags, cbVec, inMask, outMask, texMask, texTypes:{reg:'2d'|'cube'|'3d'}, ranges:[[start,count]], wgsl }
(function () {
  const F = { constRel: 1, intUndef: 2, depth: 4, pointSize: 8, derivDiag: 16, boolUndef: 32 };
  function parse(buf) {
    const dv = new DataView(buf), u8 = new Uint8Array(buf);
    if (dv.getUint32(0, true) !== 0x50475742) throw new Error('bad pack magic');
    const version = dv.getUint16(4, true);
    if (version !== 1) throw new Error('unsupported pack version ' + version);
    const nSh = dv.getUint32(8, true), nBl = dv.getUint32(12, true), offSh = dv.getUint32(16, true), offBl = dv.getUint32(20, true),
      offTx = dv.getUint32(24, true), offMe = dv.getUint32(28, true), lenMe = dv.getUint32(32, true);
    const SH = 40, td = new TextDecoder();
    const meta = JSON.parse(td.decode(u8.subarray(offMe, offMe + lenMe)));
    const offRng = offSh + nSh * SH;
    const index = new Map(), keys = [];
    const wgslCache = new Map();
    function blobText(i) {
      let t = wgslCache.get(i);
      if (t === undefined) {
        const o = offBl + i * 8, to = dv.getUint32(o, true), tl = dv.getUint32(o + 4, true);
        t = td.decode(u8.subarray(offTx + to, offTx + to + tl));
        wgslCache.set(i, t);
      }
      return t;
    }
    for (let i = 0; i < nSh; i++) {
      const o = offSh + i * SH, lo = dv.getUint32(o, true), hi = dv.getUint32(o + 4, true), len = dv.getUint32(o + 8, true);
      const k = (BigInt(hi) << 32n | BigInt(lo)).toString(16).padStart(16, '0') + '_' + len;
      index.set(k, o); keys.push(k);
    }
    function decode(o) {
      const texMask = dv.getUint16(o + 24, true), nr = dv.getUint16(o + 26, true), tt = dv.getUint32(o + 28, true), ro = dv.getUint32(o + 32, true);
      const texTypes = {};
      for (let r = 0; r < 16; r++) if (texMask >> r & 1) texTypes[r] = ['2d', 'cube', '3d'][tt >> (2 * r) & 3];
      const ranges = [];
      for (let j = 0; j < nr; j++) ranges.push([dv.getUint16(offRng + (ro + j) * 4, true), dv.getUint16(offRng + (ro + j) * 4 + 2, true)]);
      return { stage: dv.getUint8(o + 16) ? 'ps' : 'vs', flags: dv.getUint8(o + 17), cbVec: dv.getUint16(o + 18, true),
        inMask: dv.getUint16(o + 20, true), outMask: dv.getUint16(o + 22, true), texMask, texTypes, ranges, wgsl: blobText(dv.getUint32(o + 12, true)) };
    }
    let hh = 2166136261; for (let i = 0; i < u8.length; i += 499) hh = Math.imul(hh ^ u8[i], 16777619);
    return {
      meta, keys, count: nSh, blobs: nBl, id: (hh >>> 0).toString(16) + '.' + nSh + '.' + u8.length,
      has: (k) => index.has(k),
      get: (k) => { const o = index.get(k); return o === undefined ? null : decode(o); },
      getLoHi: (lo, hi, len) => { const o = index.get((BigInt(hi) << 32n | BigInt(lo)).toString(16).padStart(16, '0') + '_' + len); return o === undefined ? null : decode(o); },
    };
  }
  // fetch + gunzip + parse; the .gz is what the page downloads / stores under its content hash
  async function load(url) {
    const r = await fetch(url);
    if (!r.ok) throw new Error('pack fetch ' + r.status);
    let b;
    if (url.endsWith('.gz')) b = await new Response(r.body.pipeThrough(new DecompressionStream('gzip'))).arrayBuffer();
    else b = await r.arrayBuffer();
    return parse(b);
  }
  // key of a warm.json entry 'lo_hi_len' (decimal)
  function warmKey(k) { const [lo, hi, len] = k.split('_'); return (BigInt(hi) << 32n | BigInt(lo)).toString(16).padStart(16, '0') + '_' + len; }
  // Only needed if a PS reads an interpolant its VS never writes (D3D9: undefined/0; WebGPU: pipeline creation error).
  // 0 of the 111 real warm.json pairs need it.  Appends zero vec4 outputs for the locations in `mask` to the VS text.
  // Relies on the fixed shape of the translator's VS entry point:  struct VertexOutput {...}  /  var vo = VertexOutput(...);
  function padVsOutputs(wgsl, mask) {
    if (!mask) return wgsl;
    let members = '', zeros = '';
    for (let L = 0; L < 16; L++) if (mask >> L & 1) { members += '@location(' + L + ') pad' + L + ': vec4<f32>,\n'; zeros += ',vec4<f32>()'; }
    const i = wgsl.indexOf('struct VertexOutput {'), j = wgsl.indexOf('\n}', i);
    if (i < 0 || j < 0) throw new Error('padVsOutputs: VertexOutput not found');
    wgsl = wgsl.slice(0, j + 1) + members + wgsl.slice(j + 1);
    return wgsl.replace(/var vo = VertexOutput\(([^;]*)\);/, (a, args) => 'var vo = VertexOutput(' + args + zeros + ');');
  }
  globalThis.BWGP = { parse, load, warmKey, F, padVsOutputs };
})();
