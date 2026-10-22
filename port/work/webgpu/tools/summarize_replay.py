"""stdin: stdout of tools/replay_run.js (json, maybe preceded by log lines); prints one summary line.  usage: summarize_replay.py <label>"""
import json
import sys

label = sys.argv[1]
txt = sys.stdin.read()
r = json.loads(txt[txt.index('{\n'):])
if r.get('errors'):
    print(label, 'ERRORS', r['errors'])
    sys.exit(1)
g, p, d = r['gl'], r['gpu'], r['diff']
h = d['hist']
n = d['size'][0] * d['size'][1]
print('%-22s draws %5d | JS p50 GL %5.1f ms (min %.1f max %.1f)  GPU %5.1f ms (min %.1f max %.1f) | pixels: exact %5.1f%%  <=8 %5.1f%%  >32 %5.2f%%  mean %.3f | first-feed skipped draws %d' % (
    label, g['draws'], g['frame']['p50'], g['frame']['min'], g['frame']['max'], p['frame']['p50'], p['frame']['min'], p['frame']['max'],
    100.0 * h['eq0'] / n, 100.0 * (h['eq0'] + h['le2'] + h['le8']) / n, 100.0 * h['gt32'] / n, d['mean'], p['pass1']['skipped']))
