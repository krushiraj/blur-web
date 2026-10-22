"""Prints the result json of pack_validate.html / numeric.html as a short report; exit status 1 if anything failed."""
import json
import sys

mode, path = sys.argv[1], sys.argv[2]
r = json.load(open(path))
if 'fatal' in r:
    print('FATAL', r['fatal'])
    sys.exit(1)
bad = 0
if mode == 'validate':
    m, p = r['modules'], r['pairs']
    print('adapter: %s' % r['adapter'])
    print('shader modules : %d / %d ok (vs %d, ps %d), warnings %d, %d ms' % (m['ok'], m['total'], m['vs'], m['ps'], sum(m['warnings'].values()), r['ms']['modules']))
    print('pipelines      : %d / %d ok (distinct warm.json vs,ps pairs), PS inputs missing from VS outputs: %d, create p50 %d ms p95 %d ms' %
          (p['ok'], p['total'], p['missingVarying'], r['ms']['p50'], r['ms']['p95']))
    if 'pad' in r:
        print('padVsOutputs   : %d / %d VS modules still valid with all 13 interpolants appended' % (r['pad']['ok'], r['pad']['total']))
    for k, v in list(m['errors'].items()) + list(p['errors'].items()):
        print('  error x%d: %s' % (v, k))
    bad = (m['total'] - m['ok']) + (p['total'] - p['ok'])
else:
    s = r['summary']
    print('pairs %(pairs)d, GL link errors %(glError)d, input-location mismatches %(inMaskMismatch)d' % s)
    print('cases (pair x input set): %(cases)d, with visible pixels: %(covered)d, coverage disagreement: %(coverMismatch)d, pairs never drawing anything: %(pairsAllModesEmpty)d' % s)
    print('max abs diff per case (0..255): ' + json.dumps(s['maxdHist']))
    worst = sorted(((m.get('maxd', 0), p['vs'], p['ps']) for p in r['pairs'] for m in p['modes']), reverse=True)[:5]
    print('worst 5: ' + ', '.join('%d (%s, %s)' % w for w in worst))
    bad = s['glError'] + s['inMaskMismatch'] + s['coverMismatch'] + s['maxdHist']['gt2']
sys.exit(1 if bad else 0)
