#!/bin/bash
# Rebuild the fast native runtime: lifted code (obj_p2_nb) compiled with a fixed guest-memory base, `restrict` CPU pointer and without the
# per-function trace/preemption checks (parallel guest threads only), then link ./blurrt. About 4 min at -j6 (nice'd). See PLAN_120FPS.md F147.
# usage: build_fast.sh [jobs]
set -e
R=$(cd "$(dirname "$0")/../recomp" && pwd); H=$(cd "$(dirname "$0")" && pwd); J=${1:-6}
cd $R && nice -n 10 make -j$J GEN=gen_p2 OBJDIR=obj_p2_nb OPT="-O1 -fno-strict-aliasing -DBLUR_G_FIXED=0x7000000000ull" \
  PRE="sed -E -e 's/^void (f_[0-9a-f]+)\\(Cpu \\*c\\) \\{/void \\1(Cpu *restrict c) {/' -e '/__builtin_expect\\(rt_trace, 0\\)/d' -e '/__builtin_expect\\(--c->budget < 0, 0\\)/d'"
cd $H && make -B blurrt OBJDIR=obj_p2_nb EXTRA="-DBLUR_G_CHECK -DBLUR_NO_BUDGET"
echo "built $H/blurrt (fast, parallel threads only). The single-threaded scheduler build is blurrt_coop."
