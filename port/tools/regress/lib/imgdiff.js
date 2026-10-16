// Tolerant perceptual image comparison without any native dependency: the PNGs are decoded and compared inside a headless Chromium page (canvas).
//   const d = await imgdiff.open(browser); await d.compare('a.png', 'b.png') -> {mae, ssim}; await d.stats('a.png') -> {mean, std, ...}
// CLI: node imgdiff.js a.png b.png      |      node imgdiff.js --stats a.png
const fs = require('fs');
const { launch } = require('./browser');

const PAGE_FN = `
window.__decode = async (b64, W, H) => {
  const bin = atob(b64); const u8 = new Uint8Array(bin.length); for (let i = 0; i < bin.length; i++) u8[i] = bin.charCodeAt(i);
  const bmp = await createImageBitmap(new Blob([u8], { type: 'image/png' }));
  const c = document.createElement('canvas'); c.width = W; c.height = H; const g = c.getContext('2d', { willReadFrequently: true });
  g.imageSmoothingEnabled = true; g.drawImage(bmp, 0, 0, W, H); return Array.from(g.getImageData(0, 0, W, H).data);
};
window.__metrics = (A, B, W, H) => {                     // A, B: RGBA arrays at W x H
  let sad = 0; const n = W * H;
  const la = new Float32Array(n), lb = new Float32Array(n);
  for (let i = 0; i < n; i++) { const o = i * 4; sad += Math.abs(A[o] - B[o]) + Math.abs(A[o + 1] - B[o + 1]) + Math.abs(A[o + 2] - B[o + 2]);
    la[i] = 0.299 * A[o] + 0.587 * A[o + 1] + 0.114 * A[o + 2]; lb[i] = 0.299 * B[o] + 0.587 * B[o + 1] + 0.114 * B[o + 2]; }
  const mae = sad / (3 * 255 * n);
  // mean SSIM over 8x8 blocks (luminance)
  const C1 = (0.01 * 255) ** 2, C2 = (0.03 * 255) ** 2; let s = 0, cnt = 0;
  for (let by = 0; by + 8 <= H; by += 8) for (let bx = 0; bx + 8 <= W; bx += 8) {
    let ma = 0, mb = 0; for (let y = 0; y < 8; y++) for (let x = 0; x < 8; x++) { const i = (by + y) * W + bx + x; ma += la[i]; mb += lb[i]; } ma /= 64; mb /= 64;
    let va = 0, vb = 0, cv = 0; for (let y = 0; y < 8; y++) for (let x = 0; x < 8; x++) { const i = (by + y) * W + bx + x; va += (la[i] - ma) ** 2; vb += (lb[i] - mb) ** 2; cv += (la[i] - ma) * (lb[i] - mb); }
    va /= 63; vb /= 63; cv /= 63; s += ((2 * ma * mb + C1) * (2 * cv + C2)) / ((ma * ma + mb * mb + C1) * (va + vb + C2)); cnt++; }
  return { mae, ssim: s / cnt };
};
window.__stats = (A, W, H) => {
  const n = W * H; let sum = 0, sum2 = 0, r = 0, g = 0, b = 0;
  for (let i = 0; i < n; i++) { const o = i * 4, l = 0.299 * A[o] + 0.587 * A[o + 1] + 0.114 * A[o + 2]; sum += l; sum2 += l * l; r += A[o]; g += A[o + 1]; b += A[o + 2]; }
  const mean = sum / n; return { lumaMean: mean, lumaStd: Math.sqrt(Math.max(0, sum2 / n - mean * mean)), r: r / n, g: g / n, b: b / n };
};`;

const W = 320, H = 180;
async function open(browser) {
  const page = await browser.newPage(); await page.goto('about:blank'); await page.evaluate(PAGE_FN.replace(/^\s*/, ''));
  const dec = (f) => page.evaluate((b, w, h) => window.__decode(b, w, h), fs.readFileSync(f).toString('base64'), W, H);
  return {
    compare: async (a, b) => { const A = await dec(a), B = await dec(b); return page.evaluate((x, y, w, h) => window.__metrics(x, y, w, h), A, B, W, H); },
    stats: async (a) => { const A = await dec(a); return page.evaluate((x, w, h) => window.__stats(x, w, h), A, W, H); },
    // stats of a sub-rectangle given in fractions of the image (x0,y0,x1,y1 in 0..1)
    regionStats: async (a, x0, y0, x1, y1) => { const A = await dec(a); const px = []; const sx = Math.floor(x0 * W), ex = Math.ceil(x1 * W), sy = Math.floor(y0 * H), ey = Math.ceil(y1 * H);
      for (let y = sy; y < ey; y++) for (let x = sx; x < ex; x++) { const o = (y * W + x) * 4; px.push(A[o], A[o + 1], A[o + 2], 255); }
      return page.evaluate((p, w, h) => window.__stats(p, w, h), px, ex - sx, ey - sy); },
    close: () => page.close(),
  };
}
module.exports = { open };

if (require.main === module) (async () => {
  const br = await launch(); const d = await open(br);
  try {
    if (process.argv[2] === '--stats') console.log(JSON.stringify(await d.stats(process.argv[3])));
    else console.log(JSON.stringify(await d.compare(process.argv[2], process.argv[3])));
  } finally { await br.close(); }
})();
