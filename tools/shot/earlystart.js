// earlystart.js - first-seconds probe: samples the page every 0.5 s from load for <seconds> (cumulative skipped draws, pipelines compiled/pending, draws/frame, fps). No keys pressed.
// usage: [PROF=dir] node earlystart.js <url> <seconds> <shot-prefix>   (or DRIVER=earlystart.js tools/shot/racedrive.sh ...)
const puppeteer = require('./node_modules/puppeteer-core');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
(async () => {
  const [url, secs] = process.argv.slice(2);
  const b = await puppeteer.launch({ executablePath: process.env.CHROMIUM || '/Applications/Chromium.app/Contents/MacOS/Chromium', headless: 'new', userDataDir: process.env.PROF,
    args: ['--enable-unsafe-webgpu', '--use-angle=metal', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist', '--window-size=1320,900', '--disable-background-timer-throttling', '--disable-renderer-backgrounding'], defaultViewport: { width: 1320, height: 900 } });
  const p = await b.newPage(); await p.goto(url); const t0 = Date.now();
  while ((Date.now() - t0) / 1000 < +secs) {
    const s = await p.evaluate(() => { const r = window.__r, st = (r && r.stats) || {}; return { t: +(performance.now() / 1000).toFixed(1), name: document.querySelector('#bar b').textContent.slice(-6), skippedTotal: st.skipped, pipelines: st.pipelines, pending: r && r.pending, fps: document.getElementById('fps').textContent, draws: st.frameDraws, wireMB: window.__wire && +(window.__wire.bytes / 1048576).toFixed(0) }; }).catch((e) => ({ err: String(e) }));
    console.log(((Date.now() - t0) / 1000).toFixed(1), 'S', JSON.stringify(s)); await sleep(500);
  }
  await b.close();
})();
