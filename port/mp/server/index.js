#!/usr/bin/env node
'use strict';
// yarn start
// env: PORT HOST UDP_PORT HEARTBEAT_MS TIMEOUT_MS UDP_TIMEOUT_MS RESUME_GRACE_MS MAX_CONNS MAX_CONNS_PER_IP MAX_ROOMS_PER_IP MSG_RATE MSG_BURST MSG_STRIKES MAX_MSG_BYTES
//      ALLOW_ORIGIN SHARED_SECRET ADMIN_TOKEN TRUST_PROXY DRAIN_MS SERVE_STATIC LOG_LEVEL STUN_URLS TURN_URLS TURN_SECRET TURN_USER TURN_PASS TURN_TTL ICE_SERVERS
// SIGTERM/SIGINT: drain (clients get {t:'shutdown'}, new connections get 503), exit when empty or after DRAIN_MS; a second signal exits immediately.
const { createServer } = require('./src/server');
const s = createServer();
s.listen().catch((e) => { console.error(e); process.exit(1); });
let stopping = false;
for (const sig of ['SIGINT', 'SIGTERM']) {
  process.on(sig, () => {
    if (stopping) process.exit(1);
    stopping = true;
    s.shutdown().then(() => process.exit(0)).catch(() => process.exit(1));
  });
}
