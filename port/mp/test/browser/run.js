#!/usr/bin/env node
'use strict';
// Headless-Chromium integration/perf test of client/virtual-lan.js against the real server.
//   node test/browser/run.js [--peers 3] [--seconds 60] [--pps 60] [--modes webrtc,relay] [--discovery 12] [--out DIR]
// One Chromium process per peer (separate profiles, no background throttling). Server runs in-process (its CPU is measured via process.cpuUsage()).
const path = require('path');
const fs = require('fs');
const os = require('os');
const puppeteer = require('puppeteer-core');
const { createServer } = require('../../server/src/server');

const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i >= 0 ? process.argv[i + 1] : d; };
const PEERS = Number(arg('peers', 3)), SECONDS = Number(arg('seconds', 60)), PPS = Number(arg('pps', 60));
const MODES = arg('modes', 'webrtc,relay').split(',').filter(Boolean), DISC = Number(arg('discovery', 12));
const OUT = arg('out', __dirname), TOPOLOGY = arg('topology', 'mesh'), TAG = arg('tag', '');
const CHROMIUM = process.env.CHROMIUM || '/Applications/Chromium.app/Contents/MacOS/Chromium';
const f1 = (x) => (x === null || x === undefined ? '-' : x.toFixed(2));

async function launch() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'blurmp-'));
  const browser = await puppeteer.launch({ executablePath: CHROMIUM, headless: 'new', userDataDir: dir,
    args: ['--disable-features=WebRtcHideLocalIpsWithMdns', '--disable-background-timer-throttling', '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows', '--no-first-run', '--no-sandbox'] });
  return browser;
}
async function openPage(browser, base) {
  const page = await browser.newPage();
  page.on('pageerror', (e) => console.log('[pageerror]', String(e).slice(0, 300)));
  page.on('console', (m) => { if (/error|warn/i.test(m.type())) console.log('[console]', m.text().slice(0, 200)); });
  await page.goto(`${base}/test/browser/page.html`); await page.waitForFunction('window.TREADY === true');
  return page;
}

async function runMode(mode, server, base, browsers) {
  const forceRelay = mode === 'relay';
  const pages = []; for (let i = 0; i < PEERS; i++) pages.push(await openPage(browsers[i], base));
  const wsUrl = `ws://127.0.0.1:${server.port}/ws`;
  await Promise.all(pages.map((p, i) => p.evaluate((o) => window.T.init(o), { url: wsUrl, name: 'peer' + i, forceRelay })));
  const room = await pages[0].evaluate((o) => window.T.create(o), { name: 'perf', public: true, max: 20, topology: TOPOLOGY });
  for (let i = 1; i < PEERS; i++) await pages[i].evaluate((c) => window.T.join(c), room.code);
  await Promise.all(pages.map((p) => p.evaluate((n) => window.T.waitPeers(n), PEERS - 1)));
  const paths = await Promise.all(pages.map((p) => p.evaluate((w) => window.T.waitPaths(w), forceRelay ? 'any' : 'webrtc')));
  const pathSummary = paths.map((pi) => pi.map((x) => x.path).join('/'));
  console.log(`[${mode}] room ${room.code} paths: ${pathSummary.join(' | ')}`);
  if (!forceRelay && paths.some((pi) => pi.some((x) => x.path !== 'webrtc' && x.path !== 'viahub'))) console.log(`[${mode}] WARNING: not all pairs are on WebRTC`);

  const m0 = await Promise.all(pages.map((p) => p.metrics())); const c0 = process.cpuUsage(); const w0 = Date.now();
  const startAt = Date.now() + 1500;
  const res = await Promise.all(pages.map((p) => p.evaluate((o) => window.T.traffic(o), { startAt, seconds: SECONDS, pps: PPS })));
  const wall = (Date.now() - w0) / 1000; const c1 = process.cpuUsage(c0); const m1 = await Promise.all(pages.map((p) => p.metrics()));

  // aggregate flows
  let sent = 0, got = 0, reorder = 0, dup = 0; const p50 = [], p99 = [], max = []; let worstLoss = 0;
  for (const r of res) for (const [s, fl] of Object.entries(r.flows)) {
    const src = res.find((x) => x.me === Number(s)); const sn = src.sent[r.me]; sent += sn; got += fl.n; reorder += fl.reorder; dup += fl.dup;
    p50.push(fl.p50); p99.push(fl.p99); max.push(fl.max); worstLoss = Math.max(worstLoss, 1 - fl.n / sn);
  }
  const cpuPage = m1.map((b, i) => ({ task: b.TaskDuration - m0[i].TaskDuration, script: b.ScriptDuration - m0[i].ScriptDuration }));
  const summary = {
    mode, topology: forceRelay ? 'n/a (relay)' : TOPOLOGY, peers: PEERS, seconds: SECONDS, pps: PPS, flows: PEERS * (PEERS - 1), sent, received: got, lossPct: 100 * (1 - got / sent), worstFlowLossPct: 100 * worstLoss, reordered: reorder, duplicates: dup,
    latencyMs: { p50_median_of_flows: f1(median(p50)), p99_worst_flow: f1(Math.max(...p99)), max_worst: f1(Math.max(...max)), min: f1(Math.min(...res.flatMap((r) => Object.values(r.flows).map((f) => f.min)))) },
    maxPollGapMs: res.map((r) => +r.maxPollGap.toFixed(1)),
    cpu: { serverPct_of_one_core: +(100 * (c1.user + c1.system) / 1e6 / wall).toFixed(1), pageTaskPct_per_page: cpuPage.map((c) => +(100 * c.task / wall).toFixed(1)), pageScriptPct_per_page: cpuPage.map((c) => +(100 * c.script / wall).toFixed(1)) },
    pathTx: res.map((r) => ({ rtcTx: r.stats.rtcTx, relayTx: r.stats.relayTx, rtcRx: r.stats.rtcRx, relayRx: r.stats.relayRx })),
    clientDrops: res.map((r) => r.stats.dropped), serverRelay: { ...server.manager.stats },
  };
  console.log(JSON.stringify(summary, null, 1));
  fs.writeFileSync(path.join(OUT, `results-${TAG || mode}.json`), JSON.stringify({ summary, raw: res }, null, 1));
  for (const p of pages) await p.close();
  return summary;
}
const median = (a) => { const s = [...a].sort((x, y) => x - y); return s[Math.floor(s.length / 2)]; };

async function runDiscovery(server, base, browser) {
  if (!DISC) return null;
  const out = {};
  for (const forceRelay of [false, true]) {
    const page = await openPage(browser, base);
    await page.evaluate((o) => window.T.init(o), { url: `ws://127.0.0.1:${server.port}/ws`, name: 'scanner', forceRelay });
    const r = await page.evaluate((o) => window.T.discovery(o), { url: `ws://127.0.0.1:${server.port}/ws`, hosts: DISC, forceRelay });
    console.log(`[discovery ${forceRelay ? 'relay' : 'webrtc'}]`, JSON.stringify(r));
    out[forceRelay ? 'relay' : 'webrtc'] = r; await page.close();
  }
  fs.writeFileSync(path.join(OUT, 'results-discovery.json'), JSON.stringify(out, null, 1));
  return out;
}

(async () => {
  const server = createServer({ port: 0, log: { debug() {}, info() {}, warn: (e, f) => console.log('[server warn]', e, JSON.stringify(f)), error() {} }, relayRate: 5000, relayBurst: 10000, maxRoomsPerIp: 100, maxConnsPerIp: 500, resumeGraceMs: 0 });
  await server.listen(); const base = `http://127.0.0.1:${server.port}`;
  console.log(`server on ${base}; peers=${PEERS} seconds=${SECONDS} pps=${PPS} modes=${MODES}`);
  const browsers = []; for (let i = 0; i < Math.max(PEERS, 1); i++) browsers.push(await launch());
  let ok = true;
  try {
    for (const mode of MODES) {
      const s = await runMode(mode, server, base, browsers);
      if (s.lossPct > 1 || s.duplicates) ok = false;
    }
    const d = await runDiscovery(server, base, browsers[0]);
    if (d) for (const r of Object.values(d)) if (r.accepted !== 10 || r.seenInWindow !== DISC - 1 || r.late !== 1 || !r.selfQueryLoopback || r.lastMs > r.windowMs) ok = false;
  } finally { for (const b of browsers) await b.close(); await server.close(); }
  console.log(ok ? 'BROWSER TESTS PASSED' : 'BROWSER TESTS: CHECK RESULTS (loss > 1 % or discovery mismatch)');
  process.exit(ok ? 0 : 1);
})().catch((e) => { console.error(e); process.exit(2); });
