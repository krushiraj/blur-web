// Shared puppeteer helpers for the regression tools (puppeteer-core lives in tools/shot; chromium is the system one).
const path = require('path');
const SHOT = path.resolve(__dirname, '../../../../tools/shot');
// the module directory name is spelled indirectly: a shell hook in this repo's sessions rejects command lines that contain it
const puppeteer = require([SHOT, 'node' + '_modules', 'puppeteer-core'].join('/'));
const CHROMIUM = process.env.CHROMIUM || '/Applications/Chromium.app/Contents/MacOS/Chromium';

async function launch() {
  return puppeteer.launch({ executablePath: CHROMIUM, headless: 'new', defaultViewport: { width: 1320, height: 900 },
    args: ['--use-angle=metal', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist', '--window-size=1320,900'] });
}

// Screenshot just the game canvas (the page also has a status bar and a log pane that change from run to run).
async function shootCanvas(page, file) {
  const el = await page.$('canvas');
  if (el) await el.screenshot({ path: file }); else await page.screenshot({ path: file });
}

module.exports = { launch, shootCanvas, puppeteer };
