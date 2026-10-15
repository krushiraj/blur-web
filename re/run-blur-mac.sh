#!/bin/bash
# Launch Blur under the free macOS Wine build. Usage: re/run-blur-mac.sh [WINEDEBUG channels]
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export WINEPREFIX="$HOME/tools/wine/prefix-blur" WINEDEBUG="${1:-err+all,fixme-all}"
cd "$ROOT/game/extracted/BLUR" && exec "$HOME/tools/wine/Wine Devel.app/Contents/Resources/wine/bin/wine" Blur.exe
