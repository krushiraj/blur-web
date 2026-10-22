#!/bin/bash
# Copies the files the live page needs for #gpu=1 next to index.html (the harness serves port/harness/web as static files):
#   wgslpack.js (= tools/packreader.js), blur.wgslpack.gz (the pack).  renderer_gpu.js and gpu-warm.json live in port/harness/web already.
# Rebuild the warm list after a pack/renderer change:  see README.md "Warm list".
set -euo pipefail
W=$(cd "$(dirname "$0")/.." && pwd)
WEB=$(cd "$W/../../harness/web" && pwd)
[ -f "$W/pack/blur.wgslpack.gz" ] || { echo "no pack: run tools/make_wgsl_pack.sh first"; exit 2; }
cp "$W/tools/packreader.js" "$WEB/wgslpack.js"
cp "$W/pack/blur.wgslpack.gz" "$WEB/blur.wgslpack.gz"
ls -l "$WEB/wgslpack.js" "$WEB/blur.wgslpack.gz" "$WEB/renderer_gpu.js" "$WEB/gpu-warm.json"
