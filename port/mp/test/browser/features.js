#!/usr/bin/env node
'use strict';
// Browser feature tests: session resume, star topology (+ fallbacks + host migration), kick/ban. One headless Chromium, several VirtualLan instances in a page.
const path = require('path'); const os = require('os'); const fs = require('fs');
const puppeteer = require('puppeteer-core');
const { createServer } = require('../../server/src/server');
const CHROMIUM = process.env.CHROMIUM || '/Applications/Chromium.app/Contents/MacOS/Chromium';
let ok = true; const check = (name, cond, info) => { console.log(`${cond ? 'PASS' : 'FAIL'}  ${name}${info !== undefined ? '  ' + JSON.stringify(info) : ''}`); if (!cond) ok = false; };
(async () => {
  const server = createServer({ port: 0, log: { debug() {}, info() {}, warn() {}, error() {} }, resumeGraceMs: 8000, maxConnsPerIp: 100, maxRoomsPerIp: 100 });
  await server.listen(); const url = `ws://127.0.0.1:${server.port}/ws`;
  const browser = await puppeteer.launch({ executablePath: CHROMIUM, headless: 'new', userDataDir: fs.mkdtempSync(path.join(os.tmpdir(), 'blurmp-')), args: ['--disable-features=WebRtcHideLocalIpsWithMdns', '--no-sandbox'] });
  try {
    const page = await browser.newPage(); page.on('pageerror', (e) => console.log('[pageerror]', String(e).slice(0, 300)));
    await page.goto(`http://127.0.0.1:${server.port}/test/browser/page.html`); await page.waitForFunction('window.TREADY === true');
    const r = await page.evaluate((o) => window.T.resumeTest(o), { url });
    check('resume: data channel up before the drop', r.rtc); check('resume: client noticed the outage', r.sawOffline); check('resume: datagram delivered over the data channel during the websocket outage', r.dcDuringOutage);
    check('resume: reconnected with a session token', r.resumed, { ms: r.resumeMs, events: r.events }); check('resume: same id, slot and virtual IP', r.same, r.sameVip); check('resume: traffic flows both ways afterwards', r.afterBoth); check('resume: room kept both members', r.stillOneRoom);
    const s = await page.evaluate((o) => window.T.starTest(o), { url, n: 5 });
    check('star: room topology', s.topology === 'star'); check('star: hub has a data channel to every leaf', s.hubLinks); check('star: each leaf has exactly one data channel (the hub)', s.rtcCountLeaf.every((n) => n === 1), s.leafPaths);
    check('star: leaf -> leaf unicast through the hub, source preserved', s.l2l, { hubFwd: s.hubFwdCount }); check('star: leaf broadcast reaches everybody exactly once', s.bcast.every((n) => n === 1), s.bcast); check('star: leaf -> hub and hub -> leaf direct', s.updown.join() === '1,1', s.updown);
    check('star: fallback when the leaf\'s hub channel is down (server relay)', s.fb1.length === 1 && s.fb1[0]); check('star: hub forwards through the server when a leaf channel is down (host-only FWD, source preserved)', s.fb2, s.hubStats);
    check('star: host migration rebuilds links to the new hub and traffic continues', s.migrated && s.afterMigrate);
    const k = await page.evaluate((o) => window.T.kickTest(o), { url });
    check('kick: only the host may kick', k.guestKickDenied); check('kick: guest receives kicked(reason)', k.kicked); check('kick: others see peer_left(reason=kicked)', k.peerLeft); check('ban: kicked IP cannot rejoin', k.rejoin === 'banned', k.rejoin); check('unban: can rejoin afterwards', k.unban && k.rejoinAfterUnban === 'allowed', k.rejoinAfterUnban);
  } finally { await browser.close(); await server.close(); }
  console.log(ok ? 'FEATURE TESTS PASSED' : 'FEATURE TESTS FAILED'); process.exit(ok ? 0 : 1);
})().catch((e) => { console.error(e); process.exit(2); });
