// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Browser integration tests for the responsive_images zoom client JS.
//
// Drives the SHIPPED asset, the closure-compiled bytes embedded in
// net/instaweb/rewriter/generated/responsive_js_opt.cc, in headless
// Chromium. ResponsiveImageSecondFilter adds x-descriptor srcsets
// ("url 1.5x,url 2x") and, with responsive_images_zoom, a
// <script src=".../responsive.js"> at the end of body; the script
// self-initialises and upgrades each image's src to the candidate matching
// devicePixelRatio (times zoom), preloading it first:
//
//   1. At DPR 1 nothing changes; at DPR 2 the src is upgraded to the 2x
//      candidate.
//   2. A src with no 1x candidate counts as the 1x candidate; candidates
//      are matched in resolution order, picking the smallest that is >= the
//      device ratio.
//   3. Raising the device ratio later (a resize) upgrades again; srcsets
//      with w descriptors are ignored.
//
// Run via run_browser_tests.sh (installs playwright + chromium on demand).

import { chromium } from 'playwright';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadCompiledAsset } from './lib.mjs';

const repoRoot = join(dirname(fileURLToPath(import.meta.url)), '..', '..');

const responsiveJs = loadCompiledAsset(
    join(repoRoot, 'net', 'instaweb', 'rewriter', 'generated',
         'responsive_js_opt.cc'));

// 1x1 transparent GIF.
const GIF = Buffer.from(
    'R0lGODlhAQABAIAAAP///wAAACH5BAEAAAAALAAAAAABAAEAAAICRAEAOw==',
    'base64');

const I = (n) => `http://test/img/${n}.gif`;

const testPage = `<!DOCTYPE html><html><body>
  <img id="std" src="${I('a1')}" srcset="${I('a15')} 1.5x,${I('a2')} 2x,${I('a4')} 4x"
       width="10" height="10">
  <img id="unordered" src="${I('b1')}" srcset="${I('b3')} 3x, ${I('b2')} 2x"
       width="10" height="10">
  <img id="wdesc" src="${I('c1')}" srcset="${I('c2')} 200w" width="10" height="10">
  <script src="http://test/static/responsive.js"><\/script>
</body></html>`;

let failures = 0;
function check(ok, label) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${label}`);
  if (!ok) failures++;
}

const browser = await chromium.launch();

async function openPage(deviceScaleFactor, expected) {
  const page = await browser.newPage({
    viewport: { width: 800, height: 600 }, deviceScaleFactor,
  });
  await page.route('http://test/**', (route) => {
    const url = route.request().url();
    if (url.endsWith('/responsive.js')) {
      return route.fulfill({ contentType: 'application/javascript',
                             body: responsiveJs });
    }
    if (url.startsWith('http://test/img/')) {
      return route.fulfill({ contentType: 'image/gif', body: GIF });
    }
    return route.fulfill({ contentType: 'text/html', body: testPage });
  });
  await page.goto('http://test/');
  await page.waitForLoadState('load');
  await waitSrcs(page, expected);
  return page;
}

// Waits (condition based) until the given img id -> src pairs hold. With no
// expectation (negative checks) a short fixed settle is used instead; a slow
// machine can only make such a check weaker, never red.
async function waitSrcs(page, expected) {
  if (!expected) {
    await page.waitForTimeout(200);
    return;
  }
  try {
    await page.waitForFunction(
        (exp) => Object.entries(exp).every(
            ([id, src]) => document.getElementById(id).getAttribute('src') === src),
        expected, { timeout: 15000 });
  } catch (e) {
    // Fall through: the following check() reports the actual values.
  }
}

const srcs = (page) => page.evaluate(() => Object.fromEntries(
    ['std', 'unordered', 'wdesc'].map(
        (i) => [i, document.getElementById(i).getAttribute('src')])));

// --- 1. DPR 1 leaves sources alone ---------------------------------------
{
  const page = await openPage(1);
  const s = await srcs(page);
  check(s.std === I('a1') && s.unordered === I('b1') && s.wdesc === I('c1'),
        `DPR 1: src unchanged (got ${Object.values(s).join(' ')})`);
  await page.close();
}

// --- 1-2. DPR 2 upgrades to the 2x candidate -----------------------------
{
  const page = await openPage(2, { std: I('a2'), unordered: I('b2') });
  const s = await srcs(page);
  check(s.std === I('a2'), `DPR 2: upgraded to 2x candidate (got ${s.std})`);
  check(s.unordered === I('b2'),
        `DPR 2: unordered srcset sorted by resolution (got ${s.unordered})`);
  check(s.wdesc === I('c1'), 'DPR 2: w-descriptor srcset ignored');
  await page.close();
}

// --- 2. Fractional ratio picks the smallest sufficient candidate ---------
{
  const page = await openPage(1.25, { std: I('a15') });
  const s = await srcs(page);
  check(s.std === I('a15'), `DPR 1.25: picks 1.5x candidate (got ${s.std})`);
  await page.close();
}

// --- 3. Ratio increase after load upgrades again -------------------------
{
  const page = await openPage(1);
  const cdp = await page.context().newCDPSession(page);
  await cdp.send('Emulation.setDeviceMetricsOverride', {
    width: 800, height: 600, deviceScaleFactor: 3, mobile: false,
  });
  await page.evaluate(() => window.dispatchEvent(new Event('resize')));
  await waitSrcs(page, { std: I('a4'), unordered: I('b3') });
  const s = await srcs(page);
  check(s.std === I('a4'), `ratio 1 -> 3: upgraded to 4x candidate (got ${s.std})`);
  check(s.unordered === I('b3'),
        `ratio 1 -> 3: upgraded to 3x candidate (got ${s.unordered})`);
  // Lowering the ratio never downgrades.
  await cdp.send('Emulation.setDeviceMetricsOverride', {
    width: 800, height: 600, deviceScaleFactor: 1, mobile: false,
  });
  await page.evaluate(() => window.dispatchEvent(new Event('resize')));
  await page.waitForTimeout(200);
  check((await srcs(page)).std === I('a4'), 'ratio 3 -> 1: no downgrade');
  await page.close();
}

await browser.close();
console.log(failures ? `${failures} FAILURE(S)` : 'ALL PASS');
process.exit(failures ? 1 : 0);
