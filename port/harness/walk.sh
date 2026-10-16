#!/bin/bash
# usage: walk.sh "<BLUR_KEYS>" <seconds> <outprefix> [port]   -- runs the game with scripted keys on a private port, takes screenshots across the run,
# and on "jump to code that was not recompiled" lifts the learned targets, relinks and reruns (max 4 times).
KEYS="$1"; SECS=${2:-150}; OUT=${3:-/tmp/blur-shots3/walk}; PORT=${4:-8779}
H=$(cd "$(dirname "$0")" && pwd); R=$H/../recomp; PY=${PY:-python3}
for try in 1 2 3 4; do
  cd $H; (BLUR_KEYS="$KEYS" BLUR_COVER=${COVER:-} BLUR_WEB=$PORT BLUR_RECOMP=1 BLUR_SECONDS=$SECS ./blurrt ../../game/extracted/BLUR/Blur.exe > $OUT.log 2>&1 &)
  sleep 3; cd $H/../../tools/shot
  N=${SHOTS:-6}; FILES=""; for i in $(seq 1 $N); do FILES="$FILES $OUT.$i.png"; done
  node shot.js http://127.0.0.1:$PORT/ $((SECS-10)) $FILES > $OUT.shot.txt 2>&1
  while pgrep -f "blurrt ../../game" >/dev/null; do sleep 3; done
  if grep -a -q "not recompiled" $OUT.log; then echo "try $try: missing target, lifting"; cd $R; $PY lift_extra.py | tail -1; clang -O1 -w -ffp-contract=off -fno-unwind-tables -fno-asynchronous-unwind-tables -I. -c gen_x/extra.c -o obj_x/extra.o; clang -O1 -w -I. -c gen_x/extra_table.c -o obj_x/extra_table.o; cd $H; make blurrt OBJDIR=obj_p2 2>&1 | grep -a -i " error\|undefined" | head -3
  else echo "try $try: finished"; break; fi
done
grep -a "exit\|frames=\|not recomp" $OUT.log | tail -3
