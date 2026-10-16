'use strict';
const test = require('node:test'); const assert = require('node:assert'); const crypto = require('crypto');
const { iceServers } = require('../src/ice');
test('empty by default (localhost needs nothing)', () => assert.deepStrictEqual(iceServers({}), []));
test('stun + static turn', () => {
  const a = iceServers({ STUN_URLS: 'stun:a:1, stun:b:2', TURN_URLS: 'turn:t:3', TURN_USER: 'u', TURN_PASS: 'p' });
  assert.deepStrictEqual(a, [{ urls: ['stun:a:1', 'stun:b:2'] }, { urls: ['turn:t:3'], username: 'u', credential: 'p' }]);
});
test('ephemeral coturn credentials', () => {
  const [t] = iceServers({ TURN_URLS: 'turn:t:3', TURN_SECRET: 's3', TURN_TTL: '100' }, 'abcd', 1000000);
  assert.strictEqual(t.username, '1100:abcd'); assert.strictEqual(t.credential, crypto.createHmac('sha1', 's3').update('1100:abcd').digest('base64'));
});
test('ICE_SERVERS json wins', () => assert.deepStrictEqual(iceServers({ ICE_SERVERS: '[{"urls":"stun:x"}]', STUN_URLS: 'stun:y' }), [{ urls: 'stun:x' }]));
