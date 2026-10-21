#!/bin/bash
# Cross-compile the lifted code (port/recomp/gen_p2, referenced in place) to Windows x64 COFF objects in recomp/obj_win with llvm-mingw clang.
# Same flags as port/harness/build_fast.sh. usage: build_recomp.sh [jobs]
set -e
W=$(cd "$(dirname "$0")" && pwd); J=${1:-8}
export PATH=$HOME/tools/llvmmingw/bin:$PATH
cd $W/recomp && nice -n 10 make -j$J CC="x86_64-w64-mingw32-clang" GEN=$W/../../recomp/gen_p2 OBJDIR=obj_win OPT="-O1 -fno-strict-aliasing -DBLUR_G_FIXED=0x7000000000ull" \
  PRE="sed -E -e 's/^void (f_[0-9a-f]+)\\(Cpu \\*c\\) \\{/void \\1(Cpu *restrict c) {/' -e '/__builtin_expect\\(rt_trace, 0\\)/d' -e '/__builtin_expect\\(--c->budget < 0, 0\\)/d'"
# the "extra" lifted functions (gen_x, linked as rt_table_x) -> obj_wx
mkdir -p obj_wx; for f in extra extra_table; do sed -E -e 's/^void (f_[0-9a-f]+)\(Cpu \*c\) \{/void \1(Cpu *restrict c) {/' -e '/__builtin_expect\(rt_trace, 0\)/d' -e '/__builtin_expect\(--c->budget < 0, 0\)/d' $W/../../recomp/gen_x/$f.c | x86_64-w64-mingw32-clang -O1 -fno-strict-aliasing -DBLUR_G_FIXED=0x7000000000ull -w -ffp-contract=off -fno-unwind-tables -fno-asynchronous-unwind-tables -I. -x c -c - -o obj_wx/$f.o; done
