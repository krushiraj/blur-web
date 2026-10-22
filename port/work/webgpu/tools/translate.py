"""One D3D9 shader (SM2/SM3 bytecode) -> canonical SPIR-V -> WGSL, with a fixed binding map and fixed IO locations.

Pipeline per shader
  1. vkd3d-compiler -x d3dbc -b spirv-binary
  2. SPIR-V rewrite (this file, `canonicalise`):
       - debug info removed, BuiltIn PointSize output demoted to a private variable
       - descriptor sets/bindings replaced by the fixed map below
       - every stage-interface variable widened to vec4<f32> at a canonical @location derived from its D3D
         semantic (vertex inputs and VS outputs / PS inputs); the old variable becomes a private variable that
         is filled from / copied to the vec4 at the start / end of the entry point (component offset applied)
  3. naga (SPIR-V -> WGSL, --keep-coordinate-space); on failure retry after spirv-opt constant folding
  4. WGSL post-processing (derivative_uniformity diagnostic, whitespace minimisation)
"""
import os
import re
import subprocess
import tempfile

import d3dbc
from spirv import Module, OP, DEC, SC, BUILTIN, words_of_str, str_of_words

# ---- fixed binding map (documented in README.md, mirrored in the pack header) -------------------------------
GROUP = {'vs': 0, 'ps': 1}
BIND_CB_FLOAT = 0          # uniform array<vec4<f32>, N>, N = max used float constant register + 1
BIND_TEX0 = 16             # texture register tN -> binding 16 + N   (N < 16)
BIND_SAMP0 = 32            # sampler register sN -> binding 32 + N   (N < 16)

# ---- fixed IO location maps ---------------------------------------------------------------------------------
# VS output / PS input interpolants: TEXCOORDn -> n (0..9), COLORn -> 10+n, FOG -> 12
# VS input attributes by (usage, index)
VIN_BASE = {'POSITION': 0, 'BLENDWEIGHT': 1, 'BLENDINDICES': 2, 'NORMAL': 3, 'COLOR': 4, 'TANGENT': 6, 'BINORMAL': 7,
            'TEXCOORD': 8}
VIN_LIMIT = {'POSITION': 1, 'BLENDWEIGHT': 1, 'BLENDINDICES': 1, 'NORMAL': 1, 'COLOR': 2, 'TANGENT': 1, 'BINORMAL': 1,
             'TEXCOORD': 8}


def interp_loc(usage, idx):
    if usage == 'TEXCOORD' and idx < 10:
        return idx
    if usage == 'COLOR' and idx < 2:
        return 10 + idx
    if usage == 'FOG' and idx == 0:
        return 12
    return None


def vin_loc(usage, idx):
    if usage in VIN_BASE and idx < VIN_LIMIT[usage]:
        return VIN_BASE[usage] + idx
    return None


class Unsupported(Exception):
    pass


def run(cmd, env=None, inp=None):
    r = subprocess.run(cmd, capture_output=True, env=env, input=inp)
    return r.returncode, r.stdout, r.stderr.decode('utf8', 'replace')


class Tools:
    def __init__(self, vkd3d_dir, naga='naga', spirv_opt='spirv-opt', spirv_val='spirv-val'):
        self.vkd3d = os.path.join(vkd3d_dir, '.libs', 'vkd3d-compiler')
        self.env = dict(os.environ, DYLD_LIBRARY_PATH=os.path.join(vkd3d_dir, '.libs'),
                        LD_LIBRARY_PATH=os.path.join(vkd3d_dir, '.libs'))
        self.naga, self.spirv_opt, self.spirv_val = naga, spirv_opt, spirv_val



def _gdeps(q):
    o = q[0] & 0xffff
    if o in (23, 25, 27, 29):          # TypeVector, TypeImage, TypeSampledImage, TypeRuntimeArray
        return [q[2]]
    if o == 28:                        # TypeArray
        return [q[2], q[3]]
    if o in (30, 33):                  # TypeStruct, TypeFunction
        return q[2:]
    if o == 32:                        # TypePointer
        return [q[3]]
    if o in (41, 42, 43, 1, 45, 46):   # Constant*, Undef, ConstantSampler/Null
        return [q[1]]
    if o == 44:                        # ConstantComposite
        return [q[1]] + q[3:]
    if o == 59:                        # Variable
        return [q[1]] + q[5:]
    return []


def toposort_globals(ins):
    """Stable re-ordering of the types/constants/global-variables section so every id is defined before use."""
    first = next(i for i, q in enumerate(ins) if OP['TypeVoid'] <= q[0] & 0xffff <= OP['TypeFunction'])
    last = next(i for i, q in enumerate(ins) if q[0] & 0xffff == OP['Function'])
    sec = ins[first:last]

    def rid(q):
        o = q[0] & 0xffff
        return q[2] if o in (41, 42, 43, 44, 45, 46, 1, 59) else q[1]
    defs = {rid(q): q for q in sec}
    done, order = set(), []

    def visit(q):
        r = rid(q)
        if r in done:
            return
        done.add(r)
        for d in _gdeps(q):
            if d in defs:
                visit(defs[d])
        order.append(q)
    for q in sec:
        visit(q)
    ins[first:last] = order

# ---- SPIR-V rewrite ------------------------------------------------------------------------------------------
def canonicalise(spv, bc):
    """spv: vkd3d SPIR-V bytes, bc: parsed d3dbc dict.  returns (new spv bytes, info dict)"""
    m = Module(spv)
    kind = bc['kind']
    names = m.names()
    dec = m.decorations()
    ins = m.ins
    # strip debug lines / sources / strings
    ins[:] = [q for q in ins if (q[0] & 0xffff) not in (OP['Line'], 317, 3, OP['String'])]
    ep = next(q for q in ins if q[0] & 0xffff == OP['EntryPoint'])
    # entry-point name words, then interface ids
    k = 3
    while all(((ep[k] >> s) & 0xff) for s in (0, 8, 16, 24)):
        k += 1
    k += 1
    iface = ep[k:]
    entry_id = ep[2]

    types = {q[1]: q for q in ins if OP['TypeVoid'] <= (q[0] & 0xffff) <= OP['TypeFunction']}
    f32 = next(i for i, q in types.items() if q[0] & 0xffff == OP['TypeFloat'] and q[2] == 32)
    new_globals = []   # instructions appended to the type/constant/variable section (before first function)
    new_dec = []
    new_names = []

    def find_or_make(match, make):
        for q in ins + new_globals:
            if match(q):
                return q[1] if (q[0] & 0xffff) != OP['Variable'] else q[2]
        q = make()
        new_globals.append(q)
        return q[1]

    def vec(n):
        if n == 1:
            return f32
        return find_or_make(lambda q: q[0] & 0xffff == OP['TypeVector'] and q[2] == f32 and q[3] == n,
                            lambda: [(4 << 16) | OP['TypeVector'], m.new_id(), f32, n])

    def ptr(sc, pointee):
        return find_or_make(lambda q: q[0] & 0xffff == OP['TypePointer'] and q[2] == sc and q[3] == pointee,
                            lambda: [(4 << 16) | OP['TypePointer'], m.new_id(), sc, pointee])

    def float_zero():
        for q in ins + new_globals:
            if q[0] & 0xffff == OP['Constant'] and q[1] == f32 and q[3] == 0:
                return q[2]
        i = m.new_id()
        new_globals.append([(4 << 16) | OP['Constant'], f32, i, 0])
        return i

    var_by_id = {q[2]: q for q in ins if q[0] & 0xffff == OP['Variable']}
    ptr_pointee = {q[1]: (q[2], q[3]) for q in ins if q[0] & 0xffff == OP['TypePointer']}
    vtype = {i: (q[2], q[3]) if q[0] & 0xffff == OP['TypeVector'] else (None, None) for i, q in types.items()}

    def var_shape(vid):
        sc, pointee = ptr_pointee[var_by_id[vid][1]]
        if pointee == f32:
            return pointee, 1
        el, n = vtype[pointee]
        if el != f32:
            raise Unsupported('interface var type not float vector: %s' % names.get(vid))
        return pointee, n

    info = dict(vin=[], vout=[], pin=[], pout=[], tex={}, cb=0, pointsize=False, depth_out=False, frag_builtin=False)
    entry_pre = []     # instructions at start of entry function
    entry_post = []    # instructions before every return
    new_iface = []
    drop_iface = set()
    retargeted = set()

    def make_private(vid):
        sc, pointee = ptr_pointee[var_by_id[vid][1]]
        t = ptr(SC['Private'], pointee)
        var_by_id[vid][1] = t
        var_by_id[vid][3] = SC['Private']
        retargeted.add(vid)

    def strip_dec(vid, which):
        ins[:] = [q for q in ins if not ((q[0] & 0xffff) == OP['Decorate'] and q[1] == vid and q[2] in which)]

    for vid, q in list(var_by_id.items()):
        sc = q[3]
        nm = names.get(vid, '')
        d = dec.get(vid, {})
        if sc == SC['UniformConstant']:
            mt = re.fullmatch(r'([ts])(\d+)', nm)
            if not mt:
                raise Unsupported('unknown resource var ' + nm)
            n = int(mt.group(2))
            if n >= 16:
                raise Unsupported('sampler register >= 16')
            base = BIND_TEX0 if mt.group(1) == 't' else BIND_SAMP0
            for qq in ins:
                if (qq[0] & 0xffff) == OP['Decorate'] and qq[1] == vid:
                    if qq[2] == DEC['DescriptorSet']:
                        qq[3] = GROUP[kind]
                    elif qq[2] == DEC['Binding']:
                        qq[3] = base + n
            if mt.group(1) == 't':
                pointee = ptr_pointee[q[1]][1]
                img = types[pointee]
                info['tex'][n] = {1: '2d', 2: '3d', 3: 'cube'}[img[3]]
        elif sc == SC['Uniform']:
            if not re.fullmatch(r'cb0_0', nm):
                raise Unsupported('unexpected uniform ' + nm)
            for qq in ins:
                if (qq[0] & 0xffff) == OP['Decorate'] and qq[1] == vid:
                    if qq[2] == DEC['DescriptorSet']:
                        qq[3] = GROUP[kind]
                    elif qq[2] == DEC['Binding']:
                        qq[3] = BIND_CB_FLOAT
            st = ptr_pointee[q[1]][1]
            arr = types[types[st][2]]
            lenc = next(c for c in ins if c[0] & 0xffff == OP['Constant'] and c[2] == arr[3])
            info['cb'] = lenc[3]
        elif sc in (SC['Input'], SC['Output']):
            is_in = sc == SC['Input']
            if DEC['BuiltIn'] in d:
                b = d[DEC['BuiltIn']][0]
                if b == BUILTIN['PointSize']:       # vkd3d always declares oPts; PSIZE written by the shader is flagged below
                    strip_dec(vid, (DEC['BuiltIn'],))
                    make_private(vid)
                    drop_iface.add(vid)
                elif b == BUILTIN['Position'] and not is_in:
                    pass
                elif b == 22 and not is_in:   # FragDepth
                    info['depth_out'] = True
                else:
                    info['frag_builtin'] = True
                    if is_in:
                        raise Unsupported('fragment input builtin %d (vPos/vFace)' % b)
                continue
            loc = d[DEC['Location']][0]
            comp = d.get(DEC['Component'], [0])[0]
            pointee, n = var_shape(vid)
            mt = re.fullmatch(r'[vo](\d+)', nm)
            reg = int(mt.group(1)) if mt else None
            if kind == 'ps' and not is_in:
                # colour output: keep @location(colour index), vec4
                info['pout'].append(loc)
                continue
            # --- semantic -> canonical location
            if kind == 'vs' and is_in:
                e = [x for x in bc['dcl_in'] if x['reg'] == loc and x['rtype'] == d3dbc.R_INPUT]
                if len(e) != 1:
                    raise Unsupported('vs input v%d has no dcl' % loc)
                sem = (d3dbc.USAGES[e[0]['usage']], e[0]['index'])
                cl = vin_loc(*sem)
                if cl is None:
                    raise Unsupported('vs input semantic %s%d' % sem)
                info['vin'].append((cl, sem[0], sem[1]))
            else:
                if bc['major'] == 3:
                    dl = bc['dcl_in'] if is_in else bc['dcl_ot']
                    e = [x for x in dl if x['reg'] == loc]
                    if len(e) != 1:
                        raise Unsupported('sm3 %s reg %d has no dcl' % ('ps in' if is_in else 'vs out', loc))
                    sem = (d3dbc.USAGES[e[0]['usage']], e[0]['index'])
                else:
                    # sm2: vkd3d locations are semantic based: texcoordN -> N, colourN -> 8+N, psize -> 12
                    if loc < 8:
                        sem = ('TEXCOORD', loc)
                    elif loc < 10:
                        sem = ('COLOR', loc - 8)
                    elif loc == 12 and not is_in:
                        sem = ('PSIZE', 0)
                    else:
                        raise Unsupported('sm2 interpolant location %d' % loc)
                if sem[0] == 'PSIZE':
                    info['pointsize'] = True
                    strip_dec(vid, (DEC['Location'], DEC['Component']))
                    make_private(vid)
                    drop_iface.add(vid)
                    continue
                cl = interp_loc(*sem)
                if cl is None:
                    raise Unsupported('interpolant semantic %s%d' % sem)
                (info['pin'] if is_in else info['vout']).append((cl, sem[0], sem[1]))
            # --- widen to vec4 at canonical location
            v4 = vec(4)
            nv = m.new_id()
            new_globals.append([(4 << 16) | OP['Variable'], ptr(sc, v4), nv, sc])
            new_dec.append([(4 << 16) | OP['Decorate'], nv, DEC['Location'], cl])
            new_names.append([None, nv, ('i%d' if is_in else 'o%d') % cl])
            strip_dec(vid, (DEC['Location'], DEC['Component']))
            make_private(vid)
            drop_iface.add(vid)
            new_iface.append(nv)
            if is_in:
                t4 = m.new_id()
                entry_pre.append([(4 << 16) | OP['Load'], v4, t4, nv])
                if n == 4:
                    val = t4
                elif n == 1:
                    val = m.new_id()
                    entry_pre.append([(5 << 16) | 81, f32, val, t4, comp])    # OpCompositeExtract
                else:
                    val = m.new_id()
                    entry_pre.append([(5 + n << 16) | 79, pointee, val, t4, t4] + list(range(comp, comp + n)))
                entry_pre.append([(3 << 16) | OP['Store'], vid, val])
            else:
                cur = m.new_id()
                entry_post.append([(4 << 16) | OP['Load'], pointee, cur, vid])
                if n == 4:
                    val = cur
                else:
                    parts = []
                    for c in range(4):
                        if comp <= c < comp + n:
                            if n == 1:
                                parts.append(cur)
                            else:
                                pid = m.new_id()
                                entry_post.append([(5 << 16) | 81, f32, pid, cur, c - comp])
                                parts.append(pid)
                        else:
                            parts.append(float_zero())
                    val = m.new_id()
                    entry_post.append([(7 << 16) | 80, v4, val] + parts)           # OpCompositeConstruct
                entry_post.append([(3 << 16) | OP['Store'], nv, val])

    # access chains rooted at a demoted variable must yield Private pointers
    for q in ins:
        if q[0] & 0xffff in (65, 66) and q[3] in retargeted:
            q[1] = ptr(SC['Private'], ptr_pointee[q[1]][1])

    # ---- assemble the module
    # entry point interface
    ep[:] = ep[:k] + [i for i in iface if i not in drop_iface] + new_iface
    ep[0] = (len(ep) << 16) | OP['EntryPoint']
    # names
    for nq in new_names:
        w = words_of_str(nq[2])
        ins.insert(next(i for i, q in enumerate(ins) if q[0] & 0xffff in (OP['Decorate'], OP['MemberDecorate'])
                        or OP['TypeVoid'] <= q[0] & 0xffff <= OP['TypeFunction']),
                   [((2 + len(w)) << 16) | OP['Name'], nq[1]] + w)
    # decorations before the first type
    at = next(i for i, q in enumerate(ins) if OP['TypeVoid'] <= q[0] & 0xffff <= OP['TypeFunction'])
    ins[at:at] = new_dec
    # new types/vars/consts right before the first function (types they need are already above)
    at = next(i for i, q in enumerate(ins) if q[0] & 0xffff == OP['Function'])
    ins[at:at] = new_globals
    toposort_globals(ins)
    # entry function body
    fi = next(i for i, q in enumerate(ins) if q[0] & 0xffff == OP['Function'] and q[2] == entry_id)
    li = next(i for i in range(fi, len(ins)) if ins[i][0] & 0xffff == OP['Label'])
    pos = li + 1
    while pos < len(ins) and ins[pos][0] & 0xffff == OP['Variable']:
        pos += 1
    ins[pos:pos] = entry_pre
    i = pos + len(entry_pre)
    while i < len(ins):
        if ins[i][0] & 0xffff == 253:     # OpReturn
            ins[i:i] = [list(x) for x in entry_post]
            i += len(entry_post)
        i += 1
    info['vin'].sort()
    info['vout'].sort()
    info['pin'].sort()
    info['pout'].sort()
    return m.to_bytes(), info


# ---- tool drivers ---------------------------------------------------------------------------------------------
def to_spirv(tools, bc_path):
    with tempfile.TemporaryDirectory() as td:
        o = os.path.join(td, 'x.spv')
        rc, _, err = run([tools.vkd3d, '-x', 'd3dbc', '-b', 'spirv-binary', '-o', o, bc_path], env=tools.env)
        if rc:
            raise Unsupported('vkd3d: ' + err[:200])
        return open(o, 'rb').read()


def spirv_to_wgsl(tools, spv):
    """naga; retry after spirv-opt constant folding (fixes OpSNegate on u32 constants)."""
    with tempfile.TemporaryDirectory() as td:
        a, b, c = (os.path.join(td, n) for n in ('a.spv', 'b.spv', 'o.wgsl'))
        open(a, 'wb').write(spv)
        rc, _, err = run([tools.spirv_val, a])
        if rc:
            raise Unsupported('spirv-val: ' + err[:300])
        retried = False
        rc, _, err = run([tools.naga, '--keep-coordinate-space', a, c])
        if rc:
            rc2, _, err2 = run([tools.spirv_opt, '--ccp', '--simplify-instructions', '--eliminate-dead-code-aggressive',
                                a, '-o', b])
            if rc2:
                raise Unsupported('spirv-opt: ' + err2[:200])
            retried = True
            rc, _, err = run([tools.naga, '--keep-coordinate-space', b, c])
            if rc:
                raise Unsupported('naga: ' + err[:300])
        return open(c).read(), retried


def minify_wgsl(src):
    """whitespace removal + renumbering of naga temporaries (other identifiers untouched, output stays readable)."""
    out = []
    for line in src.split('\n'):
        s = line.strip()
        if not s:
            continue
        out.append(s)
    t = '\n'.join(out)
    # naga puts the attribute and the declaration on separate lines ("@group(1) @binding(0) \nvar ..")
    t = re.sub(r'(@(?:group|binding)\(\d+\))\s*\n(?=[@v])', r'\1 ', t)
    # naga's temporaries _e<N> use the (sparse) SPIR-V ids: renumber from 0 in order of appearance (compresses ~14% gz / ~40% br better)
    ids = {}
    t = re.sub(r'\b_e\d+\b', lambda x: ids.setdefault(x.group(0), '_e%d' % len(ids)), t)
    return t + '\n'



APASS = """fn apass(f: f32, a: f32, r: f32) -> bool {
if f < 1.5 { return false; }
if f < 2.5 { return a < r; }
if f < 3.5 { return a == r; }
if f < 4.5 { return a <= r; }
if f < 5.5 { return a > r; }
if f < 6.5 { return a != r; }
if f < 7.5 { return a >= r; }
return true;
}
"""


def add_draw_params(wgsl, kind, ncb):
    """Per-draw parameters live in one extra vec4 at cb[ncb] (the constant array is declared ncb+1 long), so no extra binding is needed:
       VS: .xy = D3D9 half-pixel offset in clip space (1/viewportW, -1/viewportH), added as pos.xy += hp * pos.w
       PS: .x = alpha-test D3DCMP function (0 = off, 1..8), .y = reference in 0..1"""
    g = GROUP[kind]
    m = re.search(r'array<vec4<f32>, (\d+)>', wgsl)
    if m:
        assert int(m.group(1)) == ncb, (m.group(1), ncb)
        wgsl = wgsl.replace(m.group(0), 'array<vec4<f32>, %d>' % (ncb + 1), 1)
        cb = re.search(r'var<uniform> (\w+): ', wgsl).group(1)
    else:
        assert ncb == 0
        cb = 'cb0_0_'
        wgsl = ('struct cbp_struct {\nmember: array<vec4<f32>, 1>,\n}\n@group(%d) @binding(%d) var<uniform> %s: cbp_struct;\n' % (g, BIND_CB_FLOAT, cb)) + wgsl
    param = '%s.member[%du]' % (cb, ncb)
    if kind == 'vs' and '-> @builtin(position) vec4<f32>' in wgsl:       # normalise position-only VS to the struct form (padVsOutputs relies on it)
        wgsl = 'struct VertexOutput {\n@builtin(position) member: vec4<f32>,\n}\n' + wgsl.replace('-> @builtin(position) vec4<f32>', '-> VertexOutput')
        wgsl = re.sub(r'\nreturn (\w+);\n}\s*$', r'\nreturn VertexOutput(\1);\n}\n', wgsl)
    i = wgsl.rindex('\nfn main(')
    head, tail = wgsl[:i], wgsl[i:]
    sig = tail[:tail.index('{')]
    rm = list(re.finditer(r'\nreturn ([^;]*);', tail))
    if len(rm) != 1:
        raise Unsupported('entry point has %d return statements' % len(rm))
    r = rm[0]
    expr = r.group(1)
    if kind == 'vs':
        if '-> VertexOutput' in sig:
            pos = re.search(r'@builtin\(position\) (\w+): vec4<f32>', head).group(1)
            assert expr.startswith('VertexOutput(')
            new = ('\nvar vo = %s;\nlet hp = %s;\nvo.%s = vec4<f32>(vo.%s.xy + hp.xy * vo.%s.w, vo.%s.zw);\nreturn vo;'
                   % (expr, param, pos, pos, pos, pos))
        else:
            raise Unsupported('unknown VS entry signature')
    else:
        a = None
        tail_extra = APASS
        if '-> FragmentOutput' in sig:
            sm = re.search(r'struct FragmentOutput \{\n(.*?)\n\}', head, re.S)
            members = sm.group(1).split('\n')
            ix = [k for k, l in enumerate(members) if l.startswith('@location(0) ')]
            args = [x.strip() for x in expr[len('FragmentOutput('):-1].split(',')]
            if ix:
                a = args[ix[0]]
        elif '-> @location(0) vec4<f32>' in sig:
            a = expr
        if a is None:       # depth-only / no colour 0: nothing to alpha test
            return wgsl
        new = '\nlet ap = %s;\nif ap.x > 0.5 && !apass(ap.x, %s.w, ap.y) { discard; }\nreturn %s;' % (param, a, expr)
        tail_extra = APASS
    tail = tail[:r.start()] + new + tail[r.end():]
    if kind == 'vs':        # per-input pipeline overrides:  cswzN swaps R/B of COLOR0/1 (D3DCOLOR is BGRA bytes); iscL (>1.5) turns a unorm/snorm vertex attribute
        for m in sorted(set(re.findall(r'\ni(\d+)_1 = i\1_;', tail)), key=int):      # into the integer value (UBYTE4 -> unorm8x4 * 255, SHORT2/4 -> snorm16 * 32767)
            loc = int(m)
            v = 'i%d_' % loc
            if loc in (4, 5):
                v = 'select(i%d_, i%d_.zyxw, cswz%d)' % (loc, loc, loc - 4)
                head = 'override cswz%d: bool = false;\n' % (loc - 4) + head
            tail = tail.replace('\ni%d_1 = i%d_;' % (loc, loc), '\ni%d_1 = select(%s, round(%s * isc%d), isc%d > 1.5);' % (loc, v, v, loc, loc))
            head = 'override isc%d: f32 = 1.0;\n' % loc + head
    return head + tail + ('\n' + tail_extra if kind == 'ps' else '')


def translate(tools, key, bc_path):
    bc = d3dbc.parse(open(bc_path, 'rb').read())
    spv = to_spirv(tools, bc_path)
    spv2, info = canonicalise(spv, bc)
    wgsl, retried = spirv_to_wgsl(tools, spv2)
    wgsl = add_draw_params(minify_wgsl(wgsl), bc['kind'], info['cb'])
    if bc['kind'] == 'ps' and 'textureSample' in wgsl:
        wgsl = 'diagnostic(off, derivative_uniformity);\n' + wgsl
    info.update(kind=bc['kind'], sm=bc['major'], retried=retried, const_read=sorted(bc['const_read']),
                const_rel=bc['const_rel'], defs=sorted(bc['defs']), int_read=sorted(bc['int_read']), bool_read=sorted(bc['bool_read']),
                defi=sorted(bc['defi']), defb=sorted(bc['defb']))
    return wgsl, info
