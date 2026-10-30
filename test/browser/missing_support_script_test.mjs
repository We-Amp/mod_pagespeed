// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Browser test: the inline onload/onerror handlers that the image filters
// inject must not throw when the support script is missing.
//
// The handler strings are read from the filter sources (kImageOnloadCode in
// critical_images_beacon_filter.cc, delay_images_filter.cc and
// lazyload_images_filter.cc), so the test cannot drift from the shipped
// code. Each page carries <img> elements with those handlers while the
// external support script request is aborted (as a blocker, a CDN outage or
// a script-deferring tool would), so `pagespeed` is never defined.
//
//   1. Control: an unguarded handler does raise a ReferenceError here, so the
//      harness would notice the bug this test guards against.
//   2. Guarded handlers: images that load and images that fail to load raise
//      no page error and no console error.
//   3. When the support script is present, the guarded handlers still call it.
//
// Run via run_browser_tests.sh (installs playwright + chromium on demand).

import { chromium } from 'playwright';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const repoRoot = join(dirname(fileURLToPath(import.meta.url)), '..', '..');

// Pull the kImageOnloadCode string literal(s) out of a filter source file.
function onloadCode(file) {
  const src = readFileSync(
      join(repoRoot, 'net', 'instaweb', 'rewriter', file), 'utf8');
  const m = src.match(/kImageOnloadCode(?:\[\])?\s*=((?:\s*"(?:[^"\\]|\\.)*")+);/);
  if (!m) throw new Error(`kImageOnloadCode not found in ${file}`);
  return m[1].match(/"(?:[^"\\]|\\.)*"/g)
      .map((l) => l.slice(1, -1)).join('');
}

const HANDLERS = {
  critical: { code: onloadCode('critical_images_beacon_filter.cc'),
              global: 'CriticalImages', method: 'checkImageForCriticality' },
  delay: { code: onloadCode('delay_images_filter.cc'),
           global: null, method: 'switchToHighResAndMaybeBeacon' },
  lazy: { code: onloadCode('lazyload_images_filter.cc'),
          global: 'lazyLoadImages', method: 'loadIfVisibleAndMaybeBeacon' },
};

// 1x1 transparent GIF.
const GIF = Buffer.from(
    'R0lGODlhAQABAIAAAP///wAAACH5BAEAAAAALAAAAAABAAEAAAICRAEAOw==',
    'base64');

let failures = 0;
function check(ok, label) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${label}`);
  if (!ok) failures++;
}

const browser = await chromium.launch();

// Opens `html` with the support script request aborted (or, when
// `supportJs` is given, served). Returns the page and the errors seen.
async function openPage(html, supportJs = null) {
  const page = await browser.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(String(e.message || e)));
  page.on('console', (m) => {
    if (m.type() === 'error' && /pagespeed/i.test(m.text())) {
      errors.push(m.text());
    }
  });
  await page.route('http://test/**', (route) => {
    const url = route.request().url();
    if (url.endsWith('/psajs/support.js')) {
      return supportJs === null
          ? route.abort()
          : route.fulfill({ contentType: 'text/javascript', body: supportJs });
    }
    if (url.includes('/img/ok')) {
      return route.fulfill({ contentType: 'image/gif', body: GIF });
    }
    if (url.includes('/img/bad')) {
      return route.fulfill({ status: 404, body: 'no' });
    }
    return route.fulfill({ contentType: 'text/html', body: html });
  });
  await page.goto('http://test/');
  await page.waitForLoadState('load');
  // Give the image load/error events time to run their handlers.
  await page.waitForTimeout(500);
  return { page, errors };
}

function pageWith(handlerCode, extraHead = '') {
  return `<!DOCTYPE html><html><head>${extraHead}
    <script src="http://test/psajs/support.js"><\/script></head><body>
    <img id="ok" src="http://test/img/ok.gif" onload="${handlerCode}"
         onerror="this.onerror=null;${handlerCode}">
    <img id="bad" src="http://test/img/bad.gif" onload="${handlerCode}"
         onerror="this.onerror=null;${handlerCode}">
  </body></html>`.replace(/&/g, '&amp;');
}

// --- 1. Control: an unguarded handler throws when the script is missing ----
{
  const { page, errors } = await openPage(
      pageWith('pagespeed.CriticalImages.checkImageForCriticality(this);'));
  check(errors.some((e) => /pagespeed/.test(e)),
        `control: unguarded handler raises a ReferenceError (${errors.length} errors)`);
  await page.close();
}

// --- 2. Guarded handlers stay silent with the support script blocked -------
for (const [name, h] of Object.entries(HANDLERS)) {
  const { page, errors } = await openPage(pageWith(h.code));
  const loaded = await page.evaluate(() =>
      document.getElementById('ok').complete &&
      document.getElementById('ok').naturalWidth === 1);
  check(loaded, `${name}: image loaded (the onload handler had a chance to run)`);
  check(errors.length === 0,
        `${name}: no error with the support script blocked (got ${JSON.stringify(errors)})`);
  await page.close();
}

// --- 3. With the support script present the guarded handlers still call it -
for (const [name, h] of Object.entries(HANDLERS)) {
  const support = h.global
      ? `window.__calls = []; window.pagespeed = { ${h.global}: { ${h.method}:
           function(img) { __calls.push(img.id); } } };`
      : `window.__calls = []; window.pagespeed = { ${h.method}:
           function(img) { __calls.push(img.id); } };`;
  const { page, errors } = await openPage(pageWith(h.code), support);
  const calls = await page.evaluate(() => window.__calls);
  check(errors.length === 0, `${name}: no error with the support script present`);
  check(calls.includes('ok') && calls.includes('bad'),
        `${name}: handler reaches the support script for load and error (got ${JSON.stringify(calls)})`);
  await page.close();
}

await browser.close();
console.log(failures ? `${failures} FAILURE(S)` : 'ALL PASS');
process.exit(failures ? 1 : 0);
