'use strict';
const test = require('node:test'); const assert = require('node:assert'); const dgram = require('dgram');
const { startServer, connect, dataFrame } = require('./helpers'); const F = require('../src/frame');

test('ws: welcome, create, list, join, signalling, leave events', async () => {
  const s = await startServer({ env: { STUN_URLS: 'stun:x:1' }, resumeGraceMs: 0 });
  try {
    const a = await connect(s), b = await connect(s);
    assert.deepStrictEqual(a.msgs[0].iceServers, [{ urls: ['stun:x:1'] }]); assert.strictEqual(a.msgs[0].maxPayload, 1288);
    const j = await a.req({ t: 'create', name: 'R1', max: 5, player: 'Alice', rid: 1 }, 'joined');
    assert.strictEqual(j.you.vip, '10.77.0.1'); assert.strictEqual(j.hostId, a.msgs[0].id); assert.strictEqual(j.rid, 1);
    const l = await b.req({ t: 'list' }, 'rooms'); assert.strictEqual(l.rooms.length, 1); assert.strictEqual(l.rooms[0].host, 'Alice'); assert.strictEqual(l.rooms[0].max, 5);
    const bj = await b.req({ t: 'join', code: j.room.code, player: 'Bob' }, 'joined'); assert.strictEqual(bj.you.slot, 2); assert.strictEqual(bj.peers[0].name, 'Alice');
    const pj = await a.until(a.msg('peer_joined')); assert.strictEqual(pj.peer.name, 'Bob');
    b.send({ t: 'signal', to: a.msgs[0].id, data: { sdp: 'offer-bytes' } });
    const sg = await a.until(a.msg('signal')); assert.deepStrictEqual(sg, { t: 'signal', from: b.msgs[0].id, data: { sdp: 'offer-bytes' } });
    const e = await a.req({ t: 'signal', to: 'deadbeef', data: {} }, 'error'); assert.strictEqual(e.code, 'no_such_peer');
    await b.close(); await a.until(a.msg('peer_left'));
    assert.strictEqual((await a.req({ t: 'join', code: 'ZZZZZ' }, 'error')).code, 'already_in_room');
    await a.close();
    const h = await (await fetch(`http://127.0.0.1:${s.port}/health`)).json(); await new Promise((r) => setTimeout(r, 50));
    assert.strictEqual(h.ok, true);
    const rl = await (await fetch(`http://127.0.0.1:${s.port}/rooms`)).json(); assert.ok(Array.isArray(rl.rooms));
  } finally { await s.close(); }
});

test('ws: relay unicast/broadcast with spoof rewrite, size cap', async () => {
  const s = await startServer();
  try {
    const a = await connect(s), b = await connect(s), c = await connect(s);
    const j = await a.req({ t: 'create' }, 'joined'); await b.req({ t: 'join', code: j.room.code }, 'joined'); await c.req({ t: 'join', code: j.room.code }, 'joined');
    a.sendBin(dataFrame(50001, '10.77.0.3', 50001, [1, 2, 3]));
    await c.until(() => c.bins.length === 1); assert.strictEqual(b.bins.length, 0);
    const f = F.decode(c.bins[0]); assert.strictEqual(F.numToIp(f.srcIp), '10.77.0.1'); assert.deepStrictEqual([...f.payload], [1, 2, 3]);
    b.sendBin(dataFrame(50001, '255.255.255.255', 50001, [9]));
    await a.until(() => a.bins.length === 1); await c.until(() => c.bins.length === 2); assert.strictEqual(b.bins.length, 0);
    a.sendBin(dataFrame(1, '10.77.0.2', 1, new Array(1289).fill(0))); a.sendBin(dataFrame(1, '10.77.0.2', 1, new Array(1288).fill(0)));
    await b.until(() => b.bins.length === 1); await new Promise((r) => setTimeout(r, 100)); assert.strictEqual(b.bins.length, 1, 'only the 1288-byte frame arrives');
    assert.strictEqual(s.manager.stats.dropped.size, 1);
    await a.close(); await b.close(); await c.close();
  } finally { await s.close(); }
});

test('heartbeat: silent client is dropped, responsive client survives', async () => {
  const s = await startServer({ heartbeatMs: 50, timeoutMs: 300, resumeGraceMs: 0 });
  try {
    const live = await connect(s), dead = await connect(s);
    await dead.req({ t: 'create' }, 'joined');
    dead.ws._socket.pause();                                                                                   // stop reading => no pong
    for (let i = 0; i < 100 && s.conns.size > 1; i++) await new Promise((r) => setTimeout(r, 20));
    assert.strictEqual(s.conns.size, 1); assert.strictEqual(s.manager.rooms.size, 0, 'room of the dropped host is closed');
  } finally { await s.close(); }
});

test('udp gateway: native peer joins a room, talks to a ws peer, times out', async () => {
  const s = await startServer({ udpPort: 0, udpTimeoutMs: 400 });
  const nat = dgram.createSocket('udp4'); const got = [];
  nat.on('message', (m) => got.push(F.decode(m)));
  const sendU = (b) => nat.send(b, s.udpPort, '127.0.0.1');
  const waitFor = async (p) => { for (let i = 0; i < 100; i++) { const v = got.find(p); if (v) return v; await new Promise((r) => setTimeout(r, 20)); } throw new Error('udp timeout'); };
  try {
    const a = await connect(s); const j = await a.req({ t: 'create', player: 'Web' }, 'joined');
    sendU(F.encode({ type: F.T.HELLO, srcIp: 0x0a4d0063, payload: `room=${j.room.code};name=Native` }));
    const w = await waitFor((f) => f.type === F.T.WELCOME); assert.deepStrictEqual([...w.payload.subarray(0, 5)], [10, 77, 0, 2, 2]); assert.strictEqual(w.payload.subarray(5).toString(), j.room.code);
    const pj = await a.until(a.msg('peer_joined')); assert.strictEqual(pj.peer.relayOnly, true); assert.strictEqual(pj.peer.vip, '10.77.0.2');
    sendU(F.encode({ type: F.T.DATA, srcIp: 0x01010101, srcPort: 50001, dstPort: 50001, dstIp: 0xffffffff, payload: 'from-native' }));
    await a.until(() => a.bins.length === 1); assert.strictEqual(F.numToIp(F.decode(a.bins[0]).srcIp), '10.77.0.2');
    a.sendBin(dataFrame(50001, '10.77.0.2', 50001, [5, 6]));
    const d = await waitFor((f) => f.type === F.T.DATA); assert.deepStrictEqual([...d.payload], [5, 6]); assert.strictEqual(F.numToIp(d.srcIp), '10.77.0.1');
    sendU(F.encode({ type: F.T.HELLO, srcIp: 0 }));                                                           // keepalive without payload
    await new Promise((r) => setTimeout(r, 600));                                                              // no more frames -> timeout
    await a.until(a.msg('peer_left'));
    sendU(F.encode({ type: F.T.HELLO, payload: 'room=ZZZZZ' })); const er = await waitFor((f) => f.type === F.T.ERROR); assert.strictEqual(er.payload.toString(), 'no_such_room');
    await a.close();
  } finally { nat.close(); await s.close(); }
});
