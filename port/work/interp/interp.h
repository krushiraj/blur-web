// x86-32 user-mode interpreter: cold-code fallback for the static recompiler (see README.md).
// It works directly on the recompiler's Cpu (rt.h): GPRs, lazy-free flags in c->fl / c->df, x87 as double, xmm/mm, TEB via c->teb.
#pragma once
#include "rt.h"

// Host policy: return non-zero when control must be handed back to the host at `pc` (recompiled function entry, API stub,
// thread-exit magic ...).  Called once per code address (the answer is cached with the decoded instruction), so it may be slow
// (a hash lookup).  Addresses >= interp_stub_floor are always handed back without calling it.
typedef int (*InterpIsNative)(uint32_t pc);
void interp_init(InterpIsNative is_native);            // also (re)creates the decode cache
void interp_flush(void);                               // drop all decoded instructions (call after the guest rewrote/loaded code)
void interp_flush_range(uint32_t lo, uint32_t hi);     // drop decoded instructions in [lo, hi)
extern uint32_t interp_stub_floor;                     // default 0x7f000000 (API stub area)
extern int32_t  interp_slice;                          // instructions per interp_run call (default 1 << 20)
extern uint64_t interp_icount;                         // total guest instructions interpreted (statistics)
typedef int (*InterpIntHook)(Cpu *c, int vec);         // `int3` (vec 3) and `int n`: return non-zero if handled (execution continues after the insn)
extern InterpIntHook interp_int_hook;                  // default NULL -> rt_unsupported
extern void (*interp_call_hook)(uint32_t target, uint32_t ret_addr);   // optional: called for every interpreted `call` (cold-function coverage: feeds the hot-set selection)

enum { INTERP_NATIVE = 0, INTERP_BUDGET = 1, INTERP_PREEMPT = 2 };
// Run guest code from c->pc.  Returns when c->pc is a native/stub address (INTERP_NATIVE), the instruction slice is used up (INTERP_BUDGET) or the thread's
// time slice is used up (INTERP_PREEMPT: c->budget went negative at a call; the host reacts exactly as it does when a lifted function's entry check fires,
// i.e. yields to the other guest threads and refills c->budget).  In all cases c->pc is the next instruction and every register is in *c, so the host can
// dispatch again at any time.  Never call it with a native c->pc.
int  interp_run_n(Cpu *c, int32_t max_insns);
void interp_run(Cpu *c);                               // interp_run_n(c, interp_slice)

// Test-only (build with -DINTERP_ENTRY_ACCOUNTING): charge c->budget at every instruction for which interp_is_entry(pc) is true -- the entry points of the lifted
// functions of a fully recompiled build -- instead of per call, so that thread time slices (and with them the whole schedule) are identical whether a function runs lifted or interpreted.
// On INTERP_PREEMPT the entry instruction has not been executed yet and is charged again when the run resumes: the host must refill the budget to its start value + 1.
#ifdef INTERP_ENTRY_ACCOUNTING
extern int (*interp_is_entry)(uint32_t pc);
extern int interp_charge_trace;                                      // debugging: print the next N charged entries
extern unsigned (*interp_entry_internal)(uint32_t fn, uint32_t pc);   // optional: bit0 = pc lies inside the body of the lifted function `fn`, bit1 = it also has a label there
#endif
