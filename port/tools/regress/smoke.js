#!/usr/bin/env node
// Native smoke test: runs the harness deterministically (BLUR_DET, frame-keyed input) with a headless Chromium viewer, takes canvas screenshots at fixed game frames
// and checks the log + the screenshots. See README.md.
//   node smoke.js [--bin <blurrt>] [--harness <dir>] [--port 8797] [--frames N] [--update-refs] [--out <dir>] [--no-shots] [--scenario <name>]
const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');
const { launch, shootCanvas } = require('./lib/browser');
const imgdiff = require('./lib/imgdiff');

const ROOT = path.resolve(__dirname, '../../..');
const args = process.argv.slice(2);
const opt = (name, dflt) => { const i = args.indexOf('--' + name); return i >= 0 ? (args[i + 1] && !args[i + 1].startsWith('--') ? args[i + 1] : true) : dflt; };
const harness = path.resolve(opt('harness', path.join(ROOT, 'port/harness')));
const bin = path.resolve(opt('bin', path.join(harness, 'blurrt')));
const port = +opt('port', 8797);
const out = path.resolve(opt('out', path.join(__dirname, 'out')));
const updateRefs = !!opt('update-refs', false);
const noShots = !!opt('no-shots', false);
const gameExe = process.env.BLUR_EXE || path.join(ROOT, 'game/extracted/BLUR/Blur.exe');

// The canonical walk. Frames are GAME frames (deterministic with BLUR_DET), not seconds, so it is independent of machine speed.
// Loading ends around frame 7450; keys are taps (held ~8 frames).
const SCENARIO = JSON.parse(fs.readFileSync(path.join(__dirname, 'scenario.json'), 'utf8'));
const frames = +opt('frames', SCENARIO.frames);
const keys = SCENARIO.keys.map((k) => `${k.key}@f${k.frame}`).join(',');
const checkpoints = SCENARIO.checkpoints.filter((c) => c.frame <= frames);

fs.mkdirSync(out, { recursive: true });
const results = [];
const check = (name, pass, detail) => { results.push({ name, pass: !!pass, detail: detail || '' }); };

(async () => {
  const t0 = Date.now();
  const env = Object.assign({}, process.env, {
    BLUR_WEB: String(port), BLUR_RECOMP: '1', BINK_SETS: '1', BINK_FAKE: '1', BLUR_DET: String(SCENARIO.det), BLUR_REL: SCENARIO.rel ? '1' : '', BLUR_MAXFRAMES: String(frames),
    BLUR_KEYS: keys, BLUR_CHECKPOINTS: checkpoints.map((c) => `${c.frame}:${c.name}`).join(','), BLUR_CHECKPOINT_MS: '1500',
    BLUR_OBJSTAT: '1', BLUR_FPSLOG: '1', BLUR_HEAPCHK: '1', BLUR_STRICT: '1', BLUR_TEXAUDIT: '1', BLUR_HANGEXIT: '1', BLUR_LEARNED: '/dev/null', BLUR_FRAMEHASH: process.env.BLUR_FRAMEHASH || '',
  });
  if (!env.BLUR_FRAMEHASH) delete env.BLUR_FRAMEHASH; if (!env.BLUR_REL) delete env.BLUR_REL;
  const lines = []; let tail = '';
  const child = spawn(bin, [gameExe], { cwd: harness, env, stdio: ['ignore', 'pipe', 'pipe'] });
  const logFile = fs.createWriteStream(path.join(out, 'harness.log'));
  const waiters = [];
  const onData = (d) => { logFile.write(d); tail += d.toString(); let i; while ((i = tail.indexOf('\n')) >= 0) { const l = tail.slice(0, i); tail = tail.slice(i + 1); lines.push(l); for (const w of waiters.slice()) w(l); } };
  child.stdout.on('data', onData); child.stderr.on('data', onData);
  let exitCode = null; const exited = new Promise((res) => child.on('exit', (c, s) => { exitCode = c === null ? 'signal ' + s : c; res(); }));

  let browser = null, page = null; const shots = [];
  if (!noShots) {
    browser = await launch(); page = await browser.newPage();
    page.on('pageerror', (e) => lines.push('[pageerror] ' + String(e).slice(0, 200)));
    await new Promise((res) => { const w = (l) => { if (l.includes('[net] waiting')) { waiters.splice(waiters.indexOf(w), 1); res(); } }; waiters.push(w); setTimeout(res, 20000); });
    await page.goto(`http://127.0.0.1:${port}/#sync`);      // #sync: synchronous shader compilation (async compile pops draws in/out depending on driver timing = non-reproducible screenshots)
    waiters.push(async (l) => {
      const m = /^\[checkpoint\] (\d+) (\S+)/.exec(l); if (!m) return;
      await new Promise((r) => setTimeout(r, 700));
      const f = path.join(out, `${m[2]}.png`); try { await shootCanvas(page, f); shots.push({ name: m[2], frame: +m[1], file: f }); } catch (e) { lines.push('[shot-error] ' + e); }
    });
  }
  await exited; await new Promise((r) => setTimeout(r, 500));
  if (browser) await browser.close();
  logFile.end();
  const log = lines.join('\n');

  // ---- log checks ----
  check('process exited by frame limit', exitCode === 0 || /\[exit\] status=5/.test(log), `exit=${exitCode}`);
  check('reached target frames', new RegExp(`\\[maxframes\\] stopping after ${frames} frames`).test(log), `frames=${frames}`);
  const bad = [/not recompiled/, /\[UNIMPLEMENTED\]/, /\[DEADLOCK\]/, /\[watchdog\] no scheduling/, /OVERRUN/, /Segmentation fault/, /guest heap exhausted/, /d3d9 object table full/];
  for (const re of bad) check(`no ${re.source}`, !re.test(log), (log.match(re) || [''])[0]);
  const strict = lines.filter((l) => l.startsWith('[strict]')); const allowed = SCENARIO.allowStrict || [];
  const unexpected = strict.filter((l) => !allowed.some((a) => l.includes(a)));
  check('no unexpected [strict] findings', unexpected.length === 0, unexpected.slice(0, 3).join(' | '));
  const hm = /\[heap\] (\d+) MB/.exec(log); check('guest heap bounded', hm && +hm[1] <= SCENARIO.maxHeapMB, hm ? `${hm[1]} MB (limit ${SCENARIO.maxHeapMB})` : 'no [heap] line');
  const ht = lines.map((l) => /\[heaptrace\] frame (\d+): guest heap (\d+) MB/.exec(l)).filter(Boolean).map((m) => [+m[1], +m[2]]);
  const rb = /\[rel\] front-end visible at absolute frame (\d+)/.exec(log); const steadyFrom = (rb ? +rb[1] : 0) + SCENARIO.steadyFromFrame;
  const steady = ht.filter(([f]) => f >= steadyFrom);
  if (steady.length >= 2) { const growth = steady[steady.length - 1][1] - steady[0][1]; check('heap steady-state growth', growth <= SCENARIO.maxSteadyGrowthMB, `${growth} MB over frames ${steady[0][0]}..${steady[steady.length - 1][0]} (limit ${SCENARIO.maxSteadyGrowthMB})`); }
  else check('heap steady-state growth', false, 'not enough [heaptrace] lines');
  const ta = /\[texaudit\] exit frame=\d+: (\d+) textures filled, (\d+) all-zero/.exec(log);
  check('textures populated', ta && +ta[1] >= SCENARIO.minTexturesFilled && +ta[2] <= SCENARIO.maxTexturesZero, ta ? `${ta[1]} filled / ${ta[2]} zero (need >= ${SCENARIO.minTexturesFilled} / <= ${SCENARIO.maxTexturesZero})` : 'no texaudit line');
  const relIdx = lines.findIndex((l) => l.startsWith('[rel] front-end visible')); const dp = lines.slice(relIdx >= 0 ? relIdx : 0).map((l) => /\[fps\] [\d.]+ fps\s+draws\/frame (\d+)/.exec(l)).filter(Boolean).map((m) => +m[1]).sort((a, b) => a - b);
  if (dp.length) { const med = dp[dp.length >> 1]; check('draws/frame sane (CPU-load independent)', med >= SCENARIO.drawsPerFrame[0] && med <= SCENARIO.drawsPerFrame[1], `median ${med} (range ${SCENARIO.drawsPerFrame})`); }
  const hc = /\[heapchk\] (\d+) allocations tracked, (\d+) overruns/.exec(log); check('heap canaries intact', hc && +hc[2] === 0, hc ? `${hc[1]} allocations, ${hc[2]} overruns` : 'no heapchk line');

  // ---- screenshot checks ----
  if (!noShots) {
    check('all checkpoints captured', shots.length === checkpoints.length, `${shots.length}/${checkpoints.length}`);
    const br = await launch(); const d = await imgdiff.open(br); const refDir = path.join(__dirname, 'refs');
    for (const s of shots) {
      const cp = checkpoints.find((c) => c.name === s.name); const st = await d.stats(s.file);
      check(`${s.name}: not flat`, st.lumaStd >= (cp.minStd || 12), `luma std ${st.lumaStd.toFixed(1)}`);
      const ref = path.join(refDir, `${s.name}.png`);
      if (updateRefs) { fs.copyFileSync(s.file, ref); check(`${s.name}: reference updated`, true, ref); continue; }
      if (!fs.existsSync(ref)) { check(`${s.name}: matches reference`, false, 'no reference image (run with --update-refs)'); continue; }
      const m = await d.compare(s.file, ref);
      check(`${s.name}: matches reference`, m.mae <= (cp.maxMae || SCENARIO.maxMae) && m.ssim >= (cp.minSsim || SCENARIO.minSsim), `MAE ${m.mae.toFixed(4)} (<= ${cp.maxMae || SCENARIO.maxMae}), SSIM ${m.ssim.toFixed(3)} (>= ${cp.minSsim || SCENARIO.minSsim})`);
    }
    await br.close();
  }

  // ---- report ----
  const w = Math.max(...results.map((r) => r.name.length));
  let fails = 0; for (const r of results) { if (!r.pass) fails++; console.log(`${r.pass ? 'PASS' : 'FAIL'}  ${r.name.padEnd(w)}  ${r.detail}`); }
  fs.writeFileSync(path.join(out, 'results.json'), JSON.stringify({ results, seconds: (Date.now() - t0) / 1000 }, null, 1));
  console.log(`${fails ? 'FAILED' : 'OK'}: ${results.length - fails}/${results.length} checks passed in ${((Date.now() - t0) / 1000).toFixed(0)} s (logs + screenshots: ${out})`);
  process.exit(fails ? 1 : 0);
})().catch((e) => { console.error('smoke.js error:', e); process.exit(2); });
