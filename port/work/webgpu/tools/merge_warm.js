// usage: node merge_warm.js <exported.json> [gpu-warm.json]   merges recipes exported from a live page (copy(__gpuWarm()) with #gpudump=1, or IndexedDB 'blur-gpuwarm') into the static warm list
const fs = require('fs'), path = require('path');
const [src, dst = path.resolve(__dirname, '../../../harness/web/gpu-warm.json')] = process.argv.slice(2);
const old = fs.existsSync(dst) ? JSON.parse(fs.readFileSync(dst, 'utf8')) : [], add0 = JSON.parse(fs.readFileSync(src, 'utf8')), add = typeof add0 === 'string' ? JSON.parse(add0) : add0, seen = new Set(old.map((c) => JSON.stringify(c)));
for (const c of add) if (c.vl && !seen.has(JSON.stringify(c))) { old.push(c); seen.add(JSON.stringify(c)); }
fs.writeFileSync(dst, JSON.stringify(old)); console.log(old.length + ' pipelines in ' + dst);
