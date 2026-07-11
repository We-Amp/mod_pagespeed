// Browser integration tests for the critical CSS beacon client JS.
//
// Drives the SHIPPED asset — the closure-compiled bytes embedded in
// net/instaweb/rewriter/generated/critical_css_beacon_opt.cc — in headless
// Chromium, and asserts on the actual beacon XHR the browser sends:
//
//   1. Viewport-aware criticality: only selectors with a match
//      above the fold are reported, with conservative keeps for
//      display:none / unmeasurable matches.
//   2. Overflow signaling: a payload that exceeds MAX_POST_SIZE
//      is truncated to fit and flagged with &of=1.
//
// Run via run_browser_tests.sh (installs playwright + chromium on demand).

import { chromium } from 'playwright';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const repoRoot = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const MAX_POST_SIZE = 131072;  // pagespeedutils.MAX_POST_SIZE, compiled in.

// --- Extract the compiled JS string from the generated data2c .cc file. ---
function loadCompiledAsset(ccPath) {
  const cc = readFileSync(ccPath, 'utf8');
  const start = cc.indexOf('=');
  const end = cc.lastIndexOf('";');
  if (start === -1 || end === -1 || end < start) {
    throw new Error(`unrecognized data2c format in ${ccPath}`);
  }
  const literals = cc.slice(start, end + 1).match(/"(?:[^"\\]|\\.)*"/g);
  if (!literals || literals.length === 0) {
    throw new Error(`no string literals found in ${ccPath}`);
  }
  const escaped = literals.map((l) => l.slice(1, -1)).join('');
  return escaped.replace(/\\(x[0-9a-fA-F]{2}|.)/g, (m, e) => {
    if (e[0] === 'x') return String.fromCharCode(parseInt(e.slice(1), 16));
    return { n: '\n', t: '\t', r: '\r' }[e] ?? e;
  });
}

const beaconJs = loadCompiledAsset(
    join(repoRoot, 'net', 'instaweb', 'rewriter', 'generated',
         'critical_css_beacon_opt.cc'));

// --- Test page: elements above, below, and outside the fold. -------------
function testPage(selectorsJson, extraBody = '') {
  return `<!DOCTYPE html><html><head><style>
    body { margin: 0; }
    #hero { height: 300px; }
    .mid { position: absolute; top: 500px; height: 50px; }
    .deep { position: absolute; top: 5000px; height: 50px; }
    #hidden { display: none; }
    .everywhere { height: 10px; }
  </style></head><body>
    <div id="hero">above the fold</div>
    <div class="mid">at 500px, inside a 768px viewport</div>
    <div class="deep">far below the fold</div>
    <div id="hidden">display:none menu</div>
    <div class="deep everywhere">below</div>
    <div class="everywhere" style="position:absolute; top: 100px;">above</div>
    ${extraBody}
    <script>${beaconJs}<\/script>
    <script>
      pagespeed.criticalCssBeaconInit('http://test/beacon', 'http://test/',
          'oh123', 'n456', ${selectorsJson});
    <\/script>
  </body></html>`;
}

async function runBeaconPage(browser, html) {
  const page = await browser.newPage({ viewport: { width: 1024, height: 768 } });
  let capturedBody = null;
  await page.route('http://test/**', (route) => {
    const req = route.request();
    if (req.url().startsWith('http://test/beacon')) {
      capturedBody = req.postData();
      return route.fulfill({ status: 204, body: '' });
    }
    return route.fulfill({ contentType: 'text/html', body: html });
  });
  await page.goto('http://test/');
  await page.waitForRequest((r) => r.url().startsWith('http://test/beacon'),
                            { timeout: 15000 });
  // criticalCssBeaconData is the exported copy of the payload.
  const exported = await page.evaluate('pagespeed.criticalCssBeaconData');
  await page.close();
  return { body: capturedBody, exported };
}

let failures = 0;
function check(ok, label) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${label}`);
  if (!ok) failures++;
}

const browser = await chromium.launch();

// --- 1. Viewport-aware criticality --------------------------------
{
  const selectors = ['#hero', '.mid', '.deep', '#hidden', '.everywhere', '.absent'];
  const { body, exported } = await runBeaconPage(browser, testPage(JSON.stringify(selectors)));
  check(body !== null, 'viewport: beacon POST captured');
  check(body === exported, 'viewport: criticalCssBeaconData matches POST body');
  const params = new URLSearchParams(body);
  check(params.get('oh') === 'oh123' && params.get('n') === 'n456',
        'viewport: oh/n params round-trip');
  const got = new Set((params.get('cs') || '').split(',').filter(Boolean)
      .map(decodeURIComponent));
  const expected = {
    '#hero': true,        // above the fold
    '.mid': true,         // top 500px < 768px viewport
    '.deep': false,       // all matches at 5000px
    '#hidden': true,      // zero-size rect: indeterminate, conservative keep
    '.everywhere': true,  // one match above the fold suffices
    '.absent': false,     // matches nothing
  };
  for (const [sel, want] of Object.entries(expected)) {
    check(got.has(sel) === want, `viewport: ${sel} critical=${want}`);
  }
  check(params.get('of') === null, 'viewport: no overflow flag on small payload');
}

// --- 2. Overflow truncation + of=1 ---------------------------------
{
  const longClass = 'c' + 'x'.repeat(100);
  // 2000 copies x ~101 encoded chars each is ~200KB of candidate payload,
  // well past MAX_POST_SIZE; every copy matches the above-fold element.
  const selectors = Array(2000).fill('.' + longClass);
  const html = testPage(JSON.stringify(selectors),
      `<div class="${longClass}" style="position:absolute; top:0;">x</div>`);
  const { body } = await runBeaconPage(browser, html);
  check(body !== null, 'overflow: beacon POST captured');
  check(body.endsWith('&of=1'), 'overflow: payload flagged with of=1');
  check(body.length <= MAX_POST_SIZE,
        `overflow: payload fits MAX_POST_SIZE (${body.length} <= ${MAX_POST_SIZE})`);
  const cs = new URLSearchParams(body).get('cs') || '';
  check(cs.length > 0, 'overflow: truncated payload still reports selectors');
}

await browser.close();
console.log(failures ? `${failures} FAILURE(S)` : 'ALL PASS');
process.exit(failures ? 1 : 0);
