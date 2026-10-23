// usage: node rbtest.js <url> <seconds> <png>  -- hooks renderer.sendInput, logs GetRenderTargetData answers (input type 10) and saves a screenshot
const puppeteer = require('puppeteer-core');
(async () => {
  const [url, secs, png] = process.argv.slice(2);
  const b = await puppeteer.launch({ executablePath: process.env.CHROMIUM || '/Applications/Chromium.app/Contents/MacOS/Chromium', headless: 'new', args: ['--use-angle=metal', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist', '--enable-unsafe-webgpu', '--window-size=1320,900'], defaultViewport: { width: 1320, height: 900 } });
  const page = await b.newPage(); page.on('console', (m) => { const t = m.text(); if (/^RB:/.test(t)) console.log(t); });
  await page.evaluateOnNewDocument(() => {
    const iv = setInterval(() => {
      const r = window.__r; if (!r || !r.sendInput || r.__w) return; r.__w = 1; clearInterval(iv);
      const o = r.sendInput; let n = 0;
      r.sendInput = (t, a, b2, c, d) => { if (t === 10) { n++; const f = new Float32Array(new Uint32Array([d >>> 0]).buffer)[0]; if (n <= 5 || n % 120 === 0) console.log('RB: type10 n=' + n + ' dst=' + a + ' bits=' + (d >>> 0) + ' float=' + f); } return o(t, a, b2, c, d); };
      console.log('RB: hooked');
    }, 50);
  });
  await page.goto(url); await new Promise((r) => setTimeout(r, +secs * 1000));
  await page.screenshot({ path: png }); await b.close();
})();
