#!/bin/zsh
# Smoke test entry point.  smoke.sh [--wasm] [smoke.js options: --bin --harness --port --frames --update-refs --out --no-shots]
#   (default)  native harness (cwd port/harness, own viewer port 8797), deterministic scripted walk, log + screenshot checks  -> smoke.js
#   --wasm     native-vs-wasm API-sequence equivalence (the wasm build has no WebGL back end yet, so no screenshots) -> wasm_check.sh
HERE=${0:A:h}
if [[ $1 == --wasm ]]; then shift; exec $HERE/wasm_check.sh "$@"; fi
exec node $HERE/smoke.js "$@"
