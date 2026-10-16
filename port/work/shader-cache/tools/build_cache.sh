#!/bin/sh
# Rebuild the offline shader cache from the game files.  Run from port/work/shader-cache.
#   ./tools/build_cache.sh [game dir] [oep image dump]
# 1. extract every D3D9 shader token stream from cache/shaders-speed.pak (zlib chunks of the 2KAP container)
# 2. scan the decrypted exe image dump for the shaders embedded in Blur.exe (UI/debug/post shaders)
# 3. translate all of them with harness/shadertool (vkd3d-shader + SPIRV-Cross, i.e. the same code as the live path)
set -e
ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
GAME=${1:-$ROOT/game/extracted/BLUR}
OEP=${2:-$ROOT/re/dump/blur_runtime.bin}
cd "$(dirname "$0")/.."
python3 tools/pakshaders.py "$GAME/cache/shaders-speed.pak" out/src/pak
python3 tools/pakshaders.py "$OEP" out/src/exe
# out/src/extra holds the few shaders that live in neither (Bink's ps_2_0 YUV shaders, captured from a real-Bink run); keep it if present
mkdir -p out/src/extra
$ROOT/port/harness/shadertool -o out/shaders.bin -q out/src/pak out/src/exe out/src/extra
gzip -9 -c out/shaders.bin > out/shaders.bin.gz
ls -l out/shaders.bin out/shaders.bin.gz
