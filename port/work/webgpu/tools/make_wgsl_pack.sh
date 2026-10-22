#!/bin/bash
# D3D9 bytecode corpus -> WGSL pack (pack/blur.wgslpack[.gz|.br]).  Reproducible: same input + same tool versions -> byte-identical pack.
#
#   tools/make_wgsl_pack.sh [--validate] [--numeric]
#
# env (all optional):
#   CORPUS   dir holding pak/ exe/ extra/ with *.bin D3D9 shader bytecode   (default: port/work/shader-cache/<build dir>/src)
#   VKD3D    vkd3d-2.1 source tree with .libs/vkd3d-compiler               (default: third_party/vkd3d-2.1)
#   NAGA     naga-cli 30.0.1 binary                                         (default: naga on PATH)
#            install:  cargo install naga-cli --version 30.0.1 --locked --root <dir>   ->  NAGA=<dir>/bin/naga
#   JOBS     parallel translations (default 4)
#   PACK_DIR output dir (default: port/work/webgpu/pack)
# needs on PATH: python3 (>= 3.9), spirv-opt, spirv-val (SPIRV-Tools), brotli.   --validate/--numeric also need node + Chromium
# (see validate.sh).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(dirname "$HERE")
ROOT=$(cd "$W/../../.." && pwd)
SC=$ROOT/port/work/shader-cache
CORPUS=${CORPUS:-$(ls -d "$SC"/o*t/src 2>/dev/null | head -1)}
VKD3D=${VKD3D:-$ROOT/third_party/vkd3d-2.1}
NAGA=${NAGA:-naga}
JOBS=${JOBS:-4}
PACK_DIR=${PACK_DIR:-$W/pack}

[ -d "$CORPUS/pak" ] || { echo "CORPUS=$CORPUS has no pak/ dir"; exit 2; }
[ -x "$VKD3D/.libs/vkd3d-compiler" ] || { echo "no $VKD3D/.libs/vkd3d-compiler"; exit 2; }
command -v "$NAGA" >/dev/null || { echo "naga not found; set NAGA=... (cargo install naga-cli --version 30.0.1 --locked --root DIR)"; exit 2; }
for t in spirv-opt spirv-val brotli python3; do command -v $t >/dev/null || { echo "missing tool: $t"; exit 2; }; done
NV=$("$NAGA" --version)
[ "$NV" = "30.0.1" ] || echo "WARNING: naga $NV (the pack was validated with 30.0.1; output may differ)"
echo "tools: naga $NV; $(spirv-opt --version | head -1); vkd3d $(DYLD_LIBRARY_PATH=$VKD3D/.libs LD_LIBRARY_PATH=$VKD3D/.libs "$VKD3D/.libs/vkd3d-compiler" --version 2>&1 | head -1)"

mkdir -p "$PACK_DIR"
python3 -I "$HERE/build_pack.py" --corpus "$CORPUS" --vkd3d "$VKD3D" --naga "$NAGA" --jobs "$JOBS" \
  --pack "$PACK_DIR/blur.wgslpack" --stats "$PACK_DIR/stats.json"
echo "sha256: $(shasum -a 256 "$PACK_DIR/blur.wgslpack" | cut -d' ' -f1)  (blur.wgslpack)"
ls -l "$PACK_DIR"

for a in "$@"; do
  case "$a" in
    --validate) "$HERE/validate.sh" validate ;;
    --numeric)  "$HERE/validate.sh" numeric ;;
    *) echo "unknown arg $a"; exit 2 ;;
  esac
done
