// usage: node shot.js <url> <seconds> <out.png> [more.png...]  -- opens the page in headless Chromium, waits, saves canvas screenshots
const puppeteer = require('puppeteer-core');
(async () => {
  const [url, secs, ...outs] = process.argv.slice(2);
  const browser = await puppeteer.launch({ executablePath: process.env.CHROMIUM || '/Applications/Chromium.app/Contents/MacOS/Chromium', headless: 'new',
    args: ['--use-angle=metal', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist', '--window-size=1320,900'], defaultViewport: { width: 1320, height: 900 } });
  const page = await browser.newPage();
  page.on('console', (m) => { const t = m.text(); if (process.env.ALLLOG || /renderer|error|warn/i.test(t)) console.log('[console]', t.slice(0, 300)); });
  page.on('pageerror', (e) => console.log('[pageerror]', String(e).slice(0, 300)));
  await page.goto(url);
  // KEYS="Enter@10,ArrowDown@15" presses keys at the given seconds after load
  for (const k of (process.env.KEYS || '').split(',').filter(Boolean)) { const [key, at] = k.split('@'); setTimeout(() => page.keyboard.press(key, { delay: 120 }), +at * 1000); }
  const per = (+secs * 1000) / outs.length;
  for (const out of outs) {
    await new Promise((r) => setTimeout(r, per));
    await page.screenshot({ path: out });
    const info = await page.evaluate(() => ['status', 'fps', 'stats'].map((id) => { const e = document.getElementById(id); return e ? e.textContent : ''; }).filter(Boolean).join(' | ') || document.title).catch(() => ''); console.log('saved', out, info);
  }
  const log = await page.evaluate(() => { const e = document.getElementById('log'); return e ? e.textContent : ''; }); if (log.trim()) console.log('--- page log ---\n' + log.slice(0, 3000));
  await browser.close();
})();
