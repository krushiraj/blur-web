#!/bin/zsh
# Determinism + golden frame-hash check (native, headless, no browser).
#   det_check.sh [--frames N] [--bin <blurrt>] [--harness <dir>] [--update-golden] [--runs 2]
# Runs the harness N frames with BLUR_DET (deterministic clock) and BLUR_FRAMEHASH (FNV-1a of every flushed command batch = one per frame), with the canonical frame-keyed
# input script, `--runs` times, and requires the per-frame hash sequences to be IDENTICAL. Then compares every 50th hash with refs/golden_hashes.txt (a change there means the
# rendering command stream changed: expected after intentional d3d9/HLE changes -> rerun with --update-golden and look at the smoke screenshots).
HERE=${0:A:h}; ROOT=${HERE:h:h:h}
FRAMES=$(node -e "console.log(require('$HERE/scenario.json').detFrames)"); RUNS=2; BIN=; HARNESS=$ROOT/port/harness; UPDATE=0
while [[ $# -gt 0 ]]; do case $1 in
  --frames) FRAMES=$2; shift 2;; --runs) RUNS=$2; shift 2;; --bin) BIN=$2; shift 2;; --harness) HARNESS=$2; shift 2;; --update-golden) UPDATE=1; shift;; *) echo "unknown arg $1"; exit 2;; esac; done
[[ -z $BIN ]] && BIN=$HARNESS/blurrt
OUT=${OUT:-$HERE/out}; mkdir -p $OUT
GAME=${BLUR_EXE:-$ROOT/game/extracted/BLUR/Blur.exe}
KEYS=$(node -e "const s=require('$HERE/scenario.json'); console.log(s.keys.map(k=>k.key+'@f'+k.frame).join(','))")
for i in $(seq 1 $RUNS); do
  ( cd $HARNESS && BLUR_REL=1 BLUR_RECOMP=1 BINK_SETS=1 BINK_FAKE=1 BLUR_DET=$(node -e "console.log(require('$HERE/scenario.json').det)") BLUR_MAXFRAMES=$FRAMES BLUR_FRAMEHASH=1 BLUR_KEYS=$KEYS BLUR_LEARNED=/dev/null \
      $BIN $GAME 2>&1 | grep '^\[fh\]' > $OUT/det_run$i.fh )
  echo "run $i: $(wc -l < $OUT/det_run$i.fh) frame hashes"
done
rc=0
for i in $(seq 2 $RUNS); do
  if cmp -s $OUT/det_run1.fh $OUT/det_run$i.fh; then echo "PASS  run 1 == run $i ($(wc -l < $OUT/det_run1.fh) frames identical)"
  else echo "FAIL  run 1 != run $i: first difference:"; diff $OUT/det_run1.fh $OUT/det_run$i.fh | head -4; rc=1; fi
done
GOLD=$HERE/refs/golden_hashes.txt
awk 'NR%50==1 {print $2, $3}' $OUT/det_run1.fh > $OUT/det_sample.txt          # "<flush#> <hash>" every 50th flush
if [[ $UPDATE == 1 ]]; then cp $OUT/det_sample.txt $GOLD; echo "golden hashes updated ($(wc -l < $GOLD) samples, $FRAMES frames)"
elif [[ -f $GOLD ]]; then
  n=$(wc -l < $GOLD); m=$(wc -l < $OUT/det_sample.txt); k=$(( n < m ? n : m ))
  if head -$k $GOLD | cmp -s - <(head -$k $OUT/det_sample.txt); then echo "PASS  golden hashes match ($k samples)"
  else echo "FAIL  golden hash mismatch (rendering command stream changed):"; diff <(head -$k $GOLD) <(head -$k $OUT/det_sample.txt) | head -4; rc=1; fi
else echo "WARN  no golden hashes yet (run with --update-golden)"; fi
exit $rc
