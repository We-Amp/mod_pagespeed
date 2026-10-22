// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Browser integration tests for the deterministic client JS.
//
// Drives the SHIPPED assets, the compiled bytes embedded in
// net/instaweb/rewriter/generated/deterministic_opt.cc and its debug twin
// deterministic.cc, in headless Chromium as the first script in the head,
// where DeterministicJsFilter inserts it:
//
//   1. Math.random() returns a fixed value that steps by 0.1 every 25 calls.
//   2. new Date() returns a fixed time that steps by 50 ms every 25 calls,
//      and Date.now() follows the same clock.
//   3. new Date(x) and new Date(y, m, ...) still construct real dates, and
//      calling Date() without new returns a string.
//   4. KNOWN-QUIRK: new Date() instanceof Date is false (see below).
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
  ['opt', gen('deterministic_opt.cc')],
  ['dbg', gen('deterministic.cc')],
];

const testPage = (js) => `<!DOCTYPE html><html><head>
  <script data-pagespeed-no-defer>${js}<\/script>
</head><body>
  <script>
    var randoms = [];
    for (var i = 0; i < 27; i++) randoms.push(Math.random());
    var times = [];
    for (var j = 0; j < 26; j++) times.push(new Date().getTime());
    window.__result = {
      randoms: randoms,
      times: times,
      now: Date.now(),
      fromMs: new Date(86400000).getTime(),
      fromParts: new Date(2020, 1, 3, 4, 5, 6, 7).getDate(),
      isDate: new Date() instanceof Date,
      realDate: Object.prototype.toString.call(new Date()),
      called: typeof Date(),
    };
  <\/script>
</body></html>`;

let failures = 0;
function check(ok, label) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${label}`);
  if (!ok) failures++;
}

const T0 = 1204251968254;
const browser = await chromium.launch();

for (const [name, js] of variants) {
  const page = await browser.newPage();
  await page.route('**/*', (route) => route.fulfill(
      { contentType: 'text/html', body: testPage(js) }));
  await page.goto('http://test/');
  await page.waitForLoadState('load');
  const r = await page.evaluate(() => window.__result);
  await page.close();

  check(r.randoms.slice(0, 25).every((v) => v === 0.462),
        `${name}: first 25 Math.random() calls return 0.462`);
  check(r.randoms[25] === (0.462 + 0.1) % 1 &&
            r.randoms[26] === (0.462 + 0.1) % 1,
        `${name}: call 26 steps the seed by 0.1 (got ${r.randoms[25]})`);
  check(r.times.slice(0, 25).every((v) => v === T0),
        `${name}: first 25 new Date() calls return the fixed time`);
  check(r.times[25] === T0 + 50,
        `${name}: call 26 steps the clock by 50 ms (got ${r.times[25]})`);
  check(r.now === T0 + 50, `${name}: Date.now() follows the fixed clock`);
  check(r.fromMs === 86400000, `${name}: new Date(ms) is a real date`);
  check(r.fromParts === 3, `${name}: new Date(y, m, d) is a real date`);
  check(r.realDate === '[object Date]',
        `${name}: new Date() returns a real Date object`);
  // KNOWN-QUIRK: the replacement constructor returns instances of the
  // original Date, whose prototype is not the replacement's, so
  // `new Date() instanceof Date` is false while the asset is loaded. Pinned
  // as today's behaviour so a rewrite neither breaks nor silently changes it.
  check(r.isDate === false,
        `${name}: KNOWN-QUIRK new Date() instanceof Date is false`);
  check(r.called === 'string', `${name}: Date() without new returns a string`);
}

await browser.close();
console.log(failures ? `${failures} FAILURE(S)` : 'ALL PASS');
process.exit(failures ? 1 : 0);
