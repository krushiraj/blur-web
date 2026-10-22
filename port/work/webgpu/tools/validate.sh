#!/bin/bash
# Headless-Chromium validation of the pack.
#   tools/validate.sh validate    every shader module + every warm.json (vs,ps) pipeline, using only the pack's tables  (pack_validate.html)
#   tools/validate.sh numeric     WebGL2(GLSL) vs WebGPU(WGSL) pixel comparison of every warm.json pair                  (numeric.html)
# env: CHROME (default /Applications/Chromium.app/Contents/MacOS/Chromium), PORT (default: first free port from 8911; ports 8080/8081/8765 are never used)
#      JSON=<file> keep the raw result json
# Needs node and tools/shot/node_modules/puppeteer-core (run.js requires it).  Only the static server started here is stopped at exit.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(dirname "$HERE")
ROOT=$(cd "$W/../../.." && pwd)
MODE=${1:-validate}
PORT=${PORT:-8911}
while lsof -iTCP:"$PORT" -sTCP:LISTEN >/dev/null 2>&1 || [ "$PORT" = 8080 ] || [ "$PORT" = 8081 ] || [ "$PORT" = 8765 ]; do PORT=$((PORT + 1)); done
[ -f "$W/pack/blur.wgslpack.gz" ] || { echo "no pack: run tools/make_wgsl_pack.sh first"; exit 2; }
node "$HERE/srv.js" "$PORT" "$W" "$ROOT/port/harness/web" >/dev/null 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null || true' EXIT
sleep 1
case "$MODE" in
  validate) PAGE=pack_validate.html ;;
  numeric)  PAGE=numeric.html ;;
  *) echo "mode: validate|numeric"; exit 2 ;;
esac
OUT=${JSON:-$(mktemp)}
(cd "$W" && node run.js "http://localhost:$PORT/$PAGE" 900 >"$OUT" 2>"$OUT.err") || { cat "$OUT.err"; exit 1; }
python3 -I "$HERE/report.py" "$MODE" "$OUT"
