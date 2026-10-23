#!/usr/bin/env bash
# Start Blur: runs the host runtime built by ./setup.sh and opens the game page in your browser.
#
#   ./play.sh [options]
#
#   --port N       web port for the page (default 8765; the next free port is used if it is taken)
#   --webgpu       open the page with #gpu=1 (WebGPU renderer; falls back to WebGL2 if unavailable or if the WGSL pack is missing)
#   --hud          show the frame-time HUD (#hud=1)
#   --lan          LAN multiplayer without a server: broadcast on UDP 47801 so other machines running this are found
#                  (game menu: Multiplayer -> Local Area Network). Allow incoming connections if your firewall asks.
#   --share        also serve the page to other computers on your network (BLUR_BIND=0.0.0.0). Anyone who can reach the
#                  port controls your game: only on a network you trust.
#   --name NAME    player name other players see (otherwise asked once, remembered in port/harness/.playername)
#   --browser APP  macOS: open the page with this application, e.g. "Google Chrome" (default: your default browser)
#   --no-open      only start the host and print the URL
#
# Chrome, Edge or another Chromium based browser is recommended. Click into the page once so the browser allows sound.
# Stop with Ctrl+C in this terminal.
set -eu
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"
die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

PORT=8765; OPEN=1; FRAG=""; LAN=0; SHARE=0; NAME=""; BROWSER=""
while [ $# -gt 0 ]; do
  case "$1" in
    --port) PORT="$2"; shift 2;;
    --webgpu) FRAG="${FRAG:+$FRAG&}gpu=1"; shift;;
    --hud) FRAG="${FRAG:+$FRAG&}hud=1"; shift;;
    --lan) LAN=1; shift;;
    --share) SHARE=1; shift;;
    --name) NAME="$2"; shift 2;;
    --browser) BROWSER="$2"; shift 2;;
    --no-open) OPEN=0; shift;;
    -h|--help) sed -n '2,/^set -eu/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 0;;
    *) die "unknown option $1 (see --help)";;
  esac
done

[ -x port/harness/blurrt ] || die "port/harness/blurrt is missing: run ./setup.sh /path/to/your/Blur first"
[ -f game/extracted/BLUR/Blur.exe ] || die "game/extracted/BLUR/Blur.exe is missing: run ./setup.sh /path/to/your/Blur first"
[ "$(cat .setup/mode 2>/dev/null || true)" = interp ] && echo "note: this build runs the game on the x86 interpreter only (setup --interp-only); expect a slideshow"

busy() { (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }
tries=0; while busy "$PORT" && [ $tries -lt 30 ]; do PORT=$((PORT+1)); tries=$((tries+1)); done
URL="http://127.0.0.1:$PORT/${FRAG:+#$FRAG}"

[ -z "$NAME" ] || export BLUR_PLAYER_NAME="$NAME"
[ "$LAN" = 0 ] || { export BLUR_NET_MESH=1; echo "LAN mode: UDP 47801. Menu: Multiplayer -> Local Area Network (host: = or PageUp; join: - or PageDown to refresh, Enter to join)."; }
if [ "$SHARE" = 1 ]; then
  export BLUR_BIND=0.0.0.0
  IP="$( (ipconfig getifaddr en0 || ipconfig getifaddr en1 || hostname -I | cut -d' ' -f1) 2>/dev/null || true)"
  echo "Sharing the page: other computers open http://${IP:-<this-computer-ip>}:$PORT/ (WebGPU there needs a secure context; WebGL2 works over plain http)."
fi

if [ "$OPEN" = 1 ]; then
  (
    for _ in $(seq 1 120); do busy "$PORT" && break; sleep 0.5; done
    if [ "$(uname -s)" = Darwin ]; then
      if [ -n "$BROWSER" ]; then open -a "$BROWSER" "$URL"; else open "$URL"; fi
    elif command -v xdg-open >/dev/null 2>&1; then xdg-open "$URL" >/dev/null 2>&1 || true
    else echo "open $URL in your browser"; fi
  ) &
fi
echo "Starting Blur... page: $URL"
exec port/harness/play.sh --port "$PORT"
