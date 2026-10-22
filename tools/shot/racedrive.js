// racedrive.js - page half of the scripted walk to a race (see racedrive.sh, which starts the game and calls this).
// usage: [HYB=<s>] [PROF=<chromium profile dir>] node racedrive.js <url> <total-seconds> <shot-prefix>
// HYB=<s>: host-injected keys (BLUR_KEYS) already do title + Skip; this script just waits <s> seconds, then presses a REAL browser Enter every 8 s.
//   (Host-injected Enters do NOT dismiss the 'Some features will be unavailable' WARNING popup; browser key events do.)
// without HYB: the page also presses title Enter / Down / Enter itself (timing fragile: the Online Account dialog may not be up yet).
// prints a stats line every 8 s ('S {draws, fps, wireMB, js ms/frame, cache summary}') and saves screenshots <prefix>.cNNN.png / .final.png.
const puppeteer = require('puppeteer-core');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
(async () => {
  const [url, secs, pre] = process.argv.slice(2);
  const b = await puppeteer.launch({ executablePath: process.env.CHROMIUM || '/Applications/Chromium.app/Contents/MacOS/Chromium', headless: 'new', userDataDir: process.env.PROF || '/tmp/blur-trackA/prof_drv', args: ['--use-angle=metal', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist', '--window-size=1320,900'], defaultViewport: { width: 1320, height: 900 } });
  const p = await b.newPage(); await p.goto(url); await p.bringToFront();
  const key = async (k, hold = 200) => { await p.keyboard.down(k); await sleep(hold); await p.keyboard.up(k); };
  const stat = () => p.evaluate(() => { const s = document.getElementById('stats').textContent, m = /draws\/frame (\d+)/.exec(s), f = document.getElementById('fps').textContent; return { draws: m ? +m[1] : 0, fps: f, wireMB: +(window.__wire.bytes / 1048576).toFixed(0), js: (/JS ([\d.]+) ms/.exec(s) || [])[1], hc: window.__hc && window.__hc.summary() }; });
  const t0 = Date.now(), el = () => ((Date.now() - t0) / 1000) | 0; let n = 0;
  const shot = async (tag) => { await p.screenshot({ path: `${pre}.${tag}.png` }); console.log(el(), tag, JSON.stringify(await stat())); };
  if (process.env.HYB) { await sleep(+process.env.HYB * 1000); await shot('a_hyb'); } else {
  await sleep(25000); await key('Enter'); await sleep(7000); await shot('a_afterTitle');
  await key('ArrowDown'); await sleep(1500); await key('Enter'); await sleep(5000); await shot('b_afterSkip'); }
  while (el() < +secs) { await key('Enter'); await sleep(8000); console.log(el(), 'S', JSON.stringify(await stat())); if (++n % 3 === 0) await shot('c' + String(el()).padStart(3, '0')); }
  await shot('final'); await b.close();
})();
