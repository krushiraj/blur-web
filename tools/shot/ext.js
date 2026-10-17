const puppeteer = require('puppeteer-core');
(async () => {
  const browser = await puppeteer.launch({ executablePath: process.env.CHROMIUM || '/Applications/Chromium.app/Contents/MacOS/Chromium', headless: 'new', args: ['--use-angle=metal', '--ignore-gpu-blocklist'] });
  const page = await browser.newPage(); await page.goto('about:blank');
  console.log(await page.evaluate(() => { const gl = document.createElement('canvas').getContext('webgl2'); const e = gl.getSupportedExtensions(); return { baseVertex: e.includes('WEBGL_draw_instanced_base_vertex_base_instance'), multiDraw: e.includes('WEBGL_multi_draw'), multiDrawBV: e.includes('WEBGL_multi_draw_instanced_base_vertex_base_instance'), parallel: e.includes('KHR_parallel_shader_compile'), n: e.length }; }));
  await browser.close();
})();
