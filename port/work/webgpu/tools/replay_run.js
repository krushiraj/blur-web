// usage: node tools/replay_run.js [dump=/frames/race_f100.dump] [outPrefix=/tmp/blur-webgpu-shaders/replay] [reps=20] [only=gl|gpu]
// Serves port/work/webgpu + port/harness/web (read only) on a free port >= 8921 (never 8080/8081/8765), runs replay.html in headless Chromium (WebGPU on), saves the PNGs and prints the result json.
const http = require('http'), fs = require('fs'), path = require('path');
const puppeteer = require('puppeteer-core');
const args = Object.fromEntries(process.argv.slice(2).map((a) => [a.slice(0, a.indexOf('=')), a.slice(a.indexOf('=') + 1)]));
const W = path.resolve(__dirname, '..'), WEB = path.resolve(W, '../../harness/web');
const roots = [W, WEB], types = { '.html': 'text/html', '.js': 'text/javascript', '.json': 'application/json', '.wasm': 'application/wasm' };
const srv = http.createServer((q, r) => {
  const u = decodeURIComponent(q.url.split('?')[0]);
  for (const root of roots) { const p = path.join(root, u === '/' ? '/index.html' : u); if (p.startsWith(root) && fs.existsSync(p) && fs.statSync(p).isFile()) { r.writeHead(200, { 'content-type': types[path.extname(p)] || 'application/octet-stream', 'content-length': fs.statSync(p).size, 'cache-control': 'no-store' }); return fs.createReadStream(p).pipe(r); } }
  r.writeHead(404); r.end();
});
(async () => {
  let port = 8921;
  for (;; port++) { if ([8080, 8081, 8765].includes(port)) continue; const ok = await new Promise((res) => { srv.once('error', () => res(false)); srv.listen(port, '127.0.0.1', () => res(true)); }); if (ok) break; }
  const out = args.out || '/tmp/blur-webgpu-shaders/replay';
  const browser = await puppeteer.launch({ executablePath: process.env.CHROME || '/Applications/Chromium.app/Contents/MacOS/Chromium', headless: 'new',
    args: ['--enable-unsafe-webgpu', '--use-angle=metal', '--ignore-gpu-blocklist', '--window-size=1400,800', '--disable-background-timer-throttling'], defaultViewport: { width: 1400, height: 800 } });
  const page = await browser.newPage();
  page.on('console', (m) => { const t = m.text(); if (process.env.ALLLOG || /error|warn|fail|\[gpu\]|renderer\]/i.test(t)) console.error('[console]', t.slice(0, 500)); });
  page.on('pageerror', (e) => console.error('[pageerror]', String(e).slice(0, 500)));
  const qs = new URLSearchParams(); for (const k of ['dump', 'reps', 'only', 'warm', 'prewarm', 'x', 'y', 'w', 'h', 'thr', 'min', 'rt', 'ids', 'at']) if (args[k]) qs.set(k, args[k]);
  let cdp = null;
  if (process.env.PROF) { cdp = await page.createCDPSession(); await cdp.send('Profiler.enable'); await cdp.send('Profiler.setSamplingInterval', { interval: 200 }); await cdp.send('Profiler.start'); }
  await page.goto(`http://127.0.0.1:${port}/${args.page || 'replay.html'}?${qs}${args.hash ? '#' + args.hash : ''}`);
  await page.waitForFunction('window.RESULT', { timeout: 1800000, polling: 1000 });
  const res = await page.evaluate('window.RESULT');
  if (cdp) {      // PROF=<n>: print the n functions with the most self time over the whole run
    const { profile } = await cdp.send('Profiler.stop'), self = new Map(), byId = new Map(profile.nodes.map((n) => [n.id, n])), dt = profile.timeDeltas;
    profile.samples.forEach((id, i) => { const n = byId.get(id), k = `${n.callFrame.functionName || '(anon)'} ${n.callFrame.url.split('/').pop()}:${n.callFrame.lineNumber + 1}`; self.set(k, (self.get(k) || 0) + (dt[i] || 0) / 1000); });
    const sum = [...self.values()].reduce((a, b) => a + b, 0);
    console.error('--- self time ms (' + sum.toFixed(0) + ' total) ---');
    for (const [k, v] of [...self.entries()].sort((a, b) => b[1] - a[1]).slice(0, +process.env.PROF || 40)) console.error(v.toFixed(1).padStart(8), k);
  }
  if (res.png) for (const [k, v] of Object.entries(res.png)) fs.writeFileSync(`${out}_${k}.png`, Buffer.from(v.split(',')[1], 'base64'));
  if (res.warm && args.warmOut) {       // warm=1 warmOut=<file>: merge the pipeline recipes of this run into <file> (union over several dumps)
    const old = fs.existsSync(args.warmOut) ? JSON.parse(fs.readFileSync(args.warmOut, 'utf8')) : [], seen = new Set(old.map((c) => JSON.stringify(c)));
    for (const c of JSON.parse(res.warm)) if (!seen.has(JSON.stringify(c))) { old.push(c); seen.add(JSON.stringify(c)); }
    fs.writeFileSync(args.warmOut, JSON.stringify(old)); console.error('warm list: ' + old.length + ' pipelines in ' + args.warmOut); delete res.warm;
  }
  delete res.png; console.log(JSON.stringify(res, null, 1));
  await browser.close(); srv.close();
})();
