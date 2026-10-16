#include "emu.h"
void rt_main(Ctx *c) { (void)c; fprintf(stderr, "recompiled mode not built into this binary\n"); exit(1); }
uint32_t g_api_ret, g_api_esp;      // diagnostics read by d3d9.c; set by the recompiled-path API dispatcher (rt.c)
void ww_arm(void) {} void ww_rearm(void) {}
long lua_binding_first_frame(const char *nm) { (void)nm; return -1; }
double lua_binding_first_time(const char *nm) { (void)nm; return -1; }
