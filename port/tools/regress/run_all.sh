#!/bin/zsh
# One command: everything a patch should pass before it is delivered.   run_all.sh [--quick] [--realtime] [--bin <blurrt>] [--harness <dir>] [--update-refs]
#   determinism (2 identical runs of 3000 frames + golden hashes), smoke walk (log checks + screenshots vs refs), wasm equivalence if the wasm build exists.
#   --quick: determinism only (scenario detFrames), no smoke walk; --realtime adds the real-time pacing check (needs a quiet machine, ~2 min).   Exit status 0 only if nothing FAILED (SKIP/WARN do not fail).
HERE=${0:A:h}; QUICK=0; PASS_ARGS=()
REALTIME=0
for a in "$@"; do case $a in --quick) QUICK=1;; --realtime) REALTIME=1;; *) PASS_ARGS+=($a);; esac; done
mkdir -p $HERE/out; : > $HERE/out/summary.txt; rc=0
step() { local name=$1; shift; local t0=$SECONDS; "$@" > $HERE/out/$name.txt 2>&1; local r=$?; local dt=$((SECONDS - t0))
  local verdict=PASS; [[ $r == 3 ]] && verdict=SKIP; [[ $r == 4 ]] && verdict=WARN; [[ $r != 0 && $r != 3 && $r != 4 ]] && { verdict=FAIL; rc=1; }
  printf "%-6s %-14s %5ds   %s\n" $verdict $name $dt "$( (grep -E 'FAILED|OK:' $HERE/out/$name.txt; grep -m1 -E 'FAIL|SKIP|WARN' $HERE/out/$name.txt; grep -E 'PASS' $HERE/out/$name.txt) | head -1 | cut -c1-90)" | tee -a $HERE/out/summary.txt; }
DETARGS=(); SMOKEARGS=()
for ((i=1;i<=${#PASS_ARGS[@]};i++)); do a=${PASS_ARGS[i]}; case $a in --bin|--harness) DETARGS+=($a ${PASS_ARGS[i+1]}); SMOKEARGS+=($a ${PASS_ARGS[i+1]}); i=$((i+1));; --update-refs) SMOKEARGS+=($a);; esac; done
if [[ $QUICK == 1 ]]; then step determinism $HERE/det_check.sh $DETARGS
else step determinism $HERE/det_check.sh $DETARGS; step smoke $HERE/smoke.sh $SMOKEARGS; step wasm $HERE/smoke.sh --wasm
  [[ $REALTIME == 1 ]] && step realtime node $HERE/realtime.js $DETARGS; fi
echo "---- details: $HERE/out/*.txt ----"; [[ $rc == 0 ]] && echo "ALL OK" || echo "SOMETHING FAILED"
exit $rc
