const puppeteer = require('puppeteer-core');
(async () => {
  const [url, secs] = process.argv.slice(2);
  const browser = await puppeteer.launch({ executablePath: process.env.CHROMIUM || '/Applications/Chromium.app/Contents/MacOS/Chromium', headless: 'new', args: ['--use-angle=metal', '--ignore-gpu-blocklist', '--window-size=1320,900'], defaultViewport: { width: 1320, height: 900 } });
  const page = await browser.newPage(); await page.goto(url);
  await new Promise((r) => setTimeout(r, +secs * 1000));
  console.log(JSON.stringify(await page.evaluate(() => { const r = window.__r; return r.diagFrames.slice(-14).map((f) => ({ d: f.d, m: Object.entries(f.rts).filter(([k]) => { const t = k.startsWith('1:') ? r.textures.get(+k.split(':')[1]) : null; return t && ((t.w === 576 && t.h === 96) || (t.w === 256 && t.h === 256)); }).map(([k, v]) => k + '=' + v).join(' ') })); }), null, 0));
  await browser.close();
})();
