#include "emu.h"
void rt_main(Ctx *c) { (void)c; fprintf(stderr, "recompiled mode not built into this binary\n"); exit(1); }
uint32_t g_api_ret, g_api_esp;      // diagnostics read by d3d9.c; set by the recompiled-path API dispatcher (rt.c)
void ww_arm(void) {} void ww_rearm(void) {}
