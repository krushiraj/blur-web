'use strict';
// Rooms = virtual LANs. Pure in-memory state; transports are injected through the Peer objects:
//   peer = { id, name, kind: 'ws'|'udp', ip, sendJson(obj), sendBin(buf) }  (+ fields set here: room, slot, vip, bucket, token, offline)
const crypto = require('crypto');
const F = require('./frame');

const CODE_ALPHABET = 'ABCDEFGHJKMNPQRSTUVWXYZ23456789';
const MAX_PLAYERS = 20;
const MESH_MAX = 8;                              // topology 'auto': full mesh up to 8 players, host-relayed star above
const noop = () => {};

class RoomManager {
  constructor({ log, now = Date.now, maxRooms = 500, maxPerRoom = MAX_PLAYERS, relayRate = 2000, relayBurst = 4000,
    maxRoomsPerIp = 3, pwFailLimit = 10, pwFailWindowMs = 60000, pwBlockMs = 300000, meshMax = MESH_MAX } = {}) {
    this.log = log || { debug() {}, info() {}, warn() {}, error() {} };
    this.now = now; this.maxRooms = maxRooms; this.maxPerRoom = maxPerRoom; this.relayRate = relayRate; this.relayBurst = relayBurst;
    this.maxRoomsPerIp = maxRoomsPerIp; this.pwFailLimit = pwFailLimit; this.pwFailWindowMs = pwFailWindowMs; this.pwBlockMs = pwBlockMs; this.meshMax = meshMax;
    this.rooms = new Map();                         // code -> Room
    this.tokens = new Map();                        // resume token -> peer
    this.ipBans = new Map();                        // ip -> expiry ms (admin / global)
    this.pwFails = new Map();                       // ip -> { n, first, blockedUntil }
    this.draining = false;
    this.stats = { relayed: 0, bytes: 0, dropped: { malformed: 0, size: 0, noroom: 0, rate: 0, nodest: 0, type: 0 }, created: 0, kicks: 0, bans: 0, resumes: 0, resumeTimeouts: 0, joinRejected: {} };
  }

  _reject(error) { this.stats.joinRejected[error] = (this.stats.joinRejected[error] || 0) + 1; return { error }; }
  _code() {
    for (let i = 0; i < 100; i++) {
      let c = ''; const r = crypto.randomBytes(5);
      for (let k = 0; k < 5; k++) c += CODE_ALPHABET[r[k] % CODE_ALPHABET.length];
      if (!this.rooms.has(c)) return c;
    }
    throw new Error('code space exhausted');
  }
  _peerInfo(p) { return { id: p.id, name: p.name, slot: p.slot, vip: F.numToIp(p.vip), relayOnly: p.kind !== 'ws', offline: !!p.offline }; }
  _summary(r) {
    return { code: r.code, name: r.name, max: r.max, players: r.peers.size, hasPassword: !!r.password, public: r.public, topology: r.topology, host: r.host ? r.host.name : '', created: r.created, age: Math.round((this.now() - r.created) / 1000) };
  }
  list() { return [...this.rooms.values()].filter((r) => r.public).map((r) => this._summary(r)).sort((a, b) => b.created - a.created); }
  get(code) { return this.rooms.get(String(code || '').toUpperCase()); }
  isIpBanned(ip) { const t = this.ipBans.get(ip); if (!t) return false; if (t < this.now()) { this.ipBans.delete(ip); return false; } return true; }
  banIp(ip, ttlMs = 3600000) { this.ipBans.set(ip, this.now() + ttlMs); this.stats.bans++; this.log.warn('ip_banned', { ip, ttlMs }); }
  unbanIp(ip) { return this.ipBans.delete(ip); }

  create(peer, opts = {}) {
    if (peer.room) return this._reject('already_in_room');
    if (this.draining) return this._reject('draining');
    if (this.isIpBanned(peer.ip)) return this._reject('banned');
    if (this.rooms.size >= this.maxRooms) return this._reject('server_full');
    if (peer.ip && [...this.rooms.values()].filter((r) => r.creatorIp === peer.ip).length >= this.maxRoomsPerIp) return this._reject('too_many_rooms');
    const max = Math.max(2, Math.min(this.maxPerRoom, Number(opts.max) || this.maxPerRoom));
    const topology = ['mesh', 'star'].includes(opts.topology) ? opts.topology : (max > this.meshMax ? 'star' : 'mesh');
    const room = { code: this._code(), name: String(opts.name || `${peer.name}'s game`).slice(0, 40), max, topology, password: opts.password ? String(opts.password).slice(0, 64) : '', public: opts.public !== false,
      created: this.now(), peers: new Map(), host: null, creatorIp: peer.ip || '', bans: new Set() };
    this.rooms.set(room.code, room);
    this.stats.created++;
    this.log.info('room_created', { room: room.code, by: peer.id, max, topology, public: room.public, pw: !!room.password });
    return this._add(room, peer);
  }
  join(peer, code, password = '') {
    if (peer.room) return this._reject('already_in_room');
    if (this.isIpBanned(peer.ip)) return this._reject('banned');
    const room = this.get(code);
    if (!room) return this._reject('no_such_room');
    const f = this.pwFails.get(peer.ip);
    if (f && f.blockedUntil > this.now()) return this._reject('blocked');
    if (peer.ip && room.bans.has(peer.ip)) return this._reject('banned');
    if (room.password && room.password !== String(password)) {
      const t = this.now(); const e = this.pwFails.get(peer.ip) && t - this.pwFails.get(peer.ip).first < this.pwFailWindowMs ? this.pwFails.get(peer.ip) : { n: 0, first: t, blockedUntil: 0 };
      e.n++; if (e.n >= this.pwFailLimit) { e.blockedUntil = t + this.pwBlockMs; this.log.warn('pw_block', { ip: peer.ip }); }
      this.pwFails.set(peer.ip, e);
      return this._reject('bad_password');
    }
    if (room.peers.size >= room.max) return this._reject('room_full');
    return this._add(room, peer);
  }
  _add(room, peer) {
    let slot = 1; const used = new Set([...room.peers.values()].map((p) => p.slot));
    while (used.has(slot)) slot++;
    peer.room = room; peer.slot = slot; peer.vip = F.vipForSlot(slot); peer.offline = false;
    peer.bucket = { tokens: this.relayBurst, last: this.now() };
    if (peer.kind === 'ws') { peer.token = crypto.randomBytes(16).toString('hex'); this.tokens.set(peer.token, peer); }
    room.peers.set(peer.id, peer);
    if (!room.host) room.host = peer;
    const others = [...room.peers.values()].filter((p) => p !== peer);
    for (const o of others) o.sendJson({ t: 'peer_joined', peer: this._peerInfo(peer) });
    this.log.info('peer_joined', { room: room.code, peer: peer.id, slot, kind: peer.kind, players: room.peers.size });
    return { ok: true, room, you: this._peerInfo(peer), peers: others.map((p) => this._peerInfo(p)), hostId: room.host.id };
  }
  roomInfo(room) { return { code: room.code, name: room.name, max: room.max, hasPassword: !!room.password, public: room.public, topology: room.topology, hostId: room.host ? room.host.id : null }; }

  leave(peer, reason = 'leave') {
    const room = peer.room; if (!room) return false;
    room.peers.delete(peer.id); peer.room = null; if (peer.token) { this.tokens.delete(peer.token); peer.token = null; }
    this.log.info('peer_left', { room: room.code, peer: peer.id, slot: peer.slot, reason, players: room.peers.size });
    if (!room.peers.size) { this.rooms.delete(room.code); this.log.info('room_closed', { room: room.code }); return true; }
    let newHost = null;
    if (room.host === peer) {                       // host flag moves to the lowest remaining online slot
      const cand = [...room.peers.values()].sort((a, b) => (a.offline - b.offline) || (a.slot - b.slot));
      newHost = cand[0]; room.host = newHost;
      this.log.info('host_changed', { room: room.code, host: newHost.id });
    }
    for (const o of room.peers.values()) { o.sendJson({ t: 'peer_left', id: peer.id, slot: peer.slot, reason }); if (newHost) o.sendJson({ t: 'host_changed', id: newHost.id }); }
    return true;
  }

  // ---- resume: a dropped websocket keeps its slot for the grace period
  detach(peer) {
    if (!peer.room || peer.kind !== 'ws' || peer.offline) return false;
    peer.offline = true; peer.offlineAt = this.now(); peer.sendJson = noop; peer.sendBin = noop;
    for (const o of peer.room.peers.values()) if (o !== peer) o.sendJson({ t: 'peer_offline', id: peer.id });
    this.log.info('peer_offline', { room: peer.room.code, peer: peer.id });
    return true;
  }
  expireOffline(graceMs) {
    const t = this.now(); let n = 0;
    for (const p of [...this.tokens.values()]) if (p.offline && t - p.offlineAt > graceMs) { this.stats.resumeTimeouts++; this.leave(p, 'resume_timeout'); n++; }
    return n;
  }
  // returns the old peer object (state to adopt) or { error }
  resume(token, transport) {
    const p = this.tokens.get(String(token || ''));
    if (!p || !p.room) return { error: 'bad_token' };
    if (!p.offline) return { error: 'not_offline' };
    p.sendJson = transport.sendJson; p.sendBin = transport.sendBin; p.offline = false; p.ip = transport.ip || p.ip;
    p.bucket = { tokens: this.relayBurst, last: this.now() };
    this.stats.resumes++;
    for (const o of p.room.peers.values()) if (o !== p) o.sendJson({ t: 'peer_online', id: p.id });
    this.log.info('peer_resumed', { room: p.room.code, peer: p.id, slot: p.slot });
    const others = [...p.room.peers.values()].filter((x) => x !== p);
    return { ok: true, peer: p, room: p.room, you: this._peerInfo(p), peers: others.map((x) => this._peerInfo(x)), hostId: p.room.host.id };
  }

  // ---- moderation
  kick(by, targetId, { ban = false, reason = 'kicked' } = {}) {
    const room = by.room; if (!room) return { error: 'not_in_room' };
    if (room.host !== by) return { error: 'not_host' };
    const t = room.peers.get(targetId); if (!t) return { error: 'no_such_peer' };
    if (t === by) return { error: 'cannot_kick_self' };
    t.sendJson({ t: 'kicked', reason, ban });
    if (ban && t.ip) { room.bans.add(t.ip); this.stats.bans++; }
    this.stats.kicks++;
    this.leave(t, 'kicked');
    return { ok: true, banned: ban };
  }
  closeRoom(code, reason = 'closed') {
    const room = this.get(code); if (!room) return false;
    for (const p of [...room.peers.values()]) { p.sendJson({ t: 'room_closed', reason }); this.leave(p, reason); }
    return true;
  }

  signal(peer, to, data) {
    if (!peer.room || peer.offline) return false;
    const dst = peer.room.peers.get(to);
    if (!dst || dst.kind !== 'ws' || dst.offline) return false;
    dst.sendJson({ t: 'signal', from: peer.id, data });
    return true;
  }

  _allow(peer) {
    const b = peer.bucket, t = this.now();
    b.tokens = Math.min(this.relayBurst, b.tokens + ((t - b.last) / 1000) * this.relayRate); b.last = t;
    if (b.tokens < 1) return false;
    b.tokens -= 1; return true;
  }
  // Relay one frame from `peer`. Returns 'ok' or the drop reason. src ip is always overwritten with the peer's virtual IP (anti-spoof).
  relay(peer, buf) {
    const d = this.stats.dropped;
    const room = peer.room;
    if (!room || peer.offline) { d.noroom++; return 'noroom'; }
    const f = F.decode(buf);
    if (!f) { d.malformed++; return 'malformed'; }
    if (f.type === F.T.FWD) return this._forward(peer, room, f, buf);
    if (f.type !== F.T.DATA) { d.type++; return 'type'; }
    if (f.payload.length > F.MAX_PAYLOAD) { d.size++; return 'size'; }
    if (!this._allow(peer)) { d.rate++; return 'rate'; }
    const out = Buffer.from(buf); out.writeUInt32BE(peer.vip, 4);
    let n = 0;
    if (F.isBroadcast(f.dstIp)) { for (const p of room.peers.values()) if (p !== peer) { p.sendBin(out); n++; } }
    else { const slot = F.slotForVip(f.dstIp); const dst = slot && [...room.peers.values()].find((p) => p.slot === slot); if (dst && dst !== peer) { dst.sendBin(out); n++; } }
    if (!n && !F.isBroadcast(f.dstIp)) { d.nodest++; return 'nodest'; }
    this.stats.relayed++; this.stats.bytes += buf.length;
    return 'ok';
  }
  // Star topology fallback: the host (hub) hands a leaf's frame to the server when a hub<->leaf data channel is down. The leaf's source IP is
  // preserved (that is the point) but only for a slot that is really in the room; only the host may do this. Delivered as plain DATA.
  _forward(peer, room, f, buf) {
    const d = this.stats.dropped;
    if (room.host !== peer) { d.type++; return 'type'; }
    if (f.payload.length > F.MAX_PAYLOAD) { d.size++; return 'size'; }
    const srcSlot = F.slotForVip(f.srcIp); const src = [...room.peers.values()].find((p) => p.slot === srcSlot);
    if (!src) { d.malformed++; return 'malformed'; }
    if (!this._allow(peer)) { d.rate++; return 'rate'; }
    const out = Buffer.from(buf); out[3] = F.T.DATA; let n = 0;
    if (F.isBroadcast(f.dstIp)) { for (const p of room.peers.values()) if (p !== peer && p !== src) { p.sendBin(out); n++; } }
    else { const dst = [...room.peers.values()].find((p) => p.slot === F.slotForVip(f.dstIp)); if (dst && dst !== peer) { dst.sendBin(out); n++; } }
    if (!n && !F.isBroadcast(f.dstIp)) { d.nodest++; return 'nodest'; }
    this.stats.relayed++; this.stats.bytes += buf.length; this.stats.forwarded = (this.stats.forwarded || 0) + 1;
    return 'ok';
  }
  counts() { let peers = 0, offline = 0; for (const r of this.rooms.values()) for (const p of r.peers.values()) { peers++; if (p.offline) offline++; } return { rooms: this.rooms.size, peers, offline }; }
}
module.exports = { RoomManager, MAX_PLAYERS, MESH_MAX };
