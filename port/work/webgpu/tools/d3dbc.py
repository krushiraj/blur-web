"""D3D9 shader model 2/3 bytecode scanner: declarations (dcl), constant usage, def constants.

Only reads what the WGSL pack needs: version, dcl semantics/sampler types, which float
constant registers are read (and whether relatively addressed), int/bool constant use.
"""
import struct

USAGES = ['POSITION', 'BLENDWEIGHT', 'BLENDINDICES', 'NORMAL', 'PSIZE', 'TEXCOORD', 'TANGENT', 'BINORMAL',
          'TESSFACTOR', 'POSITIONT', 'COLOR', 'FOG', 'DEPTH', 'SAMPLE']
(R_TEMP, R_INPUT, R_CONST, R_ADDR, R_RASTOUT, R_ATTROUT, R_TEXCRDOUT, R_CONSTINT, R_COLOROUT, R_DEPTHOUT,
 R_SAMPLER, R_CONST2, R_CONST3, R_CONST4, R_CONSTBOOL, R_LOOP, R_TEMPFLOAT16, R_MISC, R_LABEL, R_PRED) = range(20)
OP_DCL, OP_DEF, OP_DEFI, OP_DEFB = 31, 81, 82, 83
OP_COMMENT, OP_END = 0xfffe, 0xffff
TEXTURE_TYPES = {1: '1d', 2: '2d', 3: 'cube', 4: '3d'}
OUT_TYPES = (R_RASTOUT, R_ATTROUT, R_TEXCRDOUT, R_COLOROUT, R_DEPTHOUT)


def regtype(t):
    return ((t >> 28) & 7) | ((t >> 8) & 0x18)


def parse(data):
    n = len(data) // 4
    w = struct.unpack('<%dI' % n, data[:n * 4])
    ver = w[0]
    kind = {0xFFFE: 'vs', 0xFFFF: 'ps'}[ver >> 16]
    r = dict(kind=kind, major=(ver >> 8) & 0xff, minor=ver & 0xff, dcl_in=[], dcl_ot=[], samplers={},
             defs=[], defi=[], defb=[], const_read=set(), const_rel=False, int_read=set(), bool_read=set(),
             relative_any=False)
    i = 1
    while i < n:
        t = w[i]
        op = t & 0xffff
        if op == OP_END:
            break
        if op == OP_COMMENT:
            i += 1 + ((t >> 16) & 0x7fff)
            continue
        ln = (t >> 24) & 0xf
        args = w[i + 1:i + 1 + ln]
        i += 1 + ln
        if op == OP_DCL:
            d, dst = args[0], args[1]
            rt, rn, mask = regtype(dst), dst & 0x7ff, (dst >> 16) & 0xf
            if rt == R_SAMPLER:
                r['samplers'][rn] = TEXTURE_TYPES.get((d >> 27) & 0xf, '?')
            else:
                e = dict(reg=rn, rtype=rt, usage=d & 0x1f, index=(d >> 16) & 0xf, mask=mask)
                r['dcl_ot' if rt in OUT_TYPES else 'dcl_in'].append(e)
            continue
        if op == OP_DEF:
            r['defs'].append(args[0] & 0x7ff)
            continue
        if op == OP_DEFI:
            r['defi'].append(args[0] & 0x7ff)
            continue
        if op == OP_DEFB:
            r['defb'].append(args[0] & 0x7ff)
            continue
        # generic instruction: every parameter is a register token (const registers can only be sources);
        # bit 13 = relative addressing, followed by one address token (SM2+)
        j = 0
        while j < len(args):
            p = args[j]
            rt, rn = regtype(p), p & 0x7ff
            isrel = bool((p >> 13) & 1) and r['major'] >= 2
            if isrel:
                j += 1
                r['relative_any'] = True
            if rt == R_CONST:
                if isrel:
                    r['const_rel'] = True
                else:
                    r['const_read'].add(rn)
            elif rt == R_CONSTINT:
                r['int_read'].add(rn)
            elif rt == R_CONSTBOOL:
                r['bool_read'].add(rn)
            j += 1
    return r
