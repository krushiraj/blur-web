// usage: node dumpscan.js <gfx.dump> [maxPresents=2000] [maxMB=2000]
// Streams a BLUR_GFXDUMP file (messages: u32 len, u32 type, payload) and prints per-present draw counts, so a frame can be picked for replay.
const fs = require('fs');
const [file, mp = '2000', mb = '2000'] = process.argv.slice(2);
const maxP = +mp, maxB = +mb * 1048576;
const fd = fs.openSync(file, 'r'), size = fs.fstatSync(fd).size;
console.log('size', size);
const CH = 1 << 24; let buf = Buffer.alloc(CH), have = 0, pos = 0, fpos = 0, present = 0;
const names = { 20: 'texC', 21: 'texU', 22: 'surf', 30: 'bufC', 31: 'bufU', 40: 'shader', 41: 'vdecl', 42: 'sref', 70: 'draw', 71: 'drawUP', 72: 'clear', 73: 'present', 74: 'stretch', 95: 'destroy', 96: 'hcRef', 98: 'hcTag', 90: 'audio' };
let cnt = {}, bytes = {};
const tot = {};
for (;;) {
  if (have - pos < 8 && fpos < size) { buf.copy(buf, 0, pos, have); have -= pos; pos = 0; const n = fs.readSync(fd, buf, have, CH - have, fpos); fpos += n; have += n; if (!n) break; }
  if (have - pos < 8) break;
  const len = buf.readUInt32LE(pos), type = buf.readUInt32LE(pos + 4);
  if (len > CH - 16) { // too large for the window: grow
    buf = Buffer.concat([buf, Buffer.alloc(len + 16 - (CH - 0))]); }
  if (have - pos < 8 + len) { buf.copy(buf, 0, pos, have); have -= pos; pos = 0; if (buf.length < 8 + len) buf = Buffer.concat([buf, Buffer.alloc(8 + len - buf.length)]); const n = fs.readSync(fd, buf, have, buf.length - have, fpos); fpos += n; have += n; if (have < 8 + len) break; }
  cnt[type] = (cnt[type] || 0) + 1; bytes[type] = (bytes[type] || 0) + len; tot[type] = (tot[type] || 0) + 1;
  pos += 8 + len;
  if (type === 73) {
    present++;
    const s = Object.entries(cnt).map(([t, c]) => `${names[t] || t}:${c}(${(bytes[t] / 1024) | 0}K)`).join(' ');
    if (present <= 40 || present % 25 === 0) console.log(`present ${present} @${(fpos / 1048576) | 0}MB ${s}`);
    cnt = {}; bytes = {};
    if (present >= maxP || fpos > maxB) break;
  }
}
console.log('totals', Object.entries(tot).map(([t, c]) => `${names[t] || t}:${c}`).join(' '));
