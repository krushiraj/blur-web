#!/bin/bash
# headless learn loop: run the scripted walk, on "not recompiled" lift learned targets, relink, repeat (max N)
KEYS="$1"; SECS=${2:-400}; N=${3:-12}; LOG=${4:-/tmp/blur-learn_walk.log}
H=$(cd "$(dirname "$0")" && pwd); R=$H/../recomp; PY=${PY:-python3}
: > $LOG
for it in $(seq 1 $N); do
  cd $H; BLUR_SEEDACCT=${BLUR_SEEDACCT:-0} BLUR_KEYS="$KEYS" BLUR_RECOMP=1 BLUR_SECONDS=$SECS ./blurrt ../../game/extracted/BLUR/Blur.exe > /tmp/blur-lw_$it.txt 2>&1
  L=$(grep -a "not recompiled\|unsupported at" /tmp/blur-lw_$it.txt | head -1 | cut -c1-120); F=$(grep -a "frames=" /tmp/blur-lw_$it.txt | tail -1 | cut -c1-60)
  echo "iter $it: $F | $L" >> $LOG
  if [ -z "$L" ]; then echo "clean run" >> $LOG; break; fi
  cd $R; $PY lift_extra.py | tail -1 >> $LOG; clang -O1 -w -ffp-contract=off -fno-unwind-tables -fno-asynchronous-unwind-tables -I. -c gen_x/extra.c -o obj_x/extra.o; clang -O1 -w -I. -c gen_x/extra_table.c -o obj_x/extra_table.o
  cd $H; make blurrt OBJDIR=obj_p2 2>&1 | grep -a -i " error\|undefined" | head -3 >> $LOG
done
echo DONE >> $LOG
