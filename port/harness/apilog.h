// BLUR_APILOG=<file> [BLUR_APILOG_MAX=N]: one line per API-handler invocation (retries of blocked calls included):
//   <n> t<thread> <name> <arg0> <arg1>
// Used by the native harness and the wasm build to prove they execute the same call sequence (diff the two files).
#pragma once
#include <stdio.h>
#include <stdlib.h>
static FILE *g_apilog; static uint64_t g_apilog_n, g_apilog_max = 20000; static int g_apilog_init;
static inline void apilog_call(int tid, const char *name, const uint32_t *a) {
    if (!g_apilog_init) { g_apilog_init = 1; const char *p = getenv("BLUR_APILOG"); if (p) g_apilog = fopen(p, "w"); if (getenv("BLUR_APILOG_MAX")) g_apilog_max = strtoull(getenv("BLUR_APILOG_MAX"), 0, 0); }
    if (g_apilog && g_apilog_n < g_apilog_max) { fprintf(g_apilog, "%llu t%d %s %08x %08x\n", (unsigned long long)g_apilog_n, tid, name, a[0], a[1]); if (g_apilog_n + 1 == g_apilog_max) { fflush(g_apilog); if (getenv("BLUR_APILOG_EXIT")) exit(0); } }
    g_apilog_n++;
}
