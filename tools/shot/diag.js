// usage: node diag.js <url-with-#diag=1> <min-draws> <seconds>  -- loads the page, waits for the race, records per-frame draw counts by render target for <seconds>,
// then compares the heaviest 5% of frames with the median frame: which render targets get extra draws, and how big they are.
const puppeteer = require('puppeteer-core');
(async () => {
  const [url, minDraws, secs] = process.argv.slice(2);
  const browser = await puppeteer.launch({ executablePath: process.env.CHROMIUM || '/Applications/Chromium.app/Contents/MacOS/Chromium', headless: 'new', args: ['--use-angle=metal', '--ignore-gpu-blocklist', '--window-size=1320,900'], defaultViewport: { width: 1320, height: 900 } });
  const page = await browser.newPage(); await page.goto(url);
  for (;;) { await new Promise((r) => setTimeout(r, 3000)); const d = await page.evaluate(() => { const m = /draws\/frame (\d+)/.exec(document.getElementById('stats').textContent); return m ? +m[1] : 0; }); if (d >= +minDraws) break; }
  await page.evaluate(() => { window.__r.diagFrames.length = 0; });
  await new Promise((r) => setTimeout(r, +secs * 1000));
  const out = await page.evaluate(() => {
    const fr = window.__r.diagFrames.slice(), n = fr.length; const sorted = fr.map((f) => f.d).sort((a, b) => a - b); const med = sorted[n >> 1], p95 = sorted[Math.floor(n * 0.95)];
    const heavy = fr.filter((f) => f.d >= p95), typ = fr.filter((f) => Math.abs(f.d - med) < med * 0.05);
    const avg = (list) => { const o = {}; for (const f of list) for (const k in f.rts) o[k] = (o[k] || 0) + f.rts[k] / list.length; return o; };
    const A = avg(heavy), B = avg(typ), keys = new Set([...Object.keys(A), ...Object.keys(B)]);
    const rows = [...keys].map((k) => { const id = +k.split(':')[1], t = k.startsWith('1:') ? window.__r.textures.get(id) : null; return { k, size: t ? t.w + 'x' + t.h : (k.startsWith('0:') ? 'backbuffer' : '?'), heavy: +(A[k] || 0).toFixed(1), typical: +(B[k] || 0).toFixed(1), delta: +((A[k] || 0) - (B[k] || 0)).toFixed(1) }; }).sort((a, b) => b.delta - a.delta);
    return { frames: n, median: med, p95, heavyFrames: heavy.length, typicalFrames: typ.length, rows: rows.slice(0, 12) };
  });
  const gl = await page.evaluate(() => { const fr = window.__r.diagFrames.slice(-600), n = fr.length, o = {}, tot = []; for (const f of fr) { let t = 0; for (const k in f.gl) { o[k] = (o[k] || 0) + f.gl[k]; t += f.gl[k]; } tot.push([f.d, t]); } const calls = Object.entries(o).map(([k, v]) => [k, +(v / n).toFixed(0)]).sort((a, b) => b[1] - a[1]).slice(0, 18); const avgD = tot.reduce((s, x) => s + x[0], 0) / n, avgT = tot.reduce((s, x) => s + x[1], 0) / n; return { avgDraws: avgD.toFixed(0), avgGlCalls: avgT.toFixed(0), perDraw: (avgT / avgD).toFixed(1), calls }; });
  console.log('GL calls actually issued per frame: avg draws ' + gl.avgDraws + ', GL calls ' + gl.avgGlCalls + ' (' + gl.perDraw + ' per draw)'); console.log(gl.calls.map(([k, v]) => k + ' ' + v).join('  '));
  console.log(JSON.stringify({ frames: out.frames, medianDraws: out.median, p95Draws: out.p95, heavyFrames: out.heavyFrames, typicalFrames: out.typicalFrames }));
  for (const r of out.rows) console.log(`${r.k.padEnd(12)} ${r.size.padEnd(10)} heavy ${String(r.heavy).padStart(7)}  typical ${String(r.typical).padStart(7)}  extra ${String(r.delta).padStart(7)}`);
  await browser.close();
})();
