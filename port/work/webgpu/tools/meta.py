import subprocess, glob, os, re, json, sys
from concurrent.futures import ThreadPoolExecutor
def one(f):
    t = subprocess.run(['spirv-dis', '--raw-id', f] if False else ['spirv-dis', f], capture_output=True, text=True).stdout
    names = dict(re.findall(r'OpName (%\w+) "(\w+)"', t))
    loc = dict(re.findall(r'OpDecorate (%\w+) Location (\d+)', t)); comp = dict(re.findall(r'OpDecorate (%\w+) Component (\d+)', t))
    io = {}
    for i, n in names.items():
        if i in loc and re.fullmatch(r'[vo]\d+', n): io[n] = [int(loc[i]), int(comp.get(i, 0))]
    return os.path.basename(f)[:-4], io
fs = glob.glob('spv2/*.spv')
with ThreadPoolExecutor(8) as ex: res = dict(ex.map(one, fs))
json.dump(res, open('meta.json', 'w')); print(len(res))
