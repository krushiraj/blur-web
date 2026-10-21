#!/bin/bash
# Build the Linux blurrt in Docker (native arch of the Docker VM; set PLATFORM=linux/amd64 for x86-64 under emulation, slow).
# usage: build_linux.sh [jobs]   -> $W/out/blurrt (+ libs)   W defaults to /tmp/linux_build
set -e
R=$(cd "$(dirname "$0")/../../.." && pwd); W=${W:-/tmp/linux_build}; J=${1:-6}; mkdir -p "$W"
IMG=blur-linux-build${PLATFORM:+-${PLATFORM#*/}}
[ -n "$(docker images -q $IMG)" ] || docker build ${PLATFORM:+--platform $PLATFORM} -t $IMG "$(dirname "$0")"
docker run --rm ${PLATFORM:+--platform $PLATFORM} -v "$R":/src:ro -v "$W":/b $IMG bash -c "
set -e; cd /b
# vkd3d-shader (static) once
if [ ! -f third_party/vkd3d-2.1/.libs/libvkd3d-shader.a ]; then mkdir -p third_party && tar xf /src/third_party/vkd3d-2.1.tar.xz -C third_party && (cd third_party/vkd3d-2.1 && CC=clang ./configure --disable-doxygen-doc --enable-static --disable-shared >/dev/null && make -j$J >/dev/null 2>&1); fi
# unicorn 2.x (the Debian package is 1.0.x with a different default CPU: the packer derives jump targets from CPUID, so the emulator must match mac/Windows)
if [ ! -f deps/lib/libunicorn.a ]; then git clone -q --depth 1 -b 2.1.4 https://github.com/unicorn-engine/unicorn /tmp/uc && cmake -S /tmp/uc -B /tmp/uc/b -DCMAKE_BUILD_TYPE=Release -DUNICORN_ARCH=x86 -DUNICORN_BUILD_SHARED=OFF -DCMAKE_C_COMPILER=clang -DCMAKE_INSTALL_PREFIX=/b/deps >/dev/null && cmake --build /tmp/uc/b -j$J >/dev/null && cmake --install /tmp/uc/b >/dev/null; fi
mkdir -p port/recomp port/work web_tmp
cp -r /src/port/harness port/ ; cp /src/port/recomp/*.h /src/port/recomp/rt_vec.c /src/port/recomp/Makefile port/recomp/; cp -r /src/port/recomp/gen_p2 /src/port/recomp/gen_x port/recomp/ ; cp -r /src/port/work/interp port/work/; cp -r /src/port/web port/
rm -f port/harness/*.o
# x86-64 host: the arm 'yield' spin hint becomes 'pause' (a no-op once the sources use CPU_RELAX())
[ \"\$(uname -m)\" = x86_64 ] && sed -i 's/__asm__ volatile(\"yield\")/__asm__ volatile(\"pause\")/' port/harness/hostcommon.c port/harness/rt.c
cd port/recomp && make -j$J GEN=gen_p2 OBJDIR=obj_lnx OPT='-O1 -fsigned-char -fno-strict-aliasing -DBLUR_G_FIXED=0x7000000000ull' \
  PRE=\"sed -E -e 's/^void (f_[0-9a-f]+)\\\\(Cpu \\\\*c\\\\) \\\\{/void \\\\1(Cpu *restrict c) {/' -e '/__builtin_expect\\\\(rt_trace, 0\\\\)/d' -e '/__builtin_expect\\\\(--c->budget < 0, 0\\\\)/d'\"
cd ../recomp && mkdir -p obj_x_lnx && for f in extra extra_table; do clang -O1 -fsigned-char -fno-strict-aliasing -DBLUR_G_FIXED=0x7000000000ull -w -ffp-contract=off -fno-unwind-tables -fno-asynchronous-unwind-tables -I. -c gen_x/\$f.c -o obj_x_lnx/\$f.o; done
cd ../harness && make -B blurrt UC=/b/deps OBJDIR=obj_lnx XOBJDIR=obj_x_lnx V=/b/third_party/vkd3d-2.1 EXTRA='-DBLUR_G_CHECK -DBLUR_NO_BUDGET'
mkdir -p /b/out && cp blurrt /b/out/
"
