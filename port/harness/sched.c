// Cooperative green-thread scheduler on top of a single Unicorn engine.
// Threads switch when an API handler asks to block (it is re-run later until its condition holds)
// or after SLICE instructions, so a spinning thread cannot starve the others.
#include "emu.h"
#include <time.h>
#include <unistd.h>

static uint32_t SLICE_N = 200000u;
#define SLICE SLICE_N

static void set_fs(Ctx *c, uint32_t teb) {
    wr16(c, GDT_ADDR + 24 + 2, teb & 0xffff); wr8(c, GDT_ADDR + 24 + 4, (teb >> 16) & 0xff); wr8(c, GDT_ADDR + 24 + 7, teb >> 24);
    int sel = 0x18; uc_reg_write(c->uc, UC_X86_REG_FS, &sel);
}

static void finish_thread(Ctx *c, Thread *t, uint32_t code) {
    t->done = 1; t->exit_code = code;
    if (t->handle) c->handles[(t->handle - 0x100) / 4].signaled = 1;
    if (c->verbose) fprintf(stderr, "[thread] tid=%x finished code=%u\n", t->tid, code);
}

void run_scheduler(Ctx *c) {
    if (getenv("BLUR_SLICE")) SLICE_N = (uint32_t)strtoul(getenv("BLUR_SLICE"), 0, 0);
    c->api_slice = getenv("BLUR_API_SLICE") ? (uint32_t)atoi(getenv("BLUR_API_SLICE")) : 0;
    if (c->api_slice && !getenv("BLUR_SLICE")) SLICE_N = 20000000u;     // mostly API-boundary preemption; instruction slice is only a fallback for pure spin loops
    uc_engine *uc = c->uc; uint64_t idle_start = 0, t_end = getenv("BLUR_SECONDS") ? real_us() + (uint64_t)atof(getenv("BLUR_SECONDS")) * 1000000ull : 0;
    while (!c->exited) {
        if (t_end && real_us() > t_end) { c->exited = 5; break; }
        int alive = 0, progressed = 0, base = c->cur, nt = c->nthreads;      // fixed base: c->cur changes while we iterate
        for (int k = 0; k < nt && !c->exited; k++) {
            int i = (base + 1 + k) % nt; Thread *t = &c->threads[i];
            if (!t->used || t->done) continue;
            alive++;
            if (t->suspended) continue;
            c->cur = i; set_fs(c, t->teb);
            if (!t->started) {
                t->started = 1;
                if (i == 0) {                                   // main thread: stack and TEB already set up
                    uint32_t esp = STACK_TOP - 0x100 - 4; wr32(c, esp, RET_MAGIC);
                    uc_reg_write(uc, UC_X86_REG_ESP, &esp);
                } else {
                    uint32_t esp = t->stack_hi - 0x40; esp -= 4; wr32(c, esp, t->param); esp -= 4; wr32(c, esp, RET_MAGIC);
                    uc_reg_write(uc, UC_X86_REG_ESP, &esp);
                    uint32_t z = 0; int regs[] = {UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP};
                    for (int r = 0; r < 7; r++) uc_reg_write(uc, regs[r], &z);
                }
                uc_reg_write(uc, UC_X86_REG_EIP, &t->entry);
                uc_context_alloc(uc, &t->ctx);
            } else uc_context_restore(uc, t->ctx);
            uint32_t eip; uc_reg_read(uc, UC_X86_REG_EIP, &eip);
            uint64_t before = c->api_progress;
            t->blocked = 0;
            uc_err e = uc_emu_start(uc, eip, RET_MAGIC, 0, SLICE); c->slices++;
            if (c->oep_hit) { rt_main(c); return; }
            uc_reg_read(uc, UC_X86_REG_EIP, &eip);
            uc_context_save(uc, t->ctx);
            if (c->exited) break;
            if (e != UC_ERR_OK) { fprintf(stderr, "uc_emu_start(thread %d): %s\n", i, uc_strerror(e)); c->exited = 3; break; }
            if (eip == RET_MAGIC) {
                uint32_t eax; uc_reg_read(uc, UC_X86_REG_EAX, &eax);
                finish_thread(c, t, eax);
                if (i == 0) { c->exited = 1; c->exit_code = eax; }
                progressed = 1; continue;
            }
            if (!t->blocked || c->api_progress != before) progressed = 1;   // ran instructions or completed an API call
        }
        if (!alive) break;
        if (progressed) { idle_start = 0; continue; }
        if (!idle_start) idle_start = real_us();
        if (real_us() - idle_start > 20000000ull) {
            fprintf(stderr, "\n[DEADLOCK] all threads blocked for 20s:\n");
            for (int i = 0; i < c->nthreads; i++) if (c->threads[i].used && !c->threads[i].done && c->threads[i].ctx) {
                uint32_t eip = 0; uc_context_reg_read(c->threads[i].ctx, UC_X86_REG_EIP, &eip);
                fprintf(stderr, "  thread %d tid=%x blocked at %08x (stub %s)\n", i, c->threads[i].tid, eip,
                        (eip >= STUB_BASE && eip < STUB_BASE + STUB_SLOTS * 16) ? c->stubs[(eip - STUB_BASE) / 16].name : "-");
            }
            c->exited = 4; break;
        }
        usleep(200);                                            // everyone is sleeping/waiting: let wall-clock time pass
    }
    if (getenv("BLUR_THREADS")) {
        fprintf(stderr, "[perf] %llu slices of up to %u instructions\n", (unsigned long long)c->slices, SLICE);
        extern uint32_t ring[64]; extern int ring_n; fprintf(stderr, "[blocks] last executed: "); for (int i = 64; i >= 1; i--) fprintf(stderr, "%x ", ring[(ring_n - i) & 63]); fprintf(stderr, "\n");
        fprintf(stderr, "[threads]\n");
        for (int i = 0; i < c->nthreads; i++) if (c->threads[i].used && !c->threads[i].ctx) fprintf(stderr, "  #%d tid=%x entry=%08x NOT STARTED suspended=%d done=%d\n", i, c->threads[i].tid, c->threads[i].entry, c->threads[i].suspended, c->threads[i].done);
        for (int i = 0; i < c->nthreads; i++) if (c->threads[i].used && c->threads[i].ctx) {
            Thread *t = &c->threads[i]; uint32_t eip = 0, esp = 0, ret = 0; uc_context_reg_read(t->ctx, UC_X86_REG_EIP, &eip); uc_context_reg_read(t->ctx, UC_X86_REG_ESP, &esp);
            int in_stub = eip >= STUB_BASE && eip < STUB_BASE + STUB_SLOTS * 16; if (in_stub) ret = rd32(c, esp);
            char extra[160] = "";
            if (in_stub) {
                const char *nm = c->stubs[(eip - STUB_BASE) / 16].name; uint32_t h0 = rd32(c, esp + 4), h1 = rd32(c, esp + 8);
                if (!strcmp(nm, "WaitForSingleObject") || !strcmp(nm, "SignalObjectAndWait")) {
                    uint32_t h = !strcmp(nm, "SignalObjectAndWait") ? h1 : h0; Handle *o = (h >= 0x104 && (h - 0x100) / 4 < (uint32_t)c->nhandles) ? &c->handles[(h - 0x100) / 4] : NULL;
                    snprintf(extra, sizeof extra, " h=%x type=%d sig=%d man=%d owner=%x cnt=%d", h, o ? o->type : -1, o ? o->signaled : -1, o ? o->manual : -1, o ? o->owner : 0, o ? o->count : -1);
                }
            }
            fprintf(stderr, "  #%d tid=%x entry=%08x %s eip=%08x %s ret=%08x wake=%llu%s\n", i, t->tid, t->entry, t->done ? "DONE" : t->suspended ? "SUSP" : "live", eip,
                    in_stub ? c->stubs[(eip - STUB_BASE) / 16].name : "", ret, (unsigned long long)t->wake_at, extra);
        }
    }
}

uint32_t cur_esp(Ctx *c) {
    if (g_recomp && c->oep_hit) return ((uint32_t *)cur_thread(c)->cpu)[4];      // Cpu.esp (eax, ecx, edx, ebx, esp, ...)
    uint32_t esp; uc_reg_read(c->uc, UC_X86_REG_ESP, &esp); return esp;
}
