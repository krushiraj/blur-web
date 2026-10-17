// usage: node prof.js <url> <wait-for-draws> <profile-seconds>  -- opens the page in headless Chromium, waits until draws/frame >= N (the race), CPU-profiles the page
// for the given seconds via the DevTools Profiler and prints the top self-time functions (and the page's own fps stat line before/after).
const puppeteer = require('puppeteer-core');
(async () => {
  const [url, minDraws, secs, settle = '20'] = process.argv.slice(2);
  const browser = await puppeteer.launch({ executablePath: process.env.CHROMIUM || '/Applications/Chromium.app/Contents/MacOS/Chromium', headless: 'new',
    args: ['--use-angle=metal', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist', '--window-size=1320,900'], defaultViewport: { width: 1320, height: 900 } });
  const page = await browser.newPage();
  await page.goto(url);
  const stat = () => page.evaluate(() => ['fps', 'stats'].map((id) => { const e = document.getElementById(id); return e ? e.textContent : ''; }).join(' | '));
  for (;;) { await new Promise((r) => setTimeout(r, 3000)); const s = await stat(); const m = /draws\/frame (\d+)/.exec(s); if (m && +m[1] >= +minDraws) break; }
  await new Promise((r) => setTimeout(r, +settle * 1000));                       // settle into the race
  console.log('before:', await stat());
  const cdp = await page.createCDPSession(); await cdp.send('Profiler.enable'); await cdp.send('Profiler.setSamplingInterval', { interval: 200 }); await cdp.send('Profiler.start');
  await new Promise((r) => setTimeout(r, +secs * 1000));
  const { profile } = await cdp.send('Profiler.stop');
  console.log('after:', await stat());
  const self = new Map(), byId = new Map(profile.nodes.map((n) => [n.id, n])); const dt = profile.timeDeltas; let total = 0;
  profile.samples.forEach((id, i) => { const n = byId.get(id), f = n.callFrame, k = `${f.functionName || '(anon)'} ${f.url.split('/').pop()}:${f.lineNumber + 1}`; self.set(k, (self.get(k) || 0) + dt[i]); total += dt[i]; });
  console.log('total sampled ms', (total / 1000).toFixed(0));
  [...self.entries()].sort((a, b) => b[1] - a[1]).slice(0, 25).forEach(([k, v]) => console.log(((v / total) * 100).toFixed(1).padStart(5) + '%  ' + (v / 1000).toFixed(0).padStart(6) + ' ms  ' + k));
  await browser.close();
})();
