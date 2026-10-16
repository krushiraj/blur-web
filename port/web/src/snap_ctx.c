// The single description of the host state that must survive a snapshot (see snap.h). Only plain values are visited; every pointer
// (uc engine, FILE*, handler functions, host threads) is rebuilt on restore: handlers are looked up again by API name.
#include "snap.h"
#include "rt.h"

#define V(x) cb(u, &(x), sizeof(x))

static void v_cpu(Cpu *p, snap_cb cb, void *u) {
    V(p->eax); V(p->ecx); V(p->edx); V(p->ebx); V(p->esp); V(p->ebp); V(p->esi); V(p->edi); V(p->teb); V(p->df); V(p->pc); V(p->budget);
    V(p->fl.cf); V(p->fl.zf); V(p->fl.sf); V(p->fl.of); V(p->fl.pf); V(p->fl.af);
    V(p->st); V(p->top); V(p->mxcsr); V(p->fcw); V(p->fsw); V(p->xmm); V(p->mm); V(p->last_eflags);
}

void snap_visit(Ctx *c, snap_cb cb, void *u) {
    uint32_t magic = SNAP_MAGIC, ver = SNAP_VERSION; V(magic); V(ver);
    V(c->req); V(c->nthreads); V(c->cur); V(c->nhandles); V(c->yield_req); V(c->slices); V(c->slice_full); V(c->api_since_switch); V(c->api_slice);
    V(c->api_progress); V(c->block); V(c->nstubs); V(c->nmods); V(c->heap_next); V(c->exited); V(c->exit_code); V(c->last_error); V(c->tls_next);
    V(c->oep_hit); V(c->oep_regs);
    for (int i = 0; i < c->nmods; i++) V(c->mods[i]);
    for (int i = 0; i < c->nstubs; i++) { Stub *s = &c->stubs[i]; V(s->dll); V(s->name); V(s->nargs); V(s->calls); V(s->raw); V(s->cdecl_); }
    for (int i = 1; i < c->nhandles; i++) { Handle *h = &c->handles[i]; V(h->type); V(h->signaled); V(h->manual); V(h->owner); V(h->count); V(h->max); V(h->thread); V(h->pat); }
    for (int i = 0; i < c->nthreads; i++) {
        Thread *t = &c->threads[i];
        V(t->cont); V(t->ncont); V(t->used); V(t->started); V(t->done); V(t->suspended); V(t->blocked); V(t->sig_done); V(t->spin_ret); V(t->spin_addr); V(t->spin_n);
        V(t->teb); V(t->stack_lo); V(t->stack_hi); V(t->entry); V(t->param); V(t->tid); V(t->exit_code); V(t->wake_at); V(t->handle); V(t->rt_started);
        V(t->last_ret); V(t->last_a0); V(t->last_a1); V(t->n_api); V(t->last_api_us);
        uint32_t has_cpu = t->cpu != NULL; V(has_cpu);
        if (has_cpu) { if (!t->cpu) t->cpu = calloc(1, sizeof(Cpu)); v_cpu((Cpu *)t->cpu, cb, u); }
    }
    heap_snap(cb, u); hle_snap(cb, u); hle2_snap(cb, u); win_snap(cb, u); com_snap(cb, u);
    V(g_time_base); V(g_det_ticks);
}
