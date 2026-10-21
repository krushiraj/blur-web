// Native side of the OEP snapshot (BLUR_SNAPSHOT=<dir>): dumps guest memory + host state at the moment the recompiled path takes over at the
// original entry point, so the WebAssembly build can boot from it without Unicorn. Format documented in port/web/README.md:
//   mem.bin       concatenated bytes of all non-zero guest pages (runs of consecutive pages)
//   state.bin     host state, the byte stream produced by snap_visit() (port/web/src/snap_ctx.c)
//   manifest.json human/JS-readable summary: memory runs {addr,len,off}, thread-0 registers, stub table, modules, clock, sizes
#include "snap.h"
#include "../recomp/rt.h"
#ifndef _WIN32
#include <sys/mman.h>
#endif
#include <sys/stat.h>

typedef struct { FILE *f; size_t n; } Out;
static void write_cb(void *user, void *p, size_t n) { Out *o = user; fwrite(p, 1, n, o->f); o->n += n; }
extern const HleDef *hle_lookup(const char *name);

static int page_zero(const uint8_t *p) { const uint64_t *q = (const uint64_t *)p; for (int i = 0; i < 512; i++) if (q[i]) return 0; return 1; }

void snapshot_write(Ctx *c, const char *dir, uint32_t oep) {
    mkdir(dir, 0755);
    char path[600];
    // ---- sanity: host objects that are not part of the state format must not exist yet ----
    int bad = 0;
    if (hle2_snap_busy()) { fprintf(stderr, "[snapshot] WARNING: %d crypto hash contexts alive\n", hle2_snap_busy()); bad = 1; }
    if (d3d9_snap_busy()) { fprintf(stderr, "[snapshot] WARNING: D3D9 objects/frames exist (%d)\n", d3d9_snap_busy()); bad = 1; }
    if (d3dx_snap_busy()) { fprintf(stderr, "[snapshot] WARNING: D3DX tables exist (%d)\n", d3dx_snap_busy()); bad = 1; }
    for (int i = 1; i < c->nhandles; i++) if (c->handles[i].fp) { fprintf(stderr, "[snapshot] WARNING: open file handle %x\n", 0x100 + 4 * i); bad = 1; }
    for (int i = 0; i < c->nstubs; i++) {
        Stub *s = &c->stubs[i]; const HleDef *h = hle_lookup(s->name);
        if (s->fn && (!h || h->fn != s->fn) && strcmp(s->name, "__continuation")) { fprintf(stderr, "[snapshot] WARNING: stub %s!%s has a handler that cannot be re-resolved by name\n", s->dll, s->name); bad = 1; }
    }
    // ---- guest memory: every non-zero page below 2 GB ----
    snprintf(path, sizeof path, "%s/mem.bin", dir); FILE *mf = fopen(path, "wb"); if (!mf) { perror(path); exit(1); }
    typedef struct { uint32_t addr, len; uint64_t off; } Run; Run *runs = malloc(sizeof(Run) * 200000); int nr = 0; uint64_t off = 0, total_nz = 0;
    const uint64_t LIM = 0x80000000ull, CH = 64ull << 20; unsigned char *vec = malloc(CH / 4096);
    for (uint64_t base = 0x10000; base < LIM; base += CH) {
        uint64_t len = CH; if (base + len > LIM) len = LIM - base;
        memset(vec, 1, len / 4096);      // (mincore() on macOS under-reports; a plain scan of the 2 GB is cheap)
        for (uint64_t pg = 0; pg < len / 4096; pg++) {
            if (!(vec[pg] & 1)) continue;
            uint32_t a = (uint32_t)(base + pg * 4096); const uint8_t *p = G + a;
            if (page_zero(p)) continue;
            if (nr && runs[nr - 1].addr + runs[nr - 1].len == a) runs[nr - 1].len += 4096;
            else { if (nr >= 200000) { fprintf(stderr, "too many runs\n"); exit(1); } runs[nr++] = (Run){a, 4096, off}; }
            fwrite(p, 1, 4096, mf); off += 4096; total_nz++;
        }
    }
    fclose(mf); free(vec);
    // ---- host state ----
    uint64_t saved_base = g_time_base, saved_ticks = g_det_ticks; g_time_base = now_us() - 1000000ull * 1000; g_det_ticks = 0;     // resume clock = current clock
    snprintf(path, sizeof path, "%s/state.bin", dir); Out o = { fopen(path, "wb"), 0 }; if (!o.f) { perror(path); exit(1); }
    snap_visit(c, write_cb, &o); fclose(o.f);
    uint64_t resume_clock = now_us(); g_time_base = saved_base; g_det_ticks = saved_ticks;
    // ---- manifest ----
    snprintf(path, sizeof path, "%s/manifest.json", dir); FILE *jf = fopen(path, "w"); if (!jf) { perror(path); exit(1); }
    Thread *t0 = &c->threads[0]; Cpu *cpu = t0->cpu;
    fprintf(jf, "{\n  \"version\": %u,\n  \"oep\": \"0x%08x\",\n  \"state_bytes\": %zu,\n  \"mem_bytes\": %llu,\n  \"mem_pages\": %llu,\n  \"clock_us\": %llu,\n", SNAP_VERSION, oep, o.n, (unsigned long long)off, (unsigned long long)total_nz, (unsigned long long)resume_clock);
    fprintf(jf, "  \"sizeof_cpu\": %zu,\n  \"gamedir\": \"%s\",\n", sizeof(Cpu), g_gamedir);
    fprintf(jf, "  \"thread0\": {\"eax\":%u,\"ecx\":%u,\"edx\":%u,\"ebx\":%u,\"esp\":%u,\"ebp\":%u,\"esi\":%u,\"edi\":%u,\"pc\":%u,\"teb\":%u},\n", cpu->eax, cpu->ecx, cpu->edx, cpu->ebx, cpu->esp, cpu->ebp, cpu->esi, cpu->edi, cpu->pc, cpu->teb);
    fprintf(jf, "  \"heap_next\": %u,\n  \"nthreads\": %d,\n  \"nhandles\": %d,\n  \"tls_next\": %u,\n", c->heap_next, c->nthreads, c->nhandles, c->tls_next);
    fprintf(jf, "  \"modules\": [");
    for (int i = 0; i < c->nmods; i++) fprintf(jf, "%s{\"name\":\"%s\",\"base\":%u,\"size\":%u,\"entry\":%u}", i ? "," : "", c->mods[i].name, c->mods[i].base, c->mods[i].size, c->mods[i].entry);
    fprintf(jf, "],\n  \"stubs\": [\n");
    for (int i = 0; i < c->nstubs; i++) fprintf(jf, "    {\"i\":%d,\"dll\":\"%s\",\"name\":\"%s\",\"nargs\":%d,\"cdecl\":%d,\"impl\":%d}%s\n", i, c->stubs[i].dll, c->stubs[i].name, c->stubs[i].nargs, c->stubs[i].cdecl_, c->stubs[i].fn != NULL, i + 1 < c->nstubs ? "," : "");
    fprintf(jf, "  ],\n  \"runs\": [\n");
    for (int i = 0; i < nr; i++) fprintf(jf, "    [%u,%u,%llu]%s\n", runs[i].addr, runs[i].len, (unsigned long long)runs[i].off, i + 1 < nr ? "," : "");
    fprintf(jf, "  ]\n}\n"); fclose(jf);
    fprintf(stderr, "[snapshot] %s: %d runs, %llu non-zero pages (%.1f MB), state %zu bytes, %d stubs, %d modules, %d threads, %d handles%s\n", dir, nr, (unsigned long long)total_nz,
            (double)off / 1048576.0, o.n, c->nstubs, c->nmods, c->nthreads, c->nhandles - 1, bad ? "  [WARNINGS]" : "");
    free(runs);
}
