import sys, struct, os, glob
# usage: patchpts.py indir outdir ; rewrites vkd3d SPIR-V so naga accepts it:
# BuiltIn PointSize output var -> Private var, dropped from the entry point interface
src, dst = sys.argv[1], sys.argv[2]
os.makedirs(dst, exist_ok=True)
NOP = 1 << 16
npatched = 0
for f in glob.glob(src + '/*.spv'):
    d = open(f, 'rb').read(); n = len(d) // 4
    w = list(struct.unpack('<%dI' % n, d)); i = 5
    pts = None; ins = []; i = 5
    while i < n:
        op = w[i] & 0xffff; c = w[i] >> 16
        if op == 71 and c == 4 and w[i+2] == 11 and w[i+3] == 1: pts = w[i+1]
        i += c
    i = 5; ptrs = {}
    if pts is not None: npatched += 1
    while i < n:
        op = w[i] & 0xffff; c = w[i] >> 16
        if op == 32: ptrs[w[i+1]] = w[i+3]
        i += c
    i = 5
    while i < n:
        op = w[i] & 0xffff; c = w[i] >> 16; q = w[i:i+c]
        if pts is not None:
            if op == 71 and c == 4 and q[1] == pts and q[2] == 11: i += c; continue
            if op == 59 and q[2] == pts:
                nid = w[3]; w[3] += 1
                pointee = ptrs[q[1]]; ins += [(4 << 16) | 32, nid, 6, pointee]; q[1] = nid; q[3] = 6
            if op == 15:
                nm = 3
                while all(((q[nm] >> s) & 0xff) for s in (0, 8, 16, 24)): nm += 1
                nm += 1
                if pts in q[nm:]: q.remove(pts) if False else None; q = q[:nm] + [x for x in q[nm:] if x != pts]; q[0] = (len(q) << 16) | op
        ins += q; i += c
    w = w[:5] + ins
    open(dst + '/' + os.path.basename(f), 'wb').write(struct.pack('<%dI' % len(w), *w))
print('patched', npatched)
