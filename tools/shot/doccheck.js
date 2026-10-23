// usage: node doccheck.js <file.html> <out-prefix>  -- loads a local HTML doc, reports mermaid render errors, broken images, console errors, and saves screenshots
const puppeteer = require('puppeteer-core');
(async () => {
  const [file, pre] = process.argv.slice(2);
  const b = await puppeteer.launch({ executablePath: process.env.CHROMIUM || '/Applications/Chromium.app/Contents/MacOS/Chromium', headless: 'new', args: ['--allow-file-access-from-files', '--window-size=1400,1000'], defaultViewport: { width: 1400, height: 1000 } });
  const p = await b.newPage(); const errs = [];
  p.on('console', (m) => { if (m.type() === 'error' || m.type() === 'warning') errs.push(m.type() + ': ' + m.text().slice(0, 200)); });
  p.on('pageerror', (e) => errs.push('pageerror: ' + String(e).slice(0, 300)));
  await p.goto('file://' + file); await new Promise((r) => setTimeout(r, 6000));
  const r = await p.evaluate(async () => {
    const m = [...document.querySelectorAll('pre.mermaid')];
    const bad = m.map((n, i) => ({ i, svg: !!n.querySelector('svg'), txt: n.querySelector('svg') ? '' : n.textContent.slice(0, 80), err: /Syntax error|Parse error/i.test(n.textContent + (n.querySelector('svg') ? n.querySelector('svg').textContent : '')) })).filter((x) => !x.svg || x.err);
    const imgs = [...document.images]; imgs.forEach((i) => { i.loading = 'eager'; }); await Promise.race([Promise.all(imgs.map((i) => (i.complete ? 0 : new Promise((res) => { i.onload = i.onerror = res; })))), new Promise((res) => setTimeout(res, 15000))]);
    const broken = imgs.filter((i) => !i.naturalWidth).map((i) => i.src);
    const charts = [...document.querySelectorAll('.bars')].map((e) => e.id + ':' + e.children.length);
    return { diagrams: m.length, bad, images: imgs.length, broken, charts, height: document.body.scrollHeight };
  });
  console.log(JSON.stringify(r, null, 1)); console.log('console:', errs.slice(0, 15));
  const sections = ['#tldr', '#plan', '#timeline', '#how', '#bug-ui', '#bug-collision', '#perf', '#mp', '#trackA', '#trackB', '#decisions', '#blog'];
  for (const s of sections) { await p.evaluate((q) => document.querySelector(q).scrollIntoView(), s); await new Promise((r) => setTimeout(r, 400)); await p.screenshot({ path: `${pre}${s.slice(1)}.png` }); }
  await b.close();
})();
