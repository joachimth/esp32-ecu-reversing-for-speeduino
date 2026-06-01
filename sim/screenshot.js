const { chromium } = require('playwright');
const path = require('path');

(async () => {
  const browser = await chromium.launch({ args: ['--no-sandbox', '--disable-setuid-sandbox'] });

  // Mobile view (375px – iPhone)
  const mobile = await browser.newContext({ viewport: { width: 390, height: 844 } });
  const mPage = await mobile.newPage();
  await mPage.goto('file://' + path.resolve(__dirname, 'index.html'));
  await mPage.waitForTimeout(800);
  await mPage.screenshot({ path: path.resolve(__dirname, 'screenshot_mobile.png'), fullPage: true });
  await mobile.close();

  // Desktop view (600px)
  const desktop = await browser.newContext({ viewport: { width: 620, height: 900 } });
  const dPage = await desktop.newPage();
  await dPage.goto('file://' + path.resolve(__dirname, 'index.html'));
  // Wait for sim pre-population (120 ticks × 200ms = 24s simulated, runs synchronously in JS so
  // the page just needs a moment to paint after load. 800ms is enough but let's be safe.)
  await dPage.waitForTimeout(800);
  // Top section
  await dPage.screenshot({ path: path.resolve(__dirname, 'screenshot_top.png'), clip: { x: 0, y: 0, width: 620, height: 900 } });

  // Scroll down for rest
  await dPage.evaluate(() => window.scrollTo(0, 900));
  await dPage.waitForTimeout(200);
  await dPage.screenshot({ path: path.resolve(__dirname, 'screenshot_bottom.png'), clip: { x: 0, y: 0, width: 620, height: 900 } });

  // Full page
  await dPage.evaluate(() => window.scrollTo(0, 0));
  await dPage.waitForTimeout(200);
  await dPage.screenshot({ path: path.resolve(__dirname, 'screenshot_full.png'), fullPage: true });

  await desktop.close();
  await browser.close();
  console.log('Screenshots saved.');
})();
