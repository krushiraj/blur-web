'use strict';
// ICE server list handed to clients in `welcome`. Nothing external is required on localhost (host candidates work).
//   ICE_SERVERS   full JSON array (wins)
//   STUN_URLS     comma list, e.g. stun:stun.l.google.com:19302
//   TURN_URLS     comma list, e.g. turn:turn.example.com:3478?transport=udp,turns:turn.example.com:443?transport=tcp
//   TURN_SECRET   coturn "use-auth-secret" shared secret -> ephemeral credentials (username "<expiry>:<id>", HMAC-SHA1, base64)
//   TURN_USER/TURN_PASS  static credentials alternative
//   TURN_TTL      credential lifetime in seconds (default 3600)
const crypto = require('crypto');
function iceServers(env = process.env, peerId = 'anon', nowMs = Date.now()) {
  if (env.ICE_SERVERS) { try { const a = JSON.parse(env.ICE_SERVERS); if (Array.isArray(a)) return a; } catch { /* fall through */ } }
  const out = [];
  const list = (s) => (s || '').split(',').map((x) => x.trim()).filter(Boolean);
  if (env.STUN_URLS) out.push({ urls: list(env.STUN_URLS) });
  if (env.TURN_URLS) {
    const urls = list(env.TURN_URLS);
    if (env.TURN_SECRET) {
      const ttl = Number(env.TURN_TTL || 3600);
      const username = `${Math.floor(nowMs / 1000) + ttl}:${peerId}`;
      out.push({ urls, username, credential: crypto.createHmac('sha1', env.TURN_SECRET).update(username).digest('base64') });
    } else if (env.TURN_USER) out.push({ urls, username: env.TURN_USER, credential: env.TURN_PASS || '' });
  }
  return out;
}
module.exports = { iceServers };
