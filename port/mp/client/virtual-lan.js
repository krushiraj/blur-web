// Virtual LAN for the Blur browser port.
//
// A room on the lobby server is a /24 "LAN" (10.77.0.<slot>). This library keeps unordered, no-retransmit RTCDataChannels between peers
// (falls back to the server's WebSocket datagram relay when a channel is not open), delivers UDP-like datagrams between sockets bound to
// ports, implements broadcast (255.255.255.255 / x.x.x.255, looped back to the sender like Windows) and unicast, and caps datagrams at the
// game's BD_MAX_DATAGRAM_SIZE (1288 bytes).
//
// Topologies (chosen by the room, `room.topology`):
//   mesh : one data channel per peer pair (default up to 8 players)
//   star : data channels only between the host (hub) and every other peer; leaf-to-leaf and broadcast traffic is forwarded by the hub
//          (frame type 6), and by the server (host-only privilege) whenever a hub channel is not open.
//
// The socket API is synchronous and allocation-light so a wasm Winsock HLE can poll it every frame:
//     const s = lan.socket(50001);                    // bind (0 = ephemeral); returns a small integer handle
//     lan.send(s, '255.255.255.255', 50001, bytes)    // Uint8Array, <= 1288 B -> true/false
//     lan.recv(s)  -> { srcIp, srcPort, data } | null // non-blocking
//     lan.recvInto(s, buf, meta) -> length | -1       // zero-alloc variant; meta = Uint32Array(2) receives [srcIpNumber, srcPort]
//
// Wire frame (identical to the native relay, server/src/frame.js):
//   'B' 'N' ver=1 type(2=DATA, 6=FWD) src_ip[4] src_port[2 BE] dst_port[2 BE] dst_ip[4] payload
// Sessions: after create/join the server hands out a resume token; if the WebSocket drops the library reconnects and resumes the same
// slot/virtual IP within the server's grace period (data channels keep working meanwhile).
export const MAX_PAYLOAD = 1288;
const HDR = 16, T_DATA = 2, T_FWD = 6, BCAST = 0xffffffff, RXQ = 256;

export const ipToNum = (s) => { const p = String(s).split('.').map(Number); if (p.length !== 4 || p.some((x) => !(x >= 0 && x <= 255))) return null; return (((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]) >>> 0); };
export const numToIp = (n) => `${(n >>> 24) & 255}.${(n >>> 16) & 255}.${(n >>> 8) & 255}.${n & 255}`;
const isBcast = (n) => n === BCAST || (n & 255) === 255;
const slotOf = (n) => ((n >>> 8) === 0x0a4d00 ? n & 255 : 0);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function buildFrame(type, srcIp, srcPort, dstIp, dstPort, data) {
  const f = new Uint8Array(HDR + data.length), v = new DataView(f.buffer);
  f[0] = 0x42; f[1] = 0x4e; f[2] = 1; f[3] = type;
  v.setUint32(4, srcIp >>> 0); v.setUint16(8, srcPort); v.setUint16(10, dstPort); v.setUint32(12, dstIp >>> 0);
  f.set(data, HDR);
  return f;
}

export class VirtualLan {
  constructor({ url, name = 'player', forceRelay = false, rtcTimeoutMs = 4000, autoResume = true, WebSocketImpl, RTCPeerConnectionImpl, debug = false } = {}) {
    this.url = url; this.name = name; this.forceRelay = forceRelay; this.rtcTimeoutMs = rtcTimeoutMs; this.autoResume = autoResume; this.debug = debug;
    this.WS = WebSocketImpl || globalThis.WebSocket; this.RTC = RTCPeerConnectionImpl || globalThis.RTCPeerConnection;
    this.ws = null; this.id = null; this.iceServers = []; this.room = null; this.you = null; this.hostId = null; this.resume = null; this.state = 'idle';
    this.peers = new Map();                       // id -> peer
    this.sockets = new Map(); this.nextSock = 1; this.nextEph = 49152;
    this.listeners = {}; this._reqs = new Map(); this.rid = 1; this.hbTimer = null; this._userClosed = false; this._reconnecting = false;
    this.stat = { tx: 0, rx: 0, txBytes: 0, rxBytes: 0, relayTx: 0, rtcTx: 0, relayRx: 0, rtcRx: 0, hubFwd: 0, resumes: 0, dropped: { size: 0, nosock: 0, nodest: 0, malformed: 0, queue: 0, spoof: 0 }, rttMs: 0 };
  }

  // ---------- events
  on(ev, fn) { (this.listeners[ev] ||= []).push(fn); return this; }
  off(ev, fn) { this.listeners[ev] = (this.listeners[ev] || []).filter((f) => f !== fn); return this; }
  _emit(ev, a, b) { for (const f of this.listeners[ev] || []) { try { f(a, b); } catch (e) { console.error('[virtual-lan] listener', ev, e); } } }
  _log(...a) { if (this.debug) console.log('[virtual-lan]', ...a); }

  get vip() { return this.you ? this.you.vip : null; }
  get vipNum() { return this.you ? ipToNum(this.you.vip) : 0; }
  get slot() { return this.you ? this.you.slot : 0; }
  get isHost() { return !!this.you && this.hostId === this.id; }
  get topology() { return this.room ? this.room.topology || 'mesh' : 'mesh'; }
  get star() { return this.topology === 'star'; }

  // ---------- control channel
  _openWs() {
    return new Promise((resolve, reject) => {
      const ws = new this.WS(this.url); ws.binaryType = 'arraybuffer'; let done = false;
      ws.onerror = () => { if (!done) { done = true; reject(new Error('websocket error')); } };
      ws.onclose = () => { if (!done) { done = true; reject(new Error('closed')); } else this._onWsClosed(ws); };
      ws.onmessage = (ev) => {
        if (typeof ev.data !== 'string') return this._onFrame(new Uint8Array(ev.data), null);
        const m = JSON.parse(ev.data);
        if (!done && m.t === 'welcome') { done = true; resolve({ ws, welcome: m }); return; }
        if (ws === this.ws) this._onControl(m);
      };
    });
  }
  async connect() {
    this._userClosed = false;
    const { ws, welcome } = await this._openWs();
    this.ws = ws; this.id = welcome.id; this.iceServers = welcome.iceServers || []; this.state = 'connected';
    this._startHeartbeat(welcome.heartbeatMs); ws.send(JSON.stringify({ t: 'hello', name: this.name }));
    return welcome;
  }
  _startHeartbeat(ms = 5000) {
    clearInterval(this.hbTimer);
    this.hbTimer = setInterval(() => { if (this.ws && this.ws.readyState === 1) this.ws.send(JSON.stringify({ t: 'ping', ts: performance.now() })); }, Math.max(500, ms / 2));
  }
  _request(msg, okType) {
    return new Promise((resolve, reject) => {
      const rid = this.rid++; const to = setTimeout(() => { this._reqs.delete(rid); reject(new Error('timeout')); }, 5000);
      this._reqs.set(rid, { okType, resolve: (m) => { clearTimeout(to); resolve(m); }, reject: (e) => { clearTimeout(to); reject(e); } });
      this.ws.send(JSON.stringify({ ...msg, rid }));
    });
  }
  async listRooms() { return (await this._request({ t: 'list' }, 'rooms')).rooms; }
  async createRoom(opts = {}) { return this._joined(await this._request({ t: 'create', player: this.name, ...opts }, 'joined')); }
  async joinRoom(code, password = '') { return this._joined(await this._request({ t: 'join', code, password, player: this.name }, 'joined')); }
  async leaveRoom() { if (!this.room) return; await this._request({ t: 'leave' }, 'left'); this._resetRoom(); }
  async kick(id, { ban = false, reason = '' } = {}) { return this._request({ t: 'kick', id, ban, reason }, 'kick_ok'); }
  async bans() { return (await this._request({ t: 'bans' }, 'bans')).bans; }
  async unban(ip) { return (await this._request({ t: 'unban', ip }, 'unban_ok')).ok; }
  close() { this._userClosed = true; clearInterval(this.hbTimer); this._teardownPeers(); try { this.ws && this.ws.close(); } catch { /* */ } this.state = 'closed'; }
  _resetRoom() { this._teardownPeers(); this.room = null; this.you = null; this.hostId = null; this.resume = null; for (const s of [...this.sockets.keys()]) this.closeSocket(s); }

  _onWsClosed(ws) {
    if (ws !== this.ws) return;
    clearInterval(this.hbTimer);
    for (const p of this._reqs.values()) p.reject(new Error('closed')); this._reqs.clear();
    if (!this._userClosed && this.autoResume && this.room && this.resume && this.resume.token) { this._reconnect(); return; }
    this._resetRoom(); this.state = 'closed'; this._emit('closed');
  }
  async _reconnect() {
    if (this._reconnecting) return; this._reconnecting = true; this.state = 'offline'; this._emit('offline');
    const deadline = performance.now() + (this.resume.graceMs || 30000) - 1000; let delay = 200;
    while (!this._userClosed && performance.now() < deadline) {
      try {
        const { ws, welcome } = await this._openWs();
        this.ws = ws; this.id = welcome.id; this.iceServers = welcome.iceServers || this.iceServers; this._startHeartbeat(welcome.heartbeatMs);
        const j = await this._request({ t: 'resume', token: this.resume.token }, 'joined');
        this.id = j.id; this._applyJoined(j); this.state = 'connected'; this.stat.resumes++; this._reconnecting = false; this._emit('resumed', j); return;
      } catch (e) {
        if (e.code === 'bad_token' || e.code === 'not_offline') { if (e.code === 'bad_token') break; }
        await sleep(delay); delay = Math.min(delay * 2, 2000);
      }
    }
    this._reconnecting = false; this._resetRoom(); this.state = 'closed'; this._emit('closed', 'resume_failed');
  }

  _onControl(m) {
    if (m.rid && this._reqs.has(m.rid)) {
      const p = this._reqs.get(m.rid);
      if (m.t === 'error') { this._reqs.delete(m.rid); return p.reject(Object.assign(new Error(m.code), { code: m.code })); }
      if (m.t === p.okType) { this._reqs.delete(m.rid); return p.resolve(m); }
    }
    switch (m.t) {
      case 'pong': this.stat.rttMs = performance.now() - m.ts; break;
      case 'peer_joined': this._addPeer(m.peer); this._emit('peer_joined', m.peer); break;
      case 'peer_left': this._dropPeer(m.id); this._emit('peer_left', { id: m.id, slot: m.slot, reason: m.reason }); break;
      case 'peer_offline': this._emit('peer_offline', m.id); break;
      case 'peer_online': this._emit('peer_online', m.id); break;
      case 'host_changed': this.hostId = m.id; this._retopo(); this._emit('host_changed', m.id); break;
      case 'signal': this._onSignal(m.from, m.data); break;
      case 'kicked': this._userClosed = true; this._emit('kicked', m); this._resetRoom(); break;
      case 'room_closed': this._emit('room_closed', m); this._resetRoom(); break;
      case 'shutdown': this._emit('shutdown', m); break;
      case 'error': this._emit('error', m); break;
    }
  }
  _joined(m) { this._applyJoined(m); this._emit('room', m); return m; }
  _applyJoined(m) {
    this.room = m.room; this.you = m.you; this.hostId = m.hostId; if (m.iceServers) this.iceServers = m.iceServers; if (m.resume) this.resume = m.resume;
    const ids = new Set(m.peers.map((p) => p.id));
    for (const id of [...this.peers.keys()]) if (!ids.has(id)) { this._dropPeer(id); this._emit('peer_left', { id, reason: 'gone' }); }
    for (const p of m.peers) this._addPeer(p);
  }

  // ---------- peers / WebRTC
  _needLink(peer) { return !peer.relayOnly && !this.forceRelay && !!this.RTC && (!this.star || this.isHost || peer.id === this.hostId); }
  _addPeer(info) {
    if (this.peers.has(info.id)) return;
    const peer = { id: info.id, name: info.name, slot: info.slot, vip: ipToNum(info.vip), relayOnly: !!info.relayOnly, pc: null, dc: null, state: 'relay', pendingCand: [], tx: 0, rx: 0, timer: null, opened: 0 };
    this.peers.set(peer.id, peer);
    if (this._needLink(peer)) this._beginLink(peer); else if (!peer.relayOnly && this.star && !this.forceRelay) peer.state = 'viahub';
  }
  _beginLink(peer) {
    peer.state = 'connecting';
    if (this.slot > peer.slot) this._startRtc(peer, true);                   // higher slot offers (deterministic, no glare)
    clearTimeout(peer.timer);
    peer.timer = setTimeout(() => { if (peer.state === 'connecting') { peer.state = 'relay'; this._log('rtc timeout -> relay', peer.id); this._emit('transport', { peer: peer.id, state: 'relay' }); } }, this.rtcTimeoutMs);
  }
  _closeLink(peer) { clearTimeout(peer.timer); try { peer.dc && peer.dc.close(); peer.pc && peer.pc.close(); } catch { /* */ } peer.dc = null; peer.pc = null; }
  _retopo() {                                                                // host changed (star): move the data channels to the new hub
    for (const peer of this.peers.values()) {
      const need = this._needLink(peer);
      if (need && !peer.pc && !peer.dc) this._beginLink(peer);
      else if (!need && (peer.pc || peer.dc)) { this._closeLink(peer); peer.state = peer.relayOnly ? 'relay' : 'viahub'; this._emit('transport', { peer: peer.id, state: peer.state }); }
    }
  }
  _dropPeer(id) { const p = this.peers.get(id); if (!p) return; this._closeLink(p); this.peers.delete(id); }
  _teardownPeers() { for (const id of [...this.peers.keys()]) this._dropPeer(id); }
  _signal(to, data) { if (this.ws && this.ws.readyState === 1) this.ws.send(JSON.stringify({ t: 'signal', to, data })); }

  _makePc(peer) {
    const pc = peer.pc = new this.RTC({ iceServers: this.iceServers });
    pc.onicecandidate = (e) => { if (e.candidate) this._signal(peer.id, { cand: e.candidate.toJSON ? e.candidate.toJSON() : e.candidate }); };
    pc.onconnectionstatechange = () => {
      this._log('pc', peer.id, pc.connectionState);
      if (pc.connectionState === 'failed' || pc.connectionState === 'closed' || pc.connectionState === 'disconnected') { if (peer.state === 'open') { peer.state = 'relay'; this._emit('transport', { peer: peer.id, state: 'relay' }); } }
    };
    pc.ondatachannel = (e) => this._setupDc(peer, e.channel);
    return pc;
  }
  async _startRtc(peer, initiator) {
    const pc = peer.pc || this._makePc(peer);
    if (initiator) {
      this._setupDc(peer, pc.createDataChannel('udp', { ordered: false, maxRetransmits: 0 }));
      await pc.setLocalDescription(await pc.createOffer());
      this._signal(peer.id, { sdp: pc.localDescription.toJSON ? pc.localDescription.toJSON() : pc.localDescription });
    }
  }
  _setupDc(peer, dc) {
    peer.dc = dc; dc.binaryType = 'arraybuffer';
    dc.onopen = () => { clearTimeout(peer.timer); peer.state = 'open'; peer.opened = performance.now(); this._log('dc open', peer.id); this._emit('transport', { peer: peer.id, state: 'open' }); };
    dc.onclose = () => { if (peer.state === 'open') { peer.state = peer.id === this.hostId || !this.star ? 'relay' : peer.state; this._emit('transport', { peer: peer.id, state: peer.state }); } };
    dc.onmessage = (e) => this._onFrame(new Uint8Array(e.data), peer);
  }
  async _onSignal(from, data) {
    const peer = this.peers.get(from); if (!peer || peer.relayOnly || this.forceRelay || !this.RTC) return;
    const pc = peer.pc || this._makePc(peer);
    try {
      if (data.sdp) {
        await pc.setRemoteDescription(data.sdp);
        for (const c of peer.pendingCand.splice(0)) await pc.addIceCandidate(c);
        if (data.sdp.type === 'offer') { await pc.setLocalDescription(await pc.createAnswer()); this._signal(from, { sdp: pc.localDescription.toJSON ? pc.localDescription.toJSON() : pc.localDescription }); }
      } else if (data.cand) {
        if (pc.remoteDescription) await pc.addIceCandidate(data.cand); else peer.pendingCand.push(data.cand);
      }
    } catch (e) { this._log('signal error', e); }
  }

  // ---------- datagram plane
  socket(port = 0, { reuse = false } = {}) {
    if (!this.you) throw new Error('not in a room');
    if (!port) { for (let i = 0; i < 16384; i++) { const p = this.nextEph++; if (this.nextEph > 65535) this.nextEph = 49152; if (![...this.sockets.values()].some((s) => s.port === p)) { port = p; break; } } }
    else if (!reuse && [...this.sockets.values()].some((s) => s.port === port && !s.reuse)) { const e = new Error('EADDRINUSE'); e.code = 'EADDRINUSE'; throw e; }
    const h = this.nextSock++;
    this.sockets.set(h, { port, reuse, q: new Array(RXQ), head: 0, n: 0 });
    return h;
  }
  closeSocket(h) { this.sockets.delete(h); }
  portOf(h) { const s = this.sockets.get(h); return s ? s.port : 0; }

  // returns true if the datagram was accepted for transmission (local delivery counts)
  send(h, dstIp, dstPort, data) {
    const s = this.sockets.get(h); if (!s || !this.you) return false;
    return this._transmit(s.port, dstIp, dstPort, data, true);
  }
  // loopback=false: the caller (native C Winsock HLE in raw mode) already delivered local copies itself
  _transmit(srcPort, dstIp, dstPort, data, loopback) {
    if (!this.you) return false;
    if (data.length > MAX_PAYLOAD) { this.stat.dropped.size++; return false; }
    const dst = typeof dstIp === 'number' ? dstIp >>> 0 : ipToNum(dstIp); if (dst === null) return false;
    const s = { port: srcPort };
    const me = this.vipNum, data2 = buildFrame(T_DATA, me, s.port, dst, dstPort, data);
    this.stat.tx++; this.stat.txBytes += data.length;
    const peers = [...this.peers.values()];
    const hub = this.star && !this.isHost ? this.peers.get(this.hostId) : null;       // leaf's uplink in a star
    if (isBcast(dst)) {
      if (loopback) this._deliverLocal(me, s.port, dst, dstPort, data);                // Windows loops broadcasts back to the sender's own sockets
      if (!peers.length) return true;
      if (hub) { if (this._rtcOpen(hub)) { this._sendRtc(hub, buildFrame(T_FWD, me, s.port, dst, dstPort, data)); this.stat.hubFwd++; } else this._sendRelay(data2); return true; }
      if (peers.every((p) => this._rtcOpen(p))) for (const p of peers) this._sendRtc(p, data2);
      else this._sendRelay(data2);                                                     // mixed: one relay broadcast reaches everybody (rare packets: discovery)
      return true;
    }
    if (dst === me) { if (loopback) this._deliverLocal(me, s.port, dst, dstPort, data); return true; }
    const peer = peers.find((p) => p.slot === slotOf(dst));
    if (!peer) { this.stat.dropped.nodest++; return true; }                            // UDP: silently lost
    if (hub && peer !== hub) {                                                         // leaf -> leaf through the hub
      if (this._rtcOpen(hub)) { this._sendRtc(hub, buildFrame(T_FWD, me, s.port, dst, dstPort, data)); this.stat.hubFwd++; } else this._sendRelay(data2);
      return true;
    }
    if (this._rtcOpen(peer)) this._sendRtc(peer, data2); else this._sendRelay(data2);
    return true;
  }
  _rtcOpen(p) { return p.state === 'open' && p.dc && p.dc.readyState === 'open' && p.dc.bufferedAmount < 256 * 1024; }
  _sendRtc(p, frame) { p.dc.send(frame); p.tx++; this.stat.rtcTx++; }
  _sendRelay(frame) { if (this.ws && this.ws.readyState === 1 && this.ws.bufferedAmount < 1 << 20) { this.ws.send(frame); this.stat.relayTx++; } }

  _onFrame(bytes, via) {
    if (bytes.length < HDR || bytes[0] !== 0x42 || bytes[1] !== 0x4e || bytes[2] !== 1 || (bytes[3] !== T_DATA && bytes[3] !== T_FWD)) { this.stat.dropped.malformed++; return; }
    const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const len = bytes.length - HDR; if (len > MAX_PAYLOAD) { this.stat.dropped.size++; return; }
    const dstIp = v.getUint32(12), dstPort = v.getUint16(10), srcPort = v.getUint16(8); let srcIp = v.getUint32(4);
    if (via) { via.rx++; this.stat.rtcRx++; } else this.stat.relayRx++;
    if (bytes[3] === T_FWD) {                                                            // hub forwarding (star)
      if (!via) { this.stat.dropped.malformed++; return; }                               // the server turns FWD into plain DATA; FWD never arrives over the websocket
      if (this.isHost && this.star) { this._hubForward(bytes, via, srcIp, srcPort, dstIp, dstPort); return; }
      if (via.id !== this.hostId) { this.stat.dropped.spoof++; return; }                 // only the hub may forward to us
    } else if (via) srcIp = via.vip;                                                      // channel owner defines the source on RTC (anti-spoof)
    if (!isBcast(dstIp) && dstIp !== this.vipNum) { this.stat.dropped.nodest++; return; }
    this.stat.rx++; this.stat.rxBytes += len;
    this._deliverLocal(srcIp, srcPort, dstIp, dstPort, bytes.subarray(HDR));
  }
  // we are the hub: a leaf asked us to forward `bytes` (type FWD). The leaf's claimed source must be the channel owner.
  _hubForward(bytes, from, srcIp, srcPort, dstIp, dstPort) {
    if (srcIp !== from.vip) { this.stat.dropped.spoof++; return; }
    const data = bytes.subarray(HDR);
    const toServer = () => this._sendRelay(bytes.slice());                                // host-only FWD: the server delivers it as DATA with the original source
    if (isBcast(dstIp)) {
      this.stat.rx++; this.stat.rxBytes += data.length; this._deliverLocal(srcIp, srcPort, dstIp, dstPort, data);
      const others = [...this.peers.values()].filter((p) => p !== from);
      if (others.length && others.every((p) => !p.relayOnly && this._rtcOpen(p))) for (const p of others) this._sendRtc(p, bytes);
      else if (others.length) toServer();                                                 // some leaf (or a native peer) has no channel: one server broadcast reaches all of them
      this.stat.hubFwd++;
      return;
    }
    if (dstIp === this.vipNum) { this.stat.rx++; this.stat.rxBytes += data.length; this._deliverLocal(srcIp, srcPort, dstIp, dstPort, data); return; }
    const p = [...this.peers.values()].find((x) => x.slot === slotOf(dstIp)); if (!p || p === from) { this.stat.dropped.nodest++; return; }
    if (!p.relayOnly && this._rtcOpen(p)) this._sendRtc(p, bytes); else toServer();
    this.stat.hubFwd++;
  }
  _deliverLocal(srcIp, srcPort, dstIp, dstPort, data) {
    if (this.raw) { this._rawEnqueue(srcIp, srcPort, dstIp, dstPort, data); return; }
    let target = null; const bc = isBcast(dstIp);
    for (const s of this.sockets.values()) {
      if (s.port !== dstPort) continue;
      if (bc) this._enqueue(s, srcIp, srcPort, data); else target = s;                       // unicast: most recently bound match
    }
    if (target) this._enqueue(target, srcIp, srcPort, data);
    else if (!bc && ![...this.sockets.values()].some((s) => s.port === dstPort)) this.stat.dropped.nosock++;
  }
  _enqueue(s, srcIp, srcPort, data) {
    if (s.n === RXQ) { s.head = (s.head + 1) % RXQ; s.n--; this.stat.dropped.queue++; }
    s.q[(s.head + s.n) % RXQ] = { srcIp, srcPort, data: data.slice() }; s.n++;                 // copy: the source buffer belongs to the transport
  }
  // ---- raw frame mode (for the wasm Winsock HLE, which keeps its own socket table in C and only needs a frame pipe) --------------------------
  // enableRaw(): every datagram addressed to this peer is queued as a whole frame instead of being demultiplexed to sockets.
  //   rawSendFrame(frame) : frame = 16-byte header + payload as produced by wsock_hle.c (src_port, dst_ip, dst_port are used; src ip is ours)
  //   rawRecvFrame(dst)   : copies the next inbound frame (header with real src/dst + payload) into `dst`, returns its length or -1 when empty
  enableRaw() { this.raw = { q: new Array(RXQ * 4), head: 0, n: 0 }; return this; }
  _rawEnqueue(srcIp, srcPort, dstIp, dstPort, data) {
    const r = this.raw; if (r.n === r.q.length) { r.head = (r.head + 1) % r.q.length; r.n--; this.stat.dropped.queue++; }
    r.q[(r.head + r.n) % r.q.length] = buildFrame(T_DATA, srcIp, srcPort, dstIp, dstPort, data); r.n++;
  }
  rawSendFrame(frame) {
    if (frame.length < HDR || frame[0] !== 0x42 || frame[1] !== 0x4e || frame[3] !== T_DATA) return false;
    const v = new DataView(frame.buffer, frame.byteOffset, frame.byteLength);
    return this._transmit(v.getUint16(8), v.getUint32(12), v.getUint16(10), frame.subarray(HDR), false);
  }
  rawRecvFrame(dst) {
    const r = this.raw; if (!r || !r.n) return -1;
    const f = r.q[r.head]; r.q[r.head] = undefined; r.head = (r.head + 1) % r.q.length; r.n--;
    const n = Math.min(dst.length, f.length); dst.set(f.subarray(0, n)); return n;
  }
  // offline instance: no server, no peers; broadcasts and unicast to ourselves loop back. Lets the game run (and bind sockets) before/without a room.
  static local({ vip = '10.77.0.1', name = 'offline' } = {}) { const l = new VirtualLan({ name }); l.you = { id: 'local', name, slot: ipToNum(vip) & 255, vip }; l.id = 'local'; l.room = { code: 'LOCAL', topology: 'mesh', name: 'offline' }; l.hostId = 'local'; l.state = 'local'; return l; }

  pending(h) { const s = this.sockets.get(h); return s ? s.n : 0; }
  recv(h) {
    const s = this.sockets.get(h); if (!s || !s.n) return null;
    const d = s.q[s.head]; s.q[s.head] = undefined; s.head = (s.head + 1) % RXQ; s.n--;
    return { srcIp: numToIp(d.srcIp), srcPort: d.srcPort, data: d.data };
  }
  recvInto(h, buf, meta) {
    const s = this.sockets.get(h); if (!s || !s.n) return -1;
    const d = s.q[s.head]; s.q[s.head] = undefined; s.head = (s.head + 1) % RXQ; s.n--;
    const n = Math.min(buf.length, d.data.length); buf.set(d.data.subarray(0, n)); meta[0] = d.srcIp; meta[1] = d.srcPort;
    return n;
  }
  peerInfo() { return [...this.peers.values()].map((p) => ({ id: p.id, name: p.name, slot: p.slot, vip: numToIp(p.vip), path: p.relayOnly ? 'relay-only' : this._rtcOpen(p) ? 'webrtc' : p.state === 'viahub' ? 'viahub' : p.state, tx: p.tx, rx: p.rx })); }
  stats() { return { ...this.stat, dropped: { ...this.stat.dropped }, peers: this.peerInfo() }; }
}
export default VirtualLan;
