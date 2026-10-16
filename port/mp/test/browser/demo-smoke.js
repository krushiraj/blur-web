#!/usr/bin/env node
'use strict';
// Loads demo/index.html in two headless Chromium pages, creates a room in A, joins from B through the room list, pings both ways, scans, leaves.
const puppeteer = require('puppeteer-core'); const os = require('os'); const fs = require('fs'); const path = require('path');
const { createServer } = require('../../server/src/server');
const CHROMIUM = process.env.CHROMIUM || '/Applications/Chromium.app/Contents/MacOS/Chromium';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
(async () => {
  const server = createServer({ port: 0, log: { debug() {}, info() {}, warn() {}, error() {} } }); await server.listen();
  const url = `http://127.0.0.1:${server.port}/demo/`; const errs = [];
  const mk = async () => { const b = await puppeteer.launch({ executablePath: CHROMIUM, headless: 'new', userDataDir: fs.mkdtempSync(path.join(os.tmpdir(), 'blurmp-')), args: ['--disable-features=WebRtcHideLocalIpsWithMdns', '--no-sandbox'] }); const p = await b.newPage(); p.on('pageerror', (e) => errs.push(String(e))); await p.goto(url); return [b, p]; };
  const [b1, a] = await mk(), [b2, b] = await mk(); let ok = false;
  try {
    for (const p of [a, b]) await p.click('#connect');
    await a.waitForFunction(() => !document.getElementById('create').disabled);
    await a.type('#rname', 'Demo room'); await a.click('#create');
    await a.waitForFunction(() => document.getElementById('myroom').textContent.includes('Demo room'));
    await b.waitForFunction(() => document.querySelector('#rooms tbody button'), { timeout: 8000 });
    await b.click('#rooms tbody button');
    await b.waitForFunction(() => document.querySelectorAll('#peers tbody tr').length === 1 && document.querySelector('#peers tbody tr td:nth-child(3)').textContent === 'webrtc', { timeout: 15000 });
    await a.waitForFunction(() => document.querySelectorAll('#peers tbody tr').length === 1);
    await a.click('#ping'); await b.click('#bcast'); await b.click('#host'); await sleep(300);
    await a.click('#scan'); await sleep(2400);
    const pong = await a.$eval('#pong', (e) => e.textContent), scan = await a.$eval('#scanout', (e) => e.textContent), pb = await b.$eval('#pong', (e) => e.textContent);
    console.log('A pong panel:\n' + pong + '\nA scan:\n' + scan + '\nB panel:\n' + pb);
    await b.click('#leave'); await a.waitForFunction(() => document.querySelectorAll('#peers tbody tr').length === 0);
    ok = /from 10\.77\.0\.2.*rtt/.test(pong) && /1 host\(s\)/.test(scan)  && errs.length === 0;
    if (errs.length) console.log('page errors', errs);
  } finally { await b1.close(); await b2.close(); await server.close(); }
  console.log(ok ? 'DEMO SMOKE PASSED' : 'DEMO SMOKE FAILED'); process.exit(ok ? 0 : 1);
})().catch((e) => { console.error(e); process.exit(2); });
