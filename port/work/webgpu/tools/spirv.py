"""Minimal SPIR-V binary reader/editor (only what the Blur D3D9 -> WGSL toolchain needs).

A module is a list of instructions; an instruction is a list of 32-bit words whose
first word is (wordcount << 16) | opcode.  Ids are never renumbered, new ids come from
`Module.new_id()` (bumps the header bound).
"""
import struct

OP = dict(Name=5, MemberName=6, String=7, Line=8, ExtInstImport=11, MemoryModel=14, EntryPoint=15,
          ExecutionMode=16, Capability=17, TypeVoid=19, TypeBool=20, TypeInt=21, TypeFloat=22,
          TypeVector=23, TypeImage=25, TypeSampler=26, TypeSampledImage=27, TypeArray=28, TypeStruct=30,
          TypePointer=32, TypeFunction=33, Constant=43, ConstantComposite=44, Variable=59, Load=61,
          Store=62, AccessChain=65, Decorate=71, MemberDecorate=72, Function=54, Label=248)
DEC = dict(Block=2, ArrayStride=6, BuiltIn=11, Location=30, Component=31, Binding=33, DescriptorSet=34, Offset=35,
           Flat=14)
SC = dict(UniformConstant=0, Input=1, Uniform=2, Output=3, Workgroup=4, Private=6, Function=7)
BUILTIN = dict(Position=0, PointSize=1, FragCoord=15, FrontFacing=17)


def words_of_str(s):
    b = s.encode() + b'\0'
    b += b'\0' * (-len(b) % 4)
    return list(struct.unpack('<%dI' % (len(b) // 4), b))


def str_of_words(w):
    b = struct.pack('<%dI' % len(w), *w)
    return b.split(b'\0', 1)[0].decode()


class Module:
    def __init__(self, data):
        n = len(data) // 4
        w = struct.unpack('<%dI' % n, data)
        assert w[0] == 0x07230203
        self.header = list(w[:5])
        self.ins = []
        i = 5
        while i < n:
            c = w[i] >> 16
            self.ins.append(list(w[i:i + c]))
            i += c

    def to_bytes(self):
        out = list(self.header)
        for q in self.ins:
            q[0] = (len(q) << 16) | (q[0] & 0xffff)
            out += q
        return struct.pack('<%dI' % len(out), *out)

    def new_id(self):
        self.header[3] += 1
        return self.header[3] - 1

    def op(self, q):
        return q[0] & 0xffff

    def names(self):
        return {q[1]: str_of_words(q[2:]) for q in self.ins if self.op(q) == OP['Name']}

    def decorations(self):
        """id -> {decoration: [operands]}"""
        d = {}
        for q in self.ins:
            if self.op(q) == OP['Decorate']:
                d.setdefault(q[1], {})[q[2]] = q[3:]
        return d

    def insert_before_first(self, opcodes, new):
        """insert instruction `new` before the first instruction whose opcode is in opcodes"""
        for i, q in enumerate(self.ins):
            if self.op(q) in opcodes:
                self.ins.insert(i, new)
                return
        self.ins.append(new)
