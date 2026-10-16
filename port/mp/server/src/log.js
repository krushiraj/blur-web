'use strict';
// one JSON object per line on stdout: {"ts":..., "lvl":"info", "ev":"room_created", ...}
const LEVELS = { debug: 10, info: 20, warn: 30, error: 40, silent: 99 };
function makeLogger(level = process.env.LOG_LEVEL || 'info', sink = (line) => process.stdout.write(line + '\n')) {
  const min = LEVELS[level] ?? 20;
  const log = (lvl) => (ev, fields) => { if (LEVELS[lvl] >= min) sink(JSON.stringify({ ts: new Date().toISOString(), lvl, ev, ...fields })); };
  return { debug: log('debug'), info: log('info'), warn: log('warn'), error: log('error') };
}
module.exports = { makeLogger };
