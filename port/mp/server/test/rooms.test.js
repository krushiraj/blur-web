'use strict';
const test = require('node:test'); const assert = require('node:assert');
const { RoomManager } = require('../src/rooms'); const F = require('../src/frame');
function peer(id, kind = 'ws') { const p = { id, name: id, kind, room: null, sent: [], bins: [], sendJson(o) { this.sent.push(o); }, sendBin(b) { this.bins.push(b); } }; return p; }
const data = (dstIp, n = 4, dstPort = 50001) => F.encode({ type: F.T.DATA, srcIp: 0x7f000001, srcPort: 5, dstPort, dstIp, payload: Buffer.alloc(n, 7) });

test('create/list/join/leave, host flag and slots', () => {
  const m = new RoomManager(); const a = peer('a'), b = peer('b'), c = peer('c');
  const r = m.create(a, { name: 'Test', max: 4 }); assert.ok(r.ok); assert.strictEqual(a.slot, 1); assert.strictEqual(F.numToIp(a.vip), '10.77.0.1'); assert.strictEqual(r.hostId, 'a');
  assert.strictEqual(m.list().length, 1); assert.strictEqual(m.list()[0].players, 1);
  const j = m.join(b, r.room.code.toLowerCase()); assert.ok(j.ok); assert.strictEqual(b.slot, 2); assert.strictEqual(j.peers.length, 1);
  assert.strictEqual(a.sent.at(-1).t, 'peer_joined'); assert.strictEqual(a.sent.at(-1).peer.vip, '10.77.0.2');
  m.join(c, r.room.code); assert.strictEqual(c.slot, 3);
  m.leave(b); const d = peer('d'); m.join(d, r.room.code); assert.strictEqual(d.slot, 2, 'lowest free slot is reused');
  m.leave(a); assert.strictEqual(r.room.host.id, 'd', 'host moves to the lowest remaining slot (d=2, c=3)');
});
test('host promotion picks lowest slot', () => {
  const m = new RoomManager(); const a = peer('a'), b = peer('b'), c = peer('c');
  const r = m.create(a, {}); m.join(b, r.room.code); m.join(c, r.room.code);
  m.leave(a); assert.strictEqual(r.room.host, b); assert.ok(c.sent.some((x) => x.t === 'host_changed' && x.id === 'b')); assert.ok(c.sent.some((x) => x.t === 'peer_left' && x.id === 'a'));
});
test('empty room is deleted', () => { const m = new RoomManager(); const a = peer('a'); m.create(a, {}); assert.strictEqual(m.rooms.size, 1); m.leave(a); assert.strictEqual(m.rooms.size, 0); });
test('limits: max players (cap 20), password, private, unknown', () => {
  const m = new RoomManager(); const h = peer('h'); const r = m.create(h, { max: 99, password: 'pw' }); assert.strictEqual(r.room.max, 20);
  assert.strictEqual(m.join(peer('x'), r.room.code).error, 'bad_password'); assert.strictEqual(m.join(peer('x'), r.room.code, 'nope').error, 'bad_password');
  for (let i = 0; i < 19; i++) assert.ok(m.join(peer('p' + i), r.room.code, 'pw').ok);
  assert.strictEqual(m.join(peer('late'), r.room.code, 'pw').error, 'room_full');
  assert.strictEqual(m.join(peer('y'), 'NOPE1').error, 'no_such_room');
  const q = m.create(peer('priv'), { public: false }); assert.ok(q.ok); assert.strictEqual(m.list().filter((x) => x.code === q.room.code).length, 0); assert.ok(m.join(peer('z'), q.room.code).ok, 'private rooms are joinable by code');
  assert.strictEqual(m.join(h, r.room.code, 'pw').error, 'already_in_room');
});
test('relay: unicast, broadcast (not echoed), spoofed src rewritten, oversize/foreign dropped', () => {
  const m = new RoomManager(); const a = peer('a'), b = peer('b'), c = peer('c'), o = peer('o'); const r = m.create(a, {}); m.join(b, r.room.code); m.join(c, r.room.code); m.create(o, {});
  assert.strictEqual(m.relay(a, data(F.ipToNum('10.77.0.2'))), 'ok'); assert.strictEqual(b.bins.length, 1); assert.strictEqual(c.bins.length, 0);
  assert.strictEqual(F.numToIp(F.decode(b.bins[0]).srcIp), '10.77.0.1', 'src ip rewritten');
  assert.strictEqual(m.relay(b, data(0xffffffff)), 'ok'); assert.strictEqual(a.bins.length, 1); assert.strictEqual(c.bins.length, 1); assert.strictEqual(b.bins.length, 1);
  assert.strictEqual(m.relay(a, data(F.ipToNum('10.77.0.255'))), 'ok'); assert.strictEqual(b.bins.length, 2);
  assert.strictEqual(m.relay(a, data(F.ipToNum('10.77.0.9'))), 'nodest');
  assert.strictEqual(m.relay(a, data(F.ipToNum('10.77.0.2'), 1289)), 'size'); assert.strictEqual(m.relay(a, data(F.ipToNum('10.77.0.2'), 1288)), 'ok');
  assert.strictEqual(m.relay(a, Buffer.from('garbage-garbage-garbage')), 'malformed');
  assert.strictEqual(m.relay(a, F.encode({ type: F.T.HELLO })), 'type');
  assert.strictEqual(o.bins.length, 0, 'other rooms never see traffic'); assert.strictEqual(m.relay(peer('nobody'), data(0xffffffff)), 'noroom');
});
test('relay rate limit', () => {
  let t = 0; const m = new RoomManager({ now: () => t, relayRate: 100, relayBurst: 10 }); const a = peer('a'), b = peer('b'); const r = m.create(a, {}); m.join(b, r.room.code);
  let ok = 0, lim = 0; for (let i = 0; i < 30; i++) (m.relay(a, data(0xffffffff)) === 'ok' ? ok++ : lim++);
  assert.strictEqual(ok, 10); assert.strictEqual(lim, 20); t += 100; assert.strictEqual(m.relay(a, data(0xffffffff)), 'ok');
});
test('signal only inside the room and only to ws peers', () => {
  const m = new RoomManager(); const a = peer('a'), b = peer('b'), n = peer('n', 'udp'), x = peer('x'); const r = m.create(a, {}); m.join(b, r.room.code); m.join(n, r.room.code); m.create(x, {});
  assert.ok(m.signal(a, 'b', { sdp: 1 })); assert.deepStrictEqual(b.sent.at(-1), { t: 'signal', from: 'a', data: { sdp: 1 } });
  assert.ok(!m.signal(a, 'x', {})); assert.ok(!m.signal(a, 'n', {}));
  assert.ok(a.sent.some((s) => s.t === 'peer_joined' && s.peer.id === 'n' && s.peer.relayOnly === true));
});
