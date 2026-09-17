// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Browser integration tests for the add_instrumentation CWV collector.
//
// Drives the SHIPPED asset — the closure-compiled bytes embedded in
// net/instaweb/rewriter/generated/add_instrumentation_opt.cc — in headless
// Chromium against a real local HTTP server (navigator.sendBeacon requests
// are not reliably interceptable via page.route), and asserts on the actual
// beacon the browser sends:
//
//   1. A single sendBeacon POST on page dismissal carrying LCP, CLS (in
//      milli-units), Navigation Timing L2 data, and ets=load:<ms>.
//   2. INP: a slow click handler is reflected in the inp= param.
//   3. Exactly-once: repeated hidden/visible cycles yield one beacon.
//   4. Image-GET fallback when navigator.sendBeacon is unavailable, with
//      the same params in the query string.
//
// Run via run_browser_tests.sh (installs playwright + chromium on demand).

import { chromium } from 'playwright';
import { createServer } from 'node:http';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadCompiledAsset } from './lib.mjs';

const repoRoot = join(dirname(fileURLToPath(import.meta.url)), '..', '..');

const beaconJs = loadCompiledAsset(
    join(repoRoot, 'net', 'instaweb', 'rewriter', 'generated',
         'add_instrumentation_opt.cc'));

// --- Local HTTP server: serves the test page, collects beacons. ----------
const beacons = [];
let pageHtml = '';
const server = createServer((req, res) => {
  const url = new URL(req.url, `http://${req.headers.host}`);
  if (url.pathname === '/beacon') {
    let body = '';
    req.on('data', (chunk) => { body += chunk; });
    req.on('end', () => {
      beacons.push({ method: req.method, query: url.search.slice(1), body });
      res.writeHead(204);
      res.end();
    });
    return;
  }
  res.writeHead(200, { 'Content-Type': 'text/html' });
  res.end(url.pathname === '/' ? pageHtml
                               : '<!DOCTYPE html><html><body>away</body></html>');
});
await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
const origin = `http://127.0.0.1:${server.address().port}`;

// --- Test page: head timing script, LCP text, optional layout shift. -----
function testPage({ extraBody = '', shift = true, extraParams = '' } = {}) {
  const shiftScript = shift ? `<script>
      // Grow the spacer after load: shifts #main down without recent input,
      // producing a layout-shift entry.
      addEventListener('load', function() {
        setTimeout(function() {
          document.getElementById('spacer').style.height = '260px';
        }, 50);
      });
    <\/script>` : '';
  return `<!DOCTYPE html><html><head>
    <script>window.mod_pagespeed_start = Number(new Date());<\/script>
    <style>
      body { margin: 0; }
      #spacer { height: 60px; }
      #main { font-size: 24px; }
    </style>
  </head><body>
    <div id="spacer"></div>
    <div id="main">Largest contentful text block on this test page, big
      enough to produce a largest-contentful-paint entry.</div>
    ${extraBody}
    ${shiftScript}
    <script>${beaconJs}<\/script>
    <script>
      pagespeed.addInstrumentationInit('/beacon', '${extraParams}',
                                       location.href);
    <\/script>
  </body></html>`;
}

// addInitScript source: lets tests drive document.visibilityState and the
// visibilitychange event deterministically without tearing the page down.
const visibilityShim = `(() => {
  let state = 'visible';
  Object.defineProperty(Document.prototype, 'visibilityState', {
    configurable: true,
    get: () => state,
  });
  window.__forceVisibility = (next) => {
    state = next;
    document.dispatchEvent(new Event('visibilitychange'));
  };
})();`;

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function waitForBeacons(count, timeoutMs = 10000) {
  const deadline = Date.now() + timeoutMs;
  while (beacons.length < count && Date.now() < deadline) {
    await sleep(50);
  }
  return beacons.length;
}

let failures = 0;
function check(ok, label) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${label}`);
  if (!ok) failures++;
}

const browser = await chromium.launch();

// --- 1. CWV POST beacon on real page dismissal ---------------------------
{
  beacons.length = 0;
  pageHtml = testPage();
  const page = await browser.newPage(
      { viewport: { width: 1024, height: 768 } });
  await page.goto(origin + '/');
  await page.waitForLoadState('load');
  await sleep(400);  // let the layout shift happen and LCP settle
  await page.goto(origin + '/away');  // pagehide + visibilitychange
  await waitForBeacons(1);
  await sleep(250);  // room for an (incorrect) second beacon to show up
  check(beacons.length === 1, `dismissal: exactly one beacon (got ${beacons.length})`);
  const b = beacons[0];
  check(b !== undefined, 'dismissal: beacon captured');
  if (b) {
    check(b.method === 'POST', `dismissal: sent as POST (got ${b.method})`);
    const query = new URLSearchParams(b.query);
    check(query.get('url') === origin + '/',
          'dismissal: url= carried in the beacon URL query');
    check(b.body.length > 0, 'dismissal: POST body is non-empty');
    const params = new URLSearchParams(b.body);
    const ets = params.get('ets') || '';
    check(/^load:\d+$/.test(ets) && parseInt(ets.slice(5), 10) > 0,
          `dismissal: ets=load:<ms> present (got '${ets}')`);
    check(parseInt(params.get('rload'), 10) > 0, 'dismissal: rload > 0');
    check(parseInt(params.get('lcp'), 10) > 0,
          `dismissal: lcp > 0 (got '${params.get('lcp')}')`);
    check(parseInt(params.get('cls'), 10) > 0,
          `dismissal: cls milli-units > 0 (got '${params.get('cls')}')`);
    check(parseInt(params.get('c_ttfb'), 10) >= 0,
          'dismissal: c_ttfb present');
    check(params.get('ttfb') === null, 'dismissal: legacy ttfb not sent');
    check(params.get('dpr') !== null, 'dismissal: dpr present');
    check(params.get('nt') === 'navigate', 'dismissal: nt=navigate');
  }
  await page.close();
}

// --- 2. INP: slow click handler shows up in inp= -------------------------
{
  beacons.length = 0;
  pageHtml = testPage({ extraBody: `<button id="btn"
      onclick="var t = Date.now(); while (Date.now() - t < 120) {}"
      >slow<\/button>` });
  const page = await browser.newPage(
      { viewport: { width: 1024, height: 768 } });
  await page.addInitScript(visibilityShim);
  await page.goto(origin + '/');
  await page.waitForLoadState('load');
  await page.click('#btn');
  await sleep(500);  // event-timing entries arrive after presentation
  await page.evaluate("__forceVisibility('hidden')");
  await waitForBeacons(1);
  check(beacons.length === 1, `inp: exactly one beacon (got ${beacons.length})`);
  if (beacons.length === 1) {
    const params = new URLSearchParams(beacons[0].body);
    const inp = parseInt(params.get('inp'), 10);
    check(inp >= 100, `inp: >= 100ms for a 120ms-blocking click (got ${inp})`);
  }
  await page.close();
}

// --- 3. Exactly-once across repeated visibility flips --------------------
{
  beacons.length = 0;
  pageHtml = testPage();
  const page = await browser.newPage(
      { viewport: { width: 1024, height: 768 } });
  await page.addInitScript(visibilityShim);
  await page.goto(origin + '/');
  await page.waitForLoadState('load');
  await sleep(200);
  await page.evaluate("__forceVisibility('hidden')");
  await page.evaluate("__forceVisibility('visible')");
  await page.evaluate("__forceVisibility('hidden')");
  await page.goto(origin + '/away');  // and a real pagehide on top
  await waitForBeacons(1);
  await sleep(250);
  check(beacons.length === 1,
        `exactly-once: one beacon after hide/show/hide/navigate (got ${beacons.length})`);
  await page.close();
}

// --- 4. Image-GET fallback without navigator.sendBeacon ------------------
{
  beacons.length = 0;
  pageHtml = testPage();
  const page = await browser.newPage(
      { viewport: { width: 1024, height: 768 } });
  await page.addInitScript(visibilityShim);
  await page.addInitScript(`Object.defineProperty(Navigator.prototype,
      'sendBeacon', { configurable: true, value: undefined });`);
  await page.goto(origin + '/');
  await page.waitForLoadState('load');
  await sleep(400);
  await page.evaluate("__forceVisibility('hidden')");
  await waitForBeacons(1);
  check(beacons.length === 1,
        `fallback: exactly one beacon (got ${beacons.length})`);
  if (beacons.length === 1) {
    const b = beacons[0];
    check(b.method === 'GET', `fallback: sent as image GET (got ${b.method})`);
    const params = new URLSearchParams(b.query);
    check(params.get('url') === origin + '/', 'fallback: url= in query');
    check(parseInt(params.get('lcp'), 10) > 0, 'fallback: lcp in query');
    check(parseInt(params.get('cls'), 10) > 0, 'fallback: cls in query');
    check(/^load:\d+$/.test(params.get('ets') || ''),
          'fallback: ets=load:<ms> in query');
  }
  await page.close();
}

// --- 5. Tier-2: no PerformanceObserver → nav-timing-only beacon ----------
{
  beacons.length = 0;
  pageHtml = testPage();
  const page = await browser.newPage(
      { viewport: { width: 1024, height: 768 } });
  await page.addInitScript(visibilityShim);
  await page.addInitScript('delete window.PerformanceObserver;');
  await page.goto(origin + '/');
  await page.waitForLoadState('load');
  await sleep(300);
  await page.evaluate("__forceVisibility('hidden')");
  await waitForBeacons(1);
  check(beacons.length === 1,
        `tier2: exactly one beacon (got ${beacons.length})`);
  if (beacons.length === 1) {
    const params = new URLSearchParams(beacons[0].body);
    check(/^load:\d+$/.test(params.get('ets') || ''), 'tier2: ets present');
    check(parseInt(params.get('c_ttfb'), 10) >= 0, 'tier2: c_ttfb present');
    check(params.get('lcp') === null, 'tier2: no lcp');
    check(params.get('cls') === null, 'tier2: no cls');
    check(params.get('inp') === null, 'tier2: no inp');
  }
  await page.close();
}

// --- 6. Tier-3: no PO, no nav entry → bare ets=load: beacon --------------
{
  beacons.length = 0;
  pageHtml = testPage();
  const page = await browser.newPage(
      { viewport: { width: 1024, height: 768 } });
  await page.addInitScript(visibilityShim);
  await page.addInitScript(`delete window.PerformanceObserver;
      performance.getEntriesByType = function() { return []; };`);
  await page.goto(origin + '/');
  await page.waitForLoadState('load');
  await sleep(300);
  await page.evaluate("__forceVisibility('hidden')");
  await waitForBeacons(1);
  check(beacons.length === 1,
        `tier3: exactly one beacon (got ${beacons.length})`);
  if (beacons.length === 1) {
    const params = new URLSearchParams(beacons[0].body);
    check(/^load:\d+$/.test(params.get('ets') || ''),
          'tier3: bare ets=load: from mod_pagespeed_start');
    check(params.get('nav') === null && params.get('c_ttfb') === null,
          'tier3: no nav-timing params');
    check(params.get('lcp') === null && params.get('cls') === null &&
              params.get('inp') === null,
          'tier3: no CWV params');
    check(params.get('dpr') !== null, 'tier3: body still non-empty (dpr)');
  }
  await page.close();
}

// --- 7. extraParams passthrough ------------------------------------------
{
  beacons.length = 0;
  pageHtml = testPage({ extraParams: '&exptid=2' });
  const page = await browser.newPage(
      { viewport: { width: 1024, height: 768 } });
  await page.addInitScript(visibilityShim);
  await page.goto(origin + '/');
  await page.waitForLoadState('load');
  await sleep(300);
  await page.evaluate("__forceVisibility('hidden')");
  await waitForBeacons(1);
  check(beacons.length === 1,
        `extraParams: exactly one beacon (got ${beacons.length})`);
  if (beacons.length === 1) {
    const params = new URLSearchParams(beacons[0].body);
    check(params.get('exptid') === '2',
          'extraParams: &exptid=2 passed through to the POST body');
  }
  await page.close();
}

// --- 8. No layout shift → cls=0 still reported ---------------------------
{
  beacons.length = 0;
  pageHtml = testPage({ shift: false });
  const page = await browser.newPage(
      { viewport: { width: 1024, height: 768 } });
  await page.addInitScript(visibilityShim);
  await page.goto(origin + '/');
  await page.waitForLoadState('load');
  await sleep(300);
  await page.evaluate("__forceVisibility('hidden')");
  await waitForBeacons(1);
  check(beacons.length === 1,
        `cls0: exactly one beacon (got ${beacons.length})`);
  if (beacons.length === 1) {
    const params = new URLSearchParams(beacons[0].body);
    check(params.get('cls') === '0',
          `cls0: shift-free page reports cls=0 (got '${params.get('cls')}')`);
    check(parseInt(params.get('lcp'), 10) > 0, 'cls0: lcp still measured');
  }
  await page.close();
}

// --- 9. bfcache restore re-arms measurement -------------------------------
// A page hidden with sent_ latched and its observers disconnected must, on a
// pageshow with persisted=true (back/forward-cache restore), treat the
// restore as a new page view: re-initialize the observers with fresh
// accumulation state and beacon again on the next hide.
{
  beacons.length = 0;
  pageHtml = testPage({ extraBody: `<button id="btn"
      onclick="var t = Date.now(); while (Date.now() - t < 120) {}"
      >slow<\/button>` });
  const page = await browser.newPage(
      { viewport: { width: 1024, height: 768 } });
  await page.addInitScript(visibilityShim);
  await page.goto(origin + '/');
  await page.waitForLoadState('load');
  await sleep(400);  // let the initial layout shift and LCP settle
  await page.evaluate("__forceVisibility('hidden')");
  await waitForBeacons(1);
  check(beacons.length === 1,
        `bfcache: first hide beacons once (got ${beacons.length})`);

  // A plain (non-persisted) pageshow must NOT re-arm: the exactly-once
  // latch still holds, so another hide sends nothing.
  await page.evaluate("__forceVisibility('visible')");
  await page.evaluate(
      "window.dispatchEvent(new PageTransitionEvent('pageshow'))");
  await page.evaluate("__forceVisibility('hidden')");
  await sleep(600);  // room for an (incorrect) second beacon to show up
  check(beacons.length === 1,
        `bfcache: non-persisted pageshow stays latched (got ${beacons.length})`);

  // Simulate a back/forward-cache restore, then produce fresh activity: a
  // new layout shift and a slow click in the restored visit.
  await page.evaluate("__forceVisibility('visible')");
  await page.evaluate(
      "window.dispatchEvent(new PageTransitionEvent('pageshow', " +
      '{ persisted: true }))');
  await page.evaluate(
      "document.getElementById('spacer').style.height = '520px'");
  await page.click('#btn');
  await sleep(500);  // event-timing entries arrive after presentation
  await page.evaluate("__forceVisibility('hidden')");
  await waitForBeacons(2);
  await sleep(250);
  check(beacons.length === 2,
        'bfcache: restored visit beacons again on next hide ' +
        `(got ${beacons.length})`);
  if (beacons.length === 2) {
    const params = new URLSearchParams(beacons[1].body);
    // lcp/cls can arrive via buffered redelivery of pre-freeze entries
    // (documented on rearm_); they prove the observers were re-installed,
    // not that the values are fresh. inp is strictly fresh: no interaction
    // occurred before the first hide, so nothing buffered can fake it.
    check(parseInt(params.get('lcp'), 10) > 0,
          `bfcache: restored visit reports lcp (got '${params.get('lcp')}')`);
    check(parseInt(params.get('cls'), 10) > 0,
          'bfcache: restored visit re-arms cls observer ' +
          `(got '${params.get('cls')}')`);
    const inp = parseInt(params.get('inp'), 10);
    check(inp >= 100,
          `bfcache: restored visit reports fresh inp (got ${inp})`);
  }
  await page.close();
}

await browser.close();
server.close();
console.log(failures ? `${failures} FAILURE(S)` : 'ALL PASS');
process.exit(failures ? 1 : 0);
