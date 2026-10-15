#!/bin/bash
# Run the recompiled game; every code address it reaches that was not recompiled is appended to learned.txt by the runtime.
# Regenerate + rebuild + relink with the new seeds and run again, until the game stops hitting new addresses.
set -u
R=$(cd "$(dirname "$0")" && pwd)
H=$R/../harness
PY=${PY:-python3}
LOG=/tmp/blur-learn.log
MAXIT=${1:-12}
: > $LOG
cd $R
BLOCKS=blocks.txt $PY lift_par.py gen_p2 > /tmp/blur-genpar.log 2>&1
make -j14 -k GEN=gen_p2 OPT=-O1 OBJDIR=obj_p2 > /tmp/blur-make.log 2>&1
cd $H && make blurrt OBJDIR=obj_p2 > /tmp/blur-link.log 2>&1
echo "initial rebuild done" >> $LOG
for it in $(seq 1 $MAXIT); do
  before=$(wc -l < $R/learned.txt)
  cd $H
  BLUR_RECOMP=1 BLUR_SECONDS=${RUN_SECONDS:-45} BLUR_STATS=1 ./blurrt ../../game/extracted/BLUR/Blur.exe > /tmp/blur-run_it$it.txt 2>&1
  after=$(wc -l < $R/learned.txt)
  echo "iteration $it: $(grep -a 'not recompiled\|unsupported at\|UNIMPLEMENTED\|\[exit\]\|\[d3d9\]' /tmp/blur-run_it$it.txt | tr '\n' ' ' | cut -c1-300)" >> $LOG
  if [ "$after" -eq "$before" ]; then echo "no new learned target; stopping" >> $LOG; break; fi
  cd $R
  BLOCKS=blocks.txt $PY lift_par.py gen_p2 > /tmp/blur-genpar.log 2>&1
  make -j14 -k GEN=gen_p2 OPT=-O1 OBJDIR=obj_p2 > /tmp/blur-make.log 2>&1
  cd $H && make blurrt OBJDIR=obj_p2 > /tmp/blur-link.log 2>&1
  echo "  rebuilt ($(tail -1 /tmp/blur-genpar.log | cut -c1-60))" >> $LOG
done
echo DONE >> $LOG
