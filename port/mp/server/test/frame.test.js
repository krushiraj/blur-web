'use strict';
const test = require('node:test'); const assert = require('node:assert');
const F = require('../src/frame');
test('ip helpers', () => {
  assert.strictEqual(F.ipToNum('10.77.0.5'), 0x0a4d0005); assert.strictEqual(F.numToIp(0x0a4d0005), '10.77.0.5');
  assert.strictEqual(F.ipToNum('255.255.255.255'), 0xffffffff); assert.strictEqual(F.ipToNum('1.2.3'), null); assert.strictEqual(F.ipToNum('1.2.3.256'), null);
  assert.ok(F.isBroadcast(0xffffffff)); assert.ok(F.isBroadcast(F.ipToNum('10.77.0.255'))); assert.ok(!F.isBroadcast(F.ipToNum('10.77.0.20')));
  assert.strictEqual(F.vipForSlot(7), F.ipToNum('10.77.0.7')); assert.strictEqual(F.slotForVip(F.ipToNum('10.77.0.7')), 7); assert.strictEqual(F.slotForVip(F.ipToNum('10.78.0.7')), 0);
});
test('encode/decode roundtrip', () => {
  const b = F.encode({ type: F.T.DATA, srcIp: 0x0a4d0001, srcPort: 50001, dstPort: 50001, dstIp: 0xffffffff, payload: Buffer.from('hello') });
  assert.strictEqual(b.length, 21); assert.deepStrictEqual([...b.subarray(0, 4)], [0x42, 0x4e, 1, 2]);
  const f = F.decode(b); assert.strictEqual(f.srcPort, 50001); assert.strictEqual(f.dstIp, 0xffffffff); assert.strictEqual(f.payload.toString(), 'hello');
});
test('decode rejects garbage', () => {
  assert.strictEqual(F.decode(Buffer.alloc(8)), null); assert.strictEqual(F.decode(Buffer.alloc(16)), null);
  const b = F.encode({ type: 2 }); b[2] = 9; assert.strictEqual(F.decode(b), null);
  const c = F.encode({ type: 2 }); c[3] = 0; assert.strictEqual(F.decode(c), null);
});
