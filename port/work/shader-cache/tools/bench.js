// usage: node bench.js <webdir> <sync|par|all>   (webdir holds renderer.js, shaders.bin, warm.json)
const http = require('http'), fs = require('fs'), path = require('path');
const puppeteer = require('puppeteer-core');
const [webdir, mode] = process.argv.slice(2);
const srv = http.createServer((req, res) => {
  const u = decodeURIComponent(req.url.split('?')[0]); const f = (u === '/' || u === '/bench.html') ? path.join(__dirname, 'bench.html') : path.join(webdir, u);
  if (!fs.existsSync(f)) { res.writeHead(404); return res.end(); }
  res.writeHead(200, { 'content-type': f.endsWith('.js') ? 'text/javascript' : f.endsWith('.html') ? 'text/html' : 'application/octet-stream', 'cache-control': 'no-store' }); fs.createReadStream(f).pipe(res);
}).listen(8793, '127.0.0.1', async () => {
  const browser = await puppeteer.launch({ executablePath: process.env.CHROMIUM || '/Applications/Chromium.app/Contents/MacOS/Chromium', headless: 'new',
    args: ['--use-angle=metal', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'], defaultViewport: { width: 1320, height: 900 } });
  const page = await browser.newPage(); page.on('pageerror', (e) => console.log('[pageerror]', String(e).slice(0, 300)));
  await page.goto('http://127.0.0.1:8793/bench.html#' + mode);
  await page.waitForFunction('window.out && out.done', { timeout: 600000, polling: 500 });
  console.log(JSON.stringify(await page.evaluate(() => out)));
  await browser.close(); srv.close();
});
