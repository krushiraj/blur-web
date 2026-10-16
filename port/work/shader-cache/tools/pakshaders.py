#!/usr/bin/env python3
"""Extract every Direct3D 9 shader (vs/ps SM2/SM3 token streams) from Blur's cache/shaders-speed.pak.

Pak format (reverse engineered, see README):
  * 0x000..0x7ff+  obfuscated table of contents (file names etc.); not needed here.
  * Body: payload chunks, each starting at a 0x800-aligned file offset:  u32 LE compressed_size, then a zlib
    stream ('x\\xda') of exactly that size.  Chunks are padded with zeros to the next 0x800.  1255 chunks cover the file.
  * Each chunk is an effect container: magic "2BXFFXB\\0" ('PCFX' techniques/passes + CTAB-annotated shader token streams).
  * Shader token streams are embedded verbatim; they are located by walking D3D tokens from every 4-byte-aligned
    version token (0xFFFE03xx vs / 0xFFFF03xx ps, 0x02xx for SM2) until the END token 0x0000FFFF, validating opcodes.

usage: pakshaders.py <shaders-speed.pak[,extra files...]> <outdir> [--list]
  writes <outdir>/<fnv1a64>_<len>.bin  (key = FNV-1a 64 over bytecode, hex) and <outdir>/index.tsv
"""
import os, re, struct, sys, zlib

FNV_OFF, FNV_PRIME = 0xcbf29ce484222325, 0x100000001b3


def fnv64(b):
    h = FNV_OFF
    for x in b:
        h = ((h ^ x) * FNV_PRIME) & 0xffffffffffffffff
    return h


VER = re.compile(rb'[\x00\x01][\x02\x03][\xfe\xff][\xff]', re.S)


def chunks(d):
    for off in range(0, len(d), 0x800):
        if off + 5 > len(d):
            break
        csz = struct.unpack_from('<I', d, off)[0]
        if d[off + 4] == 0x78 and 0 < csz < 0x4000000:
            o = zlib.decompressobj()
            try:
                r = o.decompress(d[off + 4:off + 4 + csz])
            except zlib.error:
                continue
            yield off, r


def walk(c, p):
    """Validate a D3D9 shader token stream starting at byte offset p; return end offset (exclusive) or 0."""
    n = len(c) // 4 * 4
    t0 = struct.unpack_from('<I', c, p)[0]
    major = (t0 >> 8) & 0xff
    if (t0 >> 16) not in (0xfffe, 0xffff) or major not in (2, 3) or (t0 & 0xff) > 1:
        return 0
    q = p + 4
    ninst = 0
    while q + 4 <= n:
        tok = struct.unpack_from('<I', c, q)[0]
        if tok == 0x0000ffff:
            return q + 4 if ninst else 0
        op = tok & 0xffff
        if op == 0xfffe:                                     # comment
            q += 4 + 4 * ((tok >> 16) & 0x7fff)
            continue
        if op > 0x60 and op != 0xfffd:
            return 0
        ln = (tok >> 24) & 0xf
        q += 4 + 4 * ln
        ninst += 1
    return 0


def main():
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    if len(a) != 2:
        print(__doc__)
        sys.exit(1)
    os.makedirs(a[1], exist_ok=True)
    seen = {}
    nch = nraw = 0
    srcs = []
    for p in a[0].split(','):
        d = open(p, 'rb').read()
        if p.endswith('.pak'):
            srcs.extend(chunks(d))                                   # zlib chunks of a 2KAP pak
        else:
            srcs.append((0, d))                                      # raw file (e.g. the decrypted exe image dump)
    for off, c in srcs:
        nch += 1
        pos = 0
        for m in VER.finditer(c):                            # shaders are not 4-byte aligned inside the container
            i = m.start()
            if i < pos:
                continue
            e = walk(c, i)
            if e:
                code = bytes(c[i:e])
                nraw += 1
                seen.setdefault((fnv64(code), len(code)), (code, off))
                pos = e
    rows = []
    for (h, ln), (code, off) in sorted(seen.items()):
        t0 = struct.unpack_from('<I', code, 0)[0]
        stage = 'ps' if t0 >> 16 == 0xffff else 'vs'
        name = '%016x_%d.bin' % (h, ln)
        open(os.path.join(a[1], name), 'wb').write(code)
        rows.append('%s\t%s\t%d_%d\t%d\t0x%x' % (name, stage, (t0 >> 8) & 0xff, t0 & 0xff, ln, off))
    open(os.path.join(a[1], 'index.tsv'), 'w').write('\n'.join(rows) + '\n')
    nvs = sum(1 for r in rows if '\tvs\t' in r)
    print('chunks %d, shader streams found %d, distinct %d (vs %d, ps %d)' % (nch, nraw, len(rows), nvs, len(rows) - nvs))


if __name__ == '__main__':
    main()
