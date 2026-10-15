#!/bin/bash
# usage: run.sh <winedebug-channels> ; game mounted at /game
Xvfb :99 -screen 0 1280x720x24 >/dev/null 2>&1 &
sleep 2
[ -d /wine/drive_c ] || wineboot -i >/dev/null 2>&1
cd /game/BLUR && WINEDEBUG="${1:--all}" wine Blur.exe 2>&1
