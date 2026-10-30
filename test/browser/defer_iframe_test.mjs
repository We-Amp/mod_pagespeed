// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Browser integration tests for the defer_iframe client JS.
//
// Drives the SHIPPED assets, the compiled bytes embedded in
// net/instaweb/rewriter/generated/defer_iframe_opt.cc and its debug twin
// defer_iframe.cc, in headless Chromium against markup shaped like
// DeferIframeFilter's output: the asset plus pagespeed.deferIframeInit()
// before the first iframe, every <iframe> renamed to <pagespeed_iframe>,
// and a pagespeed.deferIframe.convertToIframe() script as the last child of
// each renamed element:
//
//   1. Every <pagespeed_iframe> is turned back into an <iframe>, in
//      document order, with all of its attributes copied.
//   2. The converted iframes load their src.
//   3. convertToIframe() with nothing left to convert is a no-op.
//
// Run via run_browser_tests.sh (installs playwright + chromium on demand).

import { chromium } from 'playwright';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadCompiledAsset } from './lib.mjs';

const repoRoot = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const gen = (f) => loadCompiledAsset(
    join(repoRoot, 'net', 'instaweb', 'rewriter', 'generated', f));

const variants = [
  ['opt', gen('defer_iframe_opt.cc')],
  ['dbg', gen('defer_iframe.cc')],
];

// Mirrors DeferIframeFilter::kDeferIframeIframeJs.
const convert = '<script type="text/javascript">\n' +
    'if(window.pagespeed&&pagespeed.deferIframe){' +
    'pagespeed.deferIframe.convertToIframe();}<\/script>';

const testPage = (js) => `<!DOCTYPE html><html><body>
  <script>${js}pagespeed.deferIframeInit();<\/script>
  <pagespeed_iframe id="f1" src="/frame1" width="120" data-x="a&quot;b">${convert}</pagespeed_iframe>
  <p>between</p>
  <pagespeed_iframe id="f2" src="/frame2" title="second">${convert}</pagespeed_iframe>
</body></html>`;

let failures = 0;
function check(ok, label) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${label}`);
  if (!ok) failures++;
}

const browser = await chromium.launch();

for (const [name, js] of variants) {
  const page = await browser.newPage();
  await page.route('**/*', (route) => {
    const url = route.request().url();
    route.fulfill({
      contentType: 'text/html',
      body: url === 'http://test/' ? testPage(js)
                                   : `<!DOCTYPE html><p>${url}</p>`,
    });
  });
  await page.goto('http://test/');
  await page.waitForLoadState('load');
  const r = await page.evaluate(() => {
    const iframes = Array.from(document.getElementsByTagName('iframe'));
    const before = document.getElementsByTagName('pagespeed_iframe').length;
    window.pagespeed.deferIframe.convertToIframe();
    return {
      left: before,
      leftAfterExtra: document.getElementsByTagName('iframe').length,
      ids: iframes.map((f) => f.id),
      f1: iframes[0] && {
        src: iframes[0].getAttribute('src'),
        width: iframes[0].getAttribute('width'),
        x: iframes[0].getAttribute('data-x'),
      },
      f2title: iframes[1] && iframes[1].getAttribute('title'),
      loaded: iframes.map((f) => f.contentDocument &&
          f.contentDocument.body && f.contentDocument.body.textContent),
    };
  });
  await page.close();

  check(r.left === 0, `${name}: no pagespeed_iframe left (got ${r.left})`);
  check(r.ids.join(',') === 'f1,f2',
        `${name}: both converted in document order (got ${r.ids})`);
  check(r.f1 && r.f1.src === '/frame1' && r.f1.width === '120' &&
            r.f1.x === 'a"b',
        `${name}: attributes copied (got ${JSON.stringify(r.f1)})`);
  check(r.f2title === 'second', `${name}: second iframe keeps its title`);
  check(r.loaded[0] === 'http://test/frame1' &&
            r.loaded[1] === 'http://test/frame2',
        `${name}: converted iframes load their src (got ${r.loaded})`);
  check(r.leftAfterExtra === 2,
        `${name}: extra convertToIframe() is a no-op`);
}

await browser.close();
console.log(failures ? `${failures} FAILURE(S)` : 'ALL PASS');
process.exit(failures ? 1 : 0);
