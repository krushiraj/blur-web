// usage: node mpdrive.js  -- attaches headless Chromium to game instances 1 and 2 (ports 8871/8872) and serves a tiny control API on :9100
//   GET /key?i=1&k=Enter&hold=150   press a key in instance i;  GET /shot?i=1  -> saves /tmp/blur-drv<i>.png
const puppeteer = require('puppeteer-core'), http = require('http');
(async () => {
  const browser = await puppeteer.launch({ executablePath: process.env.CHROMIUM || '/Applications/Chromium.app/Contents/MacOS/Chromium', headless: 'new',
    args: ['--use-angle=metal', '--ignore-gpu-blocklist', '--window-size=1320,900'], defaultViewport: { width: 1320, height: 900 } });
  const ports = (process.argv[2] || '8871,8872').split(','), pages = {};
  for (let i = 0; i < ports.length; i++) { const p = await browser.newPage(); await p.goto(`http://127.0.0.1:${ports[i]}/`); pages[i + 1] = p; }
  http.createServer(async (rq, rs) => {
    const u = new URL(rq.url, 'http://x'), i = u.searchParams.get('i'), p = pages[i];
    try {
      if (u.pathname === '/key') { await p.bringToFront(); await p.keyboard.down(u.searchParams.get('k')); await new Promise((r) => setTimeout(r, +(u.searchParams.get('hold') || 150))); await p.keyboard.up(u.searchParams.get('k')); }
      else if (u.pathname === '/shot') { await p.bringToFront(); await p.screenshot({ path: `/tmp/blur-drv${i}.png` }); }
      rs.end('ok\n');
    } catch (e) { rs.end('err ' + e.message + '\n'); }
  }).listen(9100, '127.0.0.1');
  console.log('driver ready');
})();
