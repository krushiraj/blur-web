#!/bin/bash
# play.sh [--port N | port]  -- run the current build against ../../game/extracted/BLUR on the given port (default 8765), then open http://127.0.0.1:<port>/ in Chrome.
# Hard-reload the page (Cmd+Shift+R) after a rebuild; add #hud=1 to the URL for the frame-time HUD.
PORT=8765; while [ $# -gt 0 ]; do case "$1" in --port) PORT="$2"; shift 2;; --port=*) PORT="${1#--port=}"; shift;; -h|--help) echo "usage: play.sh [--port N]"; exit 0;; *) PORT="$1"; shift;; esac; done
cd "$(dirname "$0")" && BIN=./blurrt; [ "${BLUR_PAR:-1}" = "0" ] && BIN=./blurrt_coop      # blurrt = fast build (parallel threads only); blurrt_coop = single-threaded scheduler
# player name: asked once in the terminal (default = your first name / user name), remembered in .playername; BLUR_PLAYER_NAME overrides
if [ -z "$BLUR_PLAYER_NAME" ]; then
  if [ ! -s .playername ]; then
    DEF="$(id -F 2>/dev/null | awk '{print $1}')"; [ -n "$DEF" ] || DEF="$USER"
    if [ -t 0 ]; then read -r -p "What name should other players see? [$DEF] " NAME; fi
    NAME="$(printf '%s' "${NAME:-$DEF}" | tr -cd 'A-Za-z0-9 _-' | cut -c1-15)"; [ -n "$NAME" ] || NAME=player
    printf '%s' "$NAME" > .playername
  fi
  export BLUR_PLAYER_NAME="$(cat .playername)"
fi
# skip the first-run "Online Account" dialog: title Enter, then Down + Enter (= Skip). BLUR_NOSKIP=1 leaves the dialog alone; BLUR_KEYS overrides.
if [ -z "$BLUR_NOSKIP" ] && [ -z "$BLUR_KEYS" ]; then export BLUR_SEEDACCT=clear BLUR_KEYS="Enter@L:StopScoreboardRequests+6s,Down@L:ReadOnlineAccountDetails+8s,Enter@L:ReadOnlineAccountDetails+10s"; fi
BLUR_PAR=${BLUR_PAR:-1} BLUR_WEB=$PORT BLUR_RECOMP=1 exec $BIN ../../game/extracted/BLUR/Blur.exe
