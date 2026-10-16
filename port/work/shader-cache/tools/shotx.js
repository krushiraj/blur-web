// usage: node shotx.js <url> <secs> <out.png> [warm.json-out]
// Like tools/shot/shot.js but also prints renderer stats/timing and can export the warm list (programs actually drawn).
const puppeteer = require('puppeteer-core');
(async () => {
  const [url, secs, out, warmOut] = process.argv.slice(2);
  const browser = await puppeteer.launch({ executablePath: process.env.CHROMIUM || '/Applications/Chromium.app/Contents/MacOS/Chromium', headless: 'new',
    args: ['--use-angle=metal', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist', '--window-size=1320,900'], defaultViewport: { width: 1320, height: 900 } });
  const page = await browser.newPage();
  page.on('console', (m) => { const t = m.text(); if (process.env.ALLLOG || /renderer|error|warn/i.test(t)) console.log('[console]', t.slice(0, 300)); });
  page.on('pageerror', (e) => console.log('[pageerror]', String(e).slice(0, 300)));
  await page.goto(url);
  await new Promise((r) => setTimeout(r, +secs * 1000));
  await page.screenshot({ path: out });
  console.log(await page.evaluate(() => document.getElementById('status').textContent + ' | ' + document.getElementById('fps').textContent + ' | ' + r.statLine() + ' | ' + JSON.stringify(r.stats)));
  if (warmOut) require('fs').writeFileSync(warmOut, await page.evaluate(() => r.exportWarm()));
  const log = await page.evaluate(() => document.getElementById('log').textContent); if (log.trim()) console.log('--- page log ---\n' + log.slice(0, 3000));
  await browser.close();
})();
