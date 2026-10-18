#!/bin/bash
# mp_local.sh [N]  -- N (default 2) game instances on this machine on one virtual LAN, for testing multiplayer.
# Starts the UDP relay (port 47800) if needed, then instance k on http://127.0.0.1:887<k>/ with virtual IP 10.77.0.<k>, its own save folder (save_p<k>) and network log.
# Open each URL in its own browser tab. In the game: Multiplayer -> Local area network; Right Ctrl = Host Game, Right Shift = Refresh, Enter = Join Game, F1 = Start Game (host).
cd "$(dirname "$0")"; N=${1:-2}
pgrep -f lan_relay.js >/dev/null || (nohup node ../work/net-re/relay/lan_relay.js --port 47800 > /tmp/blur_relay.log 2>&1 &)
for k in $(seq 1 $N); do
  [ -d save_p$k ] || cp -R save save_p$k
  (BLUR_PAR=1 BLUR_RECOMP=1 BLUR_NET_ID=$k BLUR_NETLOG=1 BLUR_SAVEDIR=./save_p$k BLUR_SEEDACCT=0 BLUR_WEB=887$k \
     BLUR_KEYS="${BLUR_KEYS-Enter@L:StopScoreboardRequests+400,Down@L:ReadOnlineAccountDetails+700,Down@L:ReadOnlineAccountDetails+730,Enter@L:ReadOnlineAccountDetails+800}" \
     nohup ./blurrt ../../game/extracted/BLUR/Blur.exe > /tmp/blur_mp$k.log 2>&1 &)
  echo "instance $k: http://127.0.0.1:887$k/  (log /tmp/blur_mp$k.log)"
done
