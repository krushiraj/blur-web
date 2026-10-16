# x86-32 interpreter: cold-code fallback for the recompiled game

A small, fast, WebAssembly-friendly x86-32 user-mode interpreter in C (`interp.c`, ~800 dense lines, no dependencies except the lifter's own `rt.h` / `rt_vec.c`).
It works **directly on the recompiler's `Cpu`**: GPRs, `fl` (the 6 flag bytes) + `df`, `pc`, `teb` (FS base), x87 stack as `double` + `top`/`fcw`/`fsw`,
`xmm[8][16]`, `mm[8]`, `mxcsr`, guest memory through `G`/`M8..M64`.  Recompiled functions and the interpreter hand control to each other at function boundaries
(call / ret / indirect jmp all go through the host's `rt_dispatch`), with exactly the lifter's conventions: a `call` pushes the return address, `ret` pops it, there is no
shadow stack, flags live in `c->fl`.  The decoder is hand-written (table driven, own semantics, no third-party code, so no licence question).

Purpose (PLAN Phase 3 / F19g): code that is **not** in the recompiled hot set becomes *slow* instead of *fatal*: no "learn loop + rebuild", and the wasm module can ship
only the hot set.

## Files

| file | what |
|---|---|
| `interp.h`, `interp.c` | the interpreter (decoder + executors + run loop) |
| `interp_glue.c` | `rt_cold()` for the existing `#ifdef BLUR_COLD` hook in `port/harness/rt.c` and `port/web/src/rt_wasm.c` |
| `difftest.c` | differential tester vs Unicorn (single instructions, function corpus, mixed lifted/interpreted mode) |
| `unit_test.c` | hand-checked tests for the host interface and for instructions the reference cannot judge |
| `tools/` | `make_samples.py` (real instruction encodings from the game), `declen.py` (decoder length check vs capstone on 3.4 M instructions), `mnem_hist.py` |
| `mixed/` | `gen_hot.py` + generated `hot.c` (real lifter output for half of the corpus) for the mixed-mode test |
| `bench/` | throughput benchmark (native and `emcc`) |
| `run_all.sh`, `Makefile` | everything below is reproducible with `./run_all.sh` (20-30 min on 4 cores) |
| `results/` | output of the last full run |
| `game/` | the interpreter inside the real game: reduced hot sets, native + wasm builds, equivalence/speed/cold-statistics scripts and logs (see "In-game tests") |
| `patches/` | `harness-rt-cold.patch`, `web-rt_wasm-cold.patch` (apply), `harness-test-instrumentation.patch` (optional) |

## API (`interp.h`)

```c
void interp_init(int (*is_native)(uint32_t pc));    // host policy; (re)creates the decode cache
int  interp_run_n(Cpu *c, int32_t max_insns);       // INTERP_NATIVE / INTERP_BUDGET / INTERP_PREEMPT
void interp_run(Cpu *c);                            // interp_run_n(c, interp_slice)  (default slice 1<<20 instructions)
void interp_flush(void); void interp_flush_range(uint32_t lo, uint32_t hi);   // after the guest rewrote code
extern uint32_t interp_stub_floor;                  // addresses >= this (default 0x7f000000, the API stubs) are always handed back
extern int (*interp_int_hook)(Cpu *, int vec);      // int3 / int n; default: rt_unsupported
extern void (*interp_call_hook)(uint32_t target, uint32_t ret);   // every interpreted call -> cold-function coverage for hot-set selection
extern uint64_t interp_icount;                      // statistics
```

`interp_run_n` executes from `c->pc` until

* `c->pc` is an address for which `is_native(pc)` is true (recompiled function entry, API stub, `RET_MAGIC`...) -> `INTERP_NATIVE`.  `is_native` is asked **once per code
  address** (the answer is cached with the decoded instruction), so a hash lookup is fine;
* the instruction budget is used up -> `INTERP_BUDGET`;
* the thread's time slice (`c->budget`, decremented per `call` just like the lifted code does per function entry) goes negative -> `INTERP_PREEMPT`.

In all cases every register is in `*c` and `c->pc` is the next instruction, so the host can do anything (switch threads, run an API handler) and call again later.
`cpuid` / `rdtsc` call the host's `rt_cpuid` / `rt_rdtsc` (same functions the lifted code uses); `pause` and a failed `cmpxchg` call `rt_spin`; unsupported or faulting
instructions (hlt, in/out, div by zero, ...) call `rt_unsupported(c, address, what)` with `c->pc` = the faulting instruction.

### Host integration

The existing dispatchers already have the hook (`BLUR_COLD`, `rt_cold`).  Add one predicate next to `lookup()` and compile `interp.c`, `interp_glue.c`
(+ `../recomp/rt_vec.c`, which the lifted code needs anyway):

```c
int rt_is_native(uint32_t pc) { return pc - STUB_BASE < STUB_SLOTS * 16u || pc == RET_MAGIC || lookup(pc) != NULL; }

// rt_dispatch, "address not in the table" branch (harness/rt.c, web/src/rt_wasm.c):
GuestFn f = lookup(pc);
if (!f) {
    int rc = rt_cold(cpu);          // interp_glue.c: runs the interpreter until control reaches compiled code / a stub / RET_MAGIC
    if (rc == 1) continue;
    if (rc == 2) return;            // wasm only (build interp_glue.c with -DINTERP_GLUE_WASM): time slice over -> back to the scheduler loop, rt_yield_flag set
    rt_missing(cpu, pc); return;
}
```

For the single-threaded wasm build `rt_cold` returns 2 on `INTERP_PREEMPT` after setting `cpu->budget = 200000; rt_yield_flag = 1;` (the same effect the transformed
lifted prologue has); in the native pthread harness it calls `rt_preempt(cpu)` (blocks until the thread's turn).  `G` may be the constant-0 macro of the wasm build.

## Semantics

* Integer ISA: everything seen in the game and msvcr90 (ALU, shifts/rotates incl. rcl/rcr/shld/shrd, mul/imul/div/idiv in all sizes, bt*/bs*, string ops with rep/repe/repne,
  cmov/setcc, cmpxchg/cmpxchg8b/xadd/xchg, 8/16/32-bit forms, FS segment override incl. `fs:[moffs]`, pushad/popad, pushfd/popfd, lahf/sahf, enter(0)/leave, bswap, xlat, loop/jecxz...).
* Flags are computed eagerly with the **lifter's own helpers** (`fl_add`, `fl_shl`, ... from `rt.h`), so undefined flags are defined the same way as in lifted code.
* x87: the lifter's model (`double` registers, `top`, no tag word, `fcw` rounding control honoured by `fist*`/`frndint`, `fcom*` -> `fsw` C0/C2/C3, `fcomi*` -> `fl`).
  All D8-DF forms of the lifter plus fsave/frstor, fxtract, fyl2xp1, fdecstp/fincstp, fisttp.  `fbld/fbstp` and the legacy fnop-like encodings trap.
* MMX / SSE / SSE2 (every form in `sse_oracle.py` and the rest of the 0F map used by compilers, plus movddup/movsldup/movshdup, fxsave/fxrstor, ldmxcsr/stmxcsr):
  scalar/packed float arithmetic is the host's IEEE float/double (like the lifted code), integer vector ops use `v_binop`/`v_shift` from `rt_vec.c`.  MXCSR rounding mode
  is not modelled (same as the lifter).

## Verification (all against Unicorn 2.1 as the reference; everything runs from `run_all.sh`)

**Method.** Unicorn (`brew install unicorn`, QEMU/TCG based) is run on the identical state; `difftest.c` compares eip, GPRs, the defined flags, xmm/mm/x87 state, the stack window, the
scratch memory and the TEB page.  Floating point results are compared exactly except: x87 values with a 1e-9 relative tolerance (80-bit hardware vs `double` model; with the game's
control word 0x27f the arithmetic is bit-exact in practice), NaN-vs-NaN, and flags that the architecture leaves undefined (`flag_mask()` in `difftest.c`).

1. **Single instructions, random encodings** (`difftest single`): random legacy prefixes (66/F2/F3/F0/FS), every one-byte and 0F-map opcode, random modrm/SIB/displacement/immediates, random
   registers (pointer-rich), flags (incl. DF), xmm (floats, doubles, specials), mm, x87 stack, fcw, 64 KB of random scratch memory, random stack.  The reference's instruction length must equal the
   interpreter's (the next eip is compared).
2. **Single instructions, real encodings** (`difftest single ... samples.bin`): up to 300 encodings per (mnemonic, operand shape) taken from the game's `.text` and msvcr90 (831 forms, 96 k
   distinct encodings) with random state.  `tools/forms_report.py` lists the forms with no successful trial and why (`results/forms_report.txt`).
3. **Decoder lengths** (`tools/declen.py`): every instruction capstone finds in the game and msvcr90 `.text` (3,376,745) gets the same length from `interp.c`; the 58 differences are 0x67-prefixed
   (16-bit addressing), `mov cr/dr`, VEX and 66-prefixed rel16 jumps, which are rejected on purpose.
4. **Function corpus** (`difftest funcs`): the 399 + 500 functions of `oracle_funcs.txt` / `oracle_funcs_fp.txt` (the same lists the lifter's `oracle.c` uses) are run with random registers, stack
   arguments and memory until they return to a magic address; the whole call tree is interpreted and compared with Unicorn (registers, xmm, x87 stack, stack window, scratch memory).
5. **Mixed mode** (`difftest_mixed`): half of the corpus is translated by the real lifter (`mixed/gen_hot.py` -> `mixed/hot.c`, 399 functions) and linked in, the rest is interpreted; the
   dispatcher in `difftest.c` is the production design (`lookup` -> lifted function, otherwise `interp_run_n` until the next compiled address).  This exercises the hand-over in both directions
   (calls, returns, flags in `c->fl`, `df`, budget) hundreds of thousands of times.
6. `unit_test.c`: 56 hand-checked assertions for the host interface (native exit, call/ret, hooks, FS/TEB, preempt, pause, flush, fsave/fxsave round trips, enter/leave, cmpxchg8b ...).

(The function-level and mixed-mode rows come from one complete `run_all.sh`; the two single-instruction rows were re-run with the identical commands after `popfd` was added to the out-of-scope list because Unicorn aborted on it.)

**Results of the last full run (`./run_all.sh`, files in `results/`; "reference could not run it" = random state faulted or looped in Unicorn, the trial is dropped, not counted):**

* single-instruction, random encodings: ok=1200000 bad=0 (reference could not run it: 553942)
* single-instruction, real encodings from the game / msvcr90: ok=1200000 bad=0 (reference could not run it: 353624)
* fxsave (dedicated): ok=20000 bad=0 (reference could not run it: 0)
* function corpus oracle_funcs.txt (integer): ok=156591 bad=0 (reference faulted/timed out: 483409)
* function corpus oracle_funcs_fp.txt (x87/SSE): ok=190686 bad=0 (reference faulted/timed out: 609313)
* mixed: half lifted C / half interpreted: ok=347866 bad=0 (reference faulted/timed out: 1090534)
* function trials that differ only by NaN payload/sign in memory: 37; trials whose first divergence was the QEMU sNaN-quieting quirk: 0; skipped because the random state sent the function into data and the interpreter trapped there: 1
* distinct opcode keys (prefix class + opcode [+ /group] [+ x87 register form]) with at least one successful trial: 2992
* unit.txt: 56 checks, 0 failures
* declen.txt: instructions 3376745 | LENGTH MISMATCHES 58 | add 27 ('6700b84867', 7, 5)
* throughput_cached: interpreter-only time 0.021s for 3020185 guest instructions = 145.3 Minsn/s (real game functions, random inputs)
* throughput_nocache: interpreter-only time 0.020s for 3020185 guest instructions = 149.2 Minsn/s (real game functions, random inputs)

Single-instruction trials: **2,400,000 compared trials (1.2 M random encodings + 1.2 M real encodings), 0 mismatches**, plus 20,000 fxsave.  Function level: **347,277 compared function runs in the
corpus (integer 156,591 + x87/SSE 190,686), 0 mismatches**, and 347,866 in mixed lifted/interpreted mode, 0 mismatches.  Per-form coverage of the real-code sample set
(`results/forms_report.txt`): 831 (mnemonic, operand-shape) forms occur in the game and msvcr90; 688 got at least one successful trial and none mismatched; of the remaining 143, 104 are out of scope
(privileged / BCD / far / segment forms, see below) and 37 are rare forms whose few sample encodings always fault in the reference (stores to the read-only image, `fs:` + absolute address);
their opcodes are covered by sibling forms and by the random-encoding fuzz (2,992 distinct prefix/opcode/group keys with a successful trial).


**What is excluded from the comparison, and why** (all of this is in `difftest.c`, nothing is hidden in the counts):

* *Out-of-scope instructions* are not generated (`denied_code()`): port I/O, cli/sti/hlt, int n, far control transfers and segment-register loads, BCD adjust (daa/das/aaa/aas/aam/aad/salc),
  bound/arpl, enter with nesting, 0x67/GS prefixed forms, 66-prefixed control flow/pushf/pusha/leave/ret, fbld/fbstp, fnstenv/fldenv/fnsave/frstor (tag word and pointers are not modelled; round trips
  are in `unit_test.c`), SSE3+, tzcnt/lzcnt/popcnt (F3 0F BC/BD/B8 execute as `rep bsf` on pre-BMI CPUs), mov-to/from-segment selectors.  The lifter's own `GARBAGE` list is the same set.
* *Encodings that QEMU executes although hardware raises #UD* (register-only SSE/MMX forms with a memory modrm, undefined x87 registers forms, `8f /1-7`, double mandatory prefixes ...) are skipped.
* *Reference-implementation (QEMU softfloat/x86 translator) behaviour that differs from hardware and from the lifter*, found while building this and skipped or tolerated: `ret imm16` sign-extends
  imm16; `lock neg/not` produce wrong flags; `fld m32` leaves a signalling NaN unquieted (hardware quiets); `fyl2x`/`fyl2xp1`/`fptan`/`fsin` ... outside their domain do not pop/behave as hardware;
  `fisttp` overflow stores 0x7fff... instead of the indefinite; `fcomi` leaves OF/SF/AF; `fprem`/`fprem1` are inexact for large quotients; locked `bts/btr/btc` with a far register offset.
  Unicorn also gets stuck (halted/exception state) after some guest events, and `popfd` with TF/NT bits from random data aborts the whole process (popfd is therefore tested in `unit_test.c` only):
  `difftest.c` probes and rebuilds the engine when it stops executing.
* *Undefined flags* (`mul/imul/div/idiv/shifts/rotates/bt*/bsf/bsr/shld/shrd` as architecturally undefined) are masked per instruction.
* *NaN payload / sign*: x86 returns the negative default QNaN for invalid SSE/x87 operations; the interpreter does the same (`nanf_fix`).  NaN-vs-NaN differences in propagated payloads are tolerated
  in memory/xmm comparisons.


## In-game tests: the interpreter inside the real game (`game/`)

Everything above verifies instructions and functions against Unicorn.  `game/` runs the **whole game** with most of its code interpreted and compares it with the fully recompiled
build.  Setup: a copy of the integrator's merged harness (`game/harness`, `Makefile.interp`) + the cold hook (`patches/harness-rt-cold.patch`), hot sets cut from
`port/recomp/cover_showroom.txt` by `game/reduce.py` (a hash-random N % of the covered functions; direct calls to removed functions are rewritten to `rt_dispatch`, so they really run
in the interpreter -- `hot_extract.py`'s closure over direct references would pull them back in), one binary per set (`irt_<set>`; **not** named `blurrt*`, other agents `pkill blurrt`).

| set | lifted functions | 
|---|---|
| `full` | all 373,748 (baseline: `obj_p2` + the integrator's `obj_x`) |
| `all` | the 45,964 functions of `cover_showroom.txt` (rest interpreted) |
| `h90` / `h60` / `h30` | 41,393 / 27,622 / 13,758 of them (hash-random) |
| `crt` | msvcr90 functions of the coverage list only |
| `none` | nothing: the game runs from the OEP entirely in the interpreter |

**Exact equivalence needs exact time slices.**  Guest threads are preempted when `c->budget` (200,000) runs out; lifted code charges one unit per *function entry*, the interpreter per
`call`, so the first preemption falls at a different place and the API-call sequences drift apart (wasm-core F96 saw this).  `interp.c -DINTERP_ENTRY_ACCOUNTING` (test regime; the host
supplies the entry points of the fully recompiled build: `game/gen_entries.py` -> `entries_native.c`, `entry_acct.c`) charges the budget at exactly the instructions where a lifted function
would be entered: an entry is not charged when it is reached by `goto` inside the lifted function that contains it (direct jump to an address in the body, or an indirect jump to a labelled
address), always when reached by call, ret or tail jump; after `INTERP_PREEMPT` the glue refills the budget to start+1 because the entry is charged again on resume.  With it the
thread schedule, and therefore the whole API-call sequence, is identical for every mixture of lifted and interpreted code.

Results (native, deterministic clock `BLUR_DET=50`, real preemption, API log `n thread name arg0` compared; arg1 is uninitialised stack garbage that differs between any two runs):

| set | API calls | Presents | result vs `full` | wall (shared host) | interpreted instructions (stats run) | interpreter entries from compiled code |
|---|---|---|---|---|---|---|
| `full` | 8,000,000 | 3112 | - | 29 s | 0 | 0 |
| `all` | 8,000,000 | 3112 | IDENTICAL | 70 s | 5.05 G | 42.1 M |
| `h90` | 8,000,000 | 3112 | IDENTICAL | 54 s | - | - |
| `h60` | 8,000,000 | 3112 | IDENTICAL | 51 s | - | - |
| `h30` | 8,000,000 | 3112 | IDENTICAL | 52 s | 5.39 G | 23.0 M |
| `crt` | 8,000,000 | 3112 | IDENTICAL | 46 s | - | - |
| `none` | 8,000,000 | 3112 | IDENTICAL | 46 s | 5.56 G | 7.6 M |

Every set reproduces the full build's API-call sequence over 8 M calls (about 27 s of game time: boot, loading, intro, showroom with 3112 Presents), with thread switches at the same
calls.  (Without the entry accounting `crt` diverged at call 52,262; each fix of the accounting rules moved the first divergence (98,785, 120,671, ...) until none was left -- `budget_div.sh` found each one.)

* **Wasm** (`game/web`, `build_wasm.sh`, `run_wasm_sets.sh`): the same sets built as WebAssembly + interpreter (`emcc -O1 -mtail-call`; interpreter `-Os`), run under node from one stable snapshot with the
  deterministic clock and the entry accounting: `none` (everything interpreted), `all`, `h90`, `h60`, `h30`, `crt` produce the **identical 2,000,000-call API log** (wasm interpreter 117k - 140k API
  calls/s for `none`).  The integrator's full wasm build (92 k lifted functions) stops at call 95,014 with `jump to code that was not compiled into this module: 01e8d20d`; `none` is identical to
  it for the 95,000 calls before and simply continues -- that is the interpreter doing its job in a build that has a real hot-set gap.
* **Frame times** (steady-state showroom, presents 1000-3000 of the 8 M-call run; the laptop was shared with other jobs, so only the ratios mean something):

| set | mean | median | p99 | max |
|---|---|---|---|---|
| `full` | 6.1 ms | 5.5 | 16.8 | 25 |
| `all` | 20.7 ms | 15.4 | 94 | 196 |
| `h90` | 16.0 | 12.7 | 65 | 147 |
| `h60` | 13.7 | 10.6 | 55 | 133 |
| `h30` | 14.8 | 12.6 | 53 | 312 |
| `crt` | 12.9 | 10.3 | 48 | 133 |
| `none` (everything interpreted) | 13.6 | 10.8 | 53 | 133 |

  Frame time is dominated by the host side (D3D9/Win32 HLE, ~6 ms) plus the guest's idle/worker-thread polling; interpreting *all* guest code costs about 2.2x per frame, not 20x.  `all`/`h90`
  are slower than `none` because every compiled-to-interpreted transition goes through `rt_dispatch` + `rt_cold` (42 M such entries in the run vs 7.6 M for `none`): a half-and-half
  mixture of tiny hot and cold pieces is the worst case, which is why promotion should be by *instruction share* of whole call trees, not by individual call counts.
* **Cold-function statistics** (`BLUR_COLDSTAT=<file>`, `game/cold_report.py`):

  `none` = the game's complete interpreted profile over the 8 M-call run: 33.4 M calls to 9,028 distinct functions, 5.56 G instructions (steady state: median 0.6 M cold calls/s and 131 M interpreted
  instructions/s, 52 *new* cold functions/s while the run warms up, 628 new ones in the second half).  **Promotion by instruction share**: 1 function (`0x00adfa10`, a long match-search loop (probably zlib `longest_match`)
  used by the loading phase, 6,848 calls x ~360 k instructions) is 55 % of all interpreted instructions; 20 functions are 90 %, 102 functions 99 %, 290 functions 99.9 %.  **By call count**: functions with >= 1,000 interpreted calls
  per run are 687 (99.1 % of calls), >= 10,000 are 247 (94 %), >= 100,000 are 55 (73.5 %).  With the showroom coverage list lifted (`all`), 5.05 G instructions are *still* interpreted, 72 % of
  them in `0x00adfa10` -- the recorded sessions never ran the loading phase compute.  Recommendation for the wasm hot set: keep the coverage list, add (a) every function with >= ~1,000 interpreted calls or
  >= 0.01 % of the interpreted-instruction samples in a profile run (about 300-700 functions), (b) re-profile after each new front-end state; everything else stays interpreted.  Raw data:
  `game/logs/st_none.cold(.samples)`, `st_all.*`, `st_h30.*`, per-second rates in `game/logs/cold_rates.txt`.
* **Protector handler range 0x206dd00-0x206ea00 and the ten `learned_private.txt` targets**: not reached by any run in this environment (8 M deterministic calls and a 100 s real-time run with real Bink on a
  build that has *only* `obj_p2`: zero cold hits), so no in-game proof is possible; instead `difftest stream` replays the code from all 243 chunk starts of the range and the 11 targets (10 + `01e8d20d`) with random
  registers/stack (stack seeded with handler addresses so ret-dispatch chains continue) one instruction at a time against Unicorn: 9,815 trials, 25,190 instructions, 0 mismatches (the reference faults
  quickly on random heap pointers, so the depth is shallow).  `01e8d20d`, a real gap of the wasm hot set, is covered by the wasm result above.

Reproduce (`game/`): `cd harness && make -f Makefile.interp -j4 base`, then `build_set.sh <name> <spec>` (hot set + binary), `run.sh <set> <tag> <N>` (deterministic run, logs in
`game/logs`), `cmp_logs.sh`, `run_sets.sh` (all sets), `budget_div.sh` (finds the first function entry where the thread budget of a set differs from the baseline -- how the accounting rules above were
derived), `build_wasm.sh`/`run_wasm.sh`/`run_wasm_sets.sh`.

### Integration status and what the integrator needs to do

* `patches/harness-rt-cold.patch` (applies to `port/work/integrate/harness.rebased`): `rt_is_native()`, `rt_dispatch` calls `rt_cold()`, `BLUR_NOPREEMPT`, Makefile links `interp.c` + `interp_glue.c` (`BLUR_COLD=0` at run time turns the
  fallback off).  `patches/harness-test-instrumentation.patch`: the logging used above (budget log, Present timestamps, entry counting, `rt_api_count`) -- optional.
* `patches/web-rt_wasm-cold.patch`: wasm-core already merged the hook (`rt_is_native`, `rc == 2`); this adds `BLUR_NOPREEMPT` and `COLD_FLAGS` so the entry-accounting regime can be built for wasm.
* Production builds do not define `INTERP_ENTRY_ACCOUNTING` (no per-instruction check, per-call charging).

## Performance and size

Measured on an Apple M-series laptop that was shared with other jobs (load average 30-60 during these runs, so treat the numbers as +-30 %):

| workload | native (clang -O2, arm64) | WebAssembly under node 22 (`emcc -Os -mtail-call`) |
|---|---|---|
| synthetic mixed loop (`bench/`: ALU, memory, call/ret, SSE scalar, x87, rep movs) | 125-190 Minsn/s | 65-90 Minsn/s |
| real game functions with random inputs (`difftest funcs`, ~300 insns per run) | 130-150 Minsn/s; about the same with the decode cache flushed before every run (`throughput_nocache`), i.e. decoding is cheap | not measured (no image in the bench) |

* Size: `interp.c` compiles to **44.9 KB** of wasm code + 1.6 KB data at `-Os` (49.4 KB at `-O2`, 27.8 KB at `-Oz` but ~35 % slower), plus `rt_vec.c` (11 KB, needed by the lifted code anyway) and
  `interp_glue.c` (0.1 KB).  The decode cache is 28 bytes x 65,536 = 1.8 MB of heap (`-DINTERP_CACHE_BITS=n`).
* Expect the recompiled version of the same code to be several times faster (registers in locals, dead flags removed; an eager-flag interpreter cannot do that) -- not measured here.  At 60+ Minsn/s even the
  browser build executes a 100 k-instruction cold function in about 1.5 ms.
* Reproduce: `make bench/bench_native && ./bench/bench_native 12000`; `make wasm` (builds and runs the node benchmark and prints the size).


## Known gaps and deliberate divergences

**Traps (call `rt_unsupported(c, address, what)`; `c->pc` = the instruction):** port I/O, cli/sti/hlt, `int n` (unless `interp_int_hook` handles it; `int3` likewise), far call/jmp/ret/iret,
segment-register loads (`mov sreg`, `pop sreg`, `lds/les/lss/lfs/lgs` -- `mov sreg` and `pop sreg` are *ignored*, the stores `mov r/m, sreg` / `push sreg` return the Windows XP user selectors
0x23/0x2b/0x53), BCD adjust (daa/das/aaa/aas/aam/aad/salc), bound/arpl, `enter` with nesting level > 0, 16-bit addressing (0x67), GS override, 66-prefixed jumps/calls/rets/pushf/popf/pusha/popa/leave,
fbld/fbstp, `ud2`, 3DNow, SSE3+/SSSE3/SSE4/AVX (except movddup/movsldup/movshdup), popcnt/lzcnt/tzcnt (`F3 0F BC/BD` run as plain bsf/bsr like on a pre-BMI CPU), xsave, rdrand,
`div`/`idiv` by zero or with quotient overflow (no SEH delivery).  The decoder rejects the same set the lifter calls GARBAGE; in a linear sweep of the game image these only occur where data is
mis-decoded as code.

**Deliberately like the lifted code, not like hardware:** x87 registers are `double` (no 80-bit precision; hardware at the game's control word 0x27f rounds to 53 bits anyway), no tag word / stack
fault / exception flags / C1, rounding control is honoured by `fist*`, `frndint` and `fcw` round trips only (not by arithmetic or `fst`), `fprem`/`fprem1` do not set the quotient bits in C0/C1/C3,
`fnstenv`/`fnsave`/`fxsave` store tag word 0xffff / 0xff and zero instruction/data pointers, SSE rounding mode / flush-to-zero / exceptions in MXCSR are not modelled (`ldmxcsr`/`stmxcsr` only store the
value), `rcpps`/`rsqrtps` are exact instead of the 12-bit approximation, `lock` is ignored (cooperative single-threaded scheduler), `cmpxchg`/`pause` call `rt_spin`, no alignment faults (`movaps`
on an unaligned address just works), no memory protection (guest memory is flat; an out-of-range access is whatever the host does -- a wasm trap in the browser build).

**Where the interpreter is closer to hardware than the lifter:** invalid SSE/x87 operations return the x86 default QNaN (negative sign) instead of the host's; `fsin/fcos/fptan/fsincos` with
|x| >= 2^63 set C2 and leave the stack alone.  These could be ported to `lift.py`/`rt.h` (one line each).

**Self-modifying code:** the decode cache does not watch guest stores.  Call `interp_flush_range(lo, hi)` after the guest (or the loader) rewrote code; `interp_flush()` after the host changes which
addresses are native.

**Timing / threads:** `c->budget` is decremented per interpreted `call` (the lifted code does it per function entry), so a thread that spends its time in a long cold loop *without* calls is only
preempted by the instruction slice (`interp_slice`, 1 M instructions).  `interp_run_n` returns after the slice, so the host can still interleave.

