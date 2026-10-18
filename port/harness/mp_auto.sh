#!/bin/bash
# mp_auto.sh  -- two instances (host = 2, joiner = 1) that walk the menus by themselves: Multiplayer -> Local area network; host: Host Game -> Skirmish Racing -> F1 (start);
# joiner: refresh the list and join. Timed by game frames after Lua events. Screenshots: node ../../tools/shot/shot.js http://127.0.0.1:8871/ <secs> out1.png ...
cd "$(dirname "$0")"
PRE="Enter@L:StopScoreboardRequests+400,Down@L:ReadOnlineAccountDetails+700,Down@L:ReadOnlineAccountDetails+730,Enter@L:ReadOnlineAccountDetails+800,Enter@L:ReadOnlineAccountDetails+1500~90,Right@L:ReadOnlineAccountDetails+1800~90,Enter@L:ReadOnlineAccountDetails+1900~90,Left@L:ReadOnlineAccountDetails+2300~90,Enter@L:ReadOnlineAccountDetails+2400~90,Enter@L:ReadOnlineAccountDetails+2700~90"
HOST="$PRE,RCtrl@L:ReadOnlineAccountDetails+2820~90,Enter@L:ReadOnlineAccountDetails+2940~90,Enter@L:ReadOnlineAccountDetails+3060~90,F1@L:ReadOnlineAccountDetails+5300~90"
JOIN="$PRE,RShift@L:ReadOnlineAccountDetails+3700~90,Enter@L:ReadOnlineAccountDetails+3900~90"
pgrep -f lan_relay.js >/dev/null || (nohup node ../work/net-re/relay/lan_relay.js --port 47800 > /tmp/blur_relay.log 2>&1 &)
for k in 1 2; do rm -rf save_p$k; cp -R save save_p$k; done
(BLUR_PAR=1 BLUR_RECOMP=1 BLUR_NET_ID=2 BLUR_NETLOG=1 BLUR_SAVEDIR=./save_p2 BLUR_SEEDACCT=0 BLUR_WEB=8873 BLUR_KEYS="$HOST" BLUR_FPSLOG=1 nohup ./blurrt ../../game/extracted/BLUR/Blur.exe > /tmp/blur_mp2.log 2>&1 &)
(BLUR_PAR=1 BLUR_RECOMP=1 BLUR_NET_ID=1 BLUR_NETLOG=1 BLUR_SAVEDIR=./save_p1 BLUR_SEEDACCT=0 BLUR_WEB=8871 BLUR_KEYS="$JOIN" BLUR_FPSLOG=1 nohup ./blurrt ../../game/extracted/BLUR/Blur.exe > /tmp/blur_mp1.log 2>&1 &)
echo "host http://127.0.0.1:8873/   joiner http://127.0.0.1:8871/"
