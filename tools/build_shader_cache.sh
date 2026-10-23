#!/usr/bin/env bash
# Build the offline WebGL2 shader cache (port/harness/web/shaders.bin) from YOUR game files. Optional: without it the
# host translates every shader the first time the game uses it (more hitches at the start, same picture).
#
#   tools/build_shader_cache.sh        (run ./setup.sh first)
#
# 1. extracts the D3D9 shader bytecode from cache/shaders-speed.pak and from the unpacked exe image (re/dump/blur_runtime.bin)
# 2. translates all of it with port/harness/shadertool (the same code as the live path)
# 3. copies shaders.bin(.gz) next to the page so the host and the page both find it
# The result is derived from the game: it is git-ignored, keep it private.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
[ -f re/dump/blur_runtime.bin ] || { echo "run ./setup.sh first (re/dump/blur_runtime.bin is missing)" >&2; exit 1; }
[ -f game/extracted/BLUR/cache/shaders-speed.pak ] || { echo "game/extracted/BLUR/cache/shaders-speed.pak not found" >&2; exit 1; }
V="$ROOT/third_party/vkd3d-2.1"
VARS="V=$V"
command -v brew >/dev/null 2>&1 && VARS="$VARS BREW=$(brew --prefix)"
[ -d "$ROOT/third_party/unicorn-2.1.4/install" ] && VARS="$VARS UC=$ROOT/third_party/unicorn-2.1.4/install"
# shellcheck disable=SC2086
make -C port/harness shadertool $VARS
(cd port/work/shader-cache && mkdir -p out && tools/build_cache.sh)
cp port/work/shader-cache/out/shaders.bin port/work/shader-cache/out/shaders.bin.gz port/harness/web/
ls -l port/harness/web/shaders.bin port/harness/web/shaders.bin.gz
