#!/bin/bash
# Cross-compile the Windows harness (blurrt.exe) with llvm-mingw clang. Needs: build_recomp.sh done (recomp/obj_win/*.o), deps/ (unicorn, ffmpeg, spirv-cross), vkd3d-shader (see PORT_NOTES.md).
# usage: build_harness.sh   -> work/win/blurrt.exe (sources straight from port/harness, port/web/src, port/work/interp)
set -e
W=$(cd "$(dirname "$0")" && pwd); export PATH=$HOME/tools/llvmmingw/bin:$PATH
D=$W/deps; V=${V:-$W/deps/vkd3d}
H=$W/../../harness; cd $H
SRCS="hostcommon.c audio.c audio_mix.c bink_ff.c bink_hle.c main.c hle.c hle2.c wsock_hle.c com.c d3d9.c d3dx.c win.c net.c gfx.c hashes.c shadercvt.c shcache.c sched.c"
CFLAGS="-O1 -fno-strict-aliasing -g -Wall -Wno-unused-function -Wno-incompatible-pointer-types -Wno-int-conversion -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -include winport.h -DBLUR_COLD -DBLUR_G_CHECK -DBLUR_NO_BUDGET -I$D/include -I$V/include -I../recomp -I. -I../web/src -I../work/interp"
LIBS="-L$D/lib -L$V/lib -lvkd3d-shader -lvkd3d-common -lunicorn -lspirv-cross-c -lspirv-cross-glsl -lspirv-cross-hlsl -lspirv-cross-msl -lspirv-cross-cpp -lspirv-cross-reflect -lspirv-cross-util -lspirv-cross-core -lavformat -lavcodec -lswresample -lavutil -lc++ -lws2_32 -liphlpapi -lbcrypt -lole32 -luser32 -lwinmm -lpthread -lm"
x86_64-w64-mingw32-clang $CFLAGS $SRCS rtprof.c snapshot.c ../web/src/snap_ctx.c rt.c ../recomp/rt_vec.c ../work/interp/interp.c ../work/interp/interp_glue.c $W/recomp/obj_win/*.o $W/recomp/obj_wx/*.o -static -o $W/blurrt.exe $LIBS
ls -la $W/blurrt.exe
