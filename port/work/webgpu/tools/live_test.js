// usage: node live_test.js <url> [secs=20] [shot.png] [nogpu=1] [lose=<sec>] [nobc=1]
// Opens the live page (served by mockhost.js or the real harness) in headless Chromium and reports which renderer ran, the status line, fps and the fallback note.
const fs = require('fs');
const puppeteer = require('puppeteer-core');
const [url, secs = '20', shot, ...opts] = process.argv.slice(2), o = Object.fromEntries(opts.map((x) => x.split('=')));
(async () => {
  const browser = await puppeteer.launch({ executablePath: process.env.CHROMIUM || '/Applications/Chromium.app/Contents/MacOS/Chromium', headless: 'new', userDataDir: o.profile, args: ['--enable-unsafe-webgpu', '--use-angle=metal', '--ignore-gpu-blocklist', '--window-size=1400,820'], defaultViewport: { width: 1400, height: 820 } });
  const page = await browser.newPage();
  page.on('console', (m) => { const t = m.text(); if (/gpu|WebGPU|error|warn|warm/i.test(t)) console.error('[console]', t.slice(0, 300)); });
  page.on('pageerror', (e) => console.error('[pageerror]', String(e).slice(0, 300)));
  if (o.nogpu) await page.evaluateOnNewDocument(() => { Object.defineProperty(navigator, 'gpu', { get: () => undefined }); });
  if (o.nobc) await page.evaluateOnNewDocument(() => { const f = GPUAdapter.prototype; const d = Object.getOwnPropertyDescriptor(f, 'features'); Object.defineProperty(f, 'features', { get() { const s = new Set(d.get.call(this)); s.delete('texture-compression-bc'); return s; } }); });
  await page.goto(url);
  if (o.lose) setTimeout(async () => { try { await page.evaluate('window.__r && window.__r.dev && window.__r.dev.destroy()'); console.error('device destroyed'); } catch (e) {} }, +o.lose * 1000);
  await new Promise((r) => setTimeout(r, +secs * 1000));
  const info = await page.evaluate(() => ({ renderer: window.__r && window.__r.constructor.name, bar: document.getElementById('bar').innerText.replace(/\s+/g, ' '), note: document.getElementById('gpunote').textContent, hash: location.hash,
    stats: window.__r && window.__r.statLine && window.__r.statLine(), perf: window.__r && window.__r.perf && window.__r.perf(120), draws: window.__r && window.__r.stats && (window.__r.stats.frameDraws), pending: window.__r && window.__r.pending, hc: window.__hc && window.__hc.summary && window.__hc.summary() }));
  info.learned = await page.evaluate(async () => { try { const r = window.__r; const l = await r.loadLearned(); return { learned: l.length, recipes: r.recipes.size, warmCount: r.warmCount, packId: r.pack.id }; } catch (e) { return String(e); } });
  console.log(JSON.stringify(info, null, 1));
  if (shot) await page.screenshot({ path: shot });
  await browser.close();
})();
