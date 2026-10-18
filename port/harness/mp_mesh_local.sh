#!/bin/bash
# mp_mesh_local.sh [N]  -- N (default 2) game instances on THIS machine using the serverless LAN mesh (no relay), to test LAN discovery.
# Instance k: http://127.0.0.1:889<k>/ , own save folder (save_mesh<k>), name Racer<k>. Open each URL in its own browser tab (or window).
# Menu: Multiplayer -> local area network.  Host: = (or PageUp)   Refresh: - (or PageDown)   Join: Enter   Start: F1.   Stop everything: ./mp_mesh_local.sh stop
cd "$(dirname "$0")"
if [ "$1" = "stop" ]; then pkill -f "blurrt ../../game/extracted/BLUR/Blur.exe" ; echo stopped; exit 0; fi
N=${1:-2}
for k in $(seq 1 $N); do
  lsof -iTCP:889$k -sTCP:LISTEN >/dev/null 2>&1 && { echo "port 889$k already in use - run ./mp_mesh_local.sh stop first"; exit 1; }
  mkdir -p save_mesh$k
  (BLUR_PLAYER_NAME=Racer$k BLUR_NET_MESH=1 BLUR_NET_MESH_SLOT=$k BLUR_PAR=1 BLUR_RECOMP=1 BLUR_WEB=889$k BLUR_SEEDACCT=0 BLUR_SAVEDIR=./save_mesh$k \
     nohup ./blurrt ../../game/extracted/BLUR/Blur.exe > /tmp/blur_mesh$k.log 2>&1 &)
  echo "instance $k: http://127.0.0.1:889$k/   (log /tmp/blur_mesh$k.log)"
done
