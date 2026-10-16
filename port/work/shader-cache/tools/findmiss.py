import sys, os, glob, struct
sys.path.insert(0, os.path.dirname(__file__))
from pakshaders import fnv64, chunks
d = open(sys.argv[1], 'rb').read()
cs = [c for _, c in chunks(d)]
big = b'\0\0\0\0'.join(cs)
pak = set(os.path.basename(f)[:-4] for f in glob.glob(sys.argv[2] + '/*.bin'))
n = inside = 0
ver = {}
for f in sorted(glob.glob(sys.argv[3] + '/*.bin')):
    b = open(f, 'rb').read()
    k = '%016x_%d' % (fnv64(b), len(b))
    if k in pak: continue
    n += 1
    t = struct.unpack_from('<I', b, 0)[0]
    ver[hex(t)] = ver.get(hex(t), 0) + 1
    if big.find(b) >= 0: inside += 1
print('missing', n, 'found as raw substring in pak', inside)
print(ver)
