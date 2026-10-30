// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Browser integration tests for the delay_images and delay_images_inline
// client JS.
//
// Drives the SHIPPED assets, the compiled bytes embedded in
// net/instaweb/rewriter/generated/delay_images_inline_opt.cc and
// delay_images_opt.cc, in headless Chromium against markup shaped like
// DelayImagesFilter's output: src renamed to data-pagespeed-high-res-src,
// the combined asset script with delayImagesInlineInit()/delayImagesInit(),
// per-image addLowResImages(url, data) + replaceWithLowRes() snippets, and
// replaceWithHighRes() or registerLazyLoadHighRes() at the end of body:
//
//   1. replaceWithLowRes() sets src to the mapped low-res data URL on <img>
//      and <input type=image>; images with no map entry or with a src
//      already are left alone.
//   2. replaceWithHighRes() swaps every element to its high-res src.
//   3. registerLazyLoadHighRes() swaps at window load, and a later
//      re-registration swaps again on click.
//   4. In-place mode: the filter's inline switchToHighResAndMaybeBeacon
//      onload snippet swaps in the high-res srcset and src.
//
// Run via run_browser_tests.sh (installs playwright + chromium on demand).

import { chromium } from 'playwright';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadCompiledAsset } from './lib.mjs';

const repoRoot = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const gen = (f) => loadCompiledAsset(
    join(repoRoot, 'net', 'instaweb', 'rewriter', 'generated', f));

const inlineJs = gen('delay_images_inline_opt.cc');
const delayJs = gen('delay_images_opt.cc');

// Mirrors DelayImagesFilter::kDelayImagesInlineSuffix/kDelayImagesSuffix.
const assetScript =
    `${inlineJs}\npagespeed.delayImagesInlineInit();` +
    `${delayJs}\npagespeed.delayImagesInit();`;

// Mirrors DelayImagesFilter::kImageOnloadJsSnippet / kImageOnloadCode.
const onloadSnippet =
    "window['pagespeed'] = window['pagespeed'] || {};" +
    "var pagespeed = window['pagespeed'];" +
    'pagespeed.switchToHighResAndMaybeBeacon = function(elem) {' +
    'setTimeout(function(){elem.onload = null;' +
    "var srcset = elem.getAttribute('data-pagespeed-high-res-srcset');" +
    'if (srcset) {elem.srcset = srcset;}' +
    "elem.src = elem.getAttribute('data-pagespeed-high-res-src');" +
    'if (pagespeed.CriticalImages) {elem.onload = ' +
    'pagespeed.CriticalImages.checkImageForCriticality(elem);}' +
    '}, 0);' +
    '};';
const onloadCode = 'if(window.pagespeed){if(pagespeed.switchToHighResAndMaybeBeacon){' +
    'pagespeed.switchToHighResAndMaybeBeacon(this);}}';

const GIF_B64 =
    'R0lGODlhAQABAIAAAP///wAAACH5BAEAAAAALAAAAAABAAEAAAICRAEAOw==';
const GIF = Buffer.from(GIF_B64, 'base64');
const LOW_A = `data:image/gif;base64,${GIF_B64}#a`;
const LOW_B = `data:image/gif;base64,${GIF_B64}#b`;
const HI_A = 'http://test/img/a.gif';
const HI_B = 'http://test/img/b.gif';
const HI_C = 'http://test/img/c.gif';
const HI_D = 'http://test/img/d.gif';

function deferredPage(endScript) {
  return `<!DOCTYPE html><html><head></head><body>
    <img id="a" data-pagespeed-high-res-src="${HI_A}" width="10" height="10">
    <input id="b" type="image" data-pagespeed-high-res-src="${HI_B}">
    <img id="c" data-pagespeed-high-res-src="${HI_C}" width="10" height="10">
    <img id="d" src="${HI_D}" data-pagespeed-high-res-src="${HI_D}"
         width="10" height="10">
    <img id="s" data-pagespeed-high-res-src="${HI_A}"
         data-pagespeed-high-res-srcset="${HI_A} 1x, ${HI_B} 2x"
         width="10" height="10">
    <script data-pagespeed-no-defer>${assetScript}<\/script>
    <script data-pagespeed-no-defer>
if(window.pagespeed&&pagespeed.delayImagesInline){pagespeed.delayImagesInline.addLowResImages('${HI_A}', '${LOW_A}');
pagespeed.delayImagesInline.replaceWithLowRes();}
<\/script>
    <script data-pagespeed-no-defer>
if(window.pagespeed&&pagespeed.delayImagesInline){pagespeed.delayImagesInline.addLowResImages('${HI_B}', '${LOW_B}');
pagespeed.delayImagesInline.replaceWithLowRes();}
<\/script>
    <script>
      window.__lowRes = ['a', 'b', 'c', 'd', 's'].map(
          (i) => document.getElementById(i).getAttribute('src'));
    <\/script>
    <script data-pagespeed-no-defer>${endScript}<\/script>
  </body></html>`;
}

let failures = 0;
function check(ok, label) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${label}`);
  if (!ok) failures++;
}

const browser = await chromium.launch();

async function openPage(html) {
  const page = await browser.newPage();
  await page.route('http://test/**', (route) => {
    if (route.request().url().startsWith('http://test/img/')) {
      return route.fulfill({ contentType: 'image/gif', body: GIF });
    }
    return route.fulfill({ contentType: 'text/html', body: html });
  });
  await page.goto('http://test/');
  await page.waitForLoadState('load');
  return page;
}

const srcs = (page) => page.evaluate(() => ['a', 'b', 'c', 'd', 's'].map(
    (i) => document.getElementById(i).getAttribute('src')));

// --- 1-2. Low-res then high-res ------------------------------------------
{
  const page = await openPage(deferredPage(
      'if(window.pagespeed&&pagespeed.delayImages){' +
      'pagespeed.delayImages.replaceWithHighRes();}'));
  const low = await page.evaluate(() => window.__lowRes);
  check(low[0] === LOW_A, 'replaceWithLowRes: <img> gets its low-res data URL');
  check(low[1] === LOW_B,
        'replaceWithLowRes: <input type=image> gets its low-res data URL');
  check(low[2] === null, 'replaceWithLowRes: image with no map entry untouched');
  check(low[3] === HI_D, 'replaceWithLowRes: image with a src is not overwritten');
  const hi = await srcs(page);
  check(hi[0] === HI_A && hi[1] === HI_B && hi[2] === HI_C,
        `replaceWithHighRes: all elements on high-res src (got ${hi.join(' ')})`);
  check(hi[4] === HI_A, 'replaceWithHighRes: srcset image gets its high-res src');
  // KNOWN-QUIRK: this pins existing behaviour, not a contract. A
  // fix that restores srcset should update this assertion deliberately.
  // Current behaviour: delay_images.js restores only src, never
  // data-pagespeed-high-res-srcset (the in-place onload snippet does).
  const srcset = await page.evaluate(
      () => document.getElementById('s').getAttribute('srcset'));
  check(srcset === null,
        'replaceWithHighRes: srcset is not restored (current behaviour)');
  await page.close();
}

// --- 3. registerLazyLoadHighRes ------------------------------------------
{
  const page = await openPage(deferredPage(
      'if(window.pagespeed&&pagespeed.delayImages){' +
      'pagespeed.delayImages.registerLazyLoadHighRes();}'));
  const low = await page.evaluate(() => window.__lowRes);
  check(low[0] === LOW_A, 'lazy high-res: low-res shown before onload');
  const hi = await srcs(page);
  check(hi[0] === HI_A && hi[2] === HI_C,
        'lazy high-res: high-res swapped in at window load');

  // Revert to low res and re-register: a click swaps high-res in again.
  await page.evaluate((low) => {
    document.getElementById('a').setAttribute('src', low);
    pagespeed.delayImages.registerLazyLoadHighRes();
  }, LOW_A);
  await page.waitForTimeout(100);
  check((await srcs(page))[0] === LOW_A,
        'lazy high-res: re-registered, no swap before interaction');
  await page.mouse.click(5, 5);
  check((await srcs(page))[0] === HI_A,
        'lazy high-res: click swaps to high-res again');
  await page.close();
}

// --- 4. In-place low-res with the onload snippet -------------------------
{
  const page = await openPage(`<!DOCTYPE html><html><body>
    <script data-pagespeed-no-defer>${onloadSnippet}<\/script>
    <img id="p" src="${LOW_A}" data-pagespeed-high-res-src="${HI_A}"
         data-pagespeed-high-res-srcset="${HI_A} 1x, ${HI_B} 2x"
         onload="${onloadCode}" onerror="this.onerror=null;${onloadCode}">
  </body></html>`);
  try {
    await page.waitForFunction(
        (hi) => document.getElementById('p').getAttribute('src') === hi,
        HI_A, { timeout: 15000 });
  } catch {}
  const r = await page.evaluate(() => {
    const e = document.getElementById('p');
    return { src: e.getAttribute('src'), srcset: e.getAttribute('srcset') };
  });
  check(r.src === HI_A, `in-place: onload swaps to high-res src (got ${r.src})`);
  check(r.srcset === `${HI_A} 1x, ${HI_B} 2x`,
        `in-place: high-res srcset applied (got ${r.srcset})`);
  await page.close();
}

await browser.close();
console.log(failures ? `${failures} FAILURE(S)` : 'ALL PASS');
process.exit(failures ? 1 : 0);
