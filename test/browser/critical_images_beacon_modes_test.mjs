// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Browser integration tests for the parts of the critical images beacon
// client JS that critical_images_beacon_test.mjs (srcset selection) does not
// reach: the calls the C++ filters emit and the beacon payload they produce.
//
// Drives the SHIPPED asset, the compiled bytes embedded in
// net/instaweb/rewriter/generated/critical_images_beacon_opt.cc, in headless
// Chromium. The C++ side calls, by name:
//   pagespeed.CriticalImages.Run(beaconUrl, htmlUrl, optionsHash,
//       sendBeaconAtOnload, checkRenderedImageSizes, nonce)
//       (critical_images_beacon_filter.cc)
//   pagespeed.CriticalImages.checkImageForCriticality(el)
//       (critical_images_beacon_filter.cc, delay_images_filter.cc)
// and the lazyload filter calls checkCriticalImages(); getBeaconData() is the
// test hook. The checks:
//
//   1. Sent at window load: the form body (oh, n, ci, rd), the beacon URL with
//      the page URL appended, image-then-input order, one key per image, one
//      image per location, below-the-fold and hidden images left out, and the
//      rendered-size map (largest instance per key, ow/oh natural size).
//   2. Not sent at load when the caller asked to send later; per-image checks
//      and checkCriticalImages() then produce the beacon, and the location
//      table is rebuilt by checkCriticalImages().
//   3. Nothing to report: no request, getBeaconData() still holds the form
//      body. A page without images or inputs stays empty.
//
// Run via run_browser_tests.sh (installs playwright + chromium on demand).

import { chromium } from 'playwright';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadCompiledAsset } from './lib.mjs';

const repoRoot = join(dirname(fileURLToPath(import.meta.url)), '..', '..');

const beaconJs = loadCompiledAsset(
    join(repoRoot, 'net', 'instaweb', 'rewriter', 'generated',
         'critical_images_beacon_opt.cc'));

// 1x1 transparent GIF.
const GIF = Buffer.from(
    'R0lGODlhAQABAIAAAP///wAAACH5BAEAAAAALAAAAAABAAEAAAICRAEAOw==',
    'base64');

const HA = '1001';
const HB = '1002';
const HI = '1003';
const HD = '1004';
const HH = '1005';

// Absolutely positioned, so every location is exact.
const at = (top, left, extra = '') =>
    `style="position:absolute;top:${top}px;left:${left}px;${extra}"`;

const IMAGES = `
  <img id="a" src="http://test/img/a.gif" data-pagespeed-url-hash="${HA}"
       width="100" height="50" ${at(0, 0)}>
  <img id="b" src="http://test/img/b.gif" data-pagespeed-url-hash="${HB}"
       width="60" height="30" ${at(0, 0)}>
  <img id="a2" src="http://test/img/a2.gif" data-pagespeed-url-hash="${HA}"
       width="80" height="40" ${at(100, 0)}>
  <img id="plain" src="http://test/img/plain.gif" width="40" height="40"
       ${at(300, 0)}>
  <img id="hidden" src="http://test/img/hidden.gif"
       data-pagespeed-url-hash="${HH}" width="40" height="40"
       ${at(350, 0, 'display:none;')}>
  <input id="in" type="image" src="http://test/img/in.gif"
         data-pagespeed-url-hash="${HI}" width="20" height="20" ${at(200, 0)}>
  <img id="deep" src="http://test/img/deep.gif" data-pagespeed-url-hash="${HD}"
       width="30" height="30" ${at(5000, 0)}>`;

function testPage(body, script) {
  return `<!DOCTYPE html><html><head></head><body>${body}
    <script>${beaconJs}<\/script>
    <script>${script}<\/script>
  </body></html>`;
}

let failures = 0;
function check(ok, label) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${label}`);
  if (!ok) failures++;
}

const browser = await chromium.launch();

// Opens html at http://test/ and records every beacon request. Returns the
// page and the list (each entry: {url, body}).
async function open(html) {
  const page = await browser.newPage({ viewport: { width: 800, height: 600 } });
  const beacons = [];
  await page.route('http://test/**', (route) => {
    const req = route.request();
    if (req.url().startsWith('http://test/beacon')) {
      beacons.push({ url: req.url(), body: req.postData() });
      return route.fulfill({ status: 204, body: '' });
    }
    if (req.url().startsWith('http://test/img/')) {
      return route.fulfill({ contentType: 'image/gif', body: GIF });
    }
    return route.fulfill({ contentType: 'text/html', body: html });
  });
  await page.goto('http://test/');
  await page.waitForLoadState('load');
  return { page, beacons };
}

async function waitFor(cond, ms = 5000) {
  const end = Date.now() + ms;
  while (!cond() && Date.now() < end) {
    await new Promise((r) => setTimeout(r, 25));
  }
}
const settle = () => new Promise((r) => setTimeout(r, 400));

// --- 1. Sent at window load, with rendered sizes ---------------------------
{
  const { page, beacons } = await open(testPage(IMAGES,
      `pagespeed.CriticalImages.Run('http://test/beacon?x=1',
           'http://test/page?q=1&r=2', 'oh9', true, true, 'nn');`));
  await waitFor(() => beacons.length > 0);
  await settle();
  check(beacons.length === 1, `at load: exactly one beacon (got ${beacons.length})`);
  const b = beacons[0] || { url: '', body: '' };
  check(b.url === 'http://test/beacon?x=1&url=' +
            encodeURIComponent('http://test/page?q=1&r=2'),
        `at load: beacon URL carries the page URL (got ${b.url})`);
  const params = new URLSearchParams(b.body);
  check(b.body.startsWith('oh=oh9&n=nn&ci='),
        `at load: body starts oh, n, ci (got ${b.body.slice(0, 40)})`);
  check(params.get('ci') === `${HA},${HI}`,
        `at load: critical keys are images then inputs, one per key and ` +
        `location, none below the fold or hidden (got ${params.get('ci')})`);
  // Image keys are numeric strings, so the object lists them in ascending
  // order, whatever order they were added in.
  const expected = {
    [HA]: { rw: 100, rh: 50, ow: 1, oh: 1 },
    [HB]: { rw: 60, rh: 30, ow: 1, oh: 1 },
    [HD]: { rw: 30, rh: 30, ow: 1, oh: 1 },
    [HH]: { rw: 40, rh: 40, ow: 1, oh: 1 },
  };
  check(params.get('rd') === JSON.stringify(expected),
        `at load: rendered sizes, one entry per key, images only, ` +
        `a smaller second instance does not replace the first (got ${params.get('rd')})`);
  const data = await page.evaluate(() => pagespeed.CriticalImages.getBeaconData());
  check(data === b.body, 'at load: getBeaconData() is the posted body');
  await page.close();
}

// --- 1b. No nonce, no rendered sizes ---------------------------------------
{
  const { page, beacons } = await open(testPage(IMAGES,
      `pagespeed.CriticalImages.Run('http://test/beacon', 'http://test/',
           'oh9', true, false, '');`));
  await waitFor(() => beacons.length > 0);
  await settle();
  check(beacons.length === 1 && beacons[0].body === `oh=oh9&ci=${HA},${HI}`,
        `at load: no nonce and no rendered sizes (got ` +
        `${beacons.length ? beacons[0].body : 'no beacon'})`);
  await page.close();
}

// --- 2. Sent later: per-image checks, then checkCriticalImages() -----------
{
  const { page, beacons } = await open(testPage(IMAGES,
      `pagespeed.CriticalImages.Run('http://test/beacon', 'http://test/',
           'oh9', false, false, 'nn');`));
  await settle();
  check(beacons.length === 0, 'later: nothing is sent at load');
  // b sits where a does: when a was checked first, b is not critical.
  await page.evaluate(() => {
    for (const id of ['a', 'b', 'deep']) {
      pagespeed.CriticalImages.checkImageForCriticality(
          document.getElementById(id));
    }
  });
  await settle();
  check(beacons.length === 0, 'later: per-image checks send nothing');
  await page.evaluate(() => pagespeed.CriticalImages.checkCriticalImages());
  await waitFor(() => beacons.length > 0);
  await settle();
  check(beacons.length === 1, `later: checkCriticalImages() sends one beacon (got ${beacons.length})`);
  // The location table is rebuilt: a is already known, so b is the first
  // image at that location now and is reported too.
  check(beacons.length === 1 && beacons[0].body === `oh=oh9&n=nn&ci=${HA},${HB},${HI}`,
        `later: keys found per image are kept and the rest is merged in ` +
        `(got ${beacons.length ? beacons[0].body : 'no beacon'})`);
  await page.close();
}

// --- 3. Nothing to report --------------------------------------------------
{
  const quiet = `
    <img src="http://test/img/deep.gif" data-pagespeed-url-hash="${HD}"
         width="30" height="30" ${at(5000, 0)}>
    <img src="http://test/img/hidden.gif" data-pagespeed-url-hash="${HH}"
         width="30" height="30" ${at(0, 0, 'display:none;')}>`;
  const { page, beacons } = await open(testPage(quiet,
      `pagespeed.CriticalImages.Run('http://test/beacon', 'http://test/',
           'oh9', true, false, 'nn');`));
  await settle();
  check(beacons.length === 0, 'quiet: no critical image, no request');
  const data = await page.evaluate(() => pagespeed.CriticalImages.getBeaconData());
  check(data === 'oh=oh9&n=nn', `quiet: getBeaconData() holds the form body (got ${data})`);
  await page.close();
}
{
  const { page, beacons } = await open(testPage('<p>no images</p>',
      `pagespeed.CriticalImages.Run('http://test/beacon', 'http://test/',
           'oh9', true, true, 'nn');`));
  await settle();
  const data = await page.evaluate(() => pagespeed.CriticalImages.getBeaconData());
  check(beacons.length === 0 && data === '',
        `empty: a page without images sends nothing (got ${JSON.stringify(data)})`);
  await page.close();
}

await browser.close();
console.log(failures ? `${failures} FAILURE(S)` : 'ALL PASS');
process.exit(failures ? 1 : 0);
