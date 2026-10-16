CC=clang
# vkd3d-shader tree: the main harness lives in port/harness, work copies in port/work/<name>/harness
V ?= $(firstword $(wildcard ../../third_party/vkd3d-2.1 ../../../../third_party/vkd3d-2.1))
# snap_ctx.c / snap.h (host-state description shared with the wasm build in port/web)
WEBSRC ?= $(firstword $(wildcard ../web/src ../../../web/src))
FFMPEG_CFLAGS := $(shell pkg-config --cflags libavformat libavcodec libavutil libswresample)
FFMPEG_LIBS := $(shell pkg-config --libs libavformat libavcodec libavutil libswresample)
CFLAGS=$(FFMPEG_CFLAGS) -O1 -g -mllvm -hot-cold-split=false -Wall -Wno-unused-function -I/opt/homebrew/include -I$(V)/include -I/opt/homebrew/include
LDFLAGS=-L/opt/homebrew/lib -lunicorn -Wl,-rpath,/opt/homebrew/lib -L$(V)/.libs -lvkd3d-shader -Wl,-rpath,$(abspath $(V)/.libs) -lspirv-cross-c -lspirv-cross-glsl -lspirv-cross-hlsl -lspirv-cross-msl -lspirv-cross-cpp -lspirv-cross-reflect -lspirv-cross-util -lspirv-cross-core -lc++ -lpthread $(FFMPEG_LIBS)

# sources shared by both targets
SRCS=hostcommon.c audio.c audio_mix.c bink_ff.c bink_hle.c main.c hle.c hle2.c wsock_hle.c com.c d3d9.c d3dx.c win.c net.c gfx.c hashes.c shadercvt.c shcache.c sched.c
HDRS=emu.h trivial.inc audio_mix.h bink_ff.h bink_hle.h shcache.h apilog.h

blurhle: rt_stub.c $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) $(SRCS) rt_stub.c -lm -o blurhle $(LDFLAGS)
clean:
	rm -f blurhle

OBJDIR ?= obj4
RT_OBJS=$(wildcard ../recomp/$(OBJDIR)/*.o) $(wildcard ../recomp/obj_x/*.o)
blurrt: ../recomp/rt_vec.c snapshot.c $(WEBSRC)/snap_ctx.c $(SRCS) rt.c $(HDRS) $(RT_OBJS)
	$(CC) $(CFLAGS) -I../recomp -I. -I$(WEBSRC) $(SRCS) snapshot.c $(WEBSRC)/snap_ctx.c rt.c ../recomp/rt_vec.c $(RT_OBJS) -lm -o blurrt $(LDFLAGS)

# offline D3D9 bytecode -> GLSL translator (shaders.bin for the browser renderer, see shcache.h)
shadertool: shadertool.c shadercvt.c shcache.h emu.h
	$(CC) $(CFLAGS) shadertool.c shadercvt.c -o shadertool $(LDFLAGS)
