'use strict';
// HTTP (health, room list, metrics, admin API, static client/demo) + WebSocket (/ws: JSON control, binary datagram relay)
// + optional UDP gateway for native harness instances.
const http = require('http');
const fs = require('fs');
const path = require('path');
const dgram = require('dgram');
const crypto = require('crypto');
const { WebSocketServer } = require('ws');
const F = require('./frame');
const { RoomManager } = require('./rooms');
const { iceServers } = require('./ice');
const { makeLogger } = require('./log');

const ROOT = path.join(__dirname, '..', '..');
const STATIC = { '/client/': path.join(ROOT, 'client'), '/demo/': path.join(ROOT, 'demo'), '/test/': path.join(ROOT, 'test') };
const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.json': 'application/json', '.css': 'text/css', '.map': 'application/json' };
const safeEq = (a, b) => { const x = Buffer.from(String(a)), y = Buffer.from(String(b)); return x.length === y.length && crypto.timingSafeEqual(x, y); };

function createServer(cfg = {}) {
  const env = cfg.env || process.env;
  const c = {
    port: cfg.port ?? Number(env.PORT ?? 8786), host: cfg.host ?? env.HOST ?? '127.0.0.1',
    udpPort: cfg.udpPort ?? (env.UDP_PORT !== undefined ? Number(env.UDP_PORT) : undefined),      // undefined = no UDP gateway, 0 = ephemeral
    heartbeatMs: cfg.heartbeatMs ?? Number(env.HEARTBEAT_MS ?? 5000), timeoutMs: cfg.timeoutMs ?? Number(env.TIMEOUT_MS ?? 15000),
    udpTimeoutMs: cfg.udpTimeoutMs ?? Number(env.UDP_TIMEOUT_MS ?? 5000),
    resumeGraceMs: cfg.resumeGraceMs ?? Number(env.RESUME_GRACE_MS ?? 30000),
    maxConns: cfg.maxConns ?? Number(env.MAX_CONNS ?? 2000), maxConnsPerIp: cfg.maxConnsPerIp ?? Number(env.MAX_CONNS_PER_IP ?? 20),
    maxRoomsPerIp: cfg.maxRoomsPerIp ?? Number(env.MAX_ROOMS_PER_IP ?? 3),
    msgRate: cfg.msgRate ?? Number(env.MSG_RATE ?? 50), msgBurst: cfg.msgBurst ?? Number(env.MSG_BURST ?? 100), msgStrikes: cfg.msgStrikes ?? Number(env.MSG_STRIKES ?? 30),
    maxMsgBytes: cfg.maxMsgBytes ?? Number(env.MAX_MSG_BYTES ?? 16 * 1024),
    serveStatic: cfg.serveStatic ?? env.SERVE_STATIC !== '0',
    allowOrigin: cfg.allowOrigin ?? env.ALLOW_ORIGIN ?? '',                                       // '' = no check, '*', or comma list of origins
    sharedSecret: cfg.sharedSecret ?? env.SHARED_SECRET ?? '',                                     // if set, /ws needs ?key=<secret>
    adminToken: cfg.adminToken ?? env.ADMIN_TOKEN ?? '',                                            // if set, enables /admin/* (Bearer) and protects /metrics
    trustProxy: cfg.trustProxy ?? env.TRUST_PROXY === '1',                                          // behind Caddy/nginx: client IP from X-Forwarded-For
    drainMs: cfg.drainMs ?? Number(env.DRAIN_MS ?? 10000),
  };
  const log = cfg.log || makeLogger(env.LOG_LEVEL || 'info');
  const mgr = new RoomManager({ log, relayRate: cfg.relayRate, relayBurst: cfg.relayBurst, maxRoomsPerIp: c.maxRoomsPerIp, pwFailLimit: cfg.pwFailLimit, pwBlockMs: cfg.pwBlockMs });
  const started = Date.now();
  const conns = new Set();
  const perIp = new Map();
  const udpPeers = new Map();
  const m = { wsOpen: 0, wsClose: 0, wsRejected: {}, jsonMsgs: {}, jsonRateLimited: 0, jsonStrikeClosed: 0, signals: 0, udpFrames: 0, adminCalls: 0, timeouts: 0 };
  const rej = (why) => { m.wsRejected[why] = (m.wsRejected[why] || 0) + 1; };
  let draining = false, shuttingDown = null;

  const clientIp = (req) => {
    if (c.trustProxy && req.headers['x-forwarded-for']) return String(req.headers['x-forwarded-for']).split(',')[0].trim();
    return req.socket.remoteAddress || '';
  };

  // ---------- metrics (Prometheus text)
  function metricsText() {
    const k = mgr.counts(), s = mgr.stats, mem = process.memoryUsage(), cpu = process.cpuUsage();
    const L = [];
    const g = (name, help, val, type = 'gauge') => { L.push(`# HELP ${name} ${help}`, `# TYPE ${name} ${type}`, `${name} ${val}`); };
    const lab = (name, help, obj, key, type = 'counter') => { L.push(`# HELP ${name} ${help}`, `# TYPE ${name} ${type}`); for (const [a, v] of Object.entries(obj)) L.push(`${name}{${key}="${a}"} ${v}`); };
    g('blurmp_uptime_seconds', 'Process uptime', Math.round((Date.now() - started) / 1000));
    g('blurmp_connections', 'Open WebSocket connections', conns.size);
    g('blurmp_udp_peers', 'Native UDP gateway peers', udpPeers.size);
    g('blurmp_rooms', 'Open rooms', k.rooms); g('blurmp_peers', 'Peers in rooms', k.peers); g('blurmp_peers_offline', 'Peers inside the resume grace period', k.offline);
    g('blurmp_draining', '1 while draining for shutdown', draining ? 1 : 0);
    g('blurmp_ws_opened_total', 'WebSocket connections accepted', m.wsOpen, 'counter'); g('blurmp_ws_closed_total', 'WebSocket connections closed', m.wsClose, 'counter');
    lab('blurmp_ws_rejected_total', 'Rejected upgrades by reason', m.wsRejected, 'reason');
    lab('blurmp_json_messages_total', 'Control messages by type', m.jsonMsgs, 'type');
    g('blurmp_json_rate_limited_total', 'Control messages dropped by the per-connection rate limit', m.jsonRateLimited, 'counter');
    g('blurmp_rooms_created_total', 'Rooms created', s.created, 'counter'); g('blurmp_kicks_total', 'Host kicks', s.kicks, 'counter'); g('blurmp_bans_total', 'Bans issued', s.bans, 'counter');
    g('blurmp_resumes_total', 'Successful session resumes', s.resumes, 'counter'); g('blurmp_resume_timeouts_total', 'Offline peers that did not return in time', s.resumeTimeouts, 'counter');
    lab('blurmp_join_rejected_total', 'Create/join refusals by reason', s.joinRejected, 'reason');
    g('blurmp_relay_frames_total', 'Datagram frames relayed', s.relayed, 'counter'); g('blurmp_relay_bytes_total', 'Datagram bytes relayed', s.bytes, 'counter');
    lab('blurmp_relay_dropped_total', 'Relay frames dropped by reason', s.dropped, 'reason');
    g('blurmp_signals_total', 'Signalling messages forwarded', m.signals, 'counter'); g('blurmp_udp_frames_total', 'Frames received by the UDP gateway', m.udpFrames, 'counter');
    g('blurmp_process_rss_bytes', 'Resident memory', mem.rss); g('blurmp_process_cpu_seconds_total', 'CPU time', ((cpu.user + cpu.system) / 1e6).toFixed(3), 'counter');
    return L.join('\n') + '\n';
  }
  const metricsJson = () => ({ uptime: Math.round((Date.now() - started) / 1000), conns: conns.size, udpPeers: udpPeers.size, draining, ...mgr.counts(), stats: mgr.stats, server: m, rss: process.memoryUsage().rss });

  // ---------- HTTP
  const authed = (req) => { const h = req.headers.authorization || ''; return h.startsWith('Bearer ') && safeEq(h.slice(7), c.adminToken); };
  async function body(req) { const chunks = []; let n = 0; for await (const ch of req) { n += ch.length; if (n > 16384) throw new Error('too_large'); chunks.push(ch); } try { return JSON.parse(Buffer.concat(chunks).toString() || '{}'); } catch { return {}; } }
  function adminRooms() {
    return [...mgr.rooms.values()].map((r) => ({ code: r.code, name: r.name, max: r.max, topology: r.topology, public: r.public, hasPassword: !!r.password, creatorIp: r.creatorIp, host: r.host && r.host.id, bans: [...r.bans], age: Math.round((Date.now() - r.created) / 1000),
      peers: [...r.peers.values()].map((p) => ({ id: p.id, name: p.name, slot: p.slot, vip: F.numToIp(p.vip), kind: p.kind, ip: p.ip, offline: !!p.offline })) }));
  }
  const httpServer = http.createServer(async (req, res) => {
    const url = new URL(req.url, 'http://x');
    const send = (code, bodyv, type = 'application/json', extra = {}) => { res.writeHead(code, { 'content-type': type, 'cache-control': 'no-store', ...(c.allowOrigin ? { 'access-control-allow-origin': c.allowOrigin } : {}), ...extra }); res.end(typeof bodyv === 'string' || Buffer.isBuffer(bodyv) ? bodyv : JSON.stringify(bodyv)); };
    try {
      if (url.pathname === '/health') return send(draining ? 503 : 200, { ok: !draining, draining, uptime: Math.round((Date.now() - started) / 1000), rooms: mgr.rooms.size, conns: conns.size, udpPeers: udpPeers.size, stats: mgr.stats });
      if (url.pathname === '/rooms') return send(200, { rooms: mgr.list() });
      if (url.pathname === '/metrics' || url.pathname === '/metrics.json') {
        if (c.adminToken && !authed(req)) return send(401, { error: 'unauthorized' });
        return url.pathname === '/metrics' ? send(200, metricsText(), 'text/plain; version=0.0.4') : send(200, metricsJson());
      }
      if (url.pathname.startsWith('/admin/')) {
        if (!c.adminToken) return send(404, { error: 'not_found' });
        if (!authed(req)) return send(401, { error: 'unauthorized' });
        m.adminCalls++;
        const route = `${req.method} ${url.pathname}`;
        if (route === 'GET /admin/rooms') return send(200, { rooms: adminRooms() });
        if (route === 'GET /admin/bans') return send(200, { ips: [...mgr.ipBans].map(([ip, until]) => ({ ip, until })), rooms: [...mgr.rooms.values()].map((r) => ({ code: r.code, bans: [...r.bans] })) });
        const b = req.method === 'POST' || req.method === 'DELETE' ? await body(req) : {};
        if (route === 'POST /admin/kick') {
          const room = mgr.get(b.room); const p = room && room.peers.get(b.id); if (!p) return send(404, { error: 'no_such_peer' });
          p.sendJson({ t: 'kicked', reason: b.reason || 'admin', ban: !!b.ban }); if (b.ban && p.ip) mgr.banIp(p.ip, Number(b.ttlMs) || 3600000); mgr.stats.kicks++; mgr.leave(p, 'admin_kick'); if (p.ws) p.ws.close(4000, 'kicked'); return send(200, { ok: true });
        }
        if (route === 'POST /admin/close-room') return mgr.closeRoom(b.room, 'admin') ? send(200, { ok: true }) : send(404, { error: 'no_such_room' });
        if (route === 'POST /admin/ban-ip') { if (!b.ip) return send(400, { error: 'ip_required' }); mgr.banIp(String(b.ip), Number(b.ttlMs) || 3600000); return send(200, { ok: true }); }
        if (route === 'DELETE /admin/ban-ip') return send(200, { ok: mgr.unbanIp(String(b.ip)) });
        if (route === 'POST /admin/drain') { startDrain(); return send(200, { ok: true, draining: true }); }
        return send(404, { error: 'not_found' });
      }
      if (c.serveStatic) {
        for (const [prefix, dir] of Object.entries(STATIC)) {
          if (url.pathname.startsWith(prefix) || url.pathname === prefix.slice(0, -1)) {
            if (url.pathname === prefix.slice(0, -1)) { res.writeHead(302, { location: prefix }); return res.end(); }
            let rel = decodeURIComponent(url.pathname.slice(prefix.length)); if (!rel || rel.endsWith('/')) rel += 'index.html';
            const file = path.normalize(path.join(dir, rel));
            if (!file.startsWith(dir + path.sep)) return send(403, { error: 'forbidden' });
            return fs.readFile(file, (err, data) => err ? send(404, { error: 'not_found' }) : send(200, data, MIME[path.extname(file)] || 'application/octet-stream'));
          }
        }
        if (url.pathname === '/') { res.writeHead(302, { location: '/demo/' }); return res.end(); }
      }
      send(404, { error: 'not_found' });
    } catch (e) { log.error('http_error', { msg: e.message }); send(500, { error: 'internal' }); }
  });

  // ---------- WebSocket
  const wss = new WebSocketServer({ noServer: true, maxPayload: 64 * 1024 });
  httpServer.on('upgrade', (req, socket, head) => {
    const url = new URL(req.url, 'http://x'); const origin = req.headers.origin; const ip = clientIp(req);
    const deny = (code, why) => { rej(why); socket.write(`HTTP/1.1 ${code}\r\nConnection: close\r\n\r\n`); socket.destroy(); };
    if (url.pathname !== '/ws') return deny('404 Not Found', 'path');
    if (draining) return deny('503 Service Unavailable', 'draining');
    if (conns.size >= c.maxConns) return deny('503 Service Unavailable', 'max_conns');
    if (mgr.isIpBanned(ip)) return deny('403 Forbidden', 'ip_banned');
    if ((perIp.get(ip) || 0) >= c.maxConnsPerIp) return deny('429 Too Many Requests', 'conns_per_ip');
    if (c.allowOrigin && c.allowOrigin !== '*' && origin && !c.allowOrigin.split(',').includes(origin)) return deny('403 Forbidden', 'origin');
    if (c.sharedSecret && !safeEq(url.searchParams.get('key') || '', c.sharedSecret)) return deny('401 Unauthorized', 'secret');
    wss.handleUpgrade(req, socket, head, (ws) => { ws.clientIp = ip; wss.emit('connection', ws, req); });
  });
  const transportFor = (ws) => ({
    sendJson: (o) => { if (ws.readyState === 1) ws.send(JSON.stringify(o)); },
    sendBin: (b) => { if (ws.readyState === 1 && ws.bufferedAmount < 1 << 20) ws.send(b, { binary: true }); },
  });
  // install the socket listeners for peer object `p` on `ws` (also used when a resumed session adopts a fresh socket)
  function attach(p, ws) {
    const ip = ws.clientIp;
    p.ws = ws; Object.assign(p, transportFor(ws)); p.lastSeen = Date.now();
    ws.on('pong', () => { p.lastSeen = Date.now(); });
    ws.on('message', (data, isBinary) => {
      p.lastSeen = Date.now();
      if (isBinary) { mgr.relay(p, data); return; }
      if (data.length > c.maxMsgBytes) { p.sendJson({ t: 'error', code: 'too_large' }); return; }
      const t = Date.now(), jb = p.jb; jb.tokens = Math.min(c.msgBurst, jb.tokens + ((t - jb.last) / 1000) * c.msgRate); jb.last = t;
      if (jb.tokens < 1) { m.jsonRateLimited++; if (++jb.strikes >= c.msgStrikes) { m.jsonStrikeClosed++; log.warn('ws_abuse_close', { peer: p.id, ip }); ws.close(1008, 'rate'); } else p.sendJson({ t: 'error', code: 'rate' }); return; }
      jb.tokens -= 1;
      let msg; try { msg = JSON.parse(data.toString()); } catch { return p.sendJson({ t: 'error', code: 'bad_json' }); }
      if (!msg || typeof msg !== 'object') return p.sendJson({ t: 'error', code: 'bad_json' });
      m.jsonMsgs[msg.t] = (m.jsonMsgs[msg.t] || 0) + 1;
      handle(p, msg, ws);
    });
    ws.on('close', () => {
      conns.delete(p); m.wsClose++;
      perIp.set(ip, Math.max(0, (perIp.get(ip) || 1) - 1)); if (!perIp.get(ip)) perIp.delete(ip);
      if (p.ws === ws) { if (p.room && !p.leftExplicitly && c.resumeGraceMs > 0 && !draining) mgr.detach(p); else mgr.leave(p, 'disconnect'); }
      log.info('ws_close', { peer: p.id, conns: conns.size });
    });
    ws.on('error', (e) => log.warn('ws_error', { peer: p.id, msg: e.message }));
  }
  wss.on('connection', (ws, req) => {
    const ip = ws.clientIp;
    const peer = { id: crypto.randomBytes(4).toString('hex'), name: 'player', kind: 'ws', ip, room: null, slot: 0, vip: 0, jb: { tokens: c.msgBurst, last: Date.now(), strikes: 0 } };
    attach(peer, ws);
    conns.add(peer); perIp.set(ip, (perIp.get(ip) || 0) + 1); m.wsOpen++;
    log.info('ws_open', { peer: peer.id, ip, conns: conns.size });
    peer.sendJson({ t: 'welcome', id: peer.id, iceServers: iceServers(env, peer.id), maxPayload: F.MAX_PAYLOAD, heartbeatMs: c.heartbeatMs, resumeGraceMs: c.resumeGraceMs });
  });

  const joinedMsg = (r, peer) => ({ t: 'joined', room: mgr.roomInfo(r.room), you: r.you, peers: r.peers, hostId: r.hostId, iceServers: iceServers(env, peer.id), resume: { token: peer.token, graceMs: c.resumeGraceMs } });
  function handle(peer, msg, ws) {
    const err = (code) => peer.sendJson({ t: 'error', code, req: msg.t, rid: msg.rid });
    const str = (v, n) => String(v ?? '').slice(0, n);
    switch (msg.t) {
      case 'hello': peer.name = str(msg.name || 'player', 24).replace(/[\u0000-\u001f]/g, ''); break;
      case 'ping': peer.sendJson({ t: 'pong', ts: msg.ts }); break;
      case 'list': peer.sendJson({ t: 'rooms', rid: msg.rid, rooms: mgr.list() }); break;
      case 'create': { if (msg.player) peer.name = str(msg.player, 24);
        const r = mgr.create(peer, msg); if (r.error) return err(r.error); peer.sendJson({ ...joinedMsg(r, peer), rid: msg.rid }); break; }
      case 'join': { if (msg.player) peer.name = str(msg.player, 24);
        const r = mgr.join(peer, msg.code, msg.password); if (r.error) return err(r.error); peer.sendJson({ ...joinedMsg(r, peer), rid: msg.rid }); break; }
      case 'resume': {                                  // reattach a dropped connection to its slot (same id, slot, virtual IP)
        if (peer.room) return err('already_in_room');
        const r = mgr.resume(msg.token, { ...transportFor(ws), ip: peer.ip });
        if (r.error) return err(r.error);
        const real = r.peer; real.jb = peer.jb; real.leftExplicitly = false;
        conns.delete(peer); ws.removeAllListeners('message'); ws.removeAllListeners('close'); ws.removeAllListeners('pong'); ws.removeAllListeners('error');
        attach(real, ws); conns.add(real);
        real.sendJson({ t: 'joined', room: mgr.roomInfo(r.room), you: r.you, peers: r.peers, hostId: r.hostId, iceServers: iceServers(env, real.id), resume: { token: real.token, graceMs: c.resumeGraceMs }, resumed: true, id: real.id, rid: msg.rid });
        break;
      }
      case 'leave': peer.leftExplicitly = true; mgr.leave(peer, 'leave'); peer.leftExplicitly = false; peer.sendJson({ t: 'left', rid: msg.rid }); break;
      case 'signal': if (mgr.signal(peer, msg.to, msg.data)) m.signals++; else err('no_such_peer'); break;
      case 'kick': { const r = mgr.kick(peer, msg.id, { ban: !!msg.ban, reason: str(msg.reason || 'kicked', 60) }); if (r.error) return err(r.error); peer.sendJson({ t: 'kick_ok', id: msg.id, banned: r.banned, rid: msg.rid }); break; }
      case 'bans': { const room = peer.room; if (!room || room.host !== peer) return err('not_host'); peer.sendJson({ t: 'bans', bans: [...room.bans], rid: msg.rid }); break; }
      case 'unban': { const room = peer.room; if (!room || room.host !== peer) return err('not_host'); peer.sendJson({ t: 'unban_ok', ok: room.bans.delete(String(msg.ip || '')), rid: msg.rid }); break; }
      default: err('unknown_type');
    }
  }

  const hb = setInterval(() => {
    const t = Date.now();
    for (const p of conns) {
      if (t - p.lastSeen > c.timeoutMs) { m.timeouts++; log.warn('ws_timeout', { peer: p.id }); p.ws.terminate(); continue; }
      try { p.ws.ping(); } catch { /* closing */ }
    }
    mgr.expireOffline(c.resumeGraceMs);
  }, Math.min(c.heartbeatMs, 1000));
  hb.unref();

  // ---------- UDP gateway (native wsock_hle instances): HELLO(payload "room=CODE;pw=..;name=..;create=1;max=N;public=0|1") -> WELCOME(vip[4], slot[1], code)
  let udp = null, sweeper = null;
  function startUdp() {
    udp = dgram.createSocket('udp4');
    udp.on('message', (msg, ri) => {
      const f = F.decode(msg); if (!f) return;
      m.udpFrames++;
      const key = `${ri.address}:${ri.port}`; let peer = udpPeers.get(key);
      if (f.type === F.T.HELLO) {
        if (peer) { peer.lastSeen = Date.now(); return; }
        if (draining) return;
        const kv = Object.fromEntries(f.payload.toString('utf8').split(';').filter(Boolean).map((s) => { const i = s.indexOf('='); return i < 0 ? [s, ''] : [s.slice(0, i), s.slice(i + 1)]; }));
        peer = { id: crypto.randomBytes(4).toString('hex'), name: (kv.name || 'native').slice(0, 24), kind: 'udp', ip: ri.address, room: null, slot: 0, vip: 0, lastSeen: Date.now(), key,
          sendJson() {}, sendBin: (b) => udp.send(b, ri.port, ri.address) };
        const r = kv.create === '1' ? mgr.create(peer, { name: kv.roomname, max: kv.max, password: kv.pw, public: kv.public !== '0' }) : mgr.join(peer, kv.room, kv.pw || '');
        if (r.error) { udp.send(F.encode({ type: F.T.ERROR, payload: r.error }), ri.port, ri.address); log.warn('udp_hello_rejected', { key, error: r.error }); return; }
        udpPeers.set(key, peer);
        udp.send(F.encode({ type: F.T.WELCOME, payload: Buffer.from([peer.vip >>> 24, (peer.vip >>> 16) & 255, (peer.vip >>> 8) & 255, peer.vip & 255, peer.slot, ...Buffer.from(r.room.code)]) }), ri.port, ri.address);
        log.info('udp_peer', { key, peer: peer.id, room: r.room.code, vip: F.numToIp(peer.vip) });
        return;
      }
      if (!peer) return;
      peer.lastSeen = Date.now();
      if (f.type === F.T.DATA) mgr.relay(peer, msg);
      else if (f.type === F.T.BYE) { udpPeers.delete(key); mgr.leave(peer, 'bye'); }
    });
    sweeper = setInterval(() => { const t = Date.now(); for (const [k, p] of udpPeers) if (t - p.lastSeen > c.udpTimeoutMs) { udpPeers.delete(k); mgr.leave(p, 'udp_timeout'); } }, 500);
    sweeper.unref();
    return new Promise((res, rej2) => { udp.once('error', rej2); udp.bind(c.udpPort, c.host, () => res(udp.address().port)); });
  }

  // ---------- lifecycle
  function startDrain() { if (draining) return; draining = true; mgr.draining = true; log.warn('draining', { conns: conns.size }); for (const p of conns) p.sendJson({ t: 'shutdown', inMs: c.drainMs }); }
  const api = {
    cfg: c, manager: mgr, httpServer, wss, conns, udpPeers, metrics: m, port: 0, udpPort: 0, metricsText, metricsJson,
    get draining() { return draining; },
    async listen() {
      await new Promise((res, rej2) => { httpServer.once('error', rej2); httpServer.listen(c.port, c.host, res); });
      api.port = httpServer.address().port;
      if (c.udpPort !== undefined) api.udpPort = await startUdp();
      log.info('listening', { http: `${c.host}:${api.port}`, udp: c.udpPort !== undefined ? `${c.host}:${api.udpPort}` : null, heartbeatMs: c.heartbeatMs, timeoutMs: c.timeoutMs, resumeGraceMs: c.resumeGraceMs, secret: !!c.sharedSecret, admin: !!c.adminToken });
      return api;
    },
    // announce, stop taking new work, wait (up to graceMs) for clients to leave, then close everything
    async shutdown(graceMs = c.drainMs) {
      if (shuttingDown) return shuttingDown;
      shuttingDown = (async () => {
        startDrain();
        const t0 = Date.now();
        while (Date.now() - t0 < graceMs && (conns.size || udpPeers.size)) await new Promise((r) => setTimeout(r, 50));
        log.info('shutdown', { remainingConns: conns.size, waitedMs: Date.now() - t0 });
        await api.close(1001);
      })();
      return shuttingDown;
    },
    async close(code = 1001) {
      clearInterval(hb); if (sweeper) clearInterval(sweeper);
      for (const p of conns) { try { p.ws.close(code, 'server shutdown'); } catch { /* */ } }
      await new Promise((r) => setTimeout(r, 20));
      for (const p of conns) p.ws.terminate();
      wss.close(); if (udp) { try { udp.close(); } catch { /* */ } udp = null; }
      await new Promise((res) => httpServer.close(() => res())); httpServer.closeAllConnections?.();
    },
  };
  return api;
}
module.exports = { createServer };
