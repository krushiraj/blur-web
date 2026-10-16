'use strict';
// Datagram frame shared by: native wsock_hle (UDP to the gateway), browser clients (WebSocket relay and WebRTC data channels).
//   0 'B' 'N'  2 ver=1  3 type  4 src_ip[4]  8 src_port[2 BE]  10 dst_port[2 BE]  12 dst_ip[4]  16.. payload
// IPs are handled as unsigned 32-bit numbers in "dotted order" (10.77.0.5 -> 0x0a4d0005).
const HDR = 16;
const MAX_PAYLOAD = 1288;          // BD_MAX_DATAGRAM_SIZE of the game
const T = { HELLO: 1, DATA: 2, BYE: 3, WELCOME: 4, ERROR: 5, FWD: 6 };   // FWD: hub-forwarded frame (star topology); only the room host may inject it, source IP preserved
const BCAST = 0xffffffff;

function ipToNum(s) {
  const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(s);
  if (!m) return null;
  const b = m.slice(1).map(Number);
  if (b.some((x) => x > 255)) return null;
  return (((b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]) >>> 0);
}
const numToIp = (n) => `${(n >>> 24) & 255}.${(n >>> 16) & 255}.${(n >>> 8) & 255}.${n & 255}`;
const isBroadcast = (n) => n === BCAST || (n & 255) === 255;
const vipForSlot = (slot) => ((10 << 24) | (77 << 16) | slot) >>> 0;           // 10.77.0.<slot>
const slotForVip = (n) => ((n >>> 8) === ((10 << 16) | (77 << 8) | 0) ? n & 255 : 0);

function encode({ type, srcIp = 0, srcPort = 0, dstPort = 0, dstIp = 0, payload }) {
  const p = payload ? Buffer.from(payload) : Buffer.alloc(0);
  const b = Buffer.allocUnsafe(HDR + p.length);
  b[0] = 0x42; b[1] = 0x4e; b[2] = 1; b[3] = type;
  b.writeUInt32BE(srcIp >>> 0, 4); b.writeUInt16BE(srcPort, 8); b.writeUInt16BE(dstPort, 10); b.writeUInt32BE(dstIp >>> 0, 12);
  p.copy(b, HDR);
  return b;
}
// returns null for anything malformed
function decode(buf) {
  if (!buf || buf.length < HDR || buf[0] !== 0x42 || buf[1] !== 0x4e || buf[2] !== 1) return null;
  const type = buf[3];
  if (type < 1 || type > 6) return null;
  return { type, srcIp: buf.readUInt32BE(4), srcPort: buf.readUInt16BE(8), dstPort: buf.readUInt16BE(10), dstIp: buf.readUInt32BE(12), payload: buf.subarray(HDR) };
}
module.exports = { HDR, MAX_PAYLOAD, T, BCAST, ipToNum, numToIp, isBroadcast, vipForSlot, slotForVip, encode, decode };
