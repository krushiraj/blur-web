#!/usr/bin/env node
'use strict';
// Proves the wasm build of wsock_hle.c + client/virtual-lan.js raw mode: two emscripten module instances (responder / querier), each with its own
// VirtualLan (relay path via the real lobby server, no WebRTC in node), discover each other with a broadcast and exchange unicast ping-pongs.
const { spawnSync } = require('child_process'); const path = require('path'); const os = require('os'); const fs = require('fs');
const { createServer } = require('../server/src/server');
const HLE = path.join(__dirname, '..', '..', 'work', 'net-re', 'patch', 'wsock_hle.c');
const WASMH = path.join(__dirname, '..', '..', 'work', 'wasm-core', 'harness');
const OUTDIR = fs.mkdtempSync(path.join(os.tmpdir(), 'blurmp-wasm-')); const JS = path.join(OUTDIR, 'hle.cjs');

(async () => {
  const cc = spawnSync('emcc', ['-O1', '-Wall', '-Wno-unused-function', '-I' + WASMH, path.join(__dirname, 'wasm_hle_test.c'), HLE, '-o', JS, '-sASYNCIFY', '-sMODULARIZE=1', '-sEXPORT_NAME=createModule',
    '-sENVIRONMENT=node', '-sEXPORTED_FUNCTIONS=_run_test', '-sEXPORTED_RUNTIME_METHODS=ccall', '-sINITIAL_MEMORY=64MB', '-sALLOW_MEMORY_GROWTH=1'], { encoding: 'utf8' });
  if (cc.status !== 0) { console.error(cc.stdout + cc.stderr); process.exit(2); }
  const { VirtualLan } = await import('../client/virtual-lan.js');
  const server = createServer({ port: 0, log: { debug() {}, info() {}, warn() {}, error() {} } }); await server.listen(); const url = `ws://127.0.0.1:${server.port}/ws`;
  const mkLan = async (name, create, code) => { const l = new VirtualLan({ url, name }); await l.connect(); l.enableRaw(); const j = create ? await l.createRoom({ name: 'wasm' }) : await l.joinRoom(code); return [l, j.room.code]; };
  const [A, code] = await mkLan('responder', true); const [B] = await mkLan('querier', false, code);
  for (let i = 0; i < 100 && A.peers.size < 1; i++) await new Promise((r) => setTimeout(r, 20));
  const create = require(JS); const logs = { A: [], B: [] };
  const mk = (lan, tag) => create({ blurLan: lan, print: (s) => logs[tag].push(s), printErr: (s) => logs[tag].push('ERR ' + s) });
  const [MA, MB] = await Promise.all([mk(A, 'A'), mk(B, 'B')]);
  const [ra, rb] = await Promise.all([MA.ccall('run_test', 'number', ['number', 'number'], [0, 6000], { async: true }), MB.ccall('run_test', 'number', ['number', 'number'], [1, 6000], { async: true })]);
  console.log('responder:', logs.A.join(' | ')); console.log('querier  :', logs.B.join(' | '));
  const ok = ra === 0 && rb === 0 && logs.B.some((l) => l.includes('"replyFrom":"10.77.0.1"')) && logs.A.some((l) => l.includes('"ip":"10.77.0.1"')) && logs.B.some((l) => l.includes('"ip":"10.77.0.2"'));
  A.close(); B.close(); await server.close();
  console.log(ok ? 'WASM HLE PASSED (wasm wsock_hle <-> VirtualLan raw mode <-> lobby server <-> wasm wsock_hle)' : `WASM HLE FAILED (${ra}, ${rb})`); process.exit(ok ? 0 : 1);
})().catch((e) => { console.error(e); process.exit(2); });
