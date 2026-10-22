// warmtest.js - WebGPU pipeline-warm-set test: holds the throttle (ArrowUp) with light steering in the race and logs skipped/held every 4 s; at the end writes __gpuWarm() to <prefix>.warm.json.
// usage: [PROF=dir] [THROTTLE_AT=30] node warmtest.js <url-with-#gpu=1> <total-seconds> <prefix>   (via DRIVER=warmtest.js tools/shot/racedrive.sh <tag> "gpu=1&gpudump=1" 150)
const puppeteer = require('./node_modules/puppeteer-core'), fs = require('fs');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
(async () => {
  const [url, secs, pre] = process.argv.slice(2);
  const b = await puppeteer.launch({ executablePath: process.env.CHROMIUM || '/Applications/Chromium.app/Contents/MacOS/Chromium', headless: 'new', userDataDir: process.env.PROF,
    args: ['--enable-unsafe-webgpu', '--use-angle=metal', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist', '--window-size=1320,900', '--disable-background-timer-throttling', '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows'], defaultViewport: { width: 1320, height: 900 } });
  const p = await b.newPage(); const t0 = Date.now(), el = () => ((Date.now() - t0) / 1000).toFixed(1);
  p.on('console', (m) => { const t = m.text(); if (/gpu|GPU|warm|skipped|held|used/.test(t) || m.type() === 'error') console.log(el(), 'CON', m.type(), t.slice(0, 300)); });
  p.on('pageerror', (e) => console.log(el(), 'PAGEERROR', String(e.message).slice(0, 200)));
  await p.goto(url); await p.bringToFront();
  const stat = () => p.evaluate(() => { const r = window.__r, st = r.stats || {}; const m = /draws\/frame (\d+)/.exec(document.getElementById('stats').textContent);
    return { draws: m ? +m[1] : 0, fps: document.getElementById('fps').textContent, skippedTotal: st.skipped, perFrame: r.lastFrameSkip, held: st.held, pipelines: st.pipelines, pending: r.pending, lost: !!r.lost, js: (/JS ([\d.]+) ms/.exec(document.getElementById('stats').textContent) || [])[1] }; });
  await sleep(+(process.env.THROTTLE_AT || 30) * 1000); await p.keyboard.down('ArrowUp'); let n = 0, steer = '';
  while ((Date.now() - t0) / 1000 < +secs) {
    await sleep(4000); n++; if (n % 3 === 0) { if (steer) await p.keyboard.up(steer); steer = n % 6 === 0 ? 'ArrowLeft' : 'ArrowRight'; await p.keyboard.down(steer); await sleep(400); await p.keyboard.up(steer); steer = ''; }
    console.log(el(), 'S', JSON.stringify(await stat()));
  }
  await p.keyboard.up('ArrowUp'); await p.screenshot({ path: `${pre}.final.png` });
  const w = await p.evaluate(() => (typeof __gpuWarm === 'function' ? JSON.stringify(__gpuWarm()) : null)); if (w) { fs.writeFileSync(`${pre}.warm.json`, w); console.log('WARM written', w.length); } else console.log('WARM missing: __gpuWarm not defined');
  await b.close();
})();
