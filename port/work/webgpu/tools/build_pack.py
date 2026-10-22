"""Translate the whole D3D9 bytecode corpus and write the WGSL pack (see README.md for the format).

usage: build_pack.py --corpus DIR --vkd3d DIR --naga BIN --pack FILE [--dump DIR] [--jobs 4] [--limit N]
DIR/{pak,exe,extra}/<fnv1a64>_<len>.bin
"""
import argparse
import glob
import gzip
import json
import os
import struct
import subprocess
import sys
import zlib
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import translate as T  # noqa: E402

VERSION = 1
HEADER = struct.Struct('<4sHHIIIIIII')          # magic, version, flags, nShaders, nBlobs, offShaders, offBlobs, offText, offMeta, lenMeta
SHADER = struct.Struct('<IIIIBBHHHHHIII')       # lo hi clen blob stage flags cbVec inMask outMask texMask nRanges texTypes rangeOff pad
BLOB = struct.Struct('<II')                     # textOff textLen
F_CONST_REL, F_INT_UNDEF, F_DEPTH, F_PSIZE, F_DIAG, F_BOOL_UNDEF = 1, 2, 4, 8, 16, 32


def fnv1a64(b):
    h = 0xcbf29ce484222325
    for c in b:
        h = ((h ^ c) * 0x100000001b3) & 0xffffffffffffffff
    return h


def collect(corpus):
    """key -> path.  The key is computed from the bytecode (fnv1a64 + length, same as web/shaders.bin), not from
    the file name: the corpus also holds a few non-hash-named test shaders (extra/shader_NNN_ps_2_0.bin)."""
    files = {}
    for d in ('pak', 'exe', 'extra'):
        for f in sorted(glob.glob(os.path.join(corpus, d, '*.bin'))):
            b = open(f, 'rb').read()
            files.setdefault('%016x_%d' % (fnv1a64(b), len(b)), f)
    return files


def ranges_of(regs):
    out, run = [], None
    for r in sorted(regs):
        if run and r == run[0] + run[1]:
            run[1] += 1
        else:
            run = [r, 1]
            out.append(run)
    return out


def shader_record(key, info):
    h, ln = key.split('_')
    v = int(h, 16)
    mask = lambda lst: sum(1 << c for c in lst)
    fl = 0
    if info['const_rel']:
        fl |= F_CONST_REL
    if [i for i in info['int_read'] if i not in info['defi']]:
        fl |= F_INT_UNDEF
    if [i for i in info['bool_read'] if i not in info['defb']]:
        fl |= F_BOOL_UNDEF
    if info['depth_out']:
        fl |= F_DEPTH
    if info['pointsize']:
        fl |= F_PSIZE
    if info['wgsl'].startswith('diagnostic('):
        fl |= F_DIAG
    ps = info['kind'] == 'ps'
    inm = mask([x[0] for x in (info['pin'] if ps else info['vin'])])
    outm = mask(info['pout']) if ps else mask([x[0] for x in info['vout']])
    texm = mask(info['tex'])
    tt = 0
    for n, t in info['tex'].items():
        tt |= {'2d': 0, 'cube': 1, '3d': 2}[t] << (2 * n)
    return dict(lo=v & 0xffffffff, hi=v >> 32, clen=int(ln), stage=1 if ps else 0, flags=fl, cb=info['cb'],
                inm=inm, outm=outm, texm=texm, tt=tt)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--corpus', required=True)
    ap.add_argument('--vkd3d', required=True)
    ap.add_argument('--naga', default='naga')
    ap.add_argument('--pack', required=True)
    ap.add_argument('--dump')
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--stats')
    a = ap.parse_args()
    tools = T.Tools(a.vkd3d, naga=a.naga)
    files = collect(a.corpus)
    keys = sorted(files)
    if a.limit:
        keys = keys[:a.limit]

    def one(k):
        try:
            w, info = T.translate(tools, k, files[k])
            info['wgsl'] = w
            return k, info, None
        except Exception as e:      # noqa: BLE001  report, never silently skip
            return k, None, '%s: %s' % (type(e).__name__, e)
    results, failed = {}, {}
    with ThreadPoolExecutor(a.jobs) as ex:
        for k, info, err in ex.map(one, keys):
            if err:
                failed[k] = err
            else:
                results[k] = info
    print('translated %d / %d, failed %d' % (len(results), len(keys), len(failed)))
    for k, e in sorted(failed.items())[:20]:
        print('  FAIL', k, e[:200])
    if a.dump:
        os.makedirs(a.dump, exist_ok=True)
        for k, info in results.items():
            open(os.path.join(a.dump, k + '.wgsl'), 'w').write(info['wgsl'])

    # ---- blobs (unique WGSL text, first-appearance order over sorted keys)
    blob_ix, blobs = {}, []
    for k in keys:
        if k in results:
            t = results[k]['wgsl']
            if t not in blob_ix:
                blob_ix[t] = len(blobs)
                blobs.append(t)
    text = b''
    boff = []
    for t in blobs:
        b = t.encode('utf8')
        boff.append((len(text), len(b)))
        text += b
    # ---- shader records sorted by (hi, lo, clen)
    recs, rng = [], []
    order = sorted((k for k in keys if k in results), key=lambda k: (int(k.split('_')[0], 16), int(k.split('_')[1])))
    for k in order:
        info = results[k]
        r = shader_record(k, info)
        reads = [x for x in info['const_read'] if x not in info['defs']]
        rs = ranges_of(reads)
        r['rangeOff'] = len(rng)
        r['nr'] = len(rs)
        for s, c in rs:
            rng.append((s, c))
        r['blob'] = blob_ix[info['wgsl']]
        recs.append(r)
    shaders = b''.join(SHADER.pack(r['lo'], r['hi'], r['clen'], r['blob'], r['stage'], r['flags'], r['cb'], r['inm'],
                                   r['outm'], r['texm'], r['nr'], r['tt'], r['rangeOff'], 0) for r in recs)
    shaders += b''.join(struct.pack('<HH', s, c) for s, c in rng)
    blobtab = b''.join(BLOB.pack(o, n) for o, n in boff)
    ver = lambda cmd: subprocess.run(cmd, capture_output=True, text=True).stdout.strip().split('\n')[0]
    meta = dict(
        version=VERSION,
        groups={'vs': T.GROUP['vs'], 'ps': T.GROUP['ps']},
        bindings={'cbFloat': T.BIND_CB_FLOAT, 'tex0': T.BIND_TEX0, 'samp0': T.BIND_SAMP0, 'texCount': 16, 'cbMaxVec': 256},
        drawParams='extra vec4 at cb[cbVec] (array declared cbVec+1 long; cb always bound): VS .xy = clip-space half-pixel offset (1/vpW,-1/vpH); PS .x = alpha-test D3DCMP 0=off 1..8, .y = reference 0..1',
        overrides={'iscL': 'VS, one per input location L that the shader reads: 1 = plain float attribute; 255 / 32767 with unorm8x4 / snorm16 formats = UBYTE4 / SHORT2/4 integer values', 'cswz0': 'VS: swap R/B of the vertex attribute at location 4 (COLOR0 given as D3DCOLOR)', 'cswz1': 'same for location 5 (COLOR1)'},
        interp={'TEXCOORD': 0, 'COLOR': 10, 'FOG': 12, 'count': 13},
        vertexInput={k: [T.VIN_BASE[k], T.VIN_LIMIT[k]] for k in sorted(T.VIN_BASE)},
        record={'shaderBytes': SHADER.size, 'rangeBytes': 4, 'blobBytes': BLOB.size},
        flags={'constRel': F_CONST_REL, 'intUndef': F_INT_UNDEF, 'depth': F_DEPTH, 'pointSize': F_PSIZE,
               'derivDiag': F_DIAG, 'boolUndef': F_BOOL_UNDEF},
        tools={'vkd3d': ver([os.path.join(a.vkd3d, '.libs', 'vkd3d-compiler'), '--version']), 'naga': ver([a.naga, '--version']),
               'spirv-opt': ver(['spirv-opt', '--version'])},
        counts={'shaders': len(recs), 'blobs': len(blobs), 'vs': sum(1 for r in recs if r['stage'] == 0),
                'ps': sum(1 for r in recs if r['stage'] == 1), 'retried': sum(1 for k in results if results[k]['retried'])})
    metab = json.dumps(meta, sort_keys=True, separators=(',', ':')).encode()
    off_sh = HEADER.size
    off_bl = off_sh + len(shaders)
    off_tx = off_bl + len(blobtab)
    off_me = off_tx + len(text)
    hdr = HEADER.pack(b'BWGP', VERSION, 0, len(recs), len(blobs), off_sh, off_bl, off_tx, off_me, len(metab))
    pack = hdr + shaders + blobtab + text + metab
    open(a.pack, 'wb').write(pack)
    # deterministic gzip (mtime 0) + brotli sizes
    gz = gzip.compress(pack, 9, mtime=0)
    open(a.pack + '.gz', 'wb').write(gz)
    br = subprocess.run(['brotli', '-q', '11', '-c'], input=pack, capture_output=True).stdout
    open(a.pack + '.br', 'wb').write(br)
    stats = dict(raw=len(pack), gz=len(gz), br=len(br), text=len(text), shaders=len(recs), blobs=len(blobs),
                 failed=len(failed), failed_list=sorted(failed.items()))
    print('pack: shaders %d unique-wgsl %d  raw %d  gzip %d  brotli %d  (text %d)' %
          (len(recs), len(blobs), len(pack), len(gz), len(br), len(text)))
    fc = {n: sum(1 for r in recs if r['flags'] & v) for n, v in (('constRel', F_CONST_REL), ('intUndef', F_INT_UNDEF), ('boolUndef', F_BOOL_UNDEF),
                                                              ('depthOut', F_DEPTH), ('pointSize', F_PSIZE), ('derivDiag', F_DIAG))}
    print('flags:', ' '.join('%s=%d' % kv for kv in fc.items()), ' spirv-opt retries:', meta['counts']['retried'])
    stats['flags'] = fc
    if a.stats:
        json.dump(stats, open(a.stats, 'w'), indent=1)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
