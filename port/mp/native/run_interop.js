#!/usr/bin/env node
'use strict';
// End-to-end: a NATIVE process running the real wsock_hle.c (joined through the server's UDP gateway, BLUR_NET_ROOM) <-> a headless-Chromium
// VirtualLan client in the same room, through ONE lobby server.
const { spawn, spawnSync } = require('child_process');
const path = require('path'); const os = require('os'); const fs = require('fs');
const puppeteer = require('puppeteer-core');
const { createServer } = require('../server/src/server');

const HARNESS = path.join(__dirname, '..', '..', 'harness');
const HLE = path.join(__dirname, '..', '..', 'work', 'net-re', 'patch', 'wsock_hle.c');
const BIN = path.join(os.tmpdir(), 'blurmp_native_peer');
const CHROMIUM = process.env.CHROMIUM || '/Applications/Chromium.app/Contents/MacOS/Chromium';

(async () => {
  const cc = spawnSync('clang', ['-O1', '-g', '-Wall', '-Wno-unused-function', '-I/opt/homebrew/include', '-I' + HARNESS, path.join(__dirname, 'native_peer.c'), HLE, '-L/opt/homebrew/lib', '-lunicorn', '-Wl,-rpath,/opt/homebrew/lib', '-lpthread', '-o', BIN], { encoding: 'utf8' });
  if (cc.status !== 0) { console.error(cc.stdout + cc.stderr); process.exit(2); }
  const server = createServer({ port: 0, udpPort: 0, log: { debug() {}, info: (e, f) => console.log('[server]', e, JSON.stringify(f)), warn() {}, error() {} } });
  await server.listen(); const base = `http://127.0.0.1:${server.port}`;
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'blurmp-'));
  const browser = await puppeteer.launch({ executablePath: CHROMIUM, headless: 'new', userDataDir: dir, args: ['--disable-features=WebRtcHideLocalIpsWithMdns', '--no-first-run', '--no-sandbox'] });
  let ok = false;
  try {
    const page = await browser.newPage(); page.on('pageerror', (e) => console.log('[pageerror]', String(e).slice(0, 300)));
    await page.goto(`${base}/test/browser/page.html`); await page.waitForFunction('window.TREADY === true');
    await page.evaluate((o) => window.T.init(o), { url: `ws://127.0.0.1:${server.port}/ws`, name: 'browser', forceRelay: false });
    const room = await page.evaluate(() => window.T.create({ name: 'interop', public: true }));
    const nat = spawn(BIN, ['6'], { env: { ...process.env, BLUR_NET_RELAY: `127.0.0.1:${server.udpPort}`, BLUR_NET_ROOM: room.code, BLUR_NET_NAME: 'NativeHarness', BLUR_NET_ID: '9' } });
    let out = ''; nat.stdout.on('data', (d) => (out += d)); nat.stderr.on('data', (d) => process.stdout.write('[native] ' + d));
    const r = await page.evaluate((o) => window.T.interop(o), { pings: 20 });
    await new Promise((res) => nat.on('close', res));
    const n = JSON.parse(out.trim().split('\n').pop()).native;
    console.log('browser view:', JSON.stringify(r)); console.log('native view :', JSON.stringify(n));
    ok = r.discovery && r.discovery.from === '10.77.0.2' && r.pongs >= 19 && r.announce && r.announce.from === '10.77.0.2' && n.queries >= 1 && n.pings >= 19 && n.announced === 1 && r.nativePeer.path === 'relay-only';
  } finally { await browser.close(); await server.close(); }
  console.log(ok ? 'INTEROP PASSED (native wsock_hle <-> browser VirtualLan through one room)' : 'INTEROP FAILED');
  process.exit(ok ? 0 : 1);
})().catch((e) => { console.error(e); process.exit(2); });
