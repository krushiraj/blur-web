#!/usr/bin/env node
// Real-time (non-DET) pacing check with a real viewer: measures the interval between rendered frames AS SEEN BY THE PAGE (each render-command batch arriving on the websocket is one
// Present) after the front-end is visible, and the harness' own [fps] lines.
//   node realtime.js [--bin B] [--harness H] [--port 8798] [--seconds 100] [--window 30] [--min-fps 45]
// PASS criteria (deliberately loose, because CPU load on a shared machine changes the numbers): mean fps >= --min-fps, p99 frame interval <= 100 ms, no log errors.
// The numbers are always printed (mean, p50/p90/p99/max interval, % of intervals > 20 ms and > 33 ms) together with the machine load average, so regressions can be compared by eye.
const { spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { launch } = require('./lib/browser');
const ROOT = path.resolve(__dirname, '../../..');
const args = process.argv.slice(2);
const opt = (n, d) => { const i = args.indexOf('--' + n); return i >= 0 ? args[i + 1] : d; };
const harness = path.resolve(opt('harness', path.join(ROOT, 'port/harness')));
const bin = path.resolve(opt('bin', path.join(harness, 'blurrt')));
const port = +opt('port', 8798), seconds = +opt('seconds', 100), windowS = +opt('window', 30), minFps = +opt('min-fps', 45);
const out = path.resolve(opt('out', path.join(__dirname, 'out'))); fs.mkdirSync(out, { recursive: true });
const gameExe = process.env.BLUR_EXE || path.join(ROOT, 'game/extracted/BLUR/Blur.exe');

(async () => {
  const env = Object.assign({}, process.env, { BLUR_WEB: String(port), BLUR_RECOMP: '1', BINK_SETS: '1', BINK_FAKE: '1', BLUR_FPSLOG: '1', BLUR_LEARNED: '/dev/null', BLUR_SECONDS: String(seconds + 15) });
  delete env.BLUR_DET;
  const lines = []; let tail = ''; let ready = null;
  const child = spawn(bin, [gameExe], { cwd: harness, env, stdio: ['ignore', 'pipe', 'pipe'] });
  const logFile = fs.createWriteStream(path.join(out, 'realtime.log'));
  const onData = (d) => { logFile.write(d); tail += d.toString(); let i; while ((i = tail.indexOf('\n')) >= 0) { const l = tail.slice(0, i); tail = tail.slice(i + 1); lines.push(l); if (ready && l.includes('[net] waiting')) { ready(); ready = null; } } };
  child.stdout.on('data', onData); child.stderr.on('data', onData);
  const exited = new Promise((r) => child.on('exit', r));
  await new Promise((res) => { ready = res; setTimeout(res, 20000); });
  const browser = await launch(); const page = await browser.newPage();
  await page.evaluateOnNewDocument(() => {            // the page acks every presented frame with Int32Array [9, presented-so-far, ...]: timestamp those acks (one per websocket batch carrying >= 1 Present)
    window.__t = []; const os = WebSocket.prototype.send;
    WebSocket.prototype.send = function (d) { try { const a = new Int32Array(d); if (a.length === 5 && a[0] === 9) window.__t.push([performance.now(), a[1]]); } catch (e) {} return os.apply(this, arguments); };
  });
  await page.goto(`http://127.0.0.1:${port}/`);
  const t0 = Date.now(); await new Promise((r) => setTimeout(r, seconds * 1000));
  const stamps = await page.evaluate(() => window.__t); await browser.close(); child.kill('SIGTERM'); await exited; logFile.end();

  const results = []; const check = (name, pass, detail) => results.push({ name, pass, detail });
  const end = stamps.length ? stamps[stamps.length - 1][0] : 0; const win = stamps.filter((t) => t[0] >= end - windowS * 1000);
  const iv = []; for (let i = 1; i < win.length; i++) { const dn = win[i][1] - win[i - 1][1]; if (dn > 0) for (let k = 0; k < dn; k++) iv.push((win[i][0] - win[i - 1][0]) / dn); }   // per-present interval (a batch with n presents counts n times)
  iv.sort((a, b) => a - b);
  const pct = (p) => iv.length ? iv[Math.min(iv.length - 1, Math.floor(p * iv.length))] : NaN;
  const mean = iv.length ? iv.reduce((a, b) => a + b, 0) / iv.length : NaN; const fps = 1000 / mean;
  const over = (ms) => (100 * iv.filter((x) => x > ms).length / Math.max(1, iv.length)).toFixed(1);
  const dpf = lines.map((l) => /\[fps\] [\d.]+ fps\s+draws\/frame (\d+)/.exec(l)).filter(Boolean).map((m) => +m[1]);
  const inGame = dpf.length && dpf[dpf.length - 1] >= 150;
  console.log(`load average ${os.loadavg().map((x) => x.toFixed(1)).join(' ')} on ${os.cpus().length} cores; ${stamps.length} acks received in ${seconds} s; last ${windowS} s window: ${iv.length} presents`);
  console.log(`frame interval ms: mean ${mean.toFixed(2)}  p50 ${pct(0.5).toFixed(2)}  p90 ${pct(0.9).toFixed(2)}  p99 ${pct(0.99).toFixed(2)}  max ${iv.length ? iv[iv.length - 1].toFixed(1) : NaN}   >20ms: ${over(20)}%  >33ms: ${over(33)}%   => ${fps.toFixed(1)} fps`);
  check('front-end reached (draws/frame >= 150 at the end)', inGame, `last draws/frame ${dpf[dpf.length - 1]}`);
  check(`mean fps >= ${minFps} over the last ${windowS} s`, fps >= minFps, `${fps.toFixed(1)} fps`);
  check('p99 frame interval <= 100 ms', pct(0.99) <= 100, `${pct(0.99).toFixed(1)} ms`);
  const log = lines.join('\n');
  for (const re of [/not recompiled/, /\[UNIMPLEMENTED\]/, /\[DEADLOCK\]/, /Segmentation fault/]) check(`no ${re.source}`, !re.test(log), '');
  let fails = 0; for (const r of results) { if (!r.pass) fails++; console.log(`${r.pass ? 'PASS' : 'FAIL'}  ${r.name}  ${r.detail}`); }
  console.log(`${fails ? 'FAILED' : 'OK'}: ${results.length - fails}/${results.length} real-time checks passed in ${((Date.now() - t0) / 1000).toFixed(0)} s`);
  process.exit(fails ? 1 : 0);
})().catch((e) => { console.error('realtime.js error:', e); process.exit(2); });
