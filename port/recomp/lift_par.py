#!/usr/bin/env python3
"""Parallel version of `lift.py genall`: same output, lifted on all cores. usage: lift_par.py <outdir>"""
import sys, os, io, struct, time, collections
import multiprocessing as mp
from array import array
import lift
from lift import *

def _disc(e):
    try: L = lift.gen_function(e)[3]
    except lift.Unsupported: return e, None
    return e, sorted(L.direct)

OVR = {}
for _l in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'overrides.txt')):
    _l = _l.split('#')[0].split()
    if len(_l) == 3: OVR[int(_l[0], 16)] = (int(_l[1]), None if _l[2] == '-' else int(_l[2], 0))

def _emit(e):
    if e in OVR:
        pop, val = OVR[e]
        body = [f'    {"c->eax = %#xu; " % val if val is not None else ""}c->lastret = M32(c->esp); c->esp += {4 + pop};']
        return e, ('#define JMP_DISPATCH(t) {}', [f'void f_{e:08x}(Cpu *c) {{'], body, []), (array('I'), array('I'), [])
    try:
        disp, hdr, body, L = lift.gen_function(e)
    except lift.Unsupported: return e, None
    return e, (disp, hdr, body, sorted(L.calls)), (array('I', sorted(L.ins)), array('I', sorted(L.targets)), sorted(L.direct))

def pmap(fn, items, chunk=64):
    ctx = mp.get_context('fork')
    with ctx.Pool(os.cpu_count()) as p:
        return list(p.imap(fn, items, chunksize=chunk))

if __name__ == '__main__':
    t0 = time.time(); outdir = sys.argv[1]; os.makedirs(outdir, exist_ok=True)
    lift.seed_functions()
    seeds0 = set(lift.FUNCS)
    seen = set(lift.FUNCS)
    def discover(wave):
        n = 0
        while wave:
            res = pmap(_disc, wave); new = []
            for e, direct in res:
                if direct is None: continue
                for t in direct:
                    if t not in seen and in_img(t) and decode_at(t) is not None: seen.add(t); lift.FUNCS[t] = (0, 0, 2, 0); new.append(t)
            n += 1; print(f'wave {n}: {len(wave)} lifted, {len(new)} new entries ({time.time()-t0:.0f}s)', flush=True); wave = sorted(new)
    discover(sorted(seen))
    # targets the recompiled game reached at run time but static analysis did not (rt appends them to learned.txt)
    lp = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'learned.txt')
    learned = []
    if os.path.exists(lp):
        for l in open(lp):
            if l.strip():
                a = int(l, 16)
                if a not in lift.FUNCS and decode_at(a) is not None: lift.FUNCS[a] = (0, 0, 2, 0); seen.add(a); learned.append(a)
        seeds0 |= set(learned)
        print(f'{len(learned)} learned run-time targets added as entries', flush=True)
        discover(sorted(learned))
    blocks_path = os.environ.get('BLOCKS')
    if blocks_path and os.path.exists(blocks_path):          # block starts the emulator executed; those not covered by the static flow become entries
        ok0 = {e for e, d in pmap(_disc, sorted(lift.FUNCS)) if d is not None}
        lift.KNOWN = ok0
        cover = set()
        for r in pmap(_emit, sorted(ok0)):
            if r[1] is not None: cover.update(r[2][0])
        newb = []
        for l in open(blocks_path):
            if l.strip():
                a = int(l, 16)
                forced = 0x125a000 <= a < 0x206f000          # computed jumps in the packer's section can land on any executed block start
                if (forced or a not in cover) and a not in lift.FUNCS and decode_at(a) is not None: lift.FUNCS[a] = (0, 0, 2, 0); seen.add(a); newb.append(a)
        seeds0 |= set(newb)
        print(f'{len(newb)} executed blocks were not covered statically; added as entries', flush=True)
        discover(sorted(newb))
    print('entries after discovery', len(lift.FUNCS), flush=True)
    entries = sorted(lift.FUNCS)
    # liftable set (the discovery waves already tried everything; redo cheaply by attempting emission twice would be wasteful, so reuse results)
    ok = set()
    res = pmap(_disc, entries)
    ok = {e for e, d in res if d is not None}
    lift.KNOWN = ok
    print('liftable', len(ok), flush=True)
    shared = []
    for rnd in range(4):
        emitted = pmap(_emit, sorted(ok))
        own = collections.Counter(); leaders = set(); direct_all = {}
        for r in emitted:
            if r[1] is None: continue
            e, _, (ins_a, tg, dr) = r
            own.update(ins_a); leaders.update(tg); direct_all[e] = dr
        shared = [a for a in leaders if own[a] >= 2 and a not in lift.FUNCS]
        print(f'round {rnd}: {len(ok)} functions, {len(shared)} shared blocks become entries', flush=True)
        if not shared: break
        for a in shared: lift.FUNCS[a] = (0, 0, 2, 0)
        ok = {e for e, d in pmap(_disc, sorted(lift.FUNCS)) if d is not None}
        lift.KNOWN = ok
    reach = set(); work = [a for a in seeds0 if a in ok]
    while work:
        a = work.pop()
        if a in reach: continue
        reach.add(a)
        for t in direct_all.get(a, []):
            if t in ok and t not in reach: work.append(t)
    print(f'reachable from roots: {len(reach)} of {len(ok)}', flush=True)
    ok = reach; lift.KNOWN = ok
    emitted = pmap(_emit, sorted(ok))
    traps = collections.Counter()
    chunk, decl, curkey, done_all, nfile = [], set(), 0, [], 0
    def flush():
        global chunk, decl, nfile
        if not chunk: return
        out = io.StringIO(); out.write('#include "rt.h"\n')
        for d in sorted(decl): out.write(f'void f_{d:08x}(Cpu *c);\n')
        for e, disp, hdr, body in chunk:
            if len(body) > 6000: hdr = ['OPTNONE ' + hdr[0]] + hdr[1:]
            out.write(disp + '\n' + '\n'.join(hdr + body) + '\n}\n#undef JMP_DISPATCH\n')
        path = os.path.join(outdir, f'part_{curkey:05x}.c'); txt = out.getvalue()
        if not (os.path.exists(path) and open(path).read() == txt): open(path, 'w').write(txt)
        nfile += 1; chunk = []; decl = set()
    for r in emitted:
        if r[1] is None: continue
        e, (disp, hdr, body, calls), _x = r
        key = e >> 17
        if chunk and key != curkey: flush()
        curkey = key; decl |= set(calls); chunk.append((e, disp, hdr, body)); done_all.append(e)
    flush()
    t = io.StringIO(); t.write('#include "rt.h"\n')
    for e in done_all: t.write(f'void f_{e:08x}(Cpu *c);\n')
    t.write('const struct { uint32_t a; GuestFn f; } rt_table[] = {\n')
    for e in done_all: t.write(f'  {{0x{e:x}u, f_{e:08x}}},\n')
    t.write('};\nconst unsigned rt_table_n = %d;\n' % len(done_all))
    p = os.path.join(outdir, 'table.c')
    if not (os.path.exists(p) and open(p).read() == t.getvalue()): open(p, 'w').write(t.getvalue())
    print('files', nfile, 'functions', len(done_all), f'({time.time()-t0:.0f}s)')
