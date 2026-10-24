#!/usr/bin/env bash
# blur-web setup: build the host runtime from YOUR copy of Blur.
#
#   ./setup.sh /path/to/Blur-install-folder-or-Blur.exe [options]
#
# Nothing of the game is shipped in this repository. This script recreates, on your machine, everything that is
# derived from Blur.exe: it boots the packed executable in an emulator, lifts the unpacked x86 code to C, compiles
# that C and links it with the host layer (Win32 / Direct3D 9 / audio / network) in port/harness. Run it once.
#
# options
#   --crt PATH        msvcr90.dll (9.0.21022.8, 32-bit) or Microsoft's vcredist_x86.exe (VC++ 2008) to take it from.
#                     Needed unless the game folder already contains msvcr90.dll or vcredist_x86.exe.
#   --jobs N          parallel jobs for lifting and compiling (default: number of CPUs)
#   --interp-only     skip the lift/compile stage; the game then runs on the built-in x86 interpreter only.
#                     Fast to set up, but far too slow to play (see README).
#   --shaders         also build the offline WebGL2 shader cache from the game's shader pak (optional, faster first draws)
#   --yes             install missing packages without asking (brew / apt)
#   --skip-deps       do not check or install packages
#   --force           ignore SHA-256 mismatches of Blur.exe / msvcr90.dll (lifting may then be incomplete or wrong)
#   -h, --help
set -eu

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"
STATE="$ROOT/.setup"
mkdir -p "$STATE"

# The build the address lists in port/recomp were made for.
BLUR_SHA256=7ef92b57bfd8741cf3de44225e3586c99d7365d7e212b86b3229fbb7d99deee9
CRT_SHA256=ed0170d3de86da33e02bfa1605eec8ff6010583481b1c530843867c1939d2185
VKD3D_VER=2.1
VKD3D_URL=https://dl.winehq.org/vkd3d/source/vkd3d-$VKD3D_VER.tar.xz
VKD3D_SHA256=7510146aff2adfb4ae07ab890701a607e5ff7c66e57100cfc9f630ec92eeda6a
UNICORN_VER=2.1.4

GAME_ARG=""; CRT_ARG=""; JOBS=""; INTERP_ONLY=0; SHADERS=0; YES=0; SKIP_DEPS=0; FORCE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --crt) CRT_ARG="$2"; shift 2;;
    --jobs) JOBS="$2"; shift 2;;
    --interp-only) INTERP_ONLY=1; shift;;
    --shaders) SHADERS=1; shift;;
    --yes) YES=1; shift;;
    --skip-deps) SKIP_DEPS=1; shift;;
    --force) FORCE=1; shift;;
    -h|--help) sed -n '2,/^set -eu/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 0;;
    -*) echo "unknown option $1 (see --help)" >&2; exit 2;;
    *) GAME_ARG="$1"; shift;;
  esac
done

say()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die()  { printf '\n\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }
sha256() { if have shasum; then shasum -a 256 "$1" | cut -d' ' -f1; else sha256sum "$1" | cut -d' ' -f1; fi; }
stamp() { [ -f "$STATE/$1.done" ]; }
mark()  { date +%s > "$STATE/$1.done"; }
since() { echo $(( $(date +%s) - $1 ))s; }

OS="$(uname -s)"; ARCH="$(uname -m)"
case "$OS" in
  Darwin) NCPU="$(sysctl -n hw.ncpu)";;
  Linux)  NCPU="$(nproc)";;
  *) die "unsupported system $OS. On Windows use WSL2 (Ubuntu) and run this script inside it; see README.";;
esac
[ -n "$JOBS" ] || JOBS="$NCPU"
# Homebrew is often installed but not on PATH in a fresh shell or an ssh session; pick it up before looking for tools
if ! have brew; then
  for b in /opt/homebrew/bin/brew /usr/local/bin/brew /home/linuxbrew/.linuxbrew/bin/brew; do
    [ -x "$b" ] && { eval "$("$b" shellenv)"; break; }
  done
fi

# --------------------------------------------------------------------------------------------------- 1. the game
say "1/9 locating Blur.exe"
[ -n "$GAME_ARG" ] || { sed -n '2,5p' "$0" | sed 's/^# \{0,1\}//'; die "give the path to your Blur install folder or Blur.exe"; }
EXE=""
if [ -f "$GAME_ARG" ]; then EXE="$GAME_ARG"
elif [ -d "$GAME_ARG" ]; then
  EXE="$(find "$GAME_ARG" -maxdepth 3 -iname 'blur.exe' -type f 2>/dev/null | head -n 1)"
fi
[ -n "$EXE" ] && [ -f "$EXE" ] || die "no Blur.exe found under '$GAME_ARG'"
GAME_DIR="$(cd "$(dirname "$EXE")" && pwd)"
EXE="$GAME_DIR/$(basename "$EXE")"
note "game folder: $GAME_DIR"
GOT="$(sha256 "$EXE")"
if [ "$GOT" != "$BLUR_SHA256" ]; then
  echo "    SHA-256 of Blur.exe is $GOT"
  echo "    expected             $BLUR_SHA256  (the retail build the address lists were made for)"
  [ "$FORCE" = 1 ] || die "different build of Blur.exe. Lifting would be incomplete or wrong. Use --force to try anyway."
else
  note "Blur.exe matches the supported build"
fi
[ -d "$GAME_DIR/cache" ] || note "warning: no 'cache' folder next to Blur.exe; is this the full install?"

# the harness and all helper scripts look for the game at game/extracted/BLUR
mkdir -p "$ROOT/game/extracted"
LINK="$ROOT/game/extracted/BLUR"
if [ -L "$LINK" ]; then
  rm -f "$LINK"
elif [ -d "$LINK" ]; then
  if [ -f "$LINK/.blur-web-links" ]; then rm -rf "$LINK"
  elif [ "$(cd "$LINK" && pwd -P)" = "$(cd "$GAME_DIR" && pwd -P)" ]; then LINK=""      # the game was copied there by hand: use it in place
  else die "$LINK already exists and is a real folder; remove it, or run setup with that folder as the game path"; fi
fi
if [ -n "$LINK" ]; then
  if [ "$(basename "$EXE")" = "Blur.exe" ]; then
    ln -s "$GAME_DIR" "$LINK"
  else                                   # file called blur.exe / BLUR.EXE: make a folder of links with the expected name
    mkdir -p "$LINK"; : > "$LINK/.blur-web-links"
    for f in "$GAME_DIR"/*; do [ -e "$f" ] && ln -s "$f" "$LINK/$(basename "$f")"; done
    rm -f "$LINK/$(basename "$EXE")"; ln -s "$EXE" "$LINK/Blur.exe"
  fi
  note "linked game/extracted/BLUR -> $GAME_DIR"
else
  note "using the game in place at game/extracted/BLUR"
fi
echo "$EXE" > "$STATE/game.path"

# --------------------------------------------------------------------------------------------------- 2. packages
say "2/9 build dependencies"
BREW_PREFIX=""
install_hint=""
if [ "$SKIP_DEPS" = 1 ]; then
  note "skipped (--skip-deps)"
elif [ "$OS" = Darwin ]; then
  have clang || die "no compiler: run 'xcode-select --install'"
  have brew  || die "Homebrew is needed for libraries: https://brew.sh"
  BREW_PREFIX="$(brew --prefix)"
  need=""
  for p in unicorn ffmpeg spirv-cross vulkan-headers vulkan-loader spirv-headers bison; do brew list --versions "$p" >/dev/null 2>&1 || need="$need $p"; done
  have pkg-config || need="$need pkgconf"
  have 7zz || have 7z || have 7za || need="$need sevenzip"
  if [ -n "$need" ]; then
    echo "    missing Homebrew packages:$need"
    if [ "$YES" = 1 ] || { printf '    install them now with brew? [Y/n] '; read -r a </dev/tty || a=y; case "$a" in n|N) false;; *) true;; esac; }; then
      # shellcheck disable=SC2086
      brew install $need
    else die "install$need and run setup again"; fi
  fi
  note "unicorn $(pkg-config --modversion unicorn 2>/dev/null || echo ?), ffmpeg, spirv-cross present"
elif [ "$OS" = Linux ]; then
  have apt-get || note "this script only knows apt; install clang, lld, make, cmake, pkg-config, git, curl, xz, python3-venv, libavformat/libavcodec/libavutil/libswresample dev and SPIRV-Cross (c-shared) yourself"
  if have apt-get; then
    need=""
    for p in clang lld make cmake pkg-config git curl xz-utils python3-venv python3-pip libavformat-dev libavcodec-dev libavutil-dev libswresample-dev libspirv-cross-c-shared-dev libvulkan-dev spirv-headers bison flex p7zip-full; do
      dpkg -s "$p" >/dev/null 2>&1 || need="$need $p"
    done
    if [ -n "$need" ]; then
      echo "    missing packages:$need"
      SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo
      if [ "$YES" = 1 ] || { printf '    install them now with apt? [Y/n] '; read -r a </dev/tty || a=y; case "$a" in n|N) false;; *) true;; esac; }; then
        $SUDO apt-get update && $SUDO apt-get install -y $need
      else die "install$need and run setup again"; fi
    fi
  fi
  note "Linux is best effort: see README for what was and was not tested"
fi
have make || die "make not found"
have curl || die "curl not found"
have python3 || die "python3 not found"
[ -n "$BREW_PREFIX" ] || { have brew && BREW_PREFIX="$(brew --prefix 2>/dev/null || true)"; }

# --------------------------------------------------------------------------------------------------- 3. msvcr90.dll
say "3/9 msvcr90.dll"
mkdir -p "$ROOT/port/vendor"
CRT_OUT="$ROOT/port/vendor/msvcr90.dll"
extract_crt_from_redist() {          # $1 = vcredist_x86.exe (VC++ 2008, RTM or later service pack that still ships 9.0.21022.8)
  local tmp; tmp="$(mktemp -d)"
  local sz=""; have 7zz && sz=7zz; [ -z "$sz" ] && have 7z && sz=7z; [ -z "$sz" ] && have 7za && sz=7za
  [ -n "$sz" ] || { rm -rf "$tmp"; return 1; }
  "$sz" x -y -o"$tmp" "$1" >/dev/null 2>&1 || true
  [ -f "$tmp/vc_red.cab" ] && "$sz" x -y -o"$tmp/cab" "$tmp/vc_red.cab" >/dev/null 2>&1 || true
  local f
  for f in "$tmp"/cab/nosxs_msvcr90.dll "$tmp"/cab/msvcr90.dll*; do
    [ -f "$f" ] || continue
    if [ "$(sha256 "$f")" = "$CRT_SHA256" ]; then cp "$f" "$CRT_OUT"; rm -rf "$tmp"; return 0; fi
  done
  rm -rf "$tmp"; return 1
}
if [ -f "$CRT_OUT" ] && [ "$(sha256 "$CRT_OUT")" = "$CRT_SHA256" ]; then
  note "already in place: port/vendor/msvcr90.dll"
else
  rm -f "$CRT_OUT"
  CANDS=""
  [ -n "$CRT_ARG" ] && CANDS="$CRT_ARG"
  CANDS="$CANDS $GAME_DIR/msvcr90.dll $GAME_DIR/../msvcr90.dll"
  for c in $CANDS; do
    [ -f "$c" ] || continue
    case "$(basename "$c" | tr 'A-Z' 'a-z')" in
      msvcr90.dll) if [ "$(sha256 "$c")" = "$CRT_SHA256" ] || [ "$FORCE" = 1 ]; then cp "$c" "$CRT_OUT"; break; else note "$c is a different version of msvcr90.dll; skipped"; fi;;
      *.exe) extract_crt_from_redist "$c" && break || note "could not extract msvcr90.dll 9.0.21022.8 from $c (needs 7-Zip: brew install sevenzip / apt install p7zip-full)";;
    esac
  done
  if [ ! -f "$CRT_OUT" ]; then
    for c in $(find "$GAME_DIR" -maxdepth 3 -iname 'vcredist_x86.exe' -type f 2>/dev/null | head -n 3); do
      extract_crt_from_redist "$c" && break
    done
  fi
  [ -f "$CRT_OUT" ] || die "msvcr90.dll (9.0.21022.8, x86) not found.
    The game links against Microsoft's Visual C++ 2008 runtime, which is not part of this repository.
    Get 'Microsoft Visual C++ 2008 Redistributable Package (x86)' from Microsoft (it is also on many Blur discs and
    in C:\\Windows\\WinSxS on a Windows PC) and pass it: ./setup.sh '$GAME_ARG' --crt /path/to/vcredist_x86.exe
    (or /path/to/msvcr90.dll). Expected SHA-256 of the DLL: $CRT_SHA256"
  note "msvcr90.dll ready (SHA-256 $(sha256 "$CRT_OUT" | cut -c1-16)...)"
fi

# --------------------------------------------------------------------------------------------------- 4. Unicorn (Linux) and vkd3d-shader
say "4/9 vkd3d-shader $VKD3D_VER (D3D9 shader bytecode to SPIR-V) and Unicorn"
UC_ARG=""
if [ "$OS" = Linux ]; then
  # Debian/Ubuntu ship Unicorn 1.x with a different default CPU. The packer in Blur.exe derives jump targets from CPUID, so the
  # emulator must be 2.x on every platform. Build it locally.
  if [ ! -f "$ROOT/third_party/unicorn-$UNICORN_VER/install/lib/libunicorn.a" ]; then
    have cmake || die "cmake is needed to build Unicorn $UNICORN_VER"
    mkdir -p "$ROOT/third_party"
    if [ ! -d "$ROOT/third_party/unicorn-$UNICORN_VER/src" ]; then
      git clone -q --depth 1 -b "$UNICORN_VER" https://github.com/unicorn-engine/unicorn "$ROOT/third_party/unicorn-$UNICORN_VER/src"
    fi
    ( cd "$ROOT/third_party/unicorn-$UNICORN_VER" &&
      cmake -S src -B b -DCMAKE_BUILD_TYPE=Release -DUNICORN_ARCH=x86 -DUNICORN_BUILD_SHARED=OFF -DCMAKE_C_COMPILER=clang -DCMAKE_INSTALL_PREFIX="$PWD/install" >/dev/null &&
      cmake --build b -j"$JOBS" >/dev/null && cmake --install b >/dev/null )
  fi
  UC_ARG="UC=$ROOT/third_party/unicorn-$UNICORN_VER/install"
  note "unicorn $UNICORN_VER (static) at third_party/unicorn-$UNICORN_VER/install"
fi
V="$ROOT/third_party/vkd3d-$VKD3D_VER"
if [ -f "$V/.libs/libvkd3d-shader.a" ]; then
  note "vkd3d-shader already built"
else
  mkdir -p "$ROOT/third_party"
  TARBALL="$ROOT/third_party/vkd3d-$VKD3D_VER.tar.xz"
  [ -f "$TARBALL" ] || { note "downloading $VKD3D_URL"; curl -fL --retry 3 -o "$TARBALL" "$VKD3D_URL"; }
  [ "$(sha256 "$TARBALL")" = "$VKD3D_SHA256" ] || { rm -f "$TARBALL"; die "vkd3d tarball checksum mismatch (deleted); run setup again"; }
  rm -rf "$V"; tar -xf "$TARBALL" -C "$ROOT/third_party"
  # macOS ships bison 2.3; the vkd3d release tarball needs 3.x (Homebrew's keg-only bison)
  [ -n "$BREW_PREFIX" ] && [ -x "$BREW_PREFIX/opt/bison/bin/bison" ] && PATH="$BREW_PREFIX/opt/bison/bin:$PATH"
  CPP_EXTRA=""; LD_EXTRA=""; [ -n "$BREW_PREFIX" ] && { CPP_EXTRA="-I$BREW_PREFIX/include"; LD_EXTRA="-L$BREW_PREFIX/lib"; }
  ( cd "$V" && CC=clang CPPFLAGS="$CPP_EXTRA" LDFLAGS="$LD_EXTRA" ./configure --disable-doxygen-doc --enable-static --disable-shared >"$STATE/vkd3d-configure.log" 2>&1 &&
    make -j"$JOBS" >"$STATE/vkd3d-build.log" 2>&1 ) || die "vkd3d build failed; see .setup/vkd3d-*.log"
  [ -f "$V/.libs/libvkd3d-shader.a" ] || die "vkd3d-shader library missing after build; see .setup/vkd3d-build.log"
  note "built vkd3d-shader (static)"
fi

# --------------------------------------------------------------------------------------------------- 5. python
say "5/9 python environment (Capstone, used by the lifter)"
if [ ! -x "$ROOT/.venv/bin/python" ]; then python3 -m venv "$ROOT/.venv"; fi
PY="$ROOT/.venv/bin/python"
"$PY" -c 'import capstone' 2>/dev/null || "$PY" -m pip install -q --disable-pip-version-check capstone
note "capstone $("$PY" -c 'import capstone;print(capstone.__version__)')"

# --------------------------------------------------------------------------------------------------- 6. bootstrap runtime
say "6/9 bootstrap runtime (host layer + interpreter, no lifted code yet)"
MAKE_VARS="V=$V"
[ -n "$BREW_PREFIX" ] && MAKE_VARS="$MAKE_VARS BREW=$BREW_PREFIX"
[ -n "$UC_ARG" ] && MAKE_VARS="$MAKE_VARS $UC_ARG"
EXTRA_OPT=""; [ "$OS" = Linux ] && EXTRA_OPT="-fsigned-char"
mkdir -p "$ROOT/port/recomp/obj_boot" "$ROOT/port/recomp/obj_xboot"
clang -O1 -w -c "$ROOT/port/recomp/stub_table.c" -o "$ROOT/port/recomp/obj_boot/stub_table.o"
t0=$(date +%s)
# shellcheck disable=SC2086
( cd "$ROOT/port/harness" && make -B blurrt OBJDIR=obj_boot XOBJDIR=obj_xboot $MAKE_VARS >"$STATE/boot-build.log" 2>&1 ) || { tail -n 30 "$STATE/boot-build.log"; die "bootstrap build failed (full log: .setup/boot-build.log)"; }
mv -f "$ROOT/port/harness/blurrt" "$ROOT/port/harness/blurrt_interp"
note "built port/harness/blurrt_interp in $(since $t0)"

# --------------------------------------------------------------------------------------------------- 7. images for the lifter
say "7/9 dumping the unpacked game image for the lifter"
mkdir -p "$ROOT/re/dump"
(
  cd "$ROOT/port/harness"
  # msvcr90 right after load
  BLUR_DUMP_MSV="$ROOT/re/dump/msvcr90.bin" BLUR_RECOMP=1 ./blurrt_interp ../../game/extracted/BLUR/Blur.exe >"$STATE/dump-msv.log" 2>&1 || true
  # The unpacked exe a few seconds into the run. Not at the entry point: the protector writes small pieces of code and tables
  # into its section right after it, and the lifter must see them.
  BLUR_DUMP_EXE="$ROOT/re/dump/blur_runtime.bin" BLUR_SECONDS=4 BLUR_NOWAIT=1 BLUR_GFXFAKE=1 BLUR_PAR=1 BLUR_RECOMP=1 \
    ./blurrt_interp ../../game/extracted/BLUR/Blur.exe >"$STATE/dump-exe.log" 2>&1 || true
)
[ -s "$ROOT/re/dump/msvcr90.bin" ] || { tail -n 15 "$STATE/dump-msv.log"; die "could not dump msvcr90 image (see .setup/dump-msv.log)"; }
[ -s "$ROOT/re/dump/blur_runtime.bin" ] || { tail -n 15 "$STATE/dump-exe.log"; die "could not dump the game image (see .setup/dump-exe.log). Is the CPU/emulator OK? See README, Troubleshooting."; }
note "re/dump/blur_runtime.bin $(wc -c < "$ROOT/re/dump/blur_runtime.bin" | tr -d ' ') bytes, re/dump/msvcr90.bin $(wc -c < "$ROOT/re/dump/msvcr90.bin" | tr -d ' ') bytes"

if [ "$INTERP_ONLY" = 1 ]; then
  cp -f "$ROOT/port/harness/blurrt_interp" "$ROOT/port/harness/blurrt"
  cp -f "$ROOT/port/harness/blurrt_interp" "$ROOT/port/harness/blurrt_coop"
  echo interp > "$STATE/mode"
  say "interpreter-only runtime installed. Start it with ./play.sh (expect a slideshow; see README)"
  mark setup; exit 0
fi

# --------------------------------------------------------------------------------------------------- 8. lift + compile
say "8/9 lifting the game to C ($JOBS jobs) and compiling it"
GEN=gen_p2; OBJ=obj_p2_nb
RC="$ROOT/port/recomp"
t0=$(date +%s)
if stamp lift; then note "lift: done earlier (delete .setup/lift.done to redo)"; else
  ( cd "$RC" && LIFT_IMG="$ROOT/re/dump/blur_runtime.bin" BLOCKS=blocks.txt "$PY" lift_par.py $GEN >"$STATE/lift.log" 2>&1 &&
    LIFT_IMG="$ROOT/re/dump/blur_runtime.bin" "$PY" lift_extra.py >"$STATE/lift-extra.log" 2>&1 ) || { tail -n 20 "$STATE/lift.log"; die "lifting failed (see .setup/lift*.log)"; }
  mark lift
  note "lifted: $(grep -E '^files ' "$STATE/lift.log" | tail -n 1) / $(tail -n 1 "$STATE/lift-extra.log") [$(since $t0)]"
fi
t0=$(date +%s)
COPT="-O1 -fno-strict-aliasing -DBLUR_G_FIXED=0x7000000000ull $EXTRA_OPT"
SED_PRE="sed -E -e 's/^void (f_[0-9a-f]+)\\(Cpu \\*c\\) \\{/void \\1(Cpu *restrict c) {/' -e '/__builtin_expect\\(rt_trace, 0\\)/d' -e '/__builtin_expect\\(--c->budget < 0, 0\\)/d'"
if stamp compile; then note "compile: done earlier (delete .setup/compile.done to redo)"; else
  ( cd "$RC" && nice -n 10 make -j"$JOBS" GEN=$GEN OBJDIR=$OBJ OPT="$COPT" PRE="$SED_PRE" >"$STATE/compile.log" 2>&1 ) || { tail -n 20 "$STATE/compile.log"; die "compiling the lifted code failed (see .setup/compile.log)"; }
  mkdir -p "$RC/obj_x"
  for f in extra extra_table; do
    ( cd "$RC" && sed -E -e 's/^void (f_[0-9a-f]+)\(Cpu \*c\) \{/void \1(Cpu *restrict c) {/' -e '/__builtin_expect\(rt_trace, 0\)/d' -e '/__builtin_expect\(--c->budget < 0, 0\)/d' gen_x/$f.c |
      clang $COPT -w -ffp-contract=off -fno-unwind-tables -fno-asynchronous-unwind-tables -I. -x c -c - -o obj_x/$f.o ) || die "compiling gen_x/$f.c failed"
  done
  mark compile
  note "compiled in $(since $t0)"
fi
say "9/9 linking the fast runtime"
# shellcheck disable=SC2086
( cd "$ROOT/port/harness" && make -B blurrt OBJDIR=$OBJ XOBJDIR=obj_x EXTRA="-DBLUR_G_CHECK -DBLUR_NO_BUDGET" $MAKE_VARS >"$STATE/link.log" 2>&1 ) || { tail -n 20 "$STATE/link.log"; die "link failed (see .setup/link.log)"; }
note "built port/harness/blurrt"
echo lifted > "$STATE/mode"

if [ "$SHADERS" = 1 ]; then
  say "optional: offline shader cache"
  "$ROOT/tools/build_shader_cache.sh" || note "shader cache build failed; the page still works (it translates shaders live)"
fi
mark setup
cat <<EOF

Done. Start the game with:

    ./play.sh

It prints a URL (default http://127.0.0.1:8765/) and opens it in your browser. See README.md for controls and options.
EOF
