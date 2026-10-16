// usage: node replay.js <mode live|cache> <gfx.dump> <webdir> [at=N] [every=N] [maxpresents=N] [warm] [async] [out.png]
// Serves <webdir> (renderer.js, shaders.bin, warm.json) + replay.html + the dump on 127.0.0.1:8792 and replays it in headless Chromium.
const http = require('http'), fs = require('fs'), path = require('path');
const puppeteer = require('puppeteer-core');
const [mode, dump, webdir, ...rest] = process.argv.slice(2);
const opts = Object.fromEntries(rest.filter((x) => x.includes('=')).map((x) => x.split('=')));
const flags = rest.filter((x) => !x.includes('=') && !x.endsWith('.png')), outPng = rest.find((x) => x.endsWith('.png'));
const toolsDir = __dirname;
const srv = http.createServer((req, res) => {
  const u = decodeURIComponent(req.url.split('?')[0]); let f;
  if (u === '/' || u === '/replay.html') f = path.join(toolsDir, 'replay.html'); else if (u === '/gfx.dump') f = dump; else f = path.join(webdir, u);
  if (!fs.existsSync(f)) { res.writeHead(404); return res.end(); }
  const ct = f.endsWith('.js') ? 'text/javascript' : f.endsWith('.html') ? 'text/html' : 'application/octet-stream';
  res.writeHead(200, { 'content-type': ct, 'content-length': fs.statSync(f).size, 'cache-control': 'no-store' }); fs.createReadStream(f).pipe(res);
}).listen(8792, '127.0.0.1', async () => {
  const browser = await puppeteer.launch({ executablePath: process.env.CHROMIUM || '/Applications/Chromium.app/Contents/MacOS/Chromium', headless: 'new',
    args: ['--use-angle=metal', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist', '--window-size=1320,900'], defaultViewport: { width: 1320, height: 900 } });
  const page = await browser.newPage();
  page.on('console', (m) => { const t = m.text(); if (process.env.ALLLOG || /error/i.test(t)) console.log('[console]', t.slice(0, 300)); });
  page.on('pageerror', (e) => console.log('[pageerror]', String(e).slice(0, 300)));
  const h = [`mode=${mode}`].concat(Object.entries(opts).map(([k, v]) => `${k}=${v}`), flags).join(';');
  await page.goto('http://127.0.0.1:8792/replay.html#' + h);
  await page.waitForFunction('window.out && out.done', { timeout: 1800000, polling: 1000 }).catch(() => {});
  const out = await page.evaluate(() => out);
  if (out.png && outPng) { fs.writeFileSync(outPng, Buffer.from(out.png.split(',')[1], 'base64')); delete out.png; }
  console.log(JSON.stringify(out));
  await browser.close(); srv.close();
});
