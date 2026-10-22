// Fake harness for live-page tests (no blurrt): serves port/harness/web over http and, on a websocket at /ws, streams a recorded .gfx/.dump file (messages: u32 len, u32 type, payload) the way
// gfx.c would, in message-aligned chunks, then keeps re-sending the LAST frame's messages (from `frameStart` of the .json next to the file) so the page keeps presenting.
// usage: node mockhost.js <file.gfx> [port=8931] [mb-per-chunk=2]      page: http://127.0.0.1:<port>/index.html#gpu=1
const http = require('http'), fs = require('fs'), path = require('path');
const WS = require('ws');
const [file, portArg, mbArg] = process.argv.slice(2), port = +(portArg || 8931), chunkB = (+(mbArg || 2)) << 20;
const WEB = path.resolve(__dirname, '../../../harness/web'), W = path.resolve(__dirname, '..');
const types = { '.html': 'text/html', '.js': 'text/javascript', '.json': 'application/json' };
const srv = http.createServer((q, r) => {
  const u = decodeURIComponent(q.url.split('?')[0]);
  for (const root of [WEB, W]) { const p = path.join(root, u === '/' ? '/index.html' : u); if (p.startsWith(root) && fs.existsSync(p) && fs.statSync(p).isFile()) { r.writeHead(200, { 'content-type': types[path.extname(p)] || 'application/octet-stream', 'cache-control': 'no-store' }); return fs.createReadStream(p).pipe(r); } }
  r.writeHead(404); r.end();
});
const data = fs.readFileSync(file), meta = JSON.parse(fs.readFileSync(file + '.json', 'utf8'));
const wss = new WS.Server({ server: srv, path: '/ws' });
wss.on('connection', (ws) => {
  console.log('viewer connected');
  let off = 0, sent = 0, acked = 0, loops = 0;
  ws.on('message', (m) => { const a = new Int32Array(m.buffer.slice(m.byteOffset, m.byteOffset + m.byteLength)); if (a[0] === 9) acked = a[1]; });
  const tail = data.subarray(meta.frameStart);
  const pump = () => {
    if (ws.readyState !== 1) return;
    if (off < data.length) { let end = off; while (end < data.length && end - off < chunkB) end += 8 + data.readUInt32LE(end); ws.send(data.subarray(off, end)); off = end; setTimeout(pump, 5); }
    else if (sent < 100000) { ws.send(tail); loops++; setTimeout(pump, 33); }
  };
  pump();
  ws.on('close', () => console.log('viewer gone, loops', loops));
});
srv.listen(port, '127.0.0.1', () => console.log('mock host on http://127.0.0.1:' + port));
