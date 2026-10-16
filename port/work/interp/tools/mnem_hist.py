#!/usr/bin/env python3
"""Linear-sweep mnemonic histogram over the game image .text and msvcr90 .text (capstone).
usage: mnem_hist.py <out.tsv> ; also writes <out.tsv>.forms with one sample encoding per (mnemonic, operand shape)."""
import sys, struct, collections, capstone
from capstone import x86 as X
import os
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '..', 're', 'dump') + '/'
img = open(ROOT + 'blur_runtime.bin', 'rb').read()
msv = open(ROOT + 'msvcr90.bin', 'rb').read()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.detail = True
regions = [(img, 0x400000, 0x401000, 0xe52000), (msv, 0x78520000, 0x78521000, 0x78520000 + 0x68000)]
cnt = collections.Counter(); forms = {}
def shape(i):
    s = []
    for o in i.operands:
        if o.type == X.X86_OP_REG: s.append('r%d' % o.size)
        elif o.type == X.X86_OP_IMM: s.append('i')
        else: s.append('m%d%s' % (o.size, 'f' if o.mem.segment == X.X86_REG_FS else ''))
    return ','.join(s)
for data, base, lo, hi in regions:
    a = lo
    while a < hi:
        got = False
        for i in md.disasm(data[a - base:a - base + 16], a, 1):
            got = True
            m = i.mnemonic
            cnt[m] += 1
            k = (m, shape(i), i.prefix[0] if i.prefix[0] else 0)
            if k not in forms: forms[k] = (i.address, bytes(i.bytes))
            a += i.size
        if not got: a += 1
out = sys.argv[1]
with open(out, 'w') as f:
    for m, n in cnt.most_common(): f.write(f'{m}\t{n}\n')
with open(out + '.forms', 'w') as f:
    for (m, s, p), (addr, b) in sorted(forms.items()): f.write(f'{m}\t{s}\t{p:x}\t{addr:x}\t{b.hex()}\n')
print(len(cnt), 'mnemonics', len(forms), 'forms')
