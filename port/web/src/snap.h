// OEP snapshot: shared declarations (native writer: work/wasm-core/harness/snapshot.c, wasm reader: port/web/src/boot.c).
// The host-side state is described by ONE visitor (snap_ctx.c) that calls cb(user, ptr, nbytes) for every field in a fixed order;
// the writer's callback appends the bytes to state.bin, the reader's callback copies them back. The two sides therefore cannot drift.
#pragma once
#include "emu.h"
typedef void (*snap_cb)(void *user, void *p, size_t n);
void snap_visit(Ctx *c, snap_cb cb, void *user);      // everything except guest memory (that is mem.bin)
// per-file hooks (raw, pointer-free statics)
void hle_snap(snap_cb cb, void *u); void hle2_snap(snap_cb cb, void *u); void win_snap(snap_cb cb, void *u); void com_snap(snap_cb cb, void *u);
int hle2_snap_busy(void); int d3d9_snap_busy(void); int d3dx_snap_busy(void);
void heap_snap(snap_cb cb, void *u);                    // hostcommon.c: guest heap free lists
extern uint64_t g_time_base, g_det_ticks;               // hostcommon.c: guest clock
#define SNAP_MAGIC 0x50414e53u                           // "SNAP"
#define SNAP_VERSION 1u
