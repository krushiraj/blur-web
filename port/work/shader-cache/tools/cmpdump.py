import sys, os, glob
sys.path.insert(0, os.path.dirname(__file__))
from pakshaders import fnv64
pak = set(os.path.basename(f)[:-4] for f in glob.glob(sys.argv[1] + '/*.bin'))
runtime = {}
for f in glob.glob(sys.argv[2] + '/*.bin'):
    b = open(f, 'rb').read()
    runtime['%016x_%d' % (fnv64(b), len(b))] = f
print('pak distinct', len(pak), 'runtime distinct', len(runtime), 'files', len(glob.glob(sys.argv[2] + '/*.bin')))
miss = [k for k in runtime if k not in pak]
print('runtime not in pak', len(miss))
for k in miss[:10]: print(k, runtime[k])
