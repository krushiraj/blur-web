// Glue between the interpreter and the two existing dispatchers (port/harness/rt.c and port/web/src/rt_wasm.c), which contain
//     extern int rt_cold(Cpu *cpu);  ...  if (!f) { if (rt_cold(cpu)) continue; rt_missing(...); }
// in rt_dispatch at the "address not in the recompiled table" branch.  Build with -DINTERP_GLUE_WASM for the single-threaded wasm build.
//
// Host requirement: one extra non-static predicate next to the existing lookup():
//     int rt_is_native(uint32_t pc) { return pc - STUB_BASE < STUB_SLOTS * 16u || pc == RET_MAGIC || lookup(pc) != NULL; }   (must not touch coverage state)
//
// rt_cold() contract: 1 = keep dispatching (cpu->pc is now a recompiled function / API stub / RET_MAGIC, or the interpreter slice ended),
// 2 = return to the scheduler now (wasm only: time slice over, rt_yield_flag set exactly like the transformed lifted prologue does), 0 = cannot handle.
//
// Native only: BLUR_COLDSTAT=<file> records how often every cold function (call target seen by the interpreter) is entered and when, prints a
// "[cold]" line per second to stderr and writes the table at exit (see README, "cold-function statistics").
#include "interp.h"
extern int rt_is_native(uint32_t pc);
#ifdef INTERP_GLUE_WASM
extern int rt_yield_flag;
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#ifndef SIGUSR1
#define SIGUSR1 10      /* Windows: no such signal, the debug hook is simply never triggered */
#endif
typedef struct { uint32_t addr, n; uint32_t first_ms; } Cs;
#define CS_N (1u << 20)
static Cs *cs; static const char *cs_path; static uint64_t cs_calls, cs_runs, cs_t0, cs_last_t, cs_last_calls, cs_last_insns, cs_distinct;
static Cs *ss; static uint64_t ss_n;                       // pc samples (every ~4093 interpreted instructions) -> where the interpreter spends its time
static int cs_trace_left;                // set by BLUR_COLDWATCH: single-step this many instructions printing the state
static uint64_t cs_now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }
static void cs_hook(uint32_t target, uint32_t ret) {
    { static uint32_t watch = 1; static int shown; if (watch == 1) { const char *e = getenv("BLUR_COLDWATCH"); watch = e ? (uint32_t)strtoul(e, 0, 16) : 0; }       // BLUR_COLDWATCH=<addr>: print the first 5 callers after the 100000th call
      static uint64_t wn; if (watch && target == watch && ++wn > 100000 && shown < 5) { shown++; fprintf(stderr, "[coldwatch] call %08x from ret=%08x (call #%llu)\n", target, ret, (unsigned long long)wn); if (shown == 2) cs_trace_left = 600; } }
    if (target >= interp_stub_floor) return;           // API stub calls are not cold code
    uint32_t h = (target * 2654435761u) >> 8 & (CS_N - 1); cs_calls++;
    while (cs[h].addr && cs[h].addr != target) h = (h + 1) & (CS_N - 1);
    if (!cs[h].addr) { cs[h].addr = target; cs[h].first_ms = (uint32_t)(cs_now_ms() - cs_t0); cs_distinct++; }
    cs[h].n++;
}
static int cs_cmp(const void *a, const void *b) { return (int)(((const Cs *)b)->n > ((const Cs *)a)->n) - (int)(((const Cs *)b)->n < ((const Cs *)a)->n); }
static void cs_write(void) {
    if (!cs_path) return; FILE *f = fopen(cs_path, "w"); if (!f) return;
    Cs *cs0 = cs; cs = malloc(CS_N * sizeof(Cs)); memcpy(cs, cs0, CS_N * sizeof(Cs)); qsort(cs, CS_N, sizeof(Cs), cs_cmp);       // sort a copy: a USR1 dump must not break the live hash table
                                                  // zero entries (addr 0) sort last
    fprintf(f, "# cold functions: address, interpreted calls, first seen (ms since start); %llu calls, %llu distinct targets, %llu interpreter runs, %llu interpreted instructions\n",
            (unsigned long long)cs_calls, (unsigned long long)cs_distinct, (unsigned long long)cs_runs, (unsigned long long)interp_icount);
    for (uint32_t i = 0; i < CS_N && cs[i].addr; i++) fprintf(f, "%08x %u %u\n", cs[i].addr, cs[i].n, cs[i].first_ms);
    fclose(f); free(cs); cs = cs0;
    char sp[1024]; snprintf(sp, sizeof sp, "%s.samples", cs_path); FILE *fs = fopen(sp, "w");          // pc samples: "<pc> <count>", one per ~4093 interpreted instructions
    if (fs) { Cs *ss0 = ss; ss = malloc(CS_N * sizeof(Cs)); memcpy(ss, ss0, CS_N * sizeof(Cs)); qsort(ss, CS_N, sizeof(Cs), cs_cmp); fprintf(fs, "# %llu samples, one per %d interpreted instructions\n", (unsigned long long)ss_n, 4093); for (uint32_t i = 0; i < CS_N && ss[i].addr; i++) fprintf(fs, "%08x %u\n", ss[i].addr, ss[i].n); fclose(fs); free(ss); ss = ss0; }
}
static void cs_sig(int sig) { cs_write(); if (sig != SIGUSR1) _exit(128 + sig); }       // debugging: kill -USR1 <pid> dumps the table, TERM/INT dump and exit
static void cs_tick(void) {
    uint64_t t = cs_now_ms() - cs_t0; if (t < cs_last_t + 1000) return;
    fprintf(stderr, "[cold] t=%llus calls/s=%llu insns/s=%llu distinct=%llu runs=%llu\n", (unsigned long long)(t / 1000), (unsigned long long)(cs_calls - cs_last_calls),
            (unsigned long long)(interp_icount - cs_last_insns), (unsigned long long)cs_distinct, (unsigned long long)cs_runs);
    cs_last_t = t; cs_last_calls = cs_calls; cs_last_insns = interp_icount;
}
#endif
int rt_cold(Cpu *cpu) {
    static int inited;
    if (!inited) {
        interp_init(rt_is_native); inited = 1;
#ifndef INTERP_GLUE_WASM
        cs_path = getenv("BLUR_COLDSTAT");
        if (cs_path) { cs = calloc(CS_N, sizeof(Cs)); ss = calloc(CS_N, sizeof(Cs)); cs_t0 = cs_now_ms(); interp_call_hook = cs_hook; atexit(cs_write); signal(SIGUSR1, cs_sig); signal(SIGTERM, cs_sig); signal(SIGINT, cs_sig); }
#endif
    }
#ifndef INTERP_GLUE_WASM
    if (cs) { cs_runs++; cs_tick(); }
#endif
#ifndef INTERP_GLUE_WASM
    { static uint64_t from = ~0ull, mx; static int ini; if (!ini) { ini = 1; const char *e = getenv("BLUR_ITRACE"); if (e) { from = strtoull(e, (char **)&e, 0); mx = *e == ',' ? strtoull(e + 1, 0, 0) : 500; } }
      extern uint64_t rt_api_count(void); if (from != ~0ull && rt_api_count() >= from && mx) { cs_trace_left = (int)mx; mx = 0;
#ifdef INTERP_ENTRY_ACCOUNTING
        interp_charge_trace = 100000;
#endif
      } }
    if (cs_trace_left > 0) {
        extern uint8_t *G;
        while (cs_trace_left-- > 0 && !rt_is_native(cpu->pc)) { uint8_t *b = G + cpu->pc;
            fprintf(stderr, "[itrace] %08x %02x%02x%02x%02x%02x%02x%02x%02x  eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x fl=%d%d%d%d%d\n", cpu->pc, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                    cpu->eax, cpu->ecx, cpu->edx, cpu->ebx, cpu->esp, cpu->ebp, cpu->esi, cpu->edi, cpu->fl.cf, cpu->fl.pf, cpu->fl.zf, cpu->fl.sf, cpu->fl.of);
            interp_run_n(cpu, 1); }
        return 1;
    }
#endif
#ifdef INTERP_GLUE_WASM
    int r = interp_run_n(cpu, interp_slice);
#else
    int r = interp_run_n(cpu, cs ? 4093 : interp_slice);
#endif
#ifndef INTERP_GLUE_WASM
    if (cs && r == INTERP_BUDGET) { uint32_t pc = cpu->pc, h = (pc * 2654435761u) >> 8 & (CS_N - 1); ss_n++; while (ss[h].addr && ss[h].addr != pc) h = (h + 1) & (CS_N - 1); ss[h].addr = pc; ss[h].n++; }
#endif
    if (r == INTERP_PREEMPT) {
#ifdef INTERP_GLUE_WASM
#ifdef INTERP_ENTRY_ACCOUNTING
        cpu->budget = 200001;                                               // = what the transformed lifted prologue stores: the re-entry charges the entry once more
#else
        cpu->budget = 200000;
#endif
        rt_yield_flag = 1; return 2;                 // single-threaded build: unwind to the scheduler loop, resume at cpu->pc later
#else
        rt_preempt(cpu);                                                    // native build: blocks this host thread until its turn comes again, refills cpu->budget
#ifdef INTERP_ENTRY_ACCOUNTING
        cpu->budget++;                                                      // the entry instruction is charged again on resume
#endif
#endif
    }
    return 1;
}
