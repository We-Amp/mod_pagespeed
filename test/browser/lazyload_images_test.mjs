// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Browser integration tests for the lazyload_images client JS.
//
// Drives the SHIPPED asset — the closure-compiled bytes embedded in
// net/instaweb/rewriter/generated/lazyload_images_opt.cc — in headless
// Chromium against markup shaped like LazyloadImagesFilter's output: the
// asset followed by pagespeed.lazyLoadInit(<after_onload>, <blank url>),
// and <img> elements whose src was renamed to data-pagespeed-lazy-src (and
// srcset to data-pagespeed-lazy-srcset), with the blank pixel as src and
// the kImageOnloadCode onload/onerror handlers:
//
//   1. Above-the-fold images swap in their real src (and srcset) once the
//      blank pixel loads; the lazy attributes are removed.
//   2. Below-the-fold images stay on the blank pixel until scrolled into
//      view.
//   3. overrideAttributeFunctions(): getAttribute('src') returns the real
//      URL while the image is still deferred.
//   4. loadAllImages() loads every deferred image regardless of position.
//   5. lazyLoadInit(true, ...) (LazyloadImagesAfterOnload) loads below-fold
//      images after window.onload without any scroll.
//   6. With pagespeed.CriticalImages present, each lazily loaded image is
//      checked for criticality and checkCriticalImages() runs once all
//      deferred images have loaded.
//
// Run via run_browser_tests.sh (installs playwright + chromium on demand).

import { chromium } from 'playwright';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadCompiledAsset } from './lib.mjs';

const repoRoot = join(dirname(fileURLToPath(import.meta.url)), '..', '..');

const lazyJs = loadCompiledAsset(
    join(repoRoot, 'net', 'instaweb', 'rewriter', 'generated',
         'lazyload_images_opt.cc'));

const BLANK = 'http://test/static/blank.gif';
const ONLOAD = 'pagespeed.lazyLoadImages.loadIfVisibleAndMaybeBeacon(this);';

// 1x1 transparent GIF.
const GIF = Buffer.from(
    'R0lGODlhAQABAIAAAP///wAAACH5BAEAAAAALAAAAAABAAEAAAICRAEAOw==',
    'base64');

function lazyImg(id, url, { style = '', srcset = '' } = {}) {
  const srcsetAttr = srcset ? ` data-pagespeed-lazy-srcset="${srcset}"` : '';
  return `<img id="${id}" data-pagespeed-lazy-src="${url}"${srcsetAttr}
       src="${BLANK}" width="100" height="100" style="${style}"
       onload="${ONLOAD}" onerror="this.onerror=null;${ONLOAD}">`;
}

function testPage({ afterOnload = false, preamble = '', tail = '' } = {}) {
  return `<!DOCTYPE html><html><head></head><body style="margin:0">
    ${preamble}
    <script data-pagespeed-no-defer>${lazyJs}
pagespeed.lazyLoadInit(${afterOnload}, "${BLANK}");
<\/script>
    ${lazyImg('above', 'http://test/img/above.gif')}
    ${lazyImg('srcset', 'http://test/img/s1.gif',
              { srcset: 'http://test/img/s1.gif 1x, http://test/img/s2.gif 2x' })}
    ${lazyImg('below', 'http://test/img/below.gif',
              { style: 'position:absolute; top:4000px' })}
    <div style="height:6000px"></div>
    ${tail}
  </body></html>`;
}

let failures = 0;
function check(ok, label) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${label}`);
  if (!ok) failures++;
}

const browser = await chromium.launch();

async function openPage(html) {
  const page = await browser.newPage({ viewport: { width: 800, height: 600 } });
  const requested = [];
  await page.route('http://test/**', (route) => {
    const url = route.request().url();
    requested.push(url);
    if (url.startsWith('http://test/img/') ||
        url.startsWith('http://test/static/')) {
      return route.fulfill({ contentType: 'image/gif', body: GIF });
    }
    return route.fulfill({ contentType: 'text/html', body: html });
  });
  await page.goto('http://test/');
  await page.waitForLoadState('load');
  return { page, requested };
}

async function waitFor(page, fn, arg, timeout = 3000) {
  try {
    await page.waitForFunction(fn, arg, { timeout });
    return true;
  } catch {
    return false;
  }
}

const srcOf = (page, id) =>
  page.evaluate((i) => document.getElementById(i).src, id);

// --- 1-2. Above-fold loads, below-fold waits for scroll -------------------
{
  const { page, requested } = await openPage(testPage());
  check(await waitFor(page, () =>
            document.getElementById('above').src.endsWith('/img/above.gif')),
        'above-fold image swapped to its real src');
  const above = await page.evaluate(() => {
    const e = document.getElementById('above');
    return {
      lazy: e.hasAttribute('data-pagespeed-lazy-src'),
      onload: e.hasAttribute('onload'),
    };
  });
  check(!above.lazy, 'above-fold: data-pagespeed-lazy-src removed');
  check(!above.onload, 'above-fold: onload attribute removed');
  check(await waitFor(page, () =>
            document.getElementById('srcset').getAttribute('srcset') !== null),
        'above-fold srcset image: srcset restored');
  const ss = await page.evaluate(() => {
    const e = document.getElementById('srcset');
    return { srcset: e.getAttribute('srcset'),
             lazy: e.hasAttribute('data-pagespeed-lazy-srcset') };
  });
  check(ss.srcset === 'http://test/img/s1.gif 1x, http://test/img/s2.gif 2x',
        `srcset value round-trips (got ${ss.srcset})`);
  check(!ss.lazy, 'data-pagespeed-lazy-srcset removed');

  await page.waitForTimeout(400);
  check((await srcOf(page, 'below')) === BLANK,
        'below-fold image still on the blank pixel before scrolling');
  check(!requested.includes('http://test/img/below.gif'),
        'below-fold real image not fetched before scrolling');

  await page.evaluate(() => window.scrollTo(0, 3800));
  check(await waitFor(page, () =>
            document.getElementById('below').src.endsWith('/img/below.gif')),
        'below-fold image loads after scrolling it into view');
  await page.close();
}

// --- 3. overrideAttributeFunctions ---------------------------------------
{
  const { page } = await openPage(testPage({
    tail: `<script data-pagespeed-no-defer>
             pagespeed.lazyLoadImages.overrideAttributeFunctions();
           <\/script>`,
  }));
  await page.waitForTimeout(300);
  const r = await page.evaluate(() => {
    const e = document.getElementById('below');
    return { attr: e.getAttribute('src'), prop: e.src,
             marked: e.getAttribute('data-pagespeed-lazy-replaced-functions') };
  });
  check(r.attr === 'http://test/img/below.gif',
        `override: getAttribute('src') reports the real url (got ${r.attr})`);
  check(r.prop === BLANK, 'override: .src still the blank pixel');
  check(r.marked === '1', 'override: replaced-functions marker set');
  await page.close();
}

// --- 4. loadAllImages ------------------------------------------------------
{
  const { page } = await openPage(testPage());
  await page.waitForTimeout(300);
  await page.evaluate(() => pagespeed.lazyLoadImages.loadAllImages());
  check(await waitFor(page, () =>
            document.getElementById('below').src.endsWith('/img/below.gif')),
        'loadAllImages: below-fold image loaded without scrolling');
  check(await page.evaluate(() =>
            document.querySelectorAll('[data-pagespeed-lazy-src]').length) === 0,
        'loadAllImages: no deferred images left');
  await page.close();
}

// --- 5. lazyLoadInit(true, ...) loads everything after onload -------------
{
  const { page } = await openPage(testPage({ afterOnload: true }));
  check(await waitFor(page, () =>
            document.getElementById('below').src.endsWith('/img/below.gif')),
        'after-onload mode: below-fold image loaded after window load');
  await page.close();
}

// --- 6. CriticalImages integration ----------------------------------------
{
  const { page } = await openPage(testPage({
    afterOnload: true,
    preamble: `<script>
      window.__checked = []; window.__critCalls = 0;
      window.pagespeed = { CriticalImages: {
        checkImageForCriticality: function(img) { __checked.push(img.id); },
        checkCriticalImages: function() { __critCalls++; },
      } };
    <\/script>`,
  }));
  check(await waitFor(page, () => window.__critCalls > 0),
        'CriticalImages: checkCriticalImages() called');
  await page.waitForTimeout(300);
  const r = await page.evaluate(() => ({ checked: __checked.slice().sort(),
                                         calls: __critCalls }));
  check(r.checked.includes('below'),
        `CriticalImages: lazily loaded below-fold image checked ` +
        `(got ${r.checked.join(',')})`);
  check(r.calls === 1, `CriticalImages: checkCriticalImages() once ` +
                       `(got ${r.calls})`);
  await page.close();
}

await browser.close();
console.log(failures ? `${failures} FAILURE(S)` : 'ALL PASS');
process.exit(failures ? 1 : 0);
