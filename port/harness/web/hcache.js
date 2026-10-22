// Persistent content-hash cache for texture / buffer uploads (host side: gfx.c "content-hash cache").
// Storage: IndexedDB database "blur-hcache" (works on insecure LAN origins too; OPFS and the Cache API need a secure context, so they are not used).
//   store "blobs": key = 16 hex digits of the host's 64-bit content hash -> ArrayBuffer;  store "meta": same key -> { n: bytes, t: last-use ms } (small, read in one go at start-up).
// Size cap with LRU eviction: #hcmb=<MB> (default 1536). Eviction runs once at start-up, BEFORE the announce, so the host never holds a reference to something we then drop.
// #hcache=0 disables everything (the page announces "disabled" and the host sends plain uploads, exactly as before).
// Protocol: see gfx.c. Page -> host: [0x48430A, flags, count, hashes...] announce, [0x48430C, 0, count, hashes...] "stored" acks. Host -> page: 98 tagged upload (store after use), 96 reference (rebuild from cache).
(() => {
  const H = location.hash, VERIFY = /hcverify=1/.test(location.hash), off = /hcache=0/.test(H), capMB = (/hcmb=(\d+)/.exec(H) || [0, 1536])[1] | 0;
  const MEM_CAP = 192 << 20, NW = { 21: 8, 31: 2 };
  const hex = (lo, hi) => (hi >>> 0).toString(16).padStart(8, '0') + (lo >>> 0).toString(16).padStart(8, '0');
  const req = (rq) => new Promise((res, rej) => { rq.onsuccess = () => res(rq.result); rq.onerror = () => rej(rq.error); });
  const done = (tx) => new Promise((res, rej) => { tx.oncomplete = () => res(); tx.onerror = tx.onabort = () => rej(tx.error); });
  class HCache {
    constructor(r) {
      this.r = r; this.enabled = false; this.db = null; this.have = new Set(); this.meta = new Map(); this.total = 0; this.cap = capMB * 1048576;
      this.mem = new Map(); this.memBytes = 0; this.pin = new Set(); this.pend = new Map(); this.pendBytes = 0; this.acks = []; this.touched = new Set(); this.timer = 0; this.chain = Promise.resolve(); this.busy = 0;
      this.st = { refs: 0, refBytes: 0, tags: 0, tagBytes: 0, misses: 0, memHits: 0, idbHits: 0, stored: 0, storeFail: 0, verified: 0, verifiedBytes: 0, verifyBad: 0, announced: 0, evicted: 0, idbMs: 0 };
      this.ready = off ? Promise.resolve() : this.init().catch((e) => { this.enabled = false; r.warn && r.warn('hc-init', 'hash cache disabled: ' + e); });
    }
    async init() {
      const rq = indexedDB.open('blur-hcache', 1);
      rq.onupgradeneeded = () => { rq.result.createObjectStore('blobs'); rq.result.createObjectStore('meta'); };
      this.db = await req(rq);
      try { navigator.storage && navigator.storage.persist && navigator.storage.persist().then((g) => { this.persisted = g; }); } catch (e) {}
      const tx = this.db.transaction('meta'), st = tx.objectStore('meta'), keys = await req(st.getAllKeys()), vals = await req(st.getAll());
      keys.forEach((k, i) => { this.meta.set(k, vals[i]); this.total += vals[i].n; this.have.add(k); });
      if (this.total > this.cap) await this.evict(this.cap * 0.85);
      this.enabled = true;
    }
    async evict(target) {
      const order = [...this.meta.entries()].sort((a, b) => a[1].t - b[1].t), tx = this.db.transaction(['blobs', 'meta'], 'readwrite'), b = tx.objectStore('blobs'), m = tx.objectStore('meta');
      for (const [k, v] of order) { if (this.total <= target) break; b.delete(k); m.delete(k); this.meta.delete(k); this.have.delete(k); this.total -= v.n; this.st.evicted++; }
      await done(tx);
    }
    announce(ws) {
      this.ws = ws;
      const keys = this.enabled ? [...this.have] : [], CH = 32768; this.st.announced = keys.length;
      for (let i = 0, first = true; first || i < keys.length; i += CH, first = false) {
        const part = keys.slice(i, i + CH), a = new Int32Array(3 + 2 * part.length), last = i + CH >= keys.length;
        a[0] = 0x48430A; a[1] = (last ? 1 : 0) | (this.enabled ? 2 : 0); a[2] = part.length;
        part.forEach((k, j) => { a[3 + 2 * j] = parseInt(k.slice(8), 16) | 0; a[4 + 2 * j] = parseInt(k.slice(0, 8), 16) | 0; });
        ws.send(a.buffer);
      }
    }
    // does this batch contain references (type 96)? then it must be fed asynchronously (the data may have to come out of IndexedDB first)
    hasRef(ab) { const dv = new DataView(ab); for (let o = 0; o + 8 <= ab.byteLength;) { if (dv.getUint32(o + 4, true) === 96) return true; o += 8 + dv.getUint32(o, true); } return false; }
    onData(ab, cb) {
      if (!this.busy && !this.hasRef(ab)) { cb(this.r.feed(ab)); return; }
      this.busy++; this.chain = this.chain.then(() => this.feedAsync(ab)).then(cb, (e) => this.r.warn && this.r.warn('hc-feed' + e, 'hash cache feed failed: ' + (e.stack || e))).then(() => { this.busy--; });
    }
    memGet(k) { const v = this.mem.get(k); if (v) { this.mem.delete(k); this.mem.set(k, v); } return v; }
    memPut(k, v) {
      if (this.mem.has(k)) return; this.mem.set(k, v); this.memBytes += v.byteLength;
      for (const [kk, vv] of this.mem) { if (this.memBytes <= MEM_CAP) break; if (kk === k || this.pin.has(kk) || this.pend.has(kk)) continue; this.mem.delete(kk); this.memBytes -= vv.byteLength; }
    }
    async fetchAhead(ab, dv, off) {      // load the data of the next references (this one first) from IndexedDB in one transaction
      const want = [];
      for (let o = off, n = 0, bytes = 0; o + 8 <= ab.byteLength && n < 64 && bytes < (48 << 20);) {
        const len = dv.getUint32(o, true), ty = dv.getUint32(o + 4, true);
        if (ty === 96) { const k = hex(dv.getUint32(o + 12, true), dv.getUint32(o + 16, true)); this.pin.add(k); if (!this.mem.has(k) && !want.includes(k)) { want.push(k); bytes += dv.getUint32(o + 20, true); } n++; }
        o += 8 + len;
      }
      const t0 = performance.now(), st = this.db.transaction('blobs').objectStore('blobs'), vals = await Promise.all(want.map((k) => req(st.get(k)).catch(() => null)));
      want.forEach((k, i) => { if (vals[i]) { this.memPut(k, vals[i]); this.st.idbHits++; } }); this.st.idbMs += performance.now() - t0;
    }
    async feedAsync(ab) {
      const r = this.r, dv = new DataView(ab); let off = 0, messages = 0, presents = 0; const t0 = performance.now(); let busy = 0;
      while (off + 8 <= ab.byteLength) {
        const len = dv.getUint32(off, true), type = dv.getUint32(off + 4, true), p = off + 8; off = p + len; messages++;
        if (type === 96 && !this.mem.has(hex(dv.getUint32(p + 4, true), dv.getUint32(p + 8, true)))) { busy += performance.now() - t0; await this.fetchAhead(ab, dv, p - 8); }
        const t1 = performance.now();
        try { if (r.handle(type, ab, dv, p, len)) presents++; } catch (e) { r.warn('ex' + type + e.message, `message ${type} failed: ${e.stack || e}`); }
        busy += performance.now() - t1;
      }
      this.pin.clear(); r.jsBusy = (r.jsBusy || 0) + busy; r.jsFrames = (r.jsFrames || 0) + presents; r.jsMsgs = (r.jsMsgs || 0) + messages;
      return { messages, presents };
    }
    // 98: [T, lo, hi, payload of the type-T message] -> process as T, then keep the upload data
    tagged(r, ab, dv, p, len) {
      const T = dv.getUint32(p, true), lo = dv.getUint32(p + 4, true), hi = dv.getUint32(p + 8, true);
      r.handle(T, ab, dv, p + 12, len - 12);
      if (!this.enabled) return; const k = hex(lo, hi), s = p + 12 + 4 * NW[T], n = p + len - s; this.st.tags++; this.st.tagBytes += n;
      if (VERIFY && this.have.has(k)) { this.verify(k, ab.slice(s, s + n)); return; }
      if (this.have.has(k) || this.pend.has(k)) return;
      if (this.pendBytes > (160 << 20)) { this.st.storeFail++; return; }       // write queue backed up (slow storage): do not hold more in memory
      if (this.total + this.pendBytes + n > this.cap) { this.st.storeFail++; return; }       // full: eviction only happens at start-up, so just stop adding
      const data = ab.slice(s, s + n); this.pend.set(k, data); this.pendBytes += n; this.memPut(k, data);
      if (!this.timer) this.timer = setTimeout(() => this.flush(), 400);
    }
    // #hcverify=1 with host BLUR_HCVERIFY=1: every upload arrives in full; compare it byte for byte with what the cache holds under that hash
    async verify(k, data) {
      try { let c = this.memGet(k); if (!c) { c = await req(this.db.transaction('blobs').objectStore('blobs').get(k)); }
        const a = new Uint8Array(data), b = c ? new Uint8Array(c) : null; let ok = !!b && b.length === a.length; if (ok) for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) { ok = false; break; }
        if (ok) { this.st.verified++; this.st.verifiedBytes += a.length; } else { this.st.verifyBad++; this.r.warn && this.r.warn('hc-bad' + k, `hash cache VERIFY MISMATCH ${k}`); } }
      catch (e) { this.st.verifyBad++; }
    }
    // 96: [T, lo, hi, n, header words(T)] -> synthesize the original type-T message from the cached payload
    ref(r, ab, dv, p, len) {
      const T = dv.getUint32(p, true), k = hex(dv.getUint32(p + 4, true), dv.getUint32(p + 8, true)), n = dv.getUint32(p + 12, true), hw = 4 * NW[T], data = this.memGet(k);
      if (!data || data.byteLength !== n) { this.st.misses++; r.warn('hc-miss' + k, `hash cache miss ${k} (type ${T}, ${n} bytes): texture/buffer data lost`); return; }
      const m = new ArrayBuffer(hw + n), u8 = new Uint8Array(m); u8.set(new Uint8Array(ab, p + 16, hw), 0); u8.set(new Uint8Array(data), hw);
      this.st.refs++; this.st.refBytes += n; this.touched.add(k); if (!this.timer) this.timer = setTimeout(() => this.flush(), 400);
      r.handle(T, m, new DataView(m), 0, m.byteLength);
    }
    async flush() {
      this.timer = 0; if (!this.db) return;
      // first-visit cost control: write in small slices (<= 3 MB per transaction) and only while presents are on time; when frames run slow (> 18 ms average over the last 12) wait and retry
      const iv = this.r.ivs, nn = this.r.ivn || 0; let avg = 0; if (iv && nn > 12) { for (let i = 1; i <= 12; i++) avg += iv[(nn - i) % 600]; avg /= 12; }
      if (avg > 18 && this.pend.size && (this.slowSkips = (this.slowSkips || 0) + 1) < 200) { this.timer = setTimeout(() => this.flush(), 250); return; }
      this.slowSkips = 0;
      const items = []; let sz = 0; for (const e of this.pend) { items.push(e); sz += e[1].byteLength; if (sz >= (3 << 20)) break; }
      const touched = [...this.touched]; this.touched.clear(); if (!items.length && !touched.length) return;
      const now = Date.now(), stored = [];
      try {
        const tx = this.db.transaction(['blobs', 'meta'], 'readwrite'), b = tx.objectStore('blobs'), m = tx.objectStore('meta');
        for (const [k, d] of items) { b.put(d, k); m.put({ n: d.byteLength, t: now }, k); }
        for (const k of touched) { const v = this.meta.get(k); if (v) { v.t = now; m.put(v, k); } }
        await done(tx);
        for (const [k, d] of items) { this.pend.delete(k); this.pendBytes -= d.byteLength; this.meta.set(k, { n: d.byteLength, t: now }); this.have.add(k); this.total += d.byteLength; stored.push(k); this.st.stored++; }
      } catch (e) {
        for (const [k, d] of items) { this.pend.delete(k); this.pendBytes -= d.byteLength; } this.st.storeFail += items.length;       // quota or private mode: no ack, the host keeps sending these in full
        this.r.warn && this.r.warn('hc-store', 'hash cache write failed: ' + e);
      }
      if (stored.length && this.ws && this.ws.readyState === 1) for (let i = 0; i < stored.length; i += 32768) {
        const part = stored.slice(i, i + 32768), a = new Int32Array(3 + 2 * part.length); a[0] = 0x48430C; a[2] = part.length;
        part.forEach((k, j) => { a[3 + 2 * j] = parseInt(k.slice(8), 16) | 0; a[4 + 2 * j] = parseInt(k.slice(0, 8), 16) | 0; }); this.ws.send(a.buffer);
      }
      if (this.pend.size && !this.timer) this.timer = setTimeout(() => this.flush(), 120);
    }
    summary() { const s = this.st; return `hcache ${this.enabled ? 'on' : 'off'} ${(this.total / 1048576).toFixed(0)} MB stored, refs ${s.refs} (${(s.refBytes / 1048576).toFixed(1)} MB), tags ${s.tags}, miss ${s.misses}, verified ${s.verified} (${(s.verifiedBytes / 1048576).toFixed(0)} MB) bad ${s.verifyBad}`; }
  }
  window.HCache = HCache;
})();
