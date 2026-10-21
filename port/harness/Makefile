CC=clang
# Homebrew prefix: /opt/homebrew on Apple silicon, /usr/local on Intel Macs
BREW ?= $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)
# vkd3d-shader tree: the main harness lives in port/harness, work copies in port/work/<name>/harness
V ?= $(firstword $(wildcard ../../third_party/vkd3d-2.1 ../../../../third_party/vkd3d-2.1))
# snap_ctx.c / snap.h (host-state description shared with the wasm build in port/web)
WEBSRC ?= $(firstword $(wildcard ../web/src ../../../web/src))
FFMPEG_CFLAGS := $(shell pkg-config --cflags libavformat libavcodec libavutil libswresample)
FFMPEG_LIBS := $(shell pkg-config --libs libavformat libavcodec libavutil libswresample)
UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
CFLAGS=$(FFMPEG_CFLAGS) -O1 -fno-strict-aliasing -g -mllvm -hot-cold-split=false -Wall -Wno-unused-function -I$(BREW)/include -I$(V)/include -I$(BREW)/include $(EXTRA)
LDFLAGS=-L$(BREW)/lib -lunicorn -Wl,-rpath,$(BREW)/lib -L$(V)/.libs -lvkd3d-shader -Wl,-rpath,$(abspath $(V)/.libs) -lspirv-cross-c -lspirv-cross-glsl -lspirv-cross-hlsl -lspirv-cross-msl -lspirv-cross-cpp -lspirv-cross-reflect -lspirv-cross-util -lspirv-cross-core -lc++ -lpthread $(FFMPEG_LIBS)
else
# Linux: libs from the distro (unicorn, ffmpeg, spirv-cross-c-shared) plus a static vkd3d-shader built from third_party (see pack/linux)
UC ?=
CFLAGS=$(FFMPEG_CFLAGS) -D_GNU_SOURCE -fsigned-char -O1 -fno-strict-aliasing -g -Wall -Wno-unused-function $(if $(UC),-I$(UC)/include) -I$(V)/include $(EXTRA)
LDFLAGS=-fuse-ld=lld $(if $(UC),-L$(UC)/lib) -L$(V)/.libs -lvkd3d-shader $(if $(UC),$(UC)/lib/libunicorn.a,-lunicorn) -lspirv-cross-c-shared -ldl -lpthread -lm $(FFMPEG_LIBS)
endif

# sources shared by both targets
SRCS=hostcommon.c audio.c audio_mix.c bink_ff.c bink_hle.c main.c hle.c hle2.c wsock_hle.c com.c d3d9.c d3dx.c win.c net.c gfx.c hashes.c shadercvt.c shcache.c sched.c
HDRS=emu.h trivial.inc audio_mix.h bink_ff.h bink_hle.h shcache.h apilog.h

blurhle: rt_stub.c $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) $(SRCS) rt_stub.c -lm -o blurhle $(LDFLAGS)
clean:
	rm -f blurhle

OBJDIR ?= obj4
XOBJDIR ?= obj_x
RT_OBJS=$(wildcard ../recomp/$(OBJDIR)/*.o) $(wildcard ../recomp/$(XOBJDIR)/*.o)
# x86 interpreter as cold-code fallback (port/work/interp): BLUR_COLD=0 at run time disables it again.
INTERP ?= $(firstword $(wildcard ../interp ../work/interp ../../interp))
blurrt: ../recomp/rt_vec.c rtprof.c snapshot.c $(WEBSRC)/snap_ctx.c $(SRCS) rt.c $(HDRS) $(RT_OBJS) $(INTERP)/interp.c $(INTERP)/interp_glue.c $(INTERP)/interp.h
	$(CC) $(CFLAGS) -DBLUR_COLD -I../recomp -I. -I$(WEBSRC) -I$(INTERP) $(SRCS) rtprof.c snapshot.c $(WEBSRC)/snap_ctx.c rt.c ../recomp/rt_vec.c $(INTERP)/interp.c $(INTERP)/interp_glue.c $(RT_OBJS) -lm -o blurrt $(LDFLAGS)

# offline D3D9 bytecode -> GLSL translator (shaders.bin for the browser renderer, see shcache.h)
shadertool: shadertool.c shadercvt.c shcache.h emu.h
	$(CC) $(CFLAGS) shadertool.c shadercvt.c -o shadertool $(LDFLAGS)

# host-side unit tests of the Win32 sync/thread/time/file HLE (port/work/audio/SYNC_AUDIT.md); the guest clock now_us() is replaced by a fake one
test_sync: test_sync.c main.c hostcommon.c $(SRCS) rt_stub.c $(HDRS)
	$(CC) $(CFLAGS) -c -Dnow_us=real_clock_now_us hostcommon.c -o hostcommon_test.o
	$(CC) $(CFLAGS) test_sync.c $(filter-out main.c hostcommon.c,$(SRCS)) hostcommon_test.o rt_stub.c -lm -o test_sync $(LDFLAGS)
