// livetest.js - page half for comparing renderers on the real game (use via: DRIVER=livetest.js tools/shot/racedrive.sh <tag> "gpu=1&hcache=1" 260 ...).
// Same key walk as racedrive.js (HYB: host does title+Skip, page presses Enter every 8 s after HYB seconds) plus: --enable-unsafe-webgpu, console error/warning capture,
// a stats line every 4 s (renderer name, fps, draws, skipped/frame, JS ms/frame, JS heap MB, GPU note, cache summary) and screenshots at title (35 s), career (75 s), race start and mid-race (+60 s after the first >=1500-draw frame).
// usage: [HYB=45] [PROF=dir] [CHROME_BIN=path] node livetest.js <url> <total-seconds> <shot-prefix>
const puppeteer = require('./node_modules/puppeteer-core');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
(async () => {
  const [url, secs, pre] = process.argv.slice(2);
  const b = await puppeteer.launch({ executablePath: process.env.CHROME_BIN || '/Applications/Chromium.app/Contents/MacOS/Chromium', headless: 'new', userDataDir: process.env.PROF,
    args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-angle=metal', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist', '--window-size=1320,900', '--disable-background-timer-throttling', '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows'], defaultViewport: { width: 1320, height: 900 } });
  const p = await b.newPage(); const msgs = new Map(); let nerr = 0, nwarn = 0;
  p.on('console', (m) => { const t = m.type(); if (t !== 'error' && t !== 'warning' && t !== 'warn') return; if (t === 'error') nerr++; else nwarn++; const k = m.text().slice(0, 200); msgs.set(k, (msgs.get(k) || 0) + 1); });
  p.on('pageerror', (e) => { nerr++; const k = 'PAGEERROR ' + String(e.message).slice(0, 200); msgs.set(k, (msgs.get(k) || 0) + 1); });
  await p.goto(url); await p.bringToFront();
  const key = async (k, hold = 200) => { await p.keyboard.down(k); await sleep(hold); await p.keyboard.up(k); };
  const stat = () => p.evaluate(() => { const r = window.__r, s = document.getElementById('stats').textContent, m = /draws\/frame (\d+)/.exec(s), f = document.getElementById('fps').textContent;
    return { gpu: !!(r && r.stats && r.stats.pipelines !== undefined), name: document.querySelector('#bar b').textContent, note: (document.getElementById('gpunote') || {}).textContent || '', draws: m ? +m[1] : 0, fps: f, js: (/JS ([\d.]+) ms/.exec(s) || [])[1],
      skipped: r && r.lastFrameSkip !== undefined ? r.lastFrameSkip : null, pend: r && r.pending, heapMB: performance.memory ? +(performance.memory.usedJSHeapSize / 1048576).toFixed(0) : null,
      lost: !!(r && r.lost), pipelines: r && r.stats && r.stats.pipelines, pipeFail: r && r.stats && r.stats.pipelineFail, wireMB: +(window.__wire.bytes / 1048576).toFixed(0), hc: window.__hc && window.__hc.summary() }; });
  const t0 = Date.now(), el = () => (Date.now() - t0) / 1000; let raceAt = 0; const done = {};
  const shot = async (tag) => { await p.screenshot({ path: `${pre}.${tag}.png` }); console.log(el() | 0, 'SHOT', tag, JSON.stringify(await stat())); };
  const hyb = +(process.env.HYB || 45); await sleep(33000); await shot('title'); await sleep(Math.max(0, hyb * 1000 - (Date.now() - t0)));
  let tick = 0;
  while (el() < +secs) {
    await key('Enter'); await sleep(4000);
    const s = await stat(); console.log(el() | 0, 'S', JSON.stringify(s));
    if (!raceAt && s.draws >= 1500) raceAt = el();
    if (!done.title && el() > 35) { done.title = 1; }
    if (!done.career && el() > 75 && !raceAt) { done.career = 1; await shot('career'); }
    if (raceAt && !done.rs && el() > raceAt + 5) { done.rs = 1; await shot('racestart'); }
    if (raceAt && !done.mid && el() > raceAt + 60) { done.mid = 1; await shot('midrace'); }
    await sleep(4000); if (++tick % 4 === 0) console.log(el() | 0, 'S', JSON.stringify(await stat()));
  }
  await shot('final'); console.log('RACEAT', raceAt, 'ERR', nerr, 'WARN', nwarn); for (const [k, v] of msgs) console.log('MSG x' + v, k);
  await b.close();
})();
