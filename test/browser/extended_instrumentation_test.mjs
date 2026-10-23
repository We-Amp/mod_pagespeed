// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Browser integration tests for the extended_instrumentation client JS.
//
// Drives the SHIPPED assets, the compiled bytes embedded in
// net/instaweb/rewriter/generated/extended_instrumentation_opt.cc and its
// debug twin extended_instrumentation.cc, in headless Chromium. With
// EnableExtendedInstrumentation on, AddInstrumentationFilter emits this
// asset directly in front of add_instrumentation, whose beacon appends
// pagespeed.getResourceTimingData():
//
//   1. getResourceTimingData() reports the resource timing aggregates and
//      per-initiator counts for the page's subresources.
//   2. It returns '' when the Performance API is unavailable.
//   3. Concatenated with add_instrumentation_opt as the filter emits them,
//      the aggregates reach the beacon body.
//
// Run via run_browser_tests.sh (installs playwright + chromium on demand).

import { chromium } from 'playwright';
import { createServer } from 'node:http';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadCompiledAsset } from './lib.mjs';

const repoRoot = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const gen = (f) => loadCompiledAsset(
    join(repoRoot, 'net', 'instaweb', 'rewriter', 'generated', f));

const variants = [
  ['opt', gen('extended_instrumentation_opt.cc')],
  ['dbg', gen('extended_instrumentation.cc')],
];
const addInstrumentationJs = gen('add_instrumentation_opt.cc');

const GIF = Buffer.from(
    'R0lGODlhAQABAIAAAP///wAAACH5BAEAAAAALAAAAAABAAEAAAICRAEAOw==', 'base64');

// A real server: resource timing is the subject under test.
const beacons = [];
let pageHtml = '';
const server = createServer((req, res) => {
  const url = new URL(req.url, `http://${req.headers.host}`);
  if (url.pathname === '/beacon') {
    let body = '';
    req.on('data', (chunk) => { body += chunk; });
    req.on('end', () => {
      beacons.push(body);
      res.writeHead(204);
      res.end();
    });
    return;
  }
  if (url.pathname.endsWith('.gif')) {
    res.writeHead(200, { 'Content-Type': 'image/gif' });
    res.end(GIF);
    return;
  }
  if (url.pathname.endsWith('.css')) {
    res.writeHead(200, { 'Content-Type': 'text/css' });
    res.end('p { color: black; }');
    return;
  }
  res.writeHead(200, { 'Content-Type': 'text/html' });
  res.end(pageHtml);
});
await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
const origin = `http://127.0.0.1:${server.address().port}`;

const testPage = (js, extra = '') => `<!DOCTYPE html><html><head>
  <link rel="stylesheet" href="/s.css">
</head><body>
  <img src="/a.gif"><img src="/b.gif">
  <script>${js}${extra}<\/script>
</body></html>`;

let failures = 0;
function check(ok, label) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${label}`);
  if (!ok) failures++;
}

const AGGREGATES = /^&afd=\d+&nfd=(\d+)&mfd=\d+&act=\d+&nct=\d+&adt=\d+&ndt=\d+&abt=\d+&nbt=\d+&attfb=\d+&nttfb=\d+/;
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const browser = await chromium.launch();

for (const [name, js] of variants) {
  {
    pageHtml = testPage(js);
    const page = await browser.newPage();
    await page.goto(origin + '/');
    await page.waitForLoadState('load');
    const data = await page.evaluate(
        () => window.pagespeed.getResourceTimingData());
    await page.close();
    const m = AGGREGATES.exec(data);
    check(m !== null, `${name}: aggregates present (got '${data}')`);
    check(m && parseInt(m[1], 10) >= 3,
          `${name}: nfd counts the fetched subresources`);
    check(/&rit_img=2(&|$)/.test(data), `${name}: rit_img=2`);
    check(/&rit_link=1(&|$)/.test(data), `${name}: rit_link=1`);
  }

  {
    pageHtml = testPage(js);
    const page = await browser.newPage();
    await page.addInitScript(
        'Object.defineProperty(window, "performance", { value: undefined });');
    await page.goto(origin + '/');
    await page.waitForLoadState('load');
    const data = await page.evaluate(
        () => window.pagespeed.getResourceTimingData());
    await page.close();
    check(data === '', `${name}: no Performance API yields '' (got '${data}')`);
  }

  {
    beacons.length = 0;
    pageHtml = testPage(js + addInstrumentationJs,
        "pagespeed.addInstrumentationInit('/beacon', '', location.href);");
    const page = await browser.newPage();
    await page.goto(origin + '/');
    await page.waitForLoadState('load');
    await sleep(200);
    await page.goto(origin + '/away');
    const deadline = Date.now() + 10000;
    while (beacons.length < 1 && Date.now() < deadline) await sleep(50);
    await page.close();
    const body = beacons[0] || '';
    check(/&afd=\d+&nfd=\d+/.test(body) && /&rit_img=2/.test(body),
          `${name}: aggregates reach the add_instrumentation beacon`);
  }
}

await browser.close();
server.close();
console.log(failures ? `${failures} FAILURE(S)` : 'ALL PASS');
process.exit(failures ? 1 : 0);
