#!/usr/bin/env bash
# Build the WGSL shader pack the WebGPU renderer needs (port/harness/web/blur.wgslpack.gz) from YOUR game files.
# Without it the page falls back to WebGL2 by itself. The pack is derived from the game's shaders: git-ignored, keep it private.
#
#   tools/build_wgsl_pack.sh          (run ./setup.sh first; installs naga-cli 30.0.1 with cargo when it is missing)
#
# 1. makes the shader corpus (same as tools/build_shader_cache.sh) when it does not exist yet
# 2. runs port/work/webgpu/tools/make_wgsl_pack.sh (vkd3d -> SPIR-V -> naga -> WGSL, deterministic)
# 3. copies the gzipped pack next to the page
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
have() { command -v "$1" >/dev/null 2>&1; }
if ! have brew; then for b in /opt/homebrew/bin/brew /usr/local/bin/brew; do [ -x "$b" ] && { eval "$("$b" shellenv)"; break; }; done; fi
[ -f re/dump/blur_runtime.bin ] || { echo "run ./setup.sh first (re/dump/blur_runtime.bin is missing)" >&2; exit 1; }
if have brew; then
  need=""; for p in spirv-tools brotli; do brew list --versions "$p" >/dev/null 2>&1 || need="$need $p"; done
  [ -z "$need" ] || { echo "installing$need"; brew install $need; }
elif have apt-get; then
  have spirv-opt && have brotli || { SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo; $SUDO apt-get install -y spirv-tools brotli; }
fi
TOOLS="$ROOT/.setup/tools"; mkdir -p "$TOOLS"
if [ -x "$TOOLS/bin/naga" ]; then NAGA="$TOOLS/bin/naga"
elif have naga && [ "$(naga --version)" = "30.0.1" ]; then NAGA="$(command -v naga)"
else
  have cargo || { have brew && { echo "installing rust (needed once, to build naga-cli)"; brew install rust; }; }
  have cargo || { echo "cargo not found: install Rust (https://rustup.rs) and run this again" >&2; exit 1; }
  echo "building naga-cli 30.0.1 (a few minutes, once)"
  cargo install naga-cli --version 30.0.1 --locked --root "$TOOLS" >"$ROOT/.setup/naga-build.log" 2>&1 || { tail -n 15 "$ROOT/.setup/naga-build.log"; exit 1; }
  NAGA="$TOOLS/bin/naga"
fi
export NAGA
# setup.sh builds vkd3d static, which leaves the compiler binary at the top of the tree; the pack tools look in .libs/
V="$ROOT/third_party/vkd3d-2.1"
if [ ! -x "$V/.libs/vkd3d-compiler" ] && [ -x "$V/vkd3d-compiler" ] && ! head -c 2 "$V/vkd3d-compiler" | grep -q '^#!'; then
  mkdir -p "$V/.libs" && cp "$V/vkd3d-compiler" "$V/.libs/vkd3d-compiler"
fi
if ! ls -d port/work/shader-cache/o*t/src >/dev/null 2>&1 && [ ! -d port/work/shader-cache/out/src ]; then tools/build_shader_cache.sh; fi
[ -d port/work/shader-cache/out/src ] && export CORPUS="$ROOT/port/work/shader-cache/out/src"
JOBS="$( (sysctl -n hw.ncpu 2>/dev/null || nproc) )" port/work/webgpu/tools/make_wgsl_pack.sh
cp port/work/webgpu/pack/blur.wgslpack.gz port/harness/web/blur.wgslpack.gz
ls -l port/harness/web/blur.wgslpack.gz
echo "done: ./play.sh now uses the WebGPU renderer (it falls back to WebGL2 if the browser has no WebGPU)"
