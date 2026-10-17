#!/bin/bash
# play.sh [--port N | port]  -- run the current build against ../../game/extracted/BLUR on the given port (default 8765).
# Open http://127.0.0.1:<port>/ in Chrome; hard-reload the page (Cmd+Shift+R) after a rebuild; add #hud=1 to the URL for the frame-time HUD.
PORT=8765; while [ $# -gt 0 ]; do case "$1" in --port) PORT="$2"; shift 2;; --port=*) PORT="${1#--port=}"; shift;; -h|--help) echo "usage: play.sh [--port N]"; exit 0;; *) PORT="$1"; shift;; esac; done
cd "$(dirname "$0")" && BLUR_PAR=${BLUR_PAR:-1} BLUR_WEB=$PORT BLUR_RECOMP=1 exec ./blurrt ../../game/extracted/BLUR/Blur.exe
