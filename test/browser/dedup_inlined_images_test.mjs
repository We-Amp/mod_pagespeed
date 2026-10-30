// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Browser integration tests for the dedup_inlined_images client JS.
//
// Drives the SHIPPED asset, the compiled bytes embedded in
// net/instaweb/rewriter/generated/dedup_inlined_images_opt.cc, in headless
// Chromium against markup shaped like DedupInlinedImagesFilter's output:
// the asset plus pagespeed.dedupInlinedImagesInit() before the first
// duplicate, the duplicate <img> stripped of its src, and an
// id="pagespeed_script_N" snippet calling
// pagespeed.dedupInlinedImages.inlineImg(fromId, toId, scriptId):
//
//   1. Each duplicate receives the first image's data URL and its snippet
//      script removes itself.
//   2. Author ids containing a quote (escaped by the filter as a JS string
//      literal) still resolve.
//   3. A snippet whose source image is missing is a no-op: the duplicate
//      keeps no src and the script stays in place.
//
// Run via run_browser_tests.sh (installs playwright + chromium on demand).

import { chromium } from 'playwright';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadCompiledAsset } from './lib.mjs';

const repoRoot = join(dirname(fileURLToPath(import.meta.url)), '..', '..');

const diiJs = loadCompiledAsset(
    join(repoRoot, 'net', 'instaweb', 'rewriter', 'generated',
         'dedup_inlined_images_opt.cc'));

const DATA_URL = 'data:image/gif;base64,' +
    'R0lGODlhAQABAIAAAP///wAAACH5BAEAAAAALAAAAAABAAEAAAICRAEAOw==';

const snippet = (from, to, script) =>
  `<script type="text/javascript" id="${script}" data-pagespeed-no-defer>` +
  `if(window.pagespeed&&pagespeed.dedupInlinedImages){` +
  `pagespeed.dedupInlinedImages.inlineImg('${from}','${to}','${script}');}` +
  `<\/script>`;

const testPage = `<!DOCTYPE html><html><body>
  <img id="pagespeed_img_h1" src="${DATA_URL}">
  <script type="text/javascript" data-pagespeed-no-defer>${diiJs}pagespeed.dedupInlinedImagesInit();<\/script>
  <img id="pagespeed_img_h2">${snippet('pagespeed_img_h1', 'pagespeed_img_h2', 'pagespeed_script_3')}
  <img id="pagespeed_img_h4">${snippet('pagespeed_img_h1', 'pagespeed_img_h4', 'pagespeed_script_5')}
  <img id="it's">${snippet('pagespeed_img_h1', "it\\'s", 'pagespeed_script_6')}
  <img id="orphan">${snippet('no_such_img', 'orphan', 'pagespeed_script_7')}
</body></html>`;

let failures = 0;
function check(ok, label) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${label}`);
  if (!ok) failures++;
}

const browser = await chromium.launch();
const page = await browser.newPage();
await page.route('http://test/**',
    (route) => route.fulfill({ contentType: 'text/html', body: testPage }));
await page.goto('http://test/');
await page.waitForLoadState('load');
const r = await page.evaluate(() => {
  const src = (id) => document.getElementById(id).getAttribute('src');
  const has = (id) => !!document.getElementById(id);
  return {
    h2: src('pagespeed_img_h2'), h4: src('pagespeed_img_h4'),
    quoted: src("it's"), orphan: src('orphan'),
    s3: has('pagespeed_script_3'), s5: has('pagespeed_script_5'),
    s6: has('pagespeed_script_6'), s7: has('pagespeed_script_7'),
    hasApi: typeof window.pagespeed.dedupInlinedImages.inlineImg,
  };
});
await browser.close();

check(r.hasApi === 'function', 'dedupInlinedImagesInit exposes inlineImg');
check(r.h2 === DATA_URL, 'first duplicate receives the inlined data URL');
check(r.h4 === DATA_URL, 'second duplicate receives the inlined data URL');
check(!r.s3 && !r.s5, 'snippet scripts remove themselves');
check(r.quoted === DATA_URL, 'escaped author id with a quote resolves');
check(!r.s6, 'escaped-id snippet removed itself');
check(r.orphan === null, 'missing source image: duplicate left without src');
check(r.s7, 'missing source image: snippet script left in place');

console.log(failures ? `${failures} FAILURE(S)` : 'ALL PASS');
process.exit(failures ? 1 : 0);
