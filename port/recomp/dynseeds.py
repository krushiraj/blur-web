import re, glob, sys
cov = set()
pat = re.compile(rb'^\s*/\* ([0-9a-f]{8}): ', re.M)
for f in glob.glob('gen_all/part_*.c'):
    for m in pat.finditer(open(f, 'rb').read()): cov.add(int(m.group(1), 16))
blocks = [int(l, 16) for l in open(sys.argv[1]) if l.strip()]
unc = [b for b in blocks if b not in cov]
print('covered instrs', len(cov), 'blocks', len(blocks), 'uncovered', len(unc))
old = set()
try: old = {int(l, 16) for l in open('dynseeds.txt') if l.strip()}
except OSError: pass
new = sorted(old | set(unc))
open('dynseeds.txt', 'w').write('\n'.join('%08x' % a for a in new) + '\n'); print('dynseeds', len(new))
