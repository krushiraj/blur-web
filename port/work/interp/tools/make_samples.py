#!/usr/bin/env python3
"""Collect real instruction encodings from the game image (.text) and msvcr90 for the single-instruction differential test.
Linear sweep with capstone; up to K encodings per (mnemonic, operand shape, prefix) are kept (reservoir sampling).
usage: make_samples.py <out.bin> [K]     record format: len(1) bytes(len)"""
import sys, random, collections, capstone
from capstone import x86 as X
import os
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '..', 're', 'dump') + '/'
img = open(ROOT + 'blur_runtime.bin', 'rb').read()
msv = open(ROOT + 'msvcr90.bin', 'rb').read()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.detail = True
K = int(sys.argv[2]) if len(sys.argv) > 2 else 300
regions = [(img, 0x400000, 0x401000, 0xe52000), (msv, 0x78520000, 0x78521000, 0x78520000 + 0x68000)]
random.seed(1234)
res = collections.defaultdict(list); seen = collections.Counter()
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
            k = (i.mnemonic, shape(i))
            seen[k] += 1
            b = bytes(i.bytes)
            if len(res[k]) < K: res[k].append(b)
            else:
                j = random.randrange(seen[k])
                if j < K: res[k][j] = b
            a += i.size
        if not got: a += 1
out = bytearray(); n = 0
for k, lst in res.items():
    for b in lst: out.append(len(b)); out += b; n += 1
open(sys.argv[1], 'wb').write(out)
print(len(res), 'forms', n, 'samples')
