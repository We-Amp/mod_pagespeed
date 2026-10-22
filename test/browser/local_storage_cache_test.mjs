// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Browser integration tests for the local_storage_cache client JS.
//
// Drives the SHIPPED asset, the closure-compiled bytes embedded in
// net/instaweb/rewriter/generated/local_storage_cache_opt.cc, in headless
// Chromium against markup shaped like LocalStorageCacheFilter's output (the
// asset plus pagespeed.localStorageCacheInit(), inlined resources stamped
// with data-pagespeed-lsc-url/-hash/-expiry, and on repeat views the
// pagespeed.localStorageCache.inlineImg()/inlineCss() replacement snippets):
//
//   1. First view: at onload, the inlined image and stylesheet are saved
//      to localStorage under the expected keys and value format, and the
//      _GPSLSC cookie lists their hashes.
//   2. Repeat view (hit): inlineImg() and inlineCss() replace their script
//      with an <img> (data URL and extra attributes) and a <style> from
//      localStorage.
//   3. Miss: with empty storage, inlineImg() falls back to the URL and
//      inlineCss() to a <link rel=stylesheet>.
//   4. Expired entries are treated as misses, purged from localStorage at
//      onload and left out of the cookie.
//
// Run via run_browser_tests.sh (installs playwright + chromium on demand).

import { chromium } from 'playwright';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadCompiledAsset } from './lib.mjs';

const repoRoot = join(dirname(fileURLToPath(import.meta.url)), '..', '..');

const lscJs = loadCompiledAsset(
    join(repoRoot, 'net', 'instaweb', 'rewriter', 'generated',
         'local_storage_cache_opt.cc'));

const IMG_URL = 'http://test/img/pixel.gif';
const CSS_URL = 'http://test/css/style.css';
const IMG_HASH = 'hImg1';
const CSS_HASH = 'hCss2';
const GIF_B64 =
    'R0lGODlhAQABAIAAAP///wAAACH5BAEAAAAALAAAAAABAAEAAAICRAEAOw==';
const DATA_URL = `data:image/gif;base64,${GIF_B64}`;
const CSS = '#probe{color:rgb(1, 2, 3)}';
const FUTURE = 'Tue, 01 Jan 2999 00:00:00 GMT';

const IMG_KEY = `pagespeed_lsc_url:${IMG_URL} pagespeed_lsc_hash:${IMG_HASH}`;
const CSS_KEY = `pagespeed_lsc_url:${CSS_URL}`;

const head = `<!DOCTYPE html><html><head>
  <script data-pagespeed-no-defer>${lscJs}pagespeed.localStorageCacheInit();<\/script>`;

function firstViewPage() {
  return `${head}
    <style data-pagespeed-lsc-url="${CSS_URL}"
           data-pagespeed-lsc-hash="${CSS_HASH}"
           data-pagespeed-lsc-expiry="${FUTURE}">${CSS}</style>
  </head><body>
    <div id="probe">probe</div>
    <img id="i" src="${DATA_URL}" data-pagespeed-lsc-url="${IMG_URL}"
         data-pagespeed-lsc-hash="${IMG_HASH}"
         data-pagespeed-lsc-expiry="${FUTURE}">
  </body></html>`;
}

function repeatViewPage() {
  return `${head}
    <script data-pagespeed-no-defer>pagespeed.localStorageCache.inlineCss("${CSS_URL}");<\/script>
  </head><body>
    <div id="probe">probe</div>
    <div id="holder"><script data-pagespeed-no-defer>pagespeed.localStorageCache.inlineImg("${IMG_URL}", "${IMG_HASH}", "alt=a pixel", "class=lsc");<\/script></div>
  </body></html>`;
}

let failures = 0;
function check(ok, label) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${label}`);
  if (!ok) failures++;
}

const browser = await chromium.launch();

async function newContext(pages) {
  const context = await browser.newContext();
  const requested = [];
  await context.route('http://test/**', (route) => {
    const url = route.request().url();
    requested.push(url);
    if (url === IMG_URL) {
      return route.fulfill({ contentType: 'image/gif',
                             body: Buffer.from(GIF_B64, 'base64') });
    }
    if (url === CSS_URL) {
      return route.fulfill({ contentType: 'text/css', body: CSS });
    }
    const path = new URL(url).pathname;
    return route.fulfill({ contentType: 'text/html', body: pages[path] || '' });
  });
  return { context, requested };
}

const cookieValue = (cookies) =>
  (cookies.find((c) => c.name === '_GPSLSC') || {}).value;

// --- 1-2. First view saves, repeat view restores -------------------------
{
  const { context, requested } = await newContext({
    '/first': firstViewPage(), '/repeat': repeatViewPage(),
  });
  const page = await context.newPage();
  await page.goto('http://test/first');
  await page.waitForLoadState('load');
  const stored = await page.evaluate(([ik, ck]) => ({
    img: localStorage.getItem(ik), css: localStorage.getItem(ck),
  }), [IMG_KEY, CSS_KEY]);
  const expiryMs = new Date(FUTURE).getTime();
  check(stored.img === `${expiryMs} ${IMG_HASH} ${DATA_URL}`,
        `first view: image saved as "<expiry> <hash> <data>" (got ` +
        `${String(stored.img).slice(0, 60)})`);
  check(stored.css === `${expiryMs} ${CSS_HASH} ${CSS}`,
        `first view: css saved under url-only key (got ${stored.css})`);
  const cookie = cookieValue(await context.cookies('http://test/'));
  check(cookie !== undefined &&
        cookie.split('!').sort().join('!') === [CSS_HASH, IMG_HASH].sort().join('!'),
        `first view: _GPSLSC cookie lists both hashes (got ${cookie})`);

  requested.length = 0;
  await page.goto('http://test/repeat');
  await page.waitForLoadState('load');
  const r = await page.evaluate(() => {
    const img = document.querySelector('#holder > img');
    return {
      imgSrc: img && img.getAttribute('src'),
      alt: img && img.getAttribute('alt'),
      cls: img && img.getAttribute('class'),
      scriptsInHolder: document.querySelectorAll('#holder script').length,
      styleText: [...document.querySelectorAll('head style')]
          .map((s) => s.textContent).join(''),
      links: document.querySelectorAll('link[rel=stylesheet]').length,
      color: getComputedStyle(document.getElementById('probe')).color,
    };
  });
  check(r.imgSrc === DATA_URL, 'repeat view: inlineImg restores the data URL');
  check(r.alt === 'a pixel' && r.cls === 'lsc',
        `repeat view: extra img attributes applied (alt=${r.alt} class=${r.cls})`);
  check(r.scriptsInHolder === 0, 'repeat view: inlineImg replaced its script');
  check(r.styleText === CSS && r.links === 0,
        'repeat view: inlineCss inserted a <style>, not a <link>');
  check(r.color === 'rgb(1, 2, 3)', `repeat view: restored css applies (got ${r.color})`);
  check(!requested.includes(IMG_URL) && !requested.includes(CSS_URL),
        'repeat view: no network fetch for cached resources');
  await context.close();
}

// --- 3. Miss: empty storage falls back to the network --------------------
{
  const { context, requested } = await newContext({ '/repeat': repeatViewPage() });
  const page = await context.newPage();
  await page.goto('http://test/repeat');
  await page.waitForLoadState('load');
  const r = await page.evaluate(() => {
    const img = document.querySelector('#holder > img');
    const link = document.querySelector('link[rel=stylesheet]');
    return { imgSrc: img && img.getAttribute('src'),
             alt: img && img.getAttribute('alt'),
             href: link && link.getAttribute('href'),
             color: getComputedStyle(document.getElementById('probe')).color };
  });
  check(r.imgSrc === IMG_URL && r.alt === 'a pixel',
        `miss: inlineImg falls back to the url (got ${r.imgSrc})`);
  check(r.href === CSS_URL, `miss: inlineCss falls back to a link (got ${r.href})`);
  check(r.color === 'rgb(1, 2, 3)', 'miss: linked stylesheet applies');
  check(requested.includes(IMG_URL) && requested.includes(CSS_URL),
        'miss: both resources fetched from the network');
  await context.close();
}

// --- 4. Expired entries are misses and get purged ------------------------
{
  const { context } = await newContext({
    '/seed': '<!DOCTYPE html><html><body>seed</body></html>',
    '/repeat': repeatViewPage(),
  });
  const page = await context.newPage();
  await page.goto('http://test/seed');
  await page.evaluate(([ik, ck, ih, ch, data, css]) => {
    localStorage.setItem(ik, `1000 ${ih} ${data}`);   // expired long ago
    localStorage.setItem(ck, `${Date.now() + 1e9} ${ch} ${css}`);
  }, [IMG_KEY, CSS_KEY, IMG_HASH, CSS_HASH, DATA_URL, CSS]);
  await page.goto('http://test/repeat');
  await page.waitForLoadState('load');
  const r = await page.evaluate(([ik, ck]) => ({
    imgSrc: document.querySelector('#holder > img').getAttribute('src'),
    hasStyle: !!document.querySelector('head style'),
    img: localStorage.getItem(ik), css: localStorage.getItem(ck),
  }), [IMG_KEY, CSS_KEY]);
  check(r.imgSrc === IMG_URL, 'expired: image entry treated as a miss');
  check(r.hasStyle, 'expired: unexpired css entry still a hit');
  check(r.img === null, 'expired: image entry purged at onload');
  check(r.css !== null, 'expired: unexpired css entry kept');
  const cookie = cookieValue(await context.cookies('http://test/'));
  check(cookie === CSS_HASH,
        `expired: cookie lists only the live hash (got ${cookie})`);
  await context.close();
}

await browser.close();
console.log(failures ? `${failures} FAILURE(S)` : 'ALL PASS');
process.exit(failures ? 1 : 0);
