#!/bin/sh
# Full verification run (about 20 minutes on 4 cores).  Output summaries go to ./results/.  Needs: Unicorn C library (brew install unicorn), the image dumps in
# ../../../re/dump, the capstone venv python (PY=...) for samples / decoder length / lifting the mixed-mode set.
cd "$(dirname "$0")"; mkdir -p results
PY=${PY:-python3}
IMG=../../../re/dump/blur_runtime.bin; MSV=../../../re/dump/msvcr90.bin
[ -f samples.bin ] || $PY tools/make_samples.py samples.bin 300
cat ../../recomp/oracle_funcs.txt ../../recomp/oracle_funcs_fp.txt > mixed/all_list.txt
[ -f mixed/hot.c ] || { awk 'NR%2==0' mixed/all_list.txt > mixed/hot_list.txt; $PY mixed/gen_hot.py mixed/hot_list.txt; }
make -s all probe difftest_mixed || exit 1
export UC_IGNORE_REG_BREAK=1
./unit_test > results/unit.txt 2>&1
$PY tools/declen.py > results/declen.txt 2>&1
for kind in fuzz samples; do
  S=""; [ $kind = samples ] && S=samples.bin
  i=1; while [ $i -le 4 ]; do DT_COV_FILE=results/cov_${kind}_$i.txt ./difftest single $IMG ${TRIALS:-300000} $((i*104729)) $S > results/single_${kind}_$i.txt 2>&1 & i=$((i+1)); done; wait
done
# fxsave (reserved fields masked in the comparison; excluded from the general fuzz)
DT_ALLOW=1 DT_CODE=0fae00 ./difftest single $IMG 20000 3 > results/single_fxsave.txt 2>&1
for list in oracle_funcs oracle_funcs_fp; do
  i=1; while [ $i -le 4 ]; do ./difftest funcs $IMG ../../recomp/$list.txt ${FTRIALS:-400} $((i*7727)) $MSV > results/${list}_$i.txt 2>&1 & i=$((i+1)); done; wait
done
# lifted C + interpreter in one run (half of the corpus is lifted by the real lifter), 4 seeds
i=1; while [ $i -le 4 ]; do ./difftest_mixed funcs $IMG mixed/all_list.txt ${FTRIALS:-400} $((i*31337)) $MSV > results/mixed_$i.txt 2>&1 & i=$((i+1)); done; wait
# throughput on real functions including decode cost (cache flushed before every run); run alone for stable timing
DT_NOCACHE=1 ./difftest funcs $IMG ../../recomp/oracle_funcs.txt 100 9 $MSV > results/throughput_nocache.txt 2>&1
./difftest funcs $IMG ../../recomp/oracle_funcs.txt 100 9 $MSV > results/throughput_cached.txt 2>&1
echo done > results/DONE
