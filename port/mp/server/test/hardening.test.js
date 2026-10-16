'use strict';
const test = require('node:test'); const assert = require('node:assert'); const WebSocket = require('ws');
const { startServer, Client, connect, dataFrame } = require('./helpers'); const F = require('../src/frame');
const { RoomManager } = require('../src/rooms');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const url = (s, q = '') => `ws://127.0.0.1:${s.port}/ws${q}`;
function peer(id, ip = '1.1.1.' + id.length, kind = 'ws') { return { id, name: id, kind, ip, room: null, sent: [], bins: [], sendJson(o) { this.sent.push(o); }, sendBin(b) { this.bins.push(b); } }; }
const upgradeStatus = (u) => new Promise((res) => { const w = new WebSocket(u); w.on('open', () => { w.close(); res(101); }); w.on('unexpected-response', (rq, rs) => res(rs.statusCode)); w.on('error', () => res(0)); });

test('rooms: topology auto (mesh <= 8, star above) and explicit override', () => {
  const m = new RoomManager();
  assert.strictEqual(m.create(peer('a'), { max: 8 }).room.topology, 'mesh');
  assert.strictEqual(m.create(peer('b', '2.2.2.2'), { max: 12 }).room.topology, 'star');
  assert.strictEqual(m.create(peer('c', '3.3.3.3'), { max: 4, topology: 'star' }).room.topology, 'star');
  assert.strictEqual(m.create(peer('d', '4.4.4.4'), { max: 20, topology: 'mesh' }).room.topology, 'mesh');
  assert.ok(m.list().every((r) => ['mesh', 'star'].includes(r.topology)));
});
test('rooms: detach keeps the slot, expire frees it, resume reattaches and notifies', () => {
  let t = 1000; const m = new RoomManager({ now: () => t }); const a = peer('a'), b = peer('b', '9.9.9.9'); const r = m.create(a, {}); m.join(b, r.room.code);
  const tok = b.token; assert.ok(tok);
  assert.ok(m.detach(b)); assert.ok(a.sent.some((x) => x.t === 'peer_offline' && x.id === 'b')); assert.strictEqual(r.room.peers.size, 2, 'slot reserved');
  assert.strictEqual(m.relay(a, F.encode({ type: 2, dstIp: F.vipForSlot(2), payload: 'x' })), 'ok'); assert.strictEqual(b.bins.length, 0, 'nothing delivered to an offline peer');
  t += 10000; assert.strictEqual(m.expireOffline(30000), 0);
  const nb = { sendJson: (o) => b.sent.push(o), sendBin: (x) => b.bins.push(x), ip: '9.9.9.9' };
  const res = m.resume(tok, nb); assert.ok(res.ok); assert.strictEqual(res.you.slot, 2); assert.ok(a.sent.some((x) => x.t === 'peer_online'));
  assert.strictEqual(m.resume(tok, nb).error, 'not_offline'); assert.strictEqual(m.resume('nope', nb).error, 'bad_token');
  m.detach(b); t += 31000; assert.strictEqual(m.expireOffline(30000), 1); assert.strictEqual(r.room.peers.size, 1); assert.strictEqual(m.resume(tok, nb).error, 'bad_token');
  assert.strictEqual(m.stats.resumes, 1); assert.strictEqual(m.stats.resumeTimeouts, 1);
});
test('rooms: host offline -> promotion prefers online peers', () => {
  const m = new RoomManager(); const a = peer('a'), b = peer('b', '2.2.2.2'), c = peer('c', '3.3.3.3'); const r = m.create(a, {}); m.join(b, r.room.code); m.join(c, r.room.code);
  m.detach(b); m.leave(a, 'leave'); assert.strictEqual(r.room.host, c);
});
test('rooms: kick (host only), ban by ip, unban, closeRoom', () => {
  const m = new RoomManager(); const a = peer('a', '1.0.0.1'), b = peer('b', '1.0.0.2'), c = peer('c', '1.0.0.3'); const r = m.create(a, {}); m.join(b, r.room.code); m.join(c, r.room.code);
  assert.strictEqual(m.kick(b, 'c').error, 'not_host'); assert.strictEqual(m.kick(a, 'zz').error, 'no_such_peer'); assert.strictEqual(m.kick(a, 'a').error, 'cannot_kick_self');
  assert.ok(m.kick(a, 'b', { ban: true }).ok); assert.ok(b.sent.some((x) => x.t === 'kicked' && x.ban)); assert.ok(c.sent.some((x) => x.t === 'peer_left' && x.reason === 'kicked'));
  assert.strictEqual(m.join(peer('b2', '1.0.0.2'), r.room.code).error, 'banned'); assert.ok(m.join(peer('b3', '1.0.0.9'), r.room.code).ok);
  r.room.bans.delete('1.0.0.2'); assert.ok(m.join(peer('b4', '1.0.0.2'), r.room.code).ok);
  assert.ok(m.closeRoom(r.room.code)); assert.strictEqual(m.rooms.size, 0); assert.ok(c.sent.some((x) => x.t === 'room_closed'));
});
test('rooms: per-IP room cap, global ip ban with expiry, password brute-force block', () => {
  let t = 0; const m = new RoomManager({ now: () => t, maxRoomsPerIp: 2, pwFailLimit: 3, pwBlockMs: 1000 });
  assert.ok(m.create(peer('a1', '5.5.5.5'), {}).ok); assert.ok(m.create(peer('a2', '5.5.5.5'), {}).ok); assert.strictEqual(m.create(peer('a3', '5.5.5.5'), {}).error, 'too_many_rooms'); assert.ok(m.create(peer('o', '6.6.6.6'), {}).ok);
  m.banIp('7.7.7.7', 500); assert.strictEqual(m.create(peer('x', '7.7.7.7'), {}).error, 'banned'); t += 600; assert.ok(m.create(peer('x2', '7.7.7.7'), {}).ok);
  const pr = m.create(peer('h', '8.8.8.8'), { password: 'pw' }); for (let i = 0; i < 3; i++) assert.strictEqual(m.join(peer('g' + i, '8.8.1.1'), pr.room.code, 'bad').error, 'bad_password');
  assert.strictEqual(m.join(peer('gg', '8.8.1.1'), pr.room.code, 'pw').error, 'blocked', 'even the right password is refused while blocked'); t += 1100; assert.ok(m.join(peer('gh', '8.8.1.1'), pr.room.code, 'pw').ok);
});

test('ws: resume within the grace period keeps id, slot and vip; others see offline/online', async () => {
  const s = await startServer({ resumeGraceMs: 2000 });
  try {
    const a = await connect(s), b = await connect(s); const j = await a.req({ t: 'create', player: 'A' }, 'joined'); const bj = await b.req({ t: 'join', code: j.room.code, player: 'B' }, 'joined');
    assert.ok(bj.resume.token && bj.resume.graceMs === 2000); const token = bj.resume.token, bid = b.msgs[0].id;
    await b.close(); const off = await a.until(a.msg('peer_offline')); assert.strictEqual(off.id, bid);
    assert.strictEqual(s.manager.rooms.get(j.room.code).peers.size, 2);
    const b2 = await connect(s); const rj = await b2.req({ t: 'resume', token }, 'joined');
    assert.ok(rj.resumed); assert.strictEqual(rj.id, bid); assert.strictEqual(rj.you.slot, 2); assert.strictEqual(rj.you.vip, '10.77.0.2'); assert.strictEqual(rj.peers.length, 1);
    await a.until(a.msg('peer_online'));
    a.sendBin(dataFrame(1, '10.77.0.2', 9, [1, 2, 3])); await b2.until(() => b2.bins.length === 1);                       // relay reaches the resumed socket
    b2.send({ t: 'signal', to: a.msgs[0].id, data: { x: 1 } }); await a.until(a.msg('signal'));                          // and it can signal again
    assert.strictEqual((await connect(s).then((c) => c.req({ t: 'resume', token }, 'joined').finally(() => c.close()))).code, 'not_offline');
    await b2.close(); await sleep(2300);
    await a.until(a.msg('peer_left'), 3000); assert.strictEqual(s.manager.rooms.get(j.room.code).peers.size, 1);
    const c = await connect(s); assert.strictEqual((await c.req({ t: 'resume', token }, 'joined')).code, 'bad_token');
    assert.strictEqual(s.manager.stats.resumes, 1); await a.close(); await c.close();
  } finally { await s.close(); }
});
test('ws: explicit leave does not keep a ghost; kick + ban over the wire', async () => {
  const s = await startServer({ resumeGraceMs: 5000 });
  try {
    const a = await connect(s), b = await connect(s), c = await connect(s);
    const j = await a.req({ t: 'create' }, 'joined'); await b.req({ t: 'join', code: j.room.code }, 'joined'); await c.req({ t: 'join', code: j.room.code }, 'joined');
    assert.strictEqual((await b.req({ t: 'kick', id: c.msgs[0].id }, 'kick_ok')).code, 'not_host');
    const k = await a.req({ t: 'kick', id: c.msgs[0].id, ban: true, reason: 'spam' }, 'kick_ok'); assert.ok(k.banned);
    const kk = await c.until(c.msg('kicked')); assert.strictEqual(kk.reason, 'spam');
    await b.until(b.msg('peer_left')); assert.deepStrictEqual((await a.req({ t: 'bans' }, 'bans')).bans, ['127.0.0.1']);
    const c2 = await connect(s); assert.strictEqual((await c2.req({ t: 'join', code: j.room.code }, 'joined')).code, 'banned');
    assert.ok((await a.req({ t: 'unban', ip: '127.0.0.1' }, 'unban_ok')).ok); assert.strictEqual((await c2.req({ t: 'join', code: j.room.code }, 'joined')).t, 'joined');
    await b.req({ t: 'leave' }, 'left'); assert.strictEqual(s.manager.rooms.get(j.room.code).peers.size, 2, 'b left for good, a + c2 remain');
    for (const x of [a, b, c, c2]) x.close();
  } finally { await s.close(); }
});
test('abuse limits: connections per IP, rooms per IP, control-message rate, oversize message, bad password block', async () => {
  const s = await startServer({ maxConnsPerIp: 3, maxRoomsPerIp: 1, msgRate: 5, msgBurst: 10, msgStrikes: 5, maxMsgBytes: 200, pwFailLimit: 3, resumeGraceMs: 0 });
  try {
    const cl = [await connect(s), await connect(s), await connect(s)];
    assert.strictEqual(await upgradeStatus(url(s)), 429, 'fourth connection from one IP refused');
    const j = await cl[0].req({ t: 'create', password: 'pw' }, 'joined'); assert.strictEqual((await cl[1].req({ t: 'create' }, 'joined')).code, 'too_many_rooms');
    for (let i = 0; i < 3; i++) assert.strictEqual((await cl[1].req({ t: 'join', code: j.room.code, password: 'x' }, 'joined')).code, 'bad_password');
    assert.strictEqual((await cl[1].req({ t: 'join', code: j.room.code, password: 'pw' }, 'joined')).code, 'blocked');
    cl[2].send({ t: 'hello', name: 'x'.repeat(300) }); assert.strictEqual((await cl[2].until(cl[2].msg('error'))).code, 'too_large');
    for (let i = 0; i < 60; i++) cl[2].send({ t: 'ping', ts: i });
    await cl[2].until(() => cl[2].msgs.some((m) => m.code === 'rate')); await Promise.race([cl[2].closed, sleep(1500)]);
    assert.ok(s.metrics.jsonRateLimited > 0); assert.strictEqual(cl[2].ws.readyState, 3, 'persistent abuser is disconnected (1008)');
    for (const x of cl) x.close();
  } finally { await s.close(); }
});
test('shared secret and origin allow-list on /ws', async () => {
  const s = await startServer({ sharedSecret: 's3cret', allowOrigin: 'https://good.example' });
  try {
    assert.strictEqual(await upgradeStatus(url(s)), 401); assert.strictEqual(await upgradeStatus(url(s, '?key=wrong')), 401);
    const w = new WebSocket(url(s, '?key=s3cret'), { headers: { origin: 'https://good.example' } }); const ok = await new Promise((r) => { w.on('open', () => r(true)); w.on('error', () => r(false)); }); w.close(); assert.ok(ok);
    const bad = new WebSocket(url(s, '?key=s3cret'), { headers: { origin: 'https://evil.example' } }); const st = await new Promise((r) => { bad.on('unexpected-response', (q, rs) => r(rs.statusCode)); bad.on('open', () => r(101)); bad.on('error', () => {}); }); assert.strictEqual(st, 403);
    assert.strictEqual(s.metrics.wsRejected.secret, 2); assert.strictEqual(s.metrics.wsRejected.origin, 1);
  } finally { await s.close(); }
});
test('admin API: disabled without token, bearer auth, list/kick/ban/close/drain, protects /metrics', async () => {
  const off = await startServer(); try { assert.strictEqual((await fetch(`http://127.0.0.1:${off.port}/admin/rooms`)).status, 404); assert.strictEqual((await fetch(`http://127.0.0.1:${off.port}/metrics`)).status, 200); } finally { await off.close(); }
  const s = await startServer({ adminToken: 'tok', resumeGraceMs: 0 }); const base = `http://127.0.0.1:${s.port}`; const H = { authorization: 'Bearer tok', 'content-type': 'application/json' };
  const call = async (m, p, b) => { const r = await fetch(base + p, { method: m, headers: H, body: b ? JSON.stringify(b) : undefined }); return { status: r.status, body: await r.json().catch(() => null) }; };
  try {
    assert.strictEqual((await fetch(`${base}/admin/rooms`)).status, 401); assert.strictEqual((await fetch(`${base}/admin/rooms`, { headers: { authorization: 'Bearer nope' } })).status, 401); assert.strictEqual((await fetch(`${base}/metrics`)).status, 401);
    const a = await connect(s), b = await connect(s); const j = await a.req({ t: 'create', player: 'Host' }, 'joined'); await b.req({ t: 'join', code: j.room.code, player: 'Guest' }, 'joined');
    const rooms = (await call('GET', '/admin/rooms')).body.rooms; assert.strictEqual(rooms.length, 1); assert.strictEqual(rooms[0].peers.length, 2); assert.strictEqual(rooms[0].peers[0].ip, '127.0.0.1');
    assert.strictEqual((await call('POST', '/admin/kick', { room: j.room.code, id: b.msgs[0].id, ban: true, ttlMs: 60000 })).status, 200); await b.until(b.msg('kicked'));
    assert.strictEqual((await call('GET', '/admin/bans')).body.ips[0].ip, '127.0.0.1'); assert.strictEqual(await upgradeStatus(url(s)), 403);
    assert.strictEqual((await call('DELETE', '/admin/ban-ip', { ip: '127.0.0.1' })).body.ok, true); assert.strictEqual(await upgradeStatus(url(s)), 101);
    assert.strictEqual((await call('POST', '/admin/close-room', { room: j.room.code })).status, 200); await a.until(a.msg('room_closed')); assert.strictEqual(s.manager.rooms.size, 0);
    assert.strictEqual((await call('POST', '/admin/close-room', { room: 'NOPE1' })).status, 404);
    const mt = await (await fetch(`${base}/metrics`, { headers: H })).text(); assert.match(mt, /blurmp_rooms 0/); assert.match(mt, /blurmp_kicks_total 1/);
    assert.strictEqual((await call('POST', '/admin/drain')).body.draining, true); assert.strictEqual((await fetch(`${base}/health`)).status, 503); assert.strictEqual(await upgradeStatus(url(s)), 503);
    a.close(); b.close();
  } finally { await s.close(); }
});
test('metrics: prometheus text + json reflect activity', async () => {
  const s = await startServer(); const base = `http://127.0.0.1:${s.port}`;
  try {
    const a = await connect(s), b = await connect(s); const j = await a.req({ t: 'create' }, 'joined'); await b.req({ t: 'join', code: j.room.code }, 'joined');
    a.sendBin(dataFrame(1, '10.77.0.2', 1, [1])); await b.until(() => b.bins.length === 1); a.send({ t: 'signal', to: b.msgs[0].id, data: {} }); await b.until(b.msg('signal'));
    const txt = await (await fetch(`${base}/metrics`)).text();
    for (const re of [/blurmp_connections 2/, /blurmp_rooms 1/, /blurmp_peers 2/, /blurmp_relay_frames_total 1/, /blurmp_signals_total 1/, /blurmp_json_messages_total\{type="create"\} 1/, /# TYPE blurmp_rooms gauge/, /blurmp_process_rss_bytes \d+/]) assert.match(txt, re);
    const js = await (await fetch(`${base}/metrics.json`)).json(); assert.strictEqual(js.rooms, 1); assert.strictEqual(js.stats.relayed, 1);
    a.close(); b.close();
  } finally { await s.close(); }
});
test('graceful shutdown: clients are told, new connections refused, server closes when empty or after the grace', async () => {
  const s = await startServer({ drainMs: 3000, resumeGraceMs: 0 });
  const a = await connect(s); await a.req({ t: 'create' }, 'joined');
  const done = s.shutdown(2000); await a.until(a.msg('shutdown')); assert.strictEqual(a.msgs.find((m) => m.t === 'shutdown').inMs, 3000); assert.ok(s.draining);
  assert.strictEqual(await upgradeStatus(url(s)), 503);
  const t0 = Date.now(); a.close(); await done; assert.ok(Date.now() - t0 < 1500, 'returns as soon as everybody left');
  const s2 = await startServer({ resumeGraceMs: 0 }); const b = await connect(s2); const t1 = Date.now(); await s2.shutdown(300); assert.ok(Date.now() - t1 >= 250, 'waits for the grace when clients stay'); await b.closed;
});

test('rooms: host-only FWD frames keep the leaf source IP (star fallback), exclude host+origin on broadcast, reject foreign slots', () => {
  const m = new RoomManager(); const h = peer('h', '1.0.0.1'), l1 = peer('l1', '1.0.0.2'), l2 = peer('l2', '1.0.0.3'), l3 = peer('l3', '1.0.0.4');
  const r = m.create(h, { max: 10, topology: 'star' }); for (const l of [l1, l2, l3]) m.join(l, r.room.code);
  const fwd = (src, dst, n = 3) => F.encode({ type: F.T.FWD, srcIp: F.vipForSlot(src), srcPort: 7, dstIp: dst, dstPort: 9, payload: Buffer.alloc(n, 1) });
  assert.strictEqual(m.relay(h, fwd(2, F.vipForSlot(3))), 'ok'); assert.strictEqual(l2.bins.length, 1); const f = F.decode(l2.bins[0]); assert.strictEqual(f.type, F.T.DATA); assert.strictEqual(F.numToIp(f.srcIp), '10.77.0.2');
  assert.strictEqual(m.relay(h, fwd(2, 0xffffffff)), 'ok'); assert.strictEqual(l1.bins.length, 0, 'origin excluded'); assert.strictEqual(l2.bins.length, 2); assert.strictEqual(l3.bins.length, 1); assert.strictEqual(h.bins.length, 0, 'host excluded');
  assert.strictEqual(m.relay(l1, fwd(3, F.vipForSlot(4))), 'type', 'leaves cannot forge forwards'); assert.strictEqual(m.relay(h, fwd(15, F.vipForSlot(2))), 'malformed'); assert.strictEqual(m.relay(h, fwd(2, F.vipForSlot(3), 1289)), 'size');
});
