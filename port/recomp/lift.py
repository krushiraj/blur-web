#!/usr/bin/env python3
"""x86-32 -> C static recompiler (stage 1: integer instruction set).

usage:
  lift.py stats                      count unsupported mnemonics over every function in funcs.tsv
  lift.py gen <out.c> <addr> [addr]  lift the given function addresses (hex) into one C file
  lift.py genall <outdir> [n]        lift every liftable function, n functions per output file

Input: ../../re/dump/blur_runtime.bin (decrypted image, base 0x400000) and funcs.tsv (from Ghidra).
"""
import sys, struct, collections, os
import capstone
from capstone import x86 as X

BASE = 0x400000
HERE = os.path.dirname(os.path.abspath(__file__))
IMG = open(os.environ.get('LIFT_IMG') or os.path.join(HERE, '..', '..', 're', 'dump', 'blur_runtime.bin'), 'rb').read()
FUNCS = {}   # entry -> (size, maxaddr, nranges, thunk)
for line in open(os.path.join(HERE, 'funcs.tsv')):
    a, sz, hi, nr, th = line.split('\t')
    FUNCS[int(a, 16)] = (int(sz), int(hi, 16), int(nr), int(th))

GH = frozenset(FUNCS)          # entries known to the original analysis (later seeds do not count as proof that a flow of control ends)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True

R32 = {X.X86_REG_EAX: 'eax', X.X86_REG_ECX: 'ecx', X.X86_REG_EDX: 'edx', X.X86_REG_EBX: 'ebx', X.X86_REG_ESP: 'esp', X.X86_REG_EBP: 'ebp', X.X86_REG_ESI: 'esi', X.X86_REG_EDI: 'edi'}
R16 = {X.X86_REG_AX: 'eax', X.X86_REG_CX: 'ecx', X.X86_REG_DX: 'edx', X.X86_REG_BX: 'ebx', X.X86_REG_SP: 'esp', X.X86_REG_BP: 'ebp', X.X86_REG_SI: 'esi', X.X86_REG_DI: 'edi'}
R8L = {X.X86_REG_AL: 'eax', X.X86_REG_CL: 'ecx', X.X86_REG_DL: 'edx', X.X86_REG_BL: 'ebx'}
R8H = {X.X86_REG_AH: 'eax', X.X86_REG_CH: 'ecx', X.X86_REG_DH: 'edx', X.X86_REG_BH: 'ebx'}
REGS = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
MASK = {8: 0xff, 16: 0xffff, 32: 0xffffffff}

KNOWN = None      # set of function entries that will exist in the generated output (None = every function in funcs.tsv)

def is_known(a):
    return a in (FUNCS if KNOWN is None else KNOWN)

TRAPS = collections.Counter()
GARBAGE = set('arpl outsb outsd outsw insb insd insw daa das aaa aas aad aam bound out in hlt les lds cli sti int1 into salc retf lcall ljmp sldt str popaw pushaw enter lgdt sgdt lidt sidt popfw pushfw iretd iret sysenter syscall cpuid_x wrmsr rdmsr'.split())
class Unsupported(Exception):
    pass

def rread(r):
    if r in R32: return R32[r]
    if r in R16: return f'((uint16_t){R16[r]})'
    if r in R8L: return f'((uint8_t){R8L[r]})'
    if r in R8H: return f'((uint8_t)({R8H[r]} >> 8))'
    raise Unsupported(f'reg {md.reg_name(r)}')

def rwrite(r, v):
    if r in R32: return f'{R32[r]} = (uint32_t)({v});'
    if r in R16: n = R16[r]; return f'{n} = ({n} & 0xffff0000u) | (uint16_t)({v});'
    if r in R8L: n = R8L[r]; return f'{n} = ({n} & 0xffffff00u) | (uint8_t)({v});'
    if r in R8H: n = R8H[r]; return f'{n} = ({n} & 0xffff00ffu) | ((uint32_t)(uint8_t)({v}) << 8);'
    raise Unsupported(f'reg {md.reg_name(r)}')

def ea(m):
    parts = []
    if m.base: parts.append(R32.get(m.base) or (_ for _ in ()).throw(Unsupported('base reg')))
    if m.index:
        idx = R32.get(m.index) or (_ for _ in ()).throw(Unsupported('index reg'))
        parts.append(idx if m.scale == 1 else f'({idx} * {m.scale})')
    if m.disp or not parts: parts.append(f'{m.disp & 0xffffffff:#x}u')
    e = ' + '.join(parts)
    if m.segment == X.X86_REG_FS: e = f'c->teb + {e}'
    elif m.segment not in (0, X.X86_REG_DS, X.X86_REG_ES, X.X86_REG_SS, X.X86_REG_CS): raise Unsupported('segment')
    return f'(uint32_t)({e})'

def rd(op, bits=None):
    bits = bits or op.size * 8
    if op.type == X.X86_OP_REG: return rread(op.reg)
    if op.type == X.X86_OP_IMM: return f'{op.imm & MASK[bits]:#x}u'
    if op.type == X.X86_OP_MEM: return f'M{bits}({ea(op.mem)})'
    raise Unsupported('operand')

def wr(op, v, bits=None):
    bits = bits or op.size * 8
    if op.type == X.X86_OP_REG: return rwrite(op.reg, v)
    if op.type == X.X86_OP_MEM: return f'M{bits}({ea(op.mem)}) = (uint{bits}_t)({v});'
    raise Unsupported('write operand')

COND = {'e': 'f.zf', 'ne': '!f.zf', 'b': 'f.cf', 'ae': '!f.cf', 'be': '(f.cf | f.zf)', 'a': '!(f.cf | f.zf)', 'l': '(f.sf != f.of)', 'ge': '(f.sf == f.of)',
        'le': '(f.zf | (f.sf != f.of))', 'g': '(!f.zf & (f.sf == f.of))', 's': 'f.sf', 'ns': '!f.sf', 'o': 'f.of', 'no': '!f.of', 'p': 'f.pf', 'np': '!f.pf',
        'z': 'f.zf', 'nz': '!f.zf', 'c': 'f.cf', 'nc': '!f.cf', 'nae': 'f.cf', 'nb': '!f.cf', 'na': '(f.cf | f.zf)', 'nbe': '!(f.cf | f.zf)', 'nge': '(f.sf != f.of)', 'nl': '(f.sf == f.of)',
        'ng': '(f.zf | (f.sf != f.of))', 'nle': '(!f.zf & (f.sf == f.of))', 'pe': 'f.pf', 'po': '!f.pf'}

SPILL = 'SAVE;'
RELOAD = 'LOAD;'

IMG_END = BASE + len(IMG)
MSV_BASE = 0x78520000
_msv = os.path.join(HERE, '..', '..', 're', 'dump', 'msvcr90.bin')
MSV = open(_msv, 'rb').read() if os.path.exists(_msv) else b''
MSV_END = MSV_BASE + len(MSV)
TEXT = [(0x401000, 0xe52000), (0x125a000, 0x206f000)]
if MSV:
    _pe = struct.unpack_from('<I', MSV, 0x3c)[0]; _ns = struct.unpack_from('<H', MSV, _pe + 6)[0]; _so = struct.unpack_from('<H', MSV, _pe + 0x14)[0]
    for _k in range(_ns):
        _o = _pe + 0x18 + _so + 40 * _k; _vs, _va = struct.unpack_from('<II', MSV, _o + 8); _ch = struct.unpack_from('<I', MSV, _o + 36)[0]
        if _ch & 0x20000000: TEXT.append((MSV_BASE + _va, MSV_BASE + _va + _vs))
def in_text(a): return any(lo <= a < hi for lo, hi in TEXT)
DATA = [(0xe52000, 0x1258000)] + ([(MSV_BASE + 0x68000, MSV_END)] if MSV else [])
def in_img(a): return BASE <= a < IMG_END or MSV_BASE <= a < MSV_END
def img_read(a, n):
    if BASE <= a < IMG_END: return IMG[a - BASE:a - BASE + n]
    if MSV_BASE <= a < MSV_END: return MSV[a - MSV_BASE:a - MSV_BASE + n]
    return b''

import bisect

def decode_at(a):
    d = img_read(a, 16)
    if not d: return None
    for i in md.disasm(d, a, 1): return i
    return None

class FnLifter:
    def __init__(self, entry):
        self.entry = entry; sz, hi, nr, th = FUNCS.get(entry, (0, 0, 2, 0))
        self.linear = False
        self.retaddrs = set()
        self.direct = set()
        self.end = hi + 1
        self.ins = {}
        self.jt = {}          # address of an indirect jmp -> list of table targets
        if self.linear:
            data = IMG[entry - BASE:entry - BASE + (self.end - entry)]
            for i in md.disasm(data, entry): self.ins[i.address] = i
            a = entry
            while a < self.end:
                if a not in self.ins: raise Unsupported(f'undecodable at {a:#x}')
                a += self.ins[a].size
            self.order = sorted(self.ins)
            last = self.ins[self.order[-1]]
            if last.mnemonic not in ('ret', 'jmp', 'int3', 'ud2', 'hlt', 'iretd', 'retf') and (last.address + last.size) not in GH:
                # the analysis ended this function in the middle of a flow of control: follow the flow instead
                self.linear = False; self.ins = {}; self.jt = {}; self.descend(); return_early = True
            else:
                self.endmap = {a + self.ins[a].size: a for a in self.ins}
                self.find_tables()
        else:
            self.descend()
        self.targets = set()
        self.calls = set()

    def computed_ret(self, i):
        # a `ret` whose target was just manufactured on the stack (xchg/mov/add on [esp], or push reg; ret) is an indirect jump in disguise
        a = i.address
        for _ in range(12):
            b = self.endmap.get(a) if hasattr(self, 'endmap') else None
            if b is None or a in self.targets: return False
            p = self.ins[b]
            if p.mnemonic in ('ret', 'jmp', 'call') or p.mnemonic.startswith('j'): return False
            if p.mnemonic == 'push' and p.operands[0].type != X.X86_OP_IMM and a == i.address: return True
            for op in p.operands[:1]:
                if op.type == X.X86_OP_MEM and op.mem.base == X.X86_REG_ESP and not op.mem.index and op.mem.disp == 0 and p.mnemonic in ('xchg', 'mov', 'add', 'sub', 'and', 'or', 'xor', 'inc', 'dec', 'neg', 'not', 'adc', 'sbb'): return True
            if p.mnemonic == 'xchg' and any(o.type == X.X86_OP_MEM and o.mem.base == X.X86_REG_ESP and o.mem.disp == 0 and not o.mem.index for o in p.operands): return True
            a = b
        return False

    def jmp_idiom(self, i):
        # `push R; jmp T` where R is a nearby forward code address: a call to T that returns to R
        if i.mnemonic != 'jmp': return None
        p = self.ins.get(self.endmap.get(i.address)) if hasattr(self, 'endmap') else None
        if p is None or p.mnemonic != 'push' or p.operands[0].type != X.X86_OP_IMM: return None
        R = p.operands[0].imm & 0xffffffff
        if not (i.address < R <= i.address + 0x100): return None
        op = i.operands[0]
        return (op.imm if op.type == X.X86_OP_IMM else None), R

    def find_tables(self):
        keys = sorted(self.ins)
        for a in keys:
            i = self.ins[a]
            if i.mnemonic != 'jmp' or i.operands[0].type != X.X86_OP_MEM: continue
            mm = i.operands[0].mem
            if mm.base or not mm.index or mm.scale != 4 or not in_img(mm.disp & 0xffffffff): continue
            k = bisect.bisect_left(keys, a); n = None
            for j in range(k - 1, max(k - 14, -1), -1):
                p = self.ins[keys[j]]
                if p.mnemonic == 'cmp' and p.operands[1].type == X.X86_OP_IMM and 0 <= p.operands[1].imm <= 2048: n = p.operands[1].imm + 1; break
            tbl = mm.disp & 0xffffffff; outl = []
            for e in range(n if n else 512):
                d4 = img_read(tbl + 4 * e, 4)
                if len(d4) < 4: break
                v = struct.unpack('<I', d4)[0]
                if not in_img(v) or abs(v - self.entry) > 0x40000: break
                outl.append(v)
            self.jt[a] = outl
            if not self.linear:
                for v in outl: self._work.append(v)

    def descend(self):
        self._work = [self.entry]; self.bad = set(); self.endmap = {}
        while True:
            while self._work:
                a = self._work.pop()
                while a not in self.ins:
                    i = decode_at(a)
                    if i is None: self.bad.add(a); break
                    self.ins[a] = i; self.endmap[a + i.size] = a; m = i.mnemonic; nxt = a + i.size
                    if m in ('ret', 'int3', 'ud2', 'hlt', 'iretd', 'retf'): break
                    if m == 'call':
                        self.retaddrs.add(nxt); break                  # the return address is an entry of its own: the callee returns by jumping to it
                    if (m == 'jmp' or m.startswith('j') or m.startswith('loop')) and i.operands[0].type == X.X86_OP_IMM:
                        t = i.operands[0].imm
                        if not (t != self.entry and t in FUNCS): self._work.append(t)
                        if m == 'jmp': break
                    elif m == 'jmp': break
                    if nxt in FUNCS and nxt != self.entry: break          # falls into another function: the emitted stub tail-calls it
                    a = nxt
            before = len(self.ins)
            self.order = sorted(self.ins)
            self.find_tables()
            if len(self.ins) == before and all(w in self.ins or w in self.bad for w in self._work): self._work = []
            if not self._work: break
        self.order = sorted(self.ins)
        for a in self.order:
            i = self.ins[a]
            if i.mnemonic in ('ret', 'int3', 'ud2', 'hlt', 'jmp', 'iretd', 'retf', 'call'): continue
            if a + i.size not in self.ins and a + i.size in self.bad: raise Unsupported(f'undecodable at {a + i.size:#x}')

    def lines(self):
        out = []
        for a in self.order:
            i = self.ins[a]
            if i.mnemonic.startswith('j') or i.mnemonic.startswith('loop'):
                op = i.operands[0]
                if op.type == X.X86_OP_IMM and op.imm in self.ins: self.targets.add(op.imm)
                elif op.type == X.X86_OP_IMM and self.linear and self.entry <= op.imm < self.end: raise Unsupported(f'jump into middle of instruction {op.imm:#x}')
        for v in sum(self.jt.values(), []):
            if v in self.ins: self.targets.add(v)
        for a in self.order:
            i = self.ins[a]
            if i.mnemonic in ('call', 'jmp') and i.operands[0].type == X.X86_OP_MEM:
                mm = i.operands[0].mem
                if not mm.base and not mm.index and in_img(mm.disp & 0xffffffff):
                    d4 = img_read(mm.disp & 0xffffffff, 4)
                    if len(d4) == 4:
                        v = struct.unpack('<I', d4)[0]
                        if in_text(v): self.direct.add(v)          # thunk cell: its current value is a code entry
            if i.mnemonic in ('call', 'jmp') or i.mnemonic.startswith('j'): continue
            for op in i.operands:
                v = op.imm & 0xffffffff if op.type == X.X86_OP_IMM else ((op.mem.disp & 0xffffffff) if op.type == X.X86_OP_MEM and not op.mem.base and not op.mem.index else None)
                if v is not None and in_text(v): self.direct.add(v)
        self.has_indirect_jmp = False
        self.direct |= self.retaddrs
        for a in self.order:
            try: self.insn(self.ins[a])       # first pass only collects jump targets
            except Unsupported: pass
        body = []
        if not self.order: raise Unsupported('empty function')
        if self.entry != self.order[0]:
            self.targets.add(self.entry); body.append(f'    goto L_{self.entry:08x};')
        for a in self.order:
            i = self.ins[a]
            if a in self.targets: body.append(f'L_{a:08x}:;')
            body.append(f'    /* {a:08x}: {i.mnemonic} {i.op_str} */')
            try: stmts = self.insn(i)
            except Unsupported as ex:
                if str(ex) in GARBAGE or str(ex).startswith('reg ') or str(ex) == 'segment': raise Unsupported('garbage code: ' + str(ex))
                TRAPS[str(ex)] += 1; stmts = [f'{{ {SPILL} rt_unsupported(c, {a:#x}u, "{str(ex)}"); }}']
            for s in stmts: body.append('    ' + s)
            if i.mnemonic not in ('ret', 'jmp', 'call', 'int3', 'ud2', 'hlt', 'iretd', 'retf') and a + i.size not in self.ins:
                body.append(f'    {SPILL} {self.tail(a + i.size)}  /* control continues outside the lifted region */')
        return body

    def tail(self, tgt):
        # transfer control to a known address: a direct tail call if it is an entry we lift, the dispatcher otherwise
        self.direct.add(tgt)
        if is_known(tgt): self.calls.add(tgt); return f'MUSTTAIL return f_{tgt:08x}(c);'
        return f'c->pc = {tgt:#x}u; MUSTTAIL return rt_dispatch(c);'

    def call_stmt(self, target_expr, ret_addr, direct=None):
        self.direct.add(ret_addr)
        if direct is not None:
            return [f'{{ esp -= 4; M32(esp) = {ret_addr:#x}u; {SPILL} {self.tail(direct)} }}']
        return [f'{{ uint32_t tgt = {target_expr}; esp -= 4; M32(esp) = {ret_addr:#x}u; {SPILL} c->pc = tgt; MUSTTAIL return rt_dispatch(c); }}']

    def jump_to(self, tgt_imm, cond=None):
        if tgt_imm in self.ins:
            self.targets.add(tgt_imm)
            g = f'goto L_{tgt_imm:08x};'
        else:
            g = f'{{ {SPILL} {self.tail(tgt_imm)} }}'
        return f'if ({cond}) {g}' if cond else g

    def insn(self, i):
        m = i.mnemonic; ops = i.operands; n = len(ops)
        if m.startswith('bnd '): m = m[4:]
        if m in ('repz ret', 'rep ret', 'repe ret'): m = 'ret'
        pfx = i.prefix
        # ---- data movement ----
        if m in ('nop', 'int3', 'fnop', 'ud2', 'endbr32', 'lfence', 'sfence', 'mfence', 'prefetchnta', 'prefetcht0', 'prefetcht1', 'prefetcht2', 'emms'):
            return []
        if m == 'pause': return ['rt_spin(c);']
        if m == 'mov' or m in ('movabs',):
            return [wr(ops[0], rd(ops[1]))]
        if m == 'lea': return [wr(ops[0], ea(ops[1].mem))]
        if m in ('movzx', 'movsx'):
            sbits = ops[1].size * 8; dbits = ops[0].size * 8
            v = rd(ops[1])
            if m == 'movsx': v = f'(uint32_t)(int32_t)(int{sbits}_t)({v})'
            return [wr(ops[0], v)]
        if m.startswith('lock '):
            base = m[5:]; B = ops[0].size * 8 if ops else 32
            if ops and ops[0].type == X.X86_OP_MEM:
                a = ea(ops[0].mem); T = f'uint{B}_t'
                if base in ('inc', 'dec'):
                    return [f'{{ {T} o = __atomic_fetch_{"add" if base == "inc" else "sub"}((volatile {T} *)(G + {a}), 1, __ATOMIC_SEQ_CST); fl_{base}(&f, o, {B}); }}']
                if base in ('add', 'sub', 'and', 'or', 'xor'):
                    v = rd(ops[1], B); fn = {'add': 'add', 'sub': 'sub', 'and': 'and', 'or': 'or', 'xor': 'xor'}[base]
                    fl = {'add': f'fl_add(&f, o, v, 0, {B})', 'sub': f'fl_sub(&f, o, v, 0, {B})', 'and': f'fl_logic(&f, o & v, {B})', 'or': f'fl_logic(&f, o | v, {B})', 'xor': f'fl_logic(&f, o ^ v, {B})'}[base]
                    return [f'{{ {T} v = {v}; {T} o = __atomic_fetch_{fn}((volatile {T} *)(G + {a}), v, __ATOMIC_SEQ_CST); {fl}; }}']
                if base == 'xadd':
                    return [f'{{ {T} v = {rd(ops[1], B)}; {T} o = __atomic_fetch_add((volatile {T} *)(G + {a}), v, __ATOMIC_SEQ_CST); fl_add(&f, o, v, 0, {B}); {wr(ops[1], "o")} }}']
                if base == 'cmpxchg':
                    acc = {8: '(uint8_t)eax', 16: '(uint16_t)eax', 32: 'eax'}[B]
                    return [f'{{ {T} e = {acc}; {T} nv = {rd(ops[1], B)}; int ok = __atomic_compare_exchange_n((volatile {T} *)(G + {a}), &e, nv, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); fl_sub(&f, {acc}, e, 0, {B}); f.zf = ok; if (!ok) {{ rt_spin(c); {rwrite(X.X86_REG_EAX if B == 32 else (X.X86_REG_AX if B == 16 else X.X86_REG_AL), "e")} }} }}']
                if base == 'cmpxchg8b':
                    return [f'{{ uint64_t e = ((uint64_t)edx << 32) | eax; uint64_t nv = ((uint64_t)ecx << 32) | ebx; int ok = __atomic_compare_exchange_n((volatile uint64_t *)(G + {ea(ops[0].mem)}), &e, nv, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); f.zf = ok; if (!ok) {{ eax = (uint32_t)e; edx = (uint32_t)(e >> 32); rt_spin(c); }} }}']
            raise Unsupported(m)
        if m == 'cmpxchg':
            B = ops[0].size * 8; acc = {8: '(uint8_t)eax', 16: '(uint16_t)eax', 32: 'eax'}[B]
            return [f'{{ uint{B}_t d = {rd(ops[0])}; fl_sub(&f, {acc}, d, 0, {B}); if (f.zf) {{ {wr(ops[0], rd(ops[1]))} }} else {{ rt_spin(c); {rwrite(X.X86_REG_EAX if B == 32 else (X.X86_REG_AX if B == 16 else X.X86_REG_AL), "d")} }} }}']
        if m == 'xadd':
            B = ops[0].size * 8
            return [f'{{ uint{B}_t d = {rd(ops[0])}, s = {rd(ops[1])}; uint{B}_t r = fl_add(&f, d, s, 0, {B}); {wr(ops[1], "d")} {wr(ops[0], "r")} }}']
        if m in ('wait', 'fwait'): return []
        if m == 'rdtsc': return [f'{{ {SPILL} rt_rdtsc(c); {RELOAD} }}']
        if m == 'xchg' and ops[0].type == X.X86_OP_MEM and ops[0].size == 4:
            return [f'eax = eax; {{ uint32_t t = {rd(ops[1])}; t = __atomic_exchange_n((volatile uint32_t *)(G + {ea(ops[0].mem)}), t, __ATOMIC_SEQ_CST); {wr(ops[1], "t")} }}']
        if m == 'xchg' and ops[1].type == X.X86_OP_MEM and ops[1].size == 4:
            return [f'{{ uint32_t t = {rd(ops[0])}; t = __atomic_exchange_n((volatile uint32_t *)(G + {ea(ops[1].mem)}), t, __ATOMIC_SEQ_CST); {wr(ops[0], "t")} }}']
        if m == 'xchg':
            return [f'{{ uint32_t t1 = {rd(ops[0])}, t2 = {rd(ops[1])}; {wr(ops[0], "t2")} {wr(ops[1], "t1")} }}']
        if m in ('push', 'pop') and ops[0].type == X.X86_OP_REG and ops[0].reg in (X.X86_REG_SS, X.X86_REG_DS, X.X86_REG_ES, X.X86_REG_CS, X.X86_REG_FS, X.X86_REG_GS):
            if m == 'push': return ['esp -= 4; M32(esp) = 0x2b;']
            return ['esp += 4;']
        if m == 'push':
            if ops[0].size == 2: return [f'{{ uint16_t v = (uint16_t)({rd(ops[0], 16)}); esp -= 2; M16(esp) = v; }}']
            if ops[0].size != 4: raise Unsupported('push8')
            return [f'{{ uint32_t v = {rd(ops[0], 32)}; esp -= 4; M32(esp) = v; }}']
        if m == 'pop':
            if ops[0].size == 2: return [f'{{ uint16_t v = M16(esp); esp += 2; {wr(ops[0], "v", 16)} }}']
            if ops[0].size != 4: raise Unsupported('pop8')
            return [f'{{ uint32_t v = M32(esp); esp += 4; {wr(ops[0], "v", 32)} }}']
        if m == 'pushal' or m == 'pusha':
            return ['{ uint32_t t = esp; esp -= 32; M32(esp+28)=eax; M32(esp+24)=ecx; M32(esp+20)=edx; M32(esp+16)=ebx; M32(esp+12)=t; M32(esp+8)=ebp; M32(esp+4)=esi; M32(esp)=edi; }']
        if m == 'popal' or m == 'popa':
            return ['{ edi=M32(esp); esi=M32(esp+4); ebp=M32(esp+8); ebx=M32(esp+16); edx=M32(esp+20); ecx=M32(esp+24); eax=M32(esp+28); esp += 32; }']
        if m == 'pushfd': return ['{ esp -= 4; M32(esp) = fl_eflags(&f, c->df); }']
        if m == 'popfd': return ['{ uint32_t v = M32(esp); esp += 4; fl_load(&f, v, &c->df); }']
        if m == 'leave': return ['esp = ebp; ebp = M32(esp); esp += 4;']
        if m == 'cdq': return ['edx = (uint32_t)((int32_t)eax >> 31);']
        if m == 'cwde': return ['eax = (uint32_t)(int32_t)(int16_t)eax;']
        if m == 'cwd': return ['edx = (edx & 0xffff0000u) | ((uint16_t)((int16_t)eax >> 15));']
        if m == 'cbw': return ['eax = (eax & 0xffff0000u) | (uint16_t)(int16_t)(int8_t)eax;']
        if m == 'bswap': return [wr(ops[0], f'__builtin_bswap32({rd(ops[0])})')]
        if m == 'cld': return ['c->df = 0;']
        if m == 'std': return ['c->df = 1;']
        if m == 'clc': return ['f.cf = 0;']
        if m == 'stc': return ['f.cf = 1;']
        if m == 'cmc': return ['f.cf = !f.cf;']
        if m == 'sahf': return ['{ uint8_t a = (eax >> 8) & 0xff; f.cf = a & 1; f.pf = (a >> 2) & 1; f.af = (a >> 4) & 1; f.zf = (a >> 6) & 1; f.sf = (a >> 7) & 1; }']
        if m == 'lahf': return ['{ uint32_t e = fl_eflags(&f, 0); eax = (eax & 0xffff00ffu) | ((e & 0xd7u) << 8); }']
        if m == 'cpuid': return [f'{{ {SPILL} rt_cpuid(c); {RELOAD} }}']
        # ---- ALU ----
        alu = {'add': 'fl_add(&f, a, b, 0, B)', 'sub': 'fl_sub(&f, a, b, 0, B)', 'adc': 'fl_add(&f, a, b, f.cf, B)', 'sbb': 'fl_sub(&f, a, b, f.cf, B)',
               'and': 'fl_logic(&f, a & b, B)', 'or': 'fl_logic(&f, a | b, B)', 'xor': 'fl_logic(&f, a ^ b, B)'}
        if m in alu:
            B = ops[0].size * 8; e = alu[m].replace('B', str(B))
            return [f'{{ uint32_t a = {rd(ops[0])}, b = {rd(ops[1], B)}; uint32_t r = {e}; {wr(ops[0], "r")} }}']
        if m == 'cmp':
            B = ops[0].size * 8
            return [f'{{ uint32_t a = {rd(ops[0])}, b = {rd(ops[1], B)}; fl_sub(&f, a, b, 0, {B}); }}']
        if m == 'test':
            B = ops[0].size * 8
            return [f'{{ uint32_t a = {rd(ops[0])}, b = {rd(ops[1], B)}; fl_logic(&f, a & b, {B}); }}']
        if m in ('inc', 'dec'):
            B = ops[0].size * 8
            return [f'{{ uint32_t r = fl_{m}(&f, {rd(ops[0])}, {B}); {wr(ops[0], "r")} }}']
        if m == 'neg':
            B = ops[0].size * 8
            return [f'{{ uint32_t r = fl_neg(&f, {rd(ops[0])}, {B}); {wr(ops[0], "r")} }}']
        if m == 'not':
            return [wr(ops[0], f'~{rd(ops[0])}')]
        sh = {'shl': 'fl_shl', 'sal': 'fl_shl', 'shr': 'fl_shr', 'sar': 'fl_sar', 'rol': 'fl_rol', 'ror': 'fl_ror'}
        if m in sh:
            B = ops[0].size * 8
            cnt = rd(ops[1], 8) if n > 1 else '1'
            return [f'{{ uint32_t r = {sh[m]}(&f, {rd(ops[0])}, {cnt}, {B}); {wr(ops[0], "r")} }}']
        if m in ('shld', 'shrd'):
            B = ops[0].size * 8
            if B != 32: raise Unsupported('shld16')
            cnt = rd(ops[2], 8)
            if m == 'shld':
                return [f'{{ uint32_t d = {rd(ops[0])}, s = {rd(ops[1])}, n = {cnt} & 31; if (n) {{ uint32_t r = (d << n) | (s >> (32 - n)); f.cf = (d >> (32 - n)) & 1; f.of = ((r ^ d) >> 31) & 1; fl_zsp(&f, r, 32); {wr(ops[0], "r")} }} }}']
            return [f'{{ uint32_t d = {rd(ops[0])}, s = {rd(ops[1])}, n = {cnt} & 31; if (n) {{ uint32_t r = (d >> n) | (s << (32 - n)); f.cf = (d >> (n - 1)) & 1; f.of = ((r ^ d) >> 31) & 1; fl_zsp(&f, r, 32); {wr(ops[0], "r")} }} }}']
        if m == 'mul':
            B = ops[0].size * 8
            if B == 32: return [f'{{ uint64_t p = (uint64_t)eax * {rd(ops[0])}; eax = (uint32_t)p; edx = (uint32_t)(p >> 32); f.cf = f.of = edx != 0; }}']
            if B == 16: return [f'{{ uint32_t p = (uint16_t)eax * (uint32_t){rd(ops[0])}; eax = (eax & 0xffff0000u) | (p & 0xffff); edx = (edx & 0xffff0000u) | (p >> 16); f.cf = f.of = (p >> 16) != 0; }}']
            return [f'{{ uint32_t p = (uint8_t)eax * (uint32_t){rd(ops[0])}; eax = (eax & 0xffff0000u) | (p & 0xffff); f.cf = f.of = (p >> 8) != 0; }}']
        if m == 'imul':
            if n == 1:
                B = ops[0].size * 8
                if B == 32: return [f'{{ int64_t p = (int64_t)(int32_t)eax * (int32_t){rd(ops[0])}; eax = (uint32_t)p; edx = (uint32_t)(p >> 32); f.cf = f.of = p != (int64_t)(int32_t)p; }}']
                if B == 16: return [f'{{ int32_t p = (int32_t)(int16_t)eax * (int16_t){rd(ops[0])}; eax = (eax & 0xffff0000u) | (p & 0xffff); edx = (edx & 0xffff0000u) | ((uint32_t)p >> 16 & 0xffff); f.cf = f.of = p != (int16_t)p; }}']
                return [f'{{ int32_t p = (int32_t)(int8_t)eax * (int8_t){rd(ops[0])}; eax = (eax & 0xffff0000u) | (p & 0xffff); f.cf = f.of = p != (int8_t)p; }}']
            B = ops[0].size * 8
            a, b = (ops[0], ops[1]) if n == 2 else (ops[1], ops[2])
            if B == 16: return [f'{{ int32_t p = (int32_t)(int16_t){rd(a)} * (int16_t){rd(b, 16)}; {wr(ops[0], "(uint16_t)p")} f.cf = f.of = p != (int16_t)p; }}']
            return [f'{{ int64_t p = (int64_t)(int32_t){rd(a)} * (int32_t){rd(b, 32)}; {wr(ops[0], "(uint32_t)p")} f.cf = f.of = p != (int64_t)(int32_t)p; }}']
        if m == 'div':
            B = ops[0].size * 8
            if B == 32: return [f'{{ uint32_t d = {rd(ops[0])}; uint64_t nn = ((uint64_t)edx << 32) | eax; eax = (uint32_t)(nn / d); edx = (uint32_t)(nn % d); }}']
            if B == 16: return [f'{{ uint32_t d = {rd(ops[0])}; uint32_t nn = ((edx & 0xffff) << 16) | (eax & 0xffff); eax = (eax & 0xffff0000u) | (uint16_t)(nn / d); edx = (edx & 0xffff0000u) | (uint16_t)(nn % d); }}']
            return [f'{{ uint32_t d = {rd(ops[0])}; uint32_t nn = eax & 0xffff; eax = (eax & 0xffff0000u) | ((nn % d) << 8) | (uint8_t)(nn / d); }}']
        if m == 'idiv':
            B = ops[0].size * 8
            if B == 32: return [f'{{ int32_t d = (int32_t){rd(ops[0])}; int64_t nn = (int64_t)(((uint64_t)edx << 32) | eax); eax = (uint32_t)(int32_t)(nn / d); edx = (uint32_t)(int32_t)(nn % d); }}']
            if B == 16: return [f'{{ int32_t d = (int16_t){rd(ops[0])}; int32_t nn = (int32_t)(((edx & 0xffff) << 16) | (eax & 0xffff)); eax = (eax & 0xffff0000u) | (uint16_t)(nn / d); edx = (edx & 0xffff0000u) | (uint16_t)(nn % d); }}']
            return [f'{{ int32_t d = (int8_t){rd(ops[0])}; int32_t nn = (int16_t)eax; eax = (eax & 0xffff0000u) | (((uint32_t)(nn % d) & 0xff) << 8) | (uint8_t)(nn / d); }}']
        if m in ('bt', 'bts', 'btr', 'btc'):
            B = ops[0].size * 8
            if ops[0].type == X.X86_OP_MEM and ops[1].type == X.X86_OP_REG:
                if B != 32: raise Unsupported('bt16 mem,reg')
                newv = {'bt': None, 'bts': 'v | (1u << b)', 'btr': 'v & ~(1u << b)', 'btc': 'v ^ (1u << b)'}[m]
                return [f'{{ int32_t o = (int32_t){rd(ops[1])}; uint32_t ad = {ea(ops[0].mem)} + (uint32_t)((o >> 5) * 4); uint32_t b = (uint32_t)o & 31; uint32_t v = M32(ad); f.cf = (v >> b) & 1; ' + (f'M32(ad) = {newv}; ' if newv else '') + '}']
            bit = f'({rd(ops[1], 8)} & {B - 1})' if ops[1].type == X.X86_OP_IMM else f'({rd(ops[1])} & {B - 1})'
            new = {'bt': None, 'bts': f'v | (1u << {bit})', 'btr': f'v & ~(1u << {bit})', 'btc': f'v ^ (1u << {bit})'}[m]
            s = f'{{ uint32_t v = {rd(ops[0])}; f.cf = (v >> {bit}) & 1; '
            if new: s += wr(ops[0], new) + ' '
            return [s + '}']
        if m in ('rcl', 'rcr'):
            B = ops[0].size * 8; cnt = rd(ops[1], 8) if ops[1].type == X.X86_OP_IMM else '(uint8_t)ecx'
            return [f'{{ uint32_t v = {rd(ops[0])}; v = fl_rc{m[2]}(&f, v, {cnt}, {B}); {wr(ops[0], "v")} }}']
        if m == 'xlatb' or m == 'xlat': return ['eax = (eax & 0xffffff00u) | M8(ebx + (uint8_t)eax);']
        if m == 'stmxcsr': return [f'M32({ea(ops[0].mem)}) = c->mxcsr;']
        if m == 'ldmxcsr': return [f'c->mxcsr = M32({ea(ops[0].mem)});']
        if m in ('bsf', 'bsr'):
            B = ops[0].size * 8
            if B != 32: raise Unsupported('bsf16')
            e = '__builtin_ctz(v)' if m == 'bsf' else '(31 - __builtin_clz(v))'
            return [f'{{ uint32_t v = {rd(ops[1])}; f.zf = v == 0; if (v) {{ {wr(ops[0], e)} }} }}']
        # ---- conditionals ----
        if m.startswith('set') and m[3:] in COND: return [wr(ops[0], f'({COND[m[3:]]}) ? 1 : 0', 8)]
        if m.startswith('cmov') and m[4:] in COND:
            return [f'if ({COND[m[4:]]}) {{ {wr(ops[0], rd(ops[1]))} }}']
        # ---- control flow ----
        if m == 'ret':
            pop = ops[0].imm if n else 0
            return [f'{{ uint32_t pc = M32(esp); esp += {4 + pop}; {SPILL} c->pc = pc; MUSTTAIL return rt_dispatch(c); }}']
        if m == 'call':
            op = ops[0]; ret = i.address + i.size
            if op.type == X.X86_OP_IMM: return self.call_stmt(None, ret, direct=op.imm)
            return self.call_stmt(rd(op, 32), ret)
        if m == 'jmp':
            op = ops[0]
            if op.type == X.X86_OP_IMM: return [self.jump_to(op.imm)]
            self.has_indirect_jmp = True
            return [f'{{ uint32_t tgt = {rd(op, 32)}; JMP_DISPATCH(tgt) }}']
        if m.startswith('j') and m[1:] in COND:
            return [self.jump_to(ops[0].imm, COND[m[1:]])]
        if m in ('jecxz', 'jcxz'): return [self.jump_to(ops[0].imm, 'ecx == 0')]
        if m in ('loop', 'loope', 'loopne'):
            cond = {'loop': '', 'loope': ' && f.zf', 'loopne': ' && !f.zf'}[m]
            return [f'ecx--; ' + self.jump_to(ops[0].imm, f'ecx != 0{cond}')]
        # ---- string ops ----
        parts = m.split(' '); base = parts[-1] if parts[0] in ('rep', 'repe', 'repne') else parts[0]
        if base in ('movsb', 'movsw', 'movsd', 'stosb', 'stosw', 'stosd', 'lodsb', 'lodsw', 'lodsd', 'scasb', 'scasw', 'scasd', 'cmpsb', 'cmpsw', 'cmpsd') and not any(op.type == X.X86_OP_REG and X.X86_REG_XMM0 <= op.reg <= X.X86_REG_XMM15 for op in ops):
            return self.string_op(i)
        r = self.fp_insn(i)
        if r is not None: return r
        raise Unsupported(m)

    def string_op(self, i):
        m = i.mnemonic; rep = ''
        parts = m.split(' ')
        if parts[0] in ('rep', 'repe', 'repne'): rep = parts[0]; m = parts[1] if len(parts) > 1 else ''
        else:
            if i.prefix[0] == 0xf3: rep = 'rep'
            elif i.prefix[0] == 0xf2: rep = 'repne'
        name = m[:-1]; sz = {'b': 1, 'w': 2, 'd': 4}[m[-1]]; B = sz * 8
        step = f'(c->df ? -{sz} : {sz})'
        if name == 'movs': body = f'M{B}(edi) = M{B}(esi); esi += {step}; edi += {step};'
        elif name == 'stos': body = f'M{B}(edi) = (uint{B}_t)eax; edi += {step};'
        elif name == 'lods':
            body = (f'eax = (eax & ~{MASK[B]:#x}u) | M{B}(esi); esi += {step};' if sz < 4 else f'eax = M32(esi); esi += {step};')
        elif name == 'scas': body = f'fl_sub(&f, (uint{B}_t)eax, M{B}(edi), 0, {B}); edi += {step};'
        elif name == 'cmps': body = f'fl_sub(&f, M{B}(esi), M{B}(edi), 0, {B}); esi += {step}; edi += {step};'
        else: raise Unsupported(m)
        if not rep: return [f'{{ {body} }}']
        cond = ''
        if name in ('scas', 'cmps'):
            cond = ' if (f.zf == 0) break;' if rep == 'repe' or rep == 'rep' else ' if (f.zf != 0) break;'
        return [f'while (ecx) {{ {body} ecx--;{cond} }}']

    # ---- SSE / x87 -------------------------------------------------------------------------------------------------------------
    def fp_insn(self, i):
        m = i.mnemonic; ops = i.operands; n = len(ops)
        def isx(o): return o.type == X.X86_OP_REG and X.X86_REG_XMM0 <= o.reg <= X.X86_REG_XMM7
        def xi(o): return o.reg - X.X86_REG_XMM0
        def isst(o): return o.type == X.X86_OP_REG and X.X86_REG_ST0 <= o.reg <= X.X86_REG_ST7
        def si(o): return o.reg - X.X86_REG_ST0
        if any(o.type == X.X86_OP_REG and X.X86_REG_XMM8 <= o.reg <= X.X86_REG_XMM15 for o in ops): raise Unsupported('xmm8+')
        def ismm(o): return o.type == X.X86_OP_REG and X.X86_REG_MM0 <= o.reg <= X.X86_REG_MM7
        def vecreg(o): return o.type == X.X86_OP_REG and (ismm(o) or X.X86_REG_XMM0 <= o.reg <= X.X86_REG_XMM7)
        def vp(o):
            if o.type == X.X86_OP_MEM: return f'(G + {ea(o.mem)})'
            if ismm(o): return f'((uint8_t *)&c->mm[{o.reg - X.X86_REG_MM0}])'
            return f'c->xmm[{o.reg - X.X86_REG_XMM0}]'
        def vn(o): return 8 if ismm(o) else 16
        VOPS = {k.lower(): 'V_' + k for k in ('PCMPEQB PCMPEQW PCMPEQD PCMPGTB PCMPGTW PCMPGTD PADDB PADDW PADDD PADDQ PSUBB PSUBW PSUBD PSUBQ PADDUSB PADDUSW PSUBUSB PSUBUSW '
                'PADDSB PADDSW PSUBSB PSUBSW PMAXUB PMINUB PMAXSW PMINSW PAVGB PAVGW PMULLW PMULHW PMULHUW PMADDWD PSADBW PAND PANDN POR PXOR '
                'PUNPCKLBW PUNPCKLWD PUNPCKLDQ PUNPCKHBW PUNPCKHWD PUNPCKHDQ PACKSSWB PACKUSWB PACKSSDW').split()}
        VSH = {k.lower(): 'V_' + k for k in 'PSLLW PSLLD PSLLQ PSRLW PSRLD PSRLQ PSRAW PSRAD'.split()}
        if n == 2 and vecreg(ops[0]):
            if m in VOPS and not (m in ('pand', 'pandn', 'por', 'pxor') and not ismm(ops[0]) and False):
                return [f'v_binop({VOPS[m]}, {vp(ops[0])}, {vp(ops[1])}, {vn(ops[0])});']
            if m in VSH:
                cnt = f'{ops[1].imm & 0xff}ull' if ops[1].type == X.X86_OP_IMM else f'(*(uint64_t *){vp(ops[1])})'
                return [f'v_shift({VSH[m]}, {vp(ops[0])}, {cnt}, {vn(ops[0])});']
            if m == 'movd' and ismm(ops[0]): return [f'c->mm[{ops[0].reg - X.X86_REG_MM0}] = (uint32_t)({rd(ops[1], 32)});']
            if m == 'movq' and ismm(ops[0]): return [f'c->mm[{ops[0].reg - X.X86_REG_MM0}] = *(uint64_t *){vp(ops[1])};']
        if n == 2 and ops[1].type == X.X86_OP_REG and ismm(ops[1]):
            if m == 'movd': return [wr(ops[0], f'(uint32_t)c->mm[{ops[1].reg - X.X86_REG_MM0}]', 32)]
            if m == 'movq': return [f'*(uint64_t *){vp(ops[0])} = c->mm[{ops[1].reg - X.X86_REG_MM0}];']
        if m == 'pmovmskb' and n == 2: return [wr(ops[0], f'v_movmskb({vp(ops[1])}, {vn(ops[1])})', 32)]
        if m == 'pextrw' and n == 3:
            lane = ops[2].imm & (vn(ops[1]) // 2 - 1)
            return [wr(ops[0], f'*(uint16_t *)({vp(ops[1])} + {2 * lane})', 32)]
        if m == 'pinsrw' and n == 3:
            lane = ops[2].imm & (vn(ops[0]) // 2 - 1)
            return [f'*(uint16_t *)({vp(ops[0])} + {2 * lane}) = (uint16_t)({rd(ops[1], 16)});']
        if m == 'pshufw' and n == 3 or m in ('pshuflw', 'pshufhw') and n == 3:
            imm = ops[2].imm & 0xff; off = 8 if m == 'pshufhw' else 0
            return [f'{{ uint16_t t[8]; memcpy(t, {vp(ops[1])}, {vn(ops[1])}); uint16_t *D = (uint16_t *){vp(ops[0])}; memcpy(D, t, {vn(ops[1])}); ' + ' '.join(f'D[{(off // 2) + k}] = t[{(off // 2) + ((imm >> (2 * k)) & 3)}];' for k in range(4)) + ' }']
        if m in ('prefetchw', 'prefetch', 'emms', 'femms'): return []
        # ---- SSE ----
        if m.startswith('v'): raise Unsupported(m)
        m = {'fcompi': 'fcomip', 'fucompi': 'fucomip', 'movntps': 'movaps', 'movntpd': 'movapd', 'movntdq': 'movdqa'}.get(m, m)     # non-temporal stores are plain stores here
        import re as _re
        _cm = _re.fullmatch(r'cmp(eq|lt|le|unord|neq|nlt|nle|ord)(ss|sd|ps|pd)', m)
        if any(isx(o) for o in ops) or m in ('cvttss2si', 'cvttsd2si', 'cvtss2si', 'cvtsd2si'):
            d = ops[0]; sr = ops[1] if n > 1 else None
            def m32(o): return f'XF({xi(o)}, 0)' if isx(o) else f'MF32({ea(o.mem)})'
            def m64(o): return f'XD({xi(o)}, 0)' if isx(o) else f'MF64({ea(o.mem)})'
            def q(o, k):                                       # 64-bit half k of an xmm/mem operand
                return f'XQ({xi(o)}, {k})' if isx(o) else f'M64({ea(o.mem)} + {8 * k})'
            def u(o, k):                                       # 32-bit lane k
                return f'XU({xi(o)}, {k})' if isx(o) else f'M32({ea(o.mem)} + {4 * k})'
            def ps_lane(o, k): return f'XF({xi(o)}, {k})' if isx(o) else f'MF32({ea(o.mem)} + {4 * k})'
            def pd_lane(o, k): return f'XD({xi(o)}, {k})' if isx(o) else f'MF64({ea(o.mem)} + {8 * k})'
            if m in ('movaps', 'movups', 'movdqa', 'movdqu', 'movapd', 'movupd'):
                if isx(d): return [f'{{ uint64_t a = {q(sr,0)}, b = {q(sr,1)}; XQ({xi(d)}, 0) = a; XQ({xi(d)}, 1) = b; }}']
                return [f'{{ uint64_t a = XQ({xi(sr)}, 0), b = XQ({xi(sr)}, 1); M64({ea(d.mem)}) = a; M64({ea(d.mem)} + 8) = b; }}']
            if m == 'movss':
                if isx(d):
                    if isx(sr): return [f'XU({xi(d)}, 0) = XU({xi(sr)}, 0);']
                    return [f'{{ uint32_t v = M32({ea(sr.mem)}); XU({xi(d)}, 0) = v; XU({xi(d)}, 1) = 0; XQ({xi(d)}, 1) = 0; }}']
                return [f'M32({ea(d.mem)}) = XU({xi(sr)}, 0);']
            if m == 'movsd':
                if isx(d):
                    if isx(sr): return [f'XQ({xi(d)}, 0) = XQ({xi(sr)}, 0);']
                    return [f'{{ uint64_t v = M64({ea(sr.mem)}); XQ({xi(d)}, 0) = v; XQ({xi(d)}, 1) = 0; }}']
                return [f'M64({ea(d.mem)}) = XQ({xi(sr)}, 0);']
            if m == 'movq':
                if isx(d): return [f'{{ uint64_t v = {q(sr,0)}; XQ({xi(d)}, 0) = v; XQ({xi(d)}, 1) = 0; }}']
                return [f'M64({ea(d.mem)}) = XQ({xi(sr)}, 0);']
            if m == 'movd':
                if isx(d): return [f'{{ uint32_t v = {rd(sr, 32)}; XU({xi(d)}, 0) = v; XU({xi(d)}, 1) = 0; XQ({xi(d)}, 1) = 0; }}']
                return [wr(d, f'XU({xi(sr)}, 0)', 32)]
            if m in ('movlps', 'movlpd'):
                if isx(d): return [f'XQ({xi(d)}, 0) = {q(sr,0)};']
                return [f'M64({ea(d.mem)}) = XQ({xi(sr)}, 0);']
            if m in ('movhps', 'movhpd'):
                if isx(d): return [f'XQ({xi(d)}, 1) = {q(sr,0)};']
                return [f'M64({ea(d.mem)}) = XQ({xi(sr)}, 1);']
            if m == 'movlhps': return [f'XQ({xi(d)}, 1) = XQ({xi(sr)}, 0);']
            if m == 'movhlps': return [f'XQ({xi(d)}, 0) = XQ({xi(sr)}, 1);']
            if m in ('movmskps', 'movmskpd'):
                if m == 'movmskps': return [wr(d, f'((XU({xi(sr)},0)>>31)&1) | (((XU({xi(sr)},1)>>31)&1)<<1) | (((XU({xi(sr)},2)>>31)&1)<<2) | (((XU({xi(sr)},3)>>31)&1)<<3)', 32)]
                return [wr(d, f'((XQ({xi(sr)},0)>>63)&1) | (((XQ({xi(sr)},1)>>63)&1)<<1)', 32)]
            # scalar arithmetic
            ARITH = {'add': '+', 'sub': '-', 'mul': '*', 'div': '/'}
            for suf, w in (('ss', 32), ('sd', 64)):
                if m.endswith(suf) and m[:-2] in ARITH:
                    lane = m32 if w == 32 else m64; x = f'XF({xi(d)}, 0)' if w == 32 else f'XD({xi(d)}, 0)'
                    return [f'{{ {"float" if w == 32 else "double"} b = {lane(sr)}; {x} = {x} {ARITH[m[:-2]]} b; }}']
                if m in ('min' + suf, 'max' + suf):
                    lane = m32 if w == 32 else m64; x = f'XF({xi(d)}, 0)' if w == 32 else f'XD({xi(d)}, 0)'
                    fn = 'sse_' + m[:3] + ('' if w == 32 else 'd')
                    return [f'{{ {"float" if w == 32 else "double"} b = {lane(sr)}; {x} = {fn}({x}, b); }}']
                if m == 'sqrt' + suf:
                    lane = m32 if w == 32 else m64; x = f'XF({xi(d)}, 0)' if w == 32 else f'XD({xi(d)}, 0)'
                    return [f'{x} = {"sqrtf" if w == 32 else "sqrt"}({lane(sr)});']
            if m == 'rcpss': return [f'XF({xi(d)}, 0) = 1.0f / {m32(sr)};']
            if m == 'rsqrtss': return [f'XF({xi(d)}, 0) = 1.0f / sqrtf({m32(sr)});']
            if m == 'rcpps': return ['{ ' + ' '.join(f'XF({xi(d)}, {k}) = 1.0f / {ps_lane(sr, k)};' for k in range(4)) + ' }']
            if m == 'rsqrtps': return ['{ ' + ' '.join(f'XF({xi(d)}, {k}) = 1.0f / sqrtf({ps_lane(sr, k)});' for k in range(4)) + ' }']
            if m == 'cvtpi2ps': return [f'{{ int32_t a = (int32_t)(c->mm[{sr.reg - X.X86_REG_MM0}] & 0xffffffffu), b = (int32_t)(c->mm[{sr.reg - X.X86_REG_MM0}] >> 32); XF({xi(d)}, 0) = (float)a; XF({xi(d)}, 1) = (float)b; }}'] if sr.type == X.X86_OP_REG else [f'{{ int32_t a = (int32_t)M32({ea(sr.mem)}), b = (int32_t)M32({ea(sr.mem)} + 4); XF({xi(d)}, 0) = (float)a; XF({xi(d)}, 1) = (float)b; }}']
            # packed arithmetic
            for suf, w, nl in (('ps', 32, 4), ('pd', 64, 2)):
                if m.endswith(suf) and m[:-2] in ARITH:
                    lane = ps_lane if w == 32 else pd_lane; X_ = 'XF' if w == 32 else 'XD'; t = 'float' if w == 32 else 'double'
                    return [f'{{ {t} b[{nl}]; ' + ' '.join(f'b[{k}] = {lane(sr, k)};' for k in range(nl)) + ' ' + ' '.join(f'{X_}({xi(d)}, {k}) = {X_}({xi(d)}, {k}) {ARITH[m[:-2]]} b[{k}];' for k in range(nl)) + ' }']
                if m in ('min' + suf, 'max' + suf):
                    lane = ps_lane if w == 32 else pd_lane; X_ = 'XF' if w == 32 else 'XD'; t = 'float' if w == 32 else 'double'; fn = 'sse_' + m[:3] + ('' if w == 32 else 'd')
                    return [f'{{ {t} b[{nl}]; ' + ' '.join(f'b[{k}] = {lane(sr, k)};' for k in range(nl)) + ' ' + ' '.join(f'{X_}({xi(d)}, {k}) = {fn}({X_}({xi(d)}, {k}), b[{k}]);' for k in range(nl)) + ' }']
                if m == 'sqrt' + suf:
                    lane = ps_lane if w == 32 else pd_lane; X_ = 'XF' if w == 32 else 'XD'; fnn = 'sqrtf' if w == 32 else 'sqrt'
                    return [f'{{ ' + ' '.join(f'{t_}' for t_ in []) + ' '.join(f'{X_}({xi(d)}, {k}) = {fnn}({lane(sr, k)});' for k in range(nl)) + ' }']
            # bitwise
            LOG = {'andps': '&', 'andpd': '&', 'pand': '&', 'orps': '|', 'orpd': '|', 'por': '|', 'xorps': '^', 'xorpd': '^', 'pxor': '^'}
            if m in LOG:
                if m in ('xorps', 'pxor', 'xorpd') and isx(sr) and xi(sr) == xi(d): return [f'XQ({xi(d)}, 0) = 0; XQ({xi(d)}, 1) = 0;']
                return [f'{{ uint64_t a = {q(sr,0)}, b = {q(sr,1)}; XQ({xi(d)}, 0) {LOG[m]}= a; XQ({xi(d)}, 1) {LOG[m]}= b; }}']
            if m in ('andnps', 'andnpd', 'pandn'):
                return [f'{{ uint64_t a = {q(sr,0)}, b = {q(sr,1)}; XQ({xi(d)}, 0) = ~XQ({xi(d)}, 0) & a; XQ({xi(d)}, 1) = ~XQ({xi(d)}, 1) & b; }}']
            if m in ('comiss', 'ucomiss'): return [f'sse_comis(&f, XF({xi(d)}, 0), {m32(sr)});']
            if m in ('comisd', 'ucomisd'): return [f'sse_comis(&f, XD({xi(d)}, 0), {m64(sr)});']
            # conversions
            if m == 'cvtsi2ss': return [f'XF({xi(d)}, 0) = (float)(int32_t){rd(sr, 32)};']
            if m == 'cvtsi2sd': return [f'XD({xi(d)}, 0) = (double)(int32_t){rd(sr, 32)};']
            if m == 'cvttss2si': return [wr(d, f'sse_cvtt({m32(sr)})', 32)]
            if m == 'cvttsd2si': return [wr(d, f'sse_cvtt({m64(sr)})', 32)]
            if m == 'cvtss2si': return [wr(d, f'sse_cvt({m32(sr)})', 32)]
            if m == 'cvtsd2si': return [wr(d, f'sse_cvt({m64(sr)})', 32)]
            if m == 'cvtss2sd': return [f'XD({xi(d)}, 0) = (double){m32(sr)};']
            if m == 'cvtsd2ss': return [f'XF({xi(d)}, 0) = (float){m64(sr)};']
            if m == 'cvtps2pd': return [f'{{ float a = {ps_lane(sr,0)}, b = {ps_lane(sr,1)}; XD({xi(d)}, 0) = a; XD({xi(d)}, 1) = b; }}']
            if m == 'cvtpd2ps': return [f'{{ double a = {pd_lane(sr,0)}, b = {pd_lane(sr,1)}; XF({xi(d)}, 0) = (float)a; XF({xi(d)}, 1) = (float)b; XU({xi(d)}, 2) = 0; XU({xi(d)}, 3) = 0; }}']
            if m == 'cvtdq2ps': return [f'{{ int32_t a[4]; ' + ' '.join(f'a[{k}] = (int32_t){u(sr,k)};' for k in range(4)) + ' ' + ' '.join(f'XF({xi(d)}, {k}) = (float)a[{k}];' for k in range(4)) + ' }']
            if m in ('cvttps2dq', 'cvtps2dq'):
                fn = 'sse_cvtt' if m == 'cvttps2dq' else 'sse_cvt'
                return [f'{{ float a[4]; ' + ' '.join(f'a[{k}] = {ps_lane(sr,k)};' for k in range(4)) + ' ' + ' '.join(f'XU({xi(d)}, {k}) = (uint32_t){fn}(a[{k}]);' for k in range(4)) + ' }']
            if m in ('cvttpd2dq', 'cvtpd2dq'):
                fn = 'sse_cvtt' if m == 'cvttpd2dq' else 'sse_cvt'
                return [f'{{ double a = {pd_lane(sr, 0)}, b = {pd_lane(sr, 1)}; XU({xi(d)}, 0) = (uint32_t){fn}(a); XU({xi(d)}, 1) = (uint32_t){fn}(b); XQ({xi(d)}, 1) = 0; }}']
            if m == 'pmuludq' and isx(d): return [f'{{ uint64_t p0 = (uint64_t)XU({xi(d)}, 0) * (uint32_t){u(sr,0)}, p1 = (uint64_t)XU({xi(d)}, 2) * (uint32_t){u(sr,2)}; XQ({xi(d)}, 0) = p0; XQ({xi(d)}, 1) = p1; }}']
            if m == 'cvtdq2pd': return [f'{{ int32_t a = (int32_t){u(sr,0)}, b = (int32_t){u(sr,1)}; XD({xi(d)}, 0) = a; XD({xi(d)}, 1) = b; }}']
            # shuffles
            if m == 'shufps':
                imm = ops[2].imm & 0xff
                return [f'{{ float a[4], b[4]; ' + ' '.join(f'a[{k}] = XF({xi(d)}, {k});' for k in range(4)) + ' ' + ' '.join(f'b[{k}] = {ps_lane(sr,k)};' for k in range(4)) +
                        f' XF({xi(d)}, 0) = a[{imm & 3}]; XF({xi(d)}, 1) = a[{(imm >> 2) & 3}]; XF({xi(d)}, 2) = b[{(imm >> 4) & 3}]; XF({xi(d)}, 3) = b[{(imm >> 6) & 3}]; }}']
            if m == 'shufpd':
                imm = ops[2].imm & 3
                return [f'{{ double a0 = XD({xi(d)}, {imm & 1}), b0 = {pd_lane(sr, (imm >> 1) & 1)}; XD({xi(d)}, 0) = a0; XD({xi(d)}, 1) = b0; }}']
            if m == 'pshufd':
                imm = ops[2].imm & 0xff
                return [f'{{ uint32_t t[4]; ' + ' '.join(f't[{k}] = {u(sr,k)};' for k in range(4)) + ' ' + ' '.join(f'XU({xi(d)}, {k}) = t[{(imm >> (2 * k)) & 3}];' for k in range(4)) + ' }']
            if m == 'unpcklps': return [f'{{ uint32_t a0 = XU({xi(d)},0), a1 = XU({xi(d)},1), b0 = {u(sr,0)}, b1 = {u(sr,1)}; XU({xi(d)},0) = a0; XU({xi(d)},1) = b0; XU({xi(d)},2) = a1; XU({xi(d)},3) = b1; }}']
            if m == 'unpckhps': return [f'{{ uint32_t a2 = XU({xi(d)},2), a3 = XU({xi(d)},3), b2 = {u(sr,2)}, b3 = {u(sr,3)}; XU({xi(d)},0) = a2; XU({xi(d)},1) = b2; XU({xi(d)},2) = a3; XU({xi(d)},3) = b3; }}']
            if m in ('unpcklpd', 'punpcklqdq'): return [f'XQ({xi(d)}, 1) = {q(sr,0)};']
            if m in ('unpckhpd', 'punpckhqdq'): return [f'{{ uint64_t a = XQ({xi(d)}, 1), b = {q(sr,1)}; XQ({xi(d)}, 0) = a; XQ({xi(d)}, 1) = b; }}']
            if m == 'punpckldq': return [f'{{ uint32_t a0 = XU({xi(d)},0), a1 = XU({xi(d)},1), b0 = {u(sr,0)}, b1 = {u(sr,1)}; XU({xi(d)},0) = a0; XU({xi(d)},1) = b0; XU({xi(d)},2) = a1; XU({xi(d)},3) = b1; }}']
            if m in ('cmpss', 'cmpsd', 'cmpps', 'cmppd'): return self.sse_cmp(i, ops, isx, xi, m32, m64, ps_lane, pd_lane)
            if _cm: return self.sse_cmp(i, ops, isx, xi, m32, m64, ps_lane, pd_lane, pred=['eq','lt','le','unord','neq','nlt','nle','ord'].index(_cm.group(1)), m='cmp' + _cm.group(2))
            ALL4 = {'paddd': ('uint32_t', 'XU', 4, '+'), 'psubd': ('uint32_t', 'XU', 4, '-'), 'paddq': ('uint64_t', 'XQ', 2, '+'), 'psubq': ('uint64_t', 'XQ', 2, '-')}
            if m in ALL4:
                t, X_, nl, op_ = ALL4[m]; lane = (lambda o, k: u(o, k)) if nl == 4 else (lambda o, k: q(o, k))
                return [f'{{ {t} b[{nl}]; ' + ' '.join(f'b[{k}] = {lane(sr, k)};' for k in range(nl)) + ' ' + ' '.join(f'{X_}({xi(d)}, {k}) {op_}= b[{k}];' for k in range(nl)) + ' }']
            if m in ('paddw', 'psubw', 'paddb', 'psubb'):
                bw = 16 if m[-1] == 'w' else 8; op_ = '+' if m[1] == 'a' else '-'; nl = 128 // bw
                return [f'{{ uint{bw}_t a[{nl}], b[{nl}]; memcpy(a, c->xmm[{xi(d)}], 16); ' + (f'memcpy(b, c->xmm[{xi(sr)}], 16);' if isx(sr) else f'memcpy(b, G + {ea(sr.mem)}, 16);') + f' for (int k = 0; k < {nl}; k++) a[k] = a[k] {op_} b[k]; memcpy(c->xmm[{xi(d)}], a, 16); }}']
            if m in ('pslld', 'psrld', 'psllq', 'psrlq', 'psrad') and sr.type == X.X86_OP_IMM:
                cnt = sr.imm & 0xff
                if m == 'pslld': return ['{ ' + ' '.join(f'XU({xi(d)},{k}) = {cnt} > 31 ? 0 : XU({xi(d)},{k}) << {cnt};' for k in range(4)) + ' }']
                if m == 'psrld': return ['{ ' + ' '.join(f'XU({xi(d)},{k}) = {cnt} > 31 ? 0 : XU({xi(d)},{k}) >> {cnt};' for k in range(4)) + ' }']
                if m == 'psrad': return ['{ ' + ' '.join(f'XU({xi(d)},{k}) = (uint32_t)((int32_t)XU({xi(d)},{k}) >> {min(cnt, 31)});' for k in range(4)) + ' }']
                if m == 'psllq': return ['{ ' + ' '.join(f'XQ({xi(d)},{k}) = {cnt} > 63 ? 0 : XQ({xi(d)},{k}) << {cnt};' for k in range(2)) + ' }']
                if m == 'psrlq': return ['{ ' + ' '.join(f'XQ({xi(d)},{k}) = {cnt} > 63 ? 0 : XQ({xi(d)},{k}) >> {cnt};' for k in range(2)) + ' }']
            if m in ('pslldq', 'psrldq') and sr.type == X.X86_OP_IMM:
                cnt = min(sr.imm & 0xff, 16)
                if m == 'pslldq': return [f'{{ uint8_t t[16]; memset(t, 0, 16); memcpy(t + {cnt}, c->xmm[{xi(d)}], {16 - cnt}); memcpy(c->xmm[{xi(d)}], t, 16); }}']
                return [f'{{ uint8_t t[16]; memset(t, 0, 16); memcpy(t, c->xmm[{xi(d)}] + {cnt}, {16 - cnt}); memcpy(c->xmm[{xi(d)}], t, 16); }}']
            raise Unsupported(m)
        # ---- x87 ----
        if m.startswith('f') and m not in ('fs',):
            return self.x87(i, ops, n, isst, si, m)
        return None

    def sse_cmp(self, i, ops, isx, xi, m32, m64, ps_lane, pd_lane, pred=None, m=None):
        if pred is None: pred = ops[2].imm & 7
        d = ops[0]; sr = ops[1]; m = m or i.mnemonic
        P = {0: '(a == b)', 1: '(a < b)', 2: '(a <= b)', 3: '(a != a || b != b)', 4: '!(a == b)', 5: '!(a < b)', 6: '!(a <= b)', 7: '(a == a && b == b)'}[pred]
        if m == 'cmpss': return [f'{{ float a = XF({xi(d)}, 0), b = {m32(sr)}; XU({xi(d)}, 0) = {P} ? 0xffffffffu : 0; }}']
        if m == 'cmpsd': return [f'{{ double a = XD({xi(d)}, 0), b = {m64(sr)}; XQ({xi(d)}, 0) = {P} ? ~0ull : 0; }}']
        if m == 'cmpps': return ['{ ' + ' '.join(f'{{ float a = XF({xi(d)}, {k}), b = {ps_lane(sr, k)}; XU({xi(d)}, {k}) = {P} ? 0xffffffffu : 0; }}' for k in range(4)) + ' }']
        return ['{ ' + ' '.join(f'{{ double a = XD({xi(d)}, {k}), b = {pd_lane(sr, k)}; XQ({xi(d)}, {k}) = {P} ? ~0ull : 0; }}' for k in range(2)) + ' }']

    def x87(self, i, ops, n, isst, si, m):
        BIN = {'add': lambda a, b: f'{a} + {b}', 'sub': lambda a, b: f'{a} - {b}', 'subr': lambda a, b: f'{b} - {a}', 'mul': lambda a, b: f'{a} * {b}', 'div': lambda a, b: f'{a} / {b}', 'divr': lambda a, b: f'{b} / {a}'}
        def memval(o, isint):
            sz = o.size * 8
            if isint: return f'(double)(int{sz}_t)M{sz}({ea(o.mem)})'
            return {32: f'(double)MF32({ea(o.mem)})', 64: f'MF64({ea(o.mem)})', 80: f'ld80({ea(o.mem)})'}[sz]
        # arithmetic with memory or register operands
        for base in BIN:
            if m == 'f' + base or m == 'fi' + base:
                isint = m[1] == 'i'
                if n == 1 and ops[0].type == X.X86_OP_MEM: return [f'ST(0) = {BIN[base]("ST(0)", memval(ops[0], isint))};']
                if n == 2 and isst(ops[0]) and isst(ops[1]):
                    if si(ops[0]) == 0: return [f'ST(0) = {BIN[base]("ST(0)", f"ST({si(ops[1])})")};']
                    return [f'ST({si(ops[0])}) = {BIN[base](f"ST({si(ops[0])})", "ST(0)")};']
                if n == 1 and isst(ops[0]): return [f'ST(0) = {BIN[base]("ST(0)", f"ST({si(ops[0])})")};']
            if m == 'f' + base + 'p':
                k = si(ops[0]) if n >= 1 and isst(ops[0]) else 1
                return [f'ST({k}) = {BIN[base](f"ST({k})", "ST(0)")}; fpop(c);']
        if m == 'fld':
            o = ops[0]
            if o.type == X.X86_OP_MEM: return [f'{{ double v = {memval(o, False)}; fpush(c, v); }}']
            return [f'{{ double v = ST({si(o)}); fpush(c, v); }}']
        if m == 'fild': return [f'{{ double v = {memval(ops[0], True)}; fpush(c, v); }}']
        if m == 'fld1': return ['fpush(c, 1.0);']
        if m == 'fldz': return ['fpush(c, 0.0);']
        if m == 'fldpi': return ['fpush(c, 3.14159265358979323846);']
        if m == 'fldl2e': return ['fpush(c, 1.4426950408889634074);']
        if m == 'fldln2': return ['fpush(c, 0.69314718055994530942);']
        if m == 'fldl2t': return ['fpush(c, 3.3219280948873623479);']
        if m == 'fldlg2': return ['fpush(c, 0.30102999566398119521);']
        if m in ('fst', 'fstp'):
            pop = ' fpop(c);' if m == 'fstp' else ''
            o = ops[0]
            if o.type == X.X86_OP_MEM:
                sz = o.size * 8
                st = {32: f'MF32({ea(o.mem)}) = (float)ST(0);', 64: f'MF64({ea(o.mem)}) = ST(0);', 80: f'st80({ea(o.mem)}, ST(0));'}[sz]
                return [st + pop]
            return [f'ST({si(o)}) = ST(0);' + pop]
        if m in ('fist', 'fistp', 'fisttp'):
            o = ops[0]; sz = o.size * 8; pop = ' fpop(c);' if m != 'fist' else ''
            conv = f'x87_toint(c, ST(0), {sz})' if m != 'fisttp' else f'x87_toint_t(ST(0), {sz})'
            return [f'M{sz}({ea(o.mem)}) = (uint{sz}_t){conv};' + pop]
        if m == 'fxch':
            k = max(si(o) for o in ops) if n else 1
            return [f'{{ double t = ST(0); ST(0) = ST({k}); ST({k}) = t; }}']
        if m == 'fchs': return ['ST(0) = -ST(0);']
        if m == 'fabs': return ['ST(0) = fabs(ST(0));']
        if m == 'fsqrt': return ['ST(0) = sqrt(ST(0));']
        if m == 'fsin': return ['ST(0) = sin(ST(0)); c->fsw &= ~0x400;']
        if m == 'fcos': return ['ST(0) = cos(ST(0)); c->fsw &= ~0x400;']
        if m == 'fsincos': return ['{ double v = ST(0); ST(0) = sin(v); fpush(c, cos(v)); c->fsw &= ~0x400; }']
        if m == 'fptan': return ['ST(0) = tan(ST(0)); fpush(c, 1.0); c->fsw &= ~0x400;']
        if m == 'fpatan': return ['ST(1) = atan2(ST(1), ST(0)); fpop(c);']
        if m == 'fyl2x': return ['ST(1) = ST(1) * log2(ST(0)); fpop(c);']
        if m == 'f2xm1': return ['ST(0) = exp2(ST(0)) - 1.0;']
        if m == 'frndint': return ['ST(0) = x87_rint(c, ST(0));']
        if m == 'fprem1': return ['ST(0) = remainder(ST(0), ST(1)); c->fsw &= ~0x400;']
        if m == 'fxam': return ['{ double v = ST(0); c->fsw &= ~0x4700; if (signbit(v)) c->fsw |= 0x200; if (v != v) c->fsw |= 0x100; else if (isinf(v)) c->fsw |= 0x500; else if (v == 0) c->fsw |= 0x4000; else c->fsw |= 0x400; }']
        if m in ('fnstenv', 'fstenv'): return [f'{{ uint32_t a = {ea(ops[0].mem)}; M32(a) = c->fcw; M32(a + 4) = x87_sw(c); M32(a + 8) = 0; M32(a + 12) = 0; M32(a + 16) = 0; M32(a + 20) = 0; M32(a + 24) = 0; }}']
        if m == 'fldenv': return [f'{{ uint32_t a = {ea(ops[0].mem)}; c->fcw = M16(a); c->fsw = M16(a + 4) & ~0x3800; c->top = (M16(a + 4) >> 11) & 7; }}']
        if m == 'fscale': return ['ST(0) = ldexp(ST(0), (int)trunc(ST(1)));']
        if m == 'fprem': return ['ST(0) = fmod(ST(0), ST(1)); c->fsw &= ~0x400;']
        if m in ('fcom', 'fcomp', 'fucom', 'fucomp'):
            b = f'ST({si(ops[0])})' if n and isst(ops[0]) else (memval(ops[0], False) if n else 'ST(1)')
            return [f'x87_com(c, ST(0), {b});' + (' fpop(c);' if m.endswith('p') else '')]
        if m in ('fcompp', 'fucompp'): return ['x87_com(c, ST(0), ST(1)); fpop(c); fpop(c);']
        if m in ('ficom', 'ficomp'):
            return [f'x87_com(c, ST(0), {memval(ops[0], True)});' + (' fpop(c);' if m.endswith('p') else '')]
        if m in ('fcomi', 'fcomip', 'fucomi', 'fucomip'):
            k = si(ops[1]) if n == 2 else si(ops[0])
            return [f'sse_comis(&f, ST(0), ST({k}));' + (' fpop(c);' if m.endswith('p') else '')]
        if m == 'fnstsw' or m == 'fstsw':
            if ops[0].type == X.X86_OP_MEM: return [f'M16({ea(ops[0].mem)}) = x87_sw(c);']
            return [rwrite(ops[0].reg, 'x87_sw(c)')]
        if m in ('fnstcw', 'fstcw'): return [f'M16({ea(ops[0].mem)}) = c->fcw;']
        if m == 'fldcw': return [f'c->fcw = M16({ea(ops[0].mem)});']
        if m in ('fnclex', 'fclex'): return ['c->fsw &= 0x7f00;']
        if m in ('fninit', 'finit'): return ['c->top = 0; c->fcw = 0x37f; c->fsw = 0;']
        if m == 'ffree' or m == 'fwait' or m == 'fnop': return []
        if m in ('fcmovb', 'fcmove', 'fcmovbe', 'fcmovu', 'fcmovnb', 'fcmovne', 'fcmovnbe', 'fcmovnu'):
            cnd = {'b': 'f.cf', 'e': 'f.zf', 'be': '(f.cf|f.zf)', 'u': 'f.pf', 'nb': '!f.cf', 'ne': '!f.zf', 'nbe': '!(f.cf|f.zf)', 'nu': '!f.pf'}[m[5:]]
            return [f'if ({cnd}) ST(0) = ST({si(ops[1])});']
        raise Unsupported(m)

def gen_function(entry, funcset=None):
    L = FnLifter(entry)
    body = L.lines()
    hdr = [f'void f_{entry:08x}(Cpu *c) {{',
           '    uint32_t eax = c->eax, ecx = c->ecx, edx = c->edx, ebx = c->ebx, esp = c->esp, ebp = c->ebp, esi = c->esi, edi = c->edi;',
           '    Fl f = c->fl;',
           f'    if (__builtin_expect(--c->budget < 0, 0)) rt_preempt(c);',
           f'    if (__builtin_expect(rt_trace, 0)) rt_enter(c, {entry:#x}u);']
    if L.has_indirect_jmp:
        cases = ' '.join(f'case {t:#x}u: goto L_{t:08x};' for t in sorted(L.targets))
        hdr.append('    uint32_t dtgt = 0;')
        body = list(body) + ['    L_dispatch:', f'    switch (dtgt) {{ {cases} default: {SPILL} c->pc = dtgt; MUSTTAIL return rt_dispatch(c); }}']
        disp = '#define JMP_DISPATCH(t) { dtgt = (t); goto L_dispatch; }'
    else: disp = '#define JMP_DISPATCH(t) { rt_unsupported(c, 0, "indirect jmp"); }'
    return disp, hdr, body, L

def emit(entries, out):
    global KNOWN
    KNOWN = set()
    for e in entries:
        try: gen_function(e); KNOWN.add(e)
        except Unsupported: pass
    decl, funcs = set(), []
    for e in entries:
        try:
            disp, hdr, body, L = gen_function(e)
        except Unsupported as ex:
            continue
        decl |= L.calls
        funcs.append((e, disp, hdr, body))
    out.write('#include "rt.h"\n')
    for d in sorted(decl): out.write(f'void f_{d:08x}(Cpu *c);\n')
    for e, disp, hdr, body in funcs:
        out.write(disp.replace('JMP_DISPATCH', 'JMP_DISPATCH') + '\n')
        out.write('\n'.join(hdr + body) + '\n}\n#undef JMP_DISPATCH\n')
    return [e for e, *_ in funcs]

def stats():
    uns = collections.Counter(); ok = 0; bad = 0; reasons = collections.Counter(); total_ok_bytes = 0
    for e in sorted(FUNCS):
        sz, hi, nr, th = FUNCS[e]
        try:
            L = FnLifter(e)
        except Unsupported as ex:
            bad += 1; reasons[str(ex).split(' at ')[0]] += 1; continue
        good = True; found = set()
        for a in L.order:
            try: L.insn(L.ins[a])
            except Unsupported as ex:
                good = False; found.add(str(ex))
        for f in found: uns[f] += 1
        if good: ok += 1; total_ok_bytes += sz
        else: bad += 1
    print(f'functions fully liftable: {ok} / {len(FUNCS)}  ({total_ok_bytes} bytes)')
    print('structural failures:', dict(reasons))
    print('functions blocked, by unsupported item:')
    for k, v in uns.most_common(45): print(f'  {v:6d}  {k}')

def seed_functions():
    if MSV:
        import struct as _st
        pe = _st.unpack_from('<I', MSV, 0x3c)[0]; exp_rva, exp_sz = _st.unpack_from('<II', MSV, pe + 0x78)
        ent = _st.unpack_from('<I', MSV, pe + 0x28)[0]; FUNCS[MSV_BASE + ent] = (0, 0, 2, 0)
        if exp_rva:
            nfun, nnames = _st.unpack_from('<II', MSV, exp_rva + 0x14); fa = _st.unpack_from('<I', MSV, exp_rva + 0x1c)[0]
            for k in range(nfun):
                r = _st.unpack_from('<I', MSV, fa + 4 * k)[0]
                if r and not (exp_rva <= r < exp_rva + exp_sz): FUNCS[MSV_BASE + r] = (0, 0, 2, 0)
        print('msvcr90 seeds', sum(1 for a in FUNCS if MSV_BASE <= a < MSV_END), flush=True)
    nptr = 0
    for lo, hi in DATA:
        blob = img_read(lo, hi - lo)
        for off in range(0, len(blob) - 3, 4):
            v = struct.unpack_from('<I', blob, off)[0]
            if in_text(v) and v not in FUNCS and decode_at(v) is not None: FUNCS[v] = (0, 0, 2, 0); nptr += 1
    print('code pointers found in data', nptr, flush=True)
    nsw = 0                                        # the packer's stub section is full of small routines reached through data cells: seed each ret-terminated chunk
    for lo, hi in ([(0x125a000, 0x206f000)] if os.environ.get('LIFT_SWEEP') else []):
        a = lo; start = lo; n = 0; bad = False
        while a < hi:
            ins = decode_at(a)
            if ins is None: a += 1; start = a; n = 0; bad = False; continue
            if ins.mnemonic in GARBAGE: bad = True
            n += 1; a += ins.size
            if ins.mnemonic in ('ret', 'jmp'):
                if not bad and n <= 60 and start not in FUNCS: FUNCS[start] = (0, 0, 2, 0); nsw += 1
                start = a; n = 0; bad = False
    print('stub-section chunk seeds', nsw, flush=True)
    nd = 0
    try:
        for l in open(os.path.join(HERE, 'dynseeds.txt')):                 # block starts the emulator executed that static discovery missed
            if l.strip():
                a = int(l, 16)
                if a not in FUNCS and decode_at(a) is not None: FUNCS[a] = (0, 0, 2, 0); nd += 1
    except OSError: pass
    print('dynamic seeds', nd, flush=True)


if __name__ == '__main__':
    if sys.argv[1] == 'stats': stats()
    elif sys.argv[1] == 'gen':
        with open(sys.argv[2], 'w') as f: done = emit([int(a, 16) for a in sys.argv[3:]], f)
        print('lifted', [hex(d) for d in done])
    elif sys.argv[1] == 'genall':
        outdir = sys.argv[2]; per = int(sys.argv[3]) if len(sys.argv) > 3 else 400
        os.makedirs(outdir, exist_ok=True)
        seed_functions()
        queue = sorted(FUNCS); seen = set(queue)
        while queue:                                   # discover entry points the analysis missed (targets of the push/jmp idiom, direct calls)
            e = queue.pop()
            try: L = gen_function(e)[3]
            except Unsupported: continue
            for t in L.direct:
                if t not in seen and in_img(t) and decode_at(t) is not None: seen.add(t); FUNCS[t] = (0, 0, 2, 0); queue.append(t)
        print('entries after discovery', len(FUNCS), flush=True)
        entries = sorted(FUNCS); KNOWN = set()
        for e in entries:
            try: gen_function(e); KNOWN.add(e)
            except Unsupported: pass
        print('liftable', len(KNOWN), flush=True)
        chunk, nfile, done_all, decl, curkey = [], 0, [], set(), 0
        def flush():
            global chunk, nfile, decl
            if not chunk: return
            import io
            out = io.StringIO()
            out.write('#include "rt.h"\n')
            for d in sorted(decl): out.write(f'void f_{d:08x}(Cpu *c);\n')
            for e, disp, hdr, body in chunk:
                if len(body) > 6000: hdr = ['OPTNONE ' + hdr[0]] + hdr[1:]
                out.write(disp + '\n' + '\n'.join(hdr + body) + '\n}\n#undef JMP_DISPATCH\n')
            path = os.path.join(outdir, f'part_{curkey:05x}.c'); txt = out.getvalue()
            if not (os.path.exists(path) and open(path).read() == txt): open(path, 'w').write(txt)
            nfile += 1; chunk = []; decl = set()
        for e in entries:
            if e not in KNOWN: continue
            disp, hdr, body, L = gen_function(e)
            key = e >> 17
            if chunk and key != curkey: flush()
            curkey = key; decl |= L.calls; chunk.append((e, disp, hdr, body)); done_all.append(e)
        flush()
        import io
        t = io.StringIO()
        if True:
            t.write('#include "rt.h"\n')
            for e in done_all: t.write(f'void f_{e:08x}(Cpu *c);\n')
            t.write('const struct { uint32_t a; GuestFn f; } rt_table[] = {\n')
            for e in done_all: t.write(f'  {{0x{e:x}u, f_{e:08x}}},\n')
            t.write('};\nconst unsigned rt_table_n = %d;\n' % len(done_all))
        p = os.path.join(outdir, 'table.c')
        if not (os.path.exists(p) and open(p).read() == t.getvalue()): open(p, 'w').write(t.getvalue())
        print('files', nfile, 'functions', len(done_all))
        print('trapped instructions:', dict(TRAPS.most_common(40)))
