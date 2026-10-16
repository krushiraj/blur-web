#!/usr/bin/env python3
"""Decoder length check: every instruction capstone finds in the game .text / msvcr90 .text must get the same length from interp.c's decoder.
usage: declen.py   (builds libinterp_probe.dylib first via the Makefile target 'probe')"""
import ctypes, sys, collections, capstone, os
H = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
lib = ctypes.CDLL(os.path.join(H, 'libinterp_probe.dylib'))
lib.interp_decode_probe.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_int)]
lib.interp_decode_probe.restype = ctypes.c_int
ROOT = os.path.join(H, '..', '..', '..', 're', 'dump') + '/'
img = open(ROOT + 'blur_runtime.bin', 'rb').read(); msv = open(ROOT + 'msvcr90.bin', 'rb').read()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
OP_BAD, OP_EXIT = 0x301, 0x300
tot = 0; badlen = collections.Counter(); trap = collections.Counter(); ex = {}
op = ctypes.c_int()
for data, base, lo, hi in [(img, 0x400000, 0x401000, 0xe52000), (msv, 0x78520000, 0x78521000, 0x78520000 + 0x68000)]:
    a = lo
    while a < hi:
        got = False
        chunk = data[a - base:a - base + 16].ljust(16, b'\x90')
        for i in md.disasm(chunk, a, 1):
            got = True; tot += 1
            n = lib.interp_decode_probe(chunk, ctypes.byref(op))
            m = i.mnemonic
            if n != i.size:
                badlen[m] += 1; ex.setdefault(('len', m), (bytes(i.bytes).hex(), n, i.size))
            elif op.value in (OP_BAD, OP_EXIT): trap[m] += 1; ex.setdefault(('trap', m), bytes(i.bytes).hex())
            a += i.size
            break
        if not got: a += 1
print('instructions', tot)
print('LENGTH MISMATCHES', sum(badlen.values()))
for m, n in badlen.most_common(30): print('  ', m, n, ex[('len', m)])
print('decoded as trap (unsupported by design):', sum(trap.values()))
for m, n in trap.most_common(40): print('  ', m, n, ex[('trap', m)])
