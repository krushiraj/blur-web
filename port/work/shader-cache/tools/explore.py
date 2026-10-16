import zlib, struct, sys, re, collections
d = open(sys.argv[1], 'rb').read()
chunks = []
covered = 0
bad = []
for off in range(0, len(d), 0x800):
    blk = d[off:off + 6]
    if len(blk) < 6: break
    csz = struct.unpack('<I', blk[:4])[0]
    if blk[4] == 0x78 and 0 < csz < 0x1000000:
        try:
            o = zlib.decompressobj(); r = o.decompress(d[off + 4:off + 4 + csz]);
            used = csz - len(o.unused_data)
            chunks.append((off, csz, len(r), used))
        except Exception as e:
            bad.append((off, str(e)))
print(len(chunks), 'chunks', len(bad), 'bad')
print(bad[:5])
sz = collections.Counter()
for off, csz, usz, used in chunks:
    if used != csz: sz['mismatch'] += 1
print(sz)
tot = sum(c[2] for c in chunks); print('uncompressed total', tot)
# non-zero non-chunk blocks
cs = set(c[0] for c in chunks)
nzb = [off for off in range(0, len(d), 0x800) if off not in cs and d[off:off + 0x800] != b'\0' * 0x800]
# blocks that are continuation of a chunk
cont = set()
for off, csz, usz, used in chunks:
    for k in range(off, off + 4 + csz, 0x800): cont.add(k)
rest = [o for o in nzb if o not in cont]
print('unaccounted nonzero blocks', len(rest), [hex(x) for x in rest[:20]])
