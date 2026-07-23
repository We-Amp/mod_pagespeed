// Browser integration tests for the critical images beacon client JS.
//
// Drives the SHIPPED asset — the closure-compiled bytes embedded in
// net/instaweb/rewriter/generated/critical_images_beacon_opt.cc — in headless
// Chromium, and asserts on the actual beacon XHR the browser sends:
//
//   1. A srcset-only image reports the hash of the candidate the browser
//      actually selected, matched positionally (via currentSrc) against the
//      stamped data-pagespeed-srcset-url-hashes list. The srcset here carries
//      image-rewritten candidate URLs while the stamped hashes key on the
//      originals (as when rewrite_images has rewritten srcset), so the test
//      fails if the JS ever hashes currentSrc directly instead of using the
//      positional stamped hash.
//   2. A mixed <img src srcset> image stays keyed on the stamped
//      data-pagespeed-url-hash (src hash) even when the browser selects a
//      srcset candidate: fetchpriority is element-level, so src-keyed
//      criticality already raises the right fetch.
//   3. A srcset-only image below the fold is not reported.
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

// HashString<CasePreserve, unsigned int> from pagespeed/kernel/base/
// string_hash.h: h = h * 131 + c over the URL bytes, 32-bit wrap.
function urlHash(str) {
  let h = 0;
  for (const b of Buffer.from(str, 'utf8')) {
    h = (Math.imul(h, 131) + b) >>> 0;
  }
  return String(h);
}

const SMALL = 'http://test/img/small.jpg';
const LARGE = 'http://test/img/large.jpg';
// The srcset-only image's srcset carries image-rewritten candidate URLs
// while the stamped hashes key on the original URLs.
const SMALL_RW = 'http://test/img/small.jpg.pagespeed.ic.111.webp';
const LARGE_RW = 'http://test/img/large.jpg.pagespeed.ic.222.webp';
const SRC = 'http://test/img/src.jpg';
const SRC2X = 'http://test/img/src2x.jpg';
const DEEP = 'http://test/img/deep.jpg';

const H_SMALL = urlHash(SMALL);
const H_LARGE = urlHash(LARGE);
const H_LARGE_RW = urlHash(LARGE_RW);
const H_SRC = urlHash(SRC);
const H_SRC2X = urlHash(SRC2X);
const H_DEEP = urlHash(DEEP);

// 1x1 transparent GIF.
const GIF = Buffer.from(
    'R0lGODlhAQABAIAAAP///wAAACH5BAEAAAAALAAAAAABAAEAAAICRAEAOw==',
    'base64');

function testPage() {
  return `<!DOCTYPE html><html><head></head><body>
    <img id="srconly" srcset="${SMALL_RW} 480w, ${LARGE_RW} 1024w"
         sizes="100vw" width="100" height="100"
         data-pagespeed-srcset-url-hashes="${H_SMALL},${H_LARGE}">
    <img id="mixed" src="${SRC}" srcset="${SRC2X} 2x"
         width="100" height="100"
         data-pagespeed-url-hash="${H_SRC}">
    <img id="deep" srcset="${DEEP} 1x" width="100" height="100"
         style="position:absolute; top:5000px;"
         data-pagespeed-srcset-url-hashes="${H_DEEP}">
    <script>${beaconJs}<\/script>
    <script>
      pagespeed.CriticalImages.Run('http://test/beacon', 'http://test/',
          'oh123', true, false, 'n456');
    <\/script>
  </body></html>`;
}

let failures = 0;
function check(ok, label) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${label}`);
  if (!ok) failures++;
}

const browser = await chromium.launch();
const page = await browser.newPage({
  viewport: { width: 1024, height: 768 },
  deviceScaleFactor: 2,
});
let capturedBody = null;
await page.route('http://test/**', (route) => {
  const req = route.request();
  if (req.url().startsWith('http://test/beacon')) {
    capturedBody = req.postData();
    return route.fulfill({ status: 204, body: '' });
  }
  if (req.url().startsWith('http://test/img/')) {
    return route.fulfill({ contentType: 'image/gif', body: GIF });
  }
  return route.fulfill({ contentType: 'text/html', body: testPage() });
});
const beaconRequest = page.waitForRequest(
    (r) => r.url().startsWith('http://test/beacon'), { timeout: 15000 });
await page.goto('http://test/');
await beaconRequest;
const currentSrcs = await page.evaluate(() => ({
  srconly: document.getElementById('srconly').currentSrc,
  mixed: document.getElementById('mixed').currentSrc,
}));
await page.close();
await browser.close();

check(capturedBody !== null, 'beacon POST captured');
// Sanity: the browser really did select the expected (rewritten) candidates,
// so the hash assertions below are meaningful.
check(currentSrcs.srconly === LARGE_RW,
      `srcset-only: browser selected rewritten large (got ` +
      `${currentSrcs.srconly})`);
check(currentSrcs.mixed === SRC2X,
      `mixed: browser selected src2x.jpg (got ${currentSrcs.mixed})`);

const params = new URLSearchParams(capturedBody);
check(params.get('oh') === 'oh123' && params.get('n') === 'n456',
      'oh/n params round-trip');
const ci = (params.get('ci') || '').split(',').filter(Boolean);
check(ci.includes(H_LARGE) && !ci.includes(H_SMALL),
      'srcset-only: reports stamped hash of selected candidate, not the ' +
      'unselected one');
check(!ci.includes(H_LARGE_RW),
      'srcset-only: does not hash currentSrc directly (rewritten URL hash ' +
      'absent)');
check(ci.includes(H_SRC) && !ci.includes(H_SRC2X),
      'mixed: reports stamped src hash even though a candidate was selected');
check(!ci.includes(H_DEEP), 'below-fold srcset-only image not reported');
check(ci.length === 2, `exactly two images reported (got ${ci.length})`);

console.log(failures ? `${failures} FAILURE(S)` : 'ALL PASS');
process.exit(failures ? 1 : 0);
