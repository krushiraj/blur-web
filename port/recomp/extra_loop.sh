#!/bin/bash
# Fast learn loop: run the recompiled game; each code address it reaches that was not recompiled is appended to learned_x.txt by the runtime;
# lift just those (lift_extra.py), compile the small extra file, relink, run again.
set -u
R=$(cd "$(dirname "$0")" && pwd)
H=$R/../harness
PY=${PY:-python3}
LOG=/tmp/blur-extra.log
MAXIT=${1:-60}
: > $LOG
mkdir -p $R/obj_x
rebuild() {
  cd $R && $PY lift_extra.py >> $LOG 2>&1
  clang -O1 -fno-strict-aliasing -w -ffp-contract=off -fno-unwind-tables -fno-asynchronous-unwind-tables -I. -c gen_x/extra.c -o obj_x/extra.o >> $LOG 2>&1
  clang -O1 -w -I. -c gen_x/extra_table.c -o obj_x/extra_table.o >> $LOG 2>&1
  cd $H && make blurrt OBJDIR=obj_p2 > /tmp/blur-link.log 2>&1
}
rebuild
for it in $(seq 1 $MAXIT); do
  before=$(wc -l < $R/learned_x.txt)
  cd $H
  BLUR_KEYS="${KEYS:-}" BLUR_AUTOPRESS=${AUTOPRESS:-0} BLUR_RECOMP=1 BLUR_SECONDS=${RUN_SECONDS:-60} BLUR_STATS=1 ./blurrt ../../game/extracted/BLUR/Blur.exe > /tmp/blur-run_x$it.txt 2>&1
  after=$(wc -l < $R/learned_x.txt)
  echo "iteration $it: $(grep -a 'not recompiled\|unsupported at\|UNIMPLEMENTED\|\[exit\]\|\[d3d9\]' /tmp/blur-run_x$it.txt | tr '\n' ' ' | cut -c1-260)" >> $LOG
  if [ "$after" -eq "$before" ]; then echo "no new target; stopping" >> $LOG; break; fi
  rebuild
done
echo DONE >> $LOG
