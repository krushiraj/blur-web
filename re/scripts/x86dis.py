#!/usr/bin/env python3
# usage: dis.py <pe-file> <va-hex> [count]   -- quick Capstone disassembly of a PE at a virtual address
import sys, pefile, capstone
pe = pefile.PE(sys.argv[1]); base = pe.OPTIONAL_HEADER.ImageBase
va = int(sys.argv[2], 16); n = int(sys.argv[3]) if len(sys.argv) > 3 else 16
off = pe.get_offset_from_rva(va - base); data = pe.__data__[off:off + 16 * n]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
for i in list(md.disasm(data, va))[:n]: print(f"{i.address:08x}  {i.mnemonic} {i.op_str}")
