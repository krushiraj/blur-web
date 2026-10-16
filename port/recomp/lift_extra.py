#!/usr/bin/env python3
"""Incremental lifting: code addresses the running game reached but the main generation did not contain (learned_x.txt).
Lifts only those (plus whatever they newly reach) into gen_x/extra.c and gen_x/extra_table.c, in seconds."""
import sys, os, re, io
MINE = os.path.dirname(os.path.abspath(__file__))
RECOMP = os.path.dirname(os.path.realpath(os.path.join(MINE, 'lift.py')))       # where lift.py / gen_p2 / funcs.tsv really live (MINE itself when installed in port/recomp)
sys.path.insert(0, RECOMP); os.chdir(RECOMP)
import lift
from lift import *

HERE = RECOMP
MAIN = os.path.join(HERE, 'gen_p2')
OUT = os.path.join(MINE, 'gen_x')
os.makedirs(OUT, exist_ok=True)

base = set()
for l in open(os.path.join(MAIN, 'table.c')):
    m = re.match(r'\s*\{0x([0-9a-f]+)u', l)
    if m: base.add(int(m.group(1), 16))
for a in base: lift.FUNCS[a] = (0, 0, 2, 0)

seeds = []
# union of: the shared (cleaned) learned_x.txt, this work dir's own learned_x.txt (written by the runtime, BLUR_LEARNED) and the audio agent's 10 private targets
for lp in (os.path.join(HERE, 'learned_x.txt'), os.path.join(MINE, 'learned_x.txt'), os.path.join(MINE, 'learned_private.txt')):
  if os.path.exists(lp):
    for l in open(lp):
        if l.strip():
            try: a = int(l, 16)
            except ValueError: continue
            if a not in base and in_img(a) and decode_at(a) is not None: seeds.append(a)
# protector handler stubs (0x206dd00..0x206f000 are tiny 'mov eax,x; ret'-style handlers selected dynamically): seed every address that follows a RET
rng = os.path.join(MINE, 'seed_ranges.txt')
if os.path.exists(rng):
    for l in open(rng):
        l = l.split('#')[0].strip()
        if not l: continue
        lo, hi = [int(x, 16) for x in l.split('-')]
        a = lo; start = True
        while a < hi:
            ins = decode_at(a)
            if ins is None: a += 1; start = True; continue
            if start and a not in base and in_img(a): seeds.append(a)
            start = ins.mnemonic in ('ret', 'int3')
            a += ins.size
seeds = sorted(set(seeds))

new = {}                                   # address -> generated function
queue = list(seeds)
for a in queue: lift.FUNCS[a] = (0, 0, 2, 0)
lift.KNOWN = None
while queue:
    e = queue.pop()
    try: L = lift.gen_function(e)[3]
    except lift.Unsupported: new[e] = None; continue
    new[e] = True
    for t in L.direct:
        if t not in lift.FUNCS and in_img(t) and decode_at(t) is not None:
            lift.FUNCS[t] = (0, 0, 2, 0); queue.append(t)
ok = {a for a, v in new.items() if v}
lift.KNOWN = base | ok

out = io.StringIO(); out.write('#include "rt.h"\n'); decl = set(); funcs = []
for e in sorted(ok):
    disp, hdr, body, L = lift.gen_function(e)
    decl |= L.calls; funcs.append((e, disp, hdr, body))
for d in sorted(decl): out.write(f'void f_{d:08x}(Cpu *c);\n')
for e, disp, hdr, body in funcs:
    if len(body) > 6000: hdr = ['OPTNONE ' + hdr[0]] + hdr[1:]
    out.write(disp + '\n' + '\n'.join(hdr + body) + '\n}\n#undef JMP_DISPATCH\n')
open(os.path.join(OUT, 'extra.c'), 'w').write(out.getvalue())

t = io.StringIO(); t.write('#include <stdint.h>\nstruct Cpu; typedef void (*GuestFn)(struct Cpu *);\n')
for e in sorted(ok): t.write(f'void f_{e:08x}(struct Cpu *c);\n')
t.write('const struct { uint32_t a; GuestFn f; } rt_table_x[] = {\n')
for e in sorted(ok): t.write(f'  {{0x{e:x}u, f_{e:08x}}},\n')
t.write('  {0, 0}\n};\nconst unsigned rt_table_x_n = %d;\n' % len(ok))
open(os.path.join(OUT, 'extra_table.c'), 'w').write(t.getvalue())
print(f'{len(seeds)} seeds -> {len(ok)} extra functions ({len(new) - len(ok)} not liftable)')
