'use strict';
const WebSocket = require('ws');
const { createServer } = require('../src/server');
const F = require('../src/frame');

async function startServer(cfg = {}) {
  const s = createServer({ port: 0, log: { debug() {}, info() {}, warn() {}, error() {} }, serveStatic: false, ...cfg });
  await s.listen();
  return s;
}
class Client {
  constructor(url) { this.msgs = []; this.bins = []; this.waiters = []; this.ws = new WebSocket(url); this.ws.binaryType = 'nodebuffer';
    this.ready = new Promise((res, rej) => { this.ws.on('open', res); this.ws.on('error', rej); });
    this.ws.on('message', (d, isBin) => { if (isBin) this.bins.push(d); else this.msgs.push(JSON.parse(d.toString())); this._wake(); });
    this.closed = new Promise((res) => this.ws.on('close', res)); }
  _wake() { for (const w of [...this.waiters]) w(); }
  send(o) { this.ws.send(JSON.stringify(o)); }
  sendBin(b) { this.ws.send(b, { binary: true }); }
  async until(pred, ms = 2000) {
    const t0 = Date.now();
    for (;;) {
      const v = pred(); if (v) return v;
      if (Date.now() - t0 > ms) throw new Error('timeout waiting; msgs=' + JSON.stringify(this.msgs.slice(-5)));
      await new Promise((res) => { const w = () => { this.waiters = this.waiters.filter((x) => x !== w); res(); }; this.waiters.push(w); setTimeout(w, 25); });
    }
  }
  msg(t, from = 0) { return () => this.msgs.slice(from).find((m) => m.t === t); }
  async req(o, t) { const n = this.msgs.length; this.send(o); return this.until(() => this.msgs.slice(n).find((m) => m.t === t || m.t === 'error')); }
  close() { this.ws.close(); return this.closed; }
}
async function connect(s) { const c = new Client(`ws://127.0.0.1:${s.port}/ws`); await c.ready; await c.until(c.msg('welcome')); return c; }
const dataFrame = (srcPort, dstIp, dstPort, bytes) => F.encode({ type: F.T.DATA, srcIp: 0x01020304, srcPort, dstIp: F.ipToNum(dstIp), dstPort, payload: Buffer.from(bytes) });
module.exports = { startServer, Client, connect, dataFrame };
