// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Browser integration tests for the script that turns deferred stylesheets
// on (prioritize_critical_css).
//
// Drives the SHIPPED asset — the closure-compiled bytes embedded in
// net/instaweb/rewriter/generated/critical_css_loader_opt.cc — against the
// markup the filter writes: a preload link marked data-pagespeed-deferred-css
// where the stylesheet link stood, followed by a <noscript> copy of the link.
//
// Runs in headless Chromium; also in WebKit when
// PAGESPEED_BROWSER_TESTS_WEBKIT=1 and in Firefox when
// PAGESPEED_BROWSER_TESTS_FIREFOX=1 (run_browser_tests.sh installs them then).

import { chromium, firefox, webkit } from 'playwright';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadCompiledAsset } from './lib.mjs';

const repoRoot = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const loaderJs = loadCompiledAsset(
    join(repoRoot, 'net', 'instaweb', 'rewriter', 'generated',
         'critical_css_loader_opt.cc'));
const START_LOADER = `${loaderJs}pagespeed.CriticalCssLoader.Run();`;

const ATTR = 'data-pagespeed-deferred-css';
const PNG = Buffer.from(
    'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==',
    'base64');
// How long the page's last image is held back. The window load event waits
// for it, so anything observed earlier happened before the page finished
// loading.
const HOLD_LOAD_MS = 2500;

// A deferred stylesheet exactly as the filter writes it.
function deferred(href, extra = '') {
  return `<link rel="preload" href="${href}"${extra} as="style" ${ATTR}>` +
         `<noscript><link rel="stylesheet" href="${href}"${extra}></noscript>`;
}

// A page with an early and a late inline block around the deferred
// stylesheets, so the order in which rules apply is observable:
//   #probe: early block says rgb(1,1,1), /full.css says rgb(2,2,2), the late
//           block says rgb(3,3,3). In place, the late block wins.
// withLoader=false leaves the loader script out, for the cases that start it
// later, as happens when something delays the page's inline scripts.
function testPage(head, withLoader = true) {
  return `<!DOCTYPE html><html><head>
    <style>#probe{color:rgb(1,1,1)}</style>
    ${withLoader ? `<script>${START_LOADER}<\/script>` : ''}
    ${head}
    <style>#probe{color:rgb(3,3,3)}</style>
  </head><body>
    <div id="probe">x</div><div id="late">y</div><div id="b">z</div>
    <img src="/slow.png">
  </body></html>`;
}

const FULL_CSS = { body: '#probe{color:rgb(2,2,2)}#late{margin-top:7px}' };

let failures = 0;
function check(ok, label) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${label}`);
  if (!ok) failures++;
}

// Loads opts.html at http://test/ and serves opts.css[path] = {body, delay,
// fail} (fail: abort the first N requests).
//   opts.startLoader: 'after-stylesheet' starts the loader 400 ms into the
//     load (the stylesheet has been answered, the page has not finished
//     loading); 'after-load' starts it once the page has finished loading.
//   opts.until / opts.withinMs: what to wait for, and for how long, counted
//     from the moment the loader is (or was) started.
// Returns `early` (a snapshot taken when opts.until became true, or null if
// it did not in time), `final` (after the page finished loading) and the
// number of requests per path.
async function runCase(engine, opts) {
  const browser = await engine.launch();
  const context = await browser.newContext(
      { javaScriptEnabled: opts.scripts !== false });
  const page = await context.newPage();
  if (opts.init) await page.addInitScript(opts.init);
  const hits = {};
  await page.route('http://test/**', async (route) => {
    const path = new URL(route.request().url()).pathname;
    hits[path] = (hits[path] || 0) + 1;
    if (path === '/') {
      return route.fulfill({ contentType: 'text/html', body: opts.html });
    }
    if (path === '/slow.png') {
      await new Promise((r) => setTimeout(r, HOLD_LOAD_MS));
      return route.fulfill({ contentType: 'image/png', body: PNG });
    }
    const css = opts.css[path];
    if (!css) return route.fulfill({ status: 404, body: 'not found' });
    if (css.fail && hits[path] <= css.fail) return route.abort('failed');
    if (css.delay) await new Promise((r) => setTimeout(r, css.delay));
    return route.fulfill({
      contentType: 'text/css',
      headers: { 'cache-control': 'max-age=600' },
      body: css.body,
    });
  });
  const snapshot = () => page.evaluate(() => ({
    late: getComputedStyle(document.getElementById('late')).marginTop,
    b: getComputedStyle(document.getElementById('b')).marginTop,
    color: getComputedStyle(document.getElementById('probe')).color,
    rels: [...document.querySelectorAll('head > link')]
        .map((l) => l.getAttribute('rel')),
    readyState: document.readyState,
  }));
  await page.goto('http://test/', { waitUntil: 'commit' });
  if (opts.startLoader === 'after-stylesheet') {
    await page.waitForTimeout(400);
    await page.addScriptTag({ content: START_LOADER });
  } else if (opts.startLoader === 'after-load') {
    await page.waitForLoadState('load');
    await page.waitForTimeout(200);
    await page.addScriptTag({ content: START_LOADER });
  }
  let early = null;
  try {
    await page.waitForFunction(opts.until, null,
                               { timeout: opts.withinMs || 1500 });
    early = await snapshot();
  } catch (e) {
    early = null;
  }
  await page.waitForLoadState('load');
  await page.waitForTimeout(300);
  const final = await snapshot();
  await browser.close();
  return { early, final, hits };
}

const lateIsStyled = () => {
  const e = document.getElementById('late');
  return !!e && getComputedStyle(e).marginTop === '7px';
};
const linkIsStylesheet = () => {
  const l = document.querySelector('head > link');
  return !!l && l.getAttribute('rel') === 'stylesheet';
};

const engines = [chromium];
if (process.env.PAGESPEED_BROWSER_TESTS_WEBKIT === '1') engines.push(webkit);
if (process.env.PAGESPEED_BROWSER_TESTS_FIREFOX === '1') engines.push(firefox);

for (const engine of engines) {
  const tag = engine.name();
  // Chromium and WebKit hand the preloaded file to the stylesheet. Firefox
  // has been seen to ask for it a second time under this test driver, so
  // for Firefox "once" is relaxed to "at most twice".
  const once = (n) => (tag === 'firefox' ? n >= 1 && n <= 2 : n === 1);

  // --- 1. Applied as soon as it has arrived, where the link stood ----------
  {
    const r = await runCase(engine, {
      html: testPage(deferred('/full.css')),
      css: { '/full.css': FULL_CSS },
      until: lateIsStyled,
    });
    check(r.early !== null && r.early.readyState !== 'complete',
          `${tag} loader: applied before the page finished loading`);
    check(r.final.late === '7px', `${tag} loader: the stylesheet applies`);
    check(r.final.color === 'rgb(3, 3, 3)',
          `${tag} loader: in place — a later style block still wins`);
    check(r.final.rels.join() === 'stylesheet',
          `${tag} loader: the preload became the stylesheet`);
    check(once(r.hits['/full.css']),
          `${tag} loader: the stylesheet is requested once`);
  }

  // --- 2. Two stylesheets, the second arrives first --------------------------
  {
    const r = await runCase(engine, {
      html: testPage(deferred('/a.css') + deferred('/full.css')),
      css: {
        // Both set #late's margin; /full.css comes later in the page and
        // must win even though /a.css arrives 900 ms after it.
        '/a.css': { body: '#b{margin-top:5px}#late{margin-top:9px}', delay: 900 },
        '/full.css': FULL_CSS,
      },
      until: lateIsStyled,
    });
    check(r.early !== null && r.early.rels.join() === 'preload,stylesheet',
          `${tag} loader: two stylesheets, second arrives first — it applies ` +
          'without waiting for the first');
    check(r.final.b === '5px' && r.final.late === '7px',
          `${tag} loader: two stylesheets, second arrives first — the later ` +
          'stylesheet still wins');
    check(once(r.hits['/a.css']) && once(r.hits['/full.css']),
          `${tag} loader: two stylesheets — one request each`);
  }

  // --- 3. A stylesheet whose media does not match ----------------------------
  {
    const r = await runCase(engine, {
      html: testPage(deferred('/full.css', ' media="print"')),
      css: { '/full.css': FULL_CSS },
      until: lateIsStyled,
      withinMs: 800,
    });
    check(r.early === null && r.final.late === '0px',
          `${tag} loader: media that does not match never styles the screen`);
    check(r.final.rels.join() === 'stylesheet' && once(r.hits['/full.css']),
          `${tag} loader: media that does not match applies after load`);
  }

  // --- 4. The loader starts late ----------------------------------------------
  // Something delayed the page's inline scripts. Whenever the loader does
  // start, every deferred stylesheet already in the document is turned on.
  {
    const r = await runCase(engine, {
      html: testPage(deferred('/full.css'), false),
      css: { '/full.css': FULL_CSS },
      startLoader: 'after-stylesheet',
      until: lateIsStyled,
    });
    check(r.early !== null && r.early.readyState !== 'complete' &&
              r.final.color === 'rgb(3, 3, 3)',
          `${tag} loader: started after the stylesheet arrived, before the ` +
          'page finished loading');
  }
  {
    const r = await runCase(engine, {
      html: testPage(deferred('/full.css'), false),
      css: { '/full.css': FULL_CSS },
      startLoader: 'after-load',
      until: lateIsStyled,
    });
    check(r.early !== null && r.final.late === '7px' &&
              r.final.rels.join() === 'stylesheet',
          `${tag} loader: started after the page finished loading`);
  }
  {
    const r = await runCase(engine, {
      html: testPage(deferred('/full.css'), false),
      css: { '/full.css': { ...FULL_CSS, fail: 99 } },
      startLoader: 'after-load',
      until: linkIsStylesheet,
    });
    check(r.early !== null && r.final.rels.join() === 'stylesheet',
          `${tag} loader: started after the page finished loading and the ` +
          'request had failed');
  }

  // --- 5. The stylesheet request fails ---------------------------------------
  {
    const r = await runCase(engine, {
      html: testPage(deferred('/full.css')),
      css: { '/full.css': { ...FULL_CSS, fail: 99 } },
      until: linkIsStylesheet,
    });
    check(r.early !== null && r.early.readyState !== 'complete',
          `${tag} loader: a failed preload becomes a plain stylesheet link ` +
          'at once');
    check(r.final.rels.join() === 'stylesheet' &&
              r.final.color === 'rgb(3, 3, 3)',
          `${tag} loader: a failed preload leaves the page's other styles ` +
          'as they were');
  }

  // --- 6. A browser without preload support ----------------------------------
  {
    const r = await runCase(engine, {
      html: testPage(deferred('/full.css')),
      // The stylesheet takes longer than parsing the page, and the load
      // event is held back longer still: only the end of parsing can be
      // what turns the link into a stylesheet this early.
      css: { '/full.css': { ...FULL_CSS, delay: 1200 } },
      init: () => {
        const original = DOMTokenList.prototype.supports;
        DOMTokenList.prototype.supports = function(token) {
          return token === 'preload' ? false : original.call(this, token);
        };
      },
      until: linkIsStylesheet,
      withinMs: 800,
    });
    check(r.early !== null && r.early.late === '0px',
          `${tag} loader: without preload support the stylesheet applies ` +
          'when the document is parsed');
    check(r.final.late === '7px' && r.final.color === 'rgb(3, 3, 3)',
          `${tag} loader: without preload support the page ends up fully ` +
          'styled, in place');
  }

  // --- 7. Scripts off (pin: holds from the markup alone) ---------------------
  {
    const r = await runCase(engine, {
      html: testPage(deferred('/full.css')),
      css: { '/full.css': FULL_CSS },
      scripts: false,
      until: () => {
        const e = document.getElementById('late');
        return !!e && getComputedStyle(e).marginTop === '7px';
      },
    });
    check(r.final.late === '7px' && r.final.color === 'rgb(3, 3, 3)',
          `${tag} loader: scripts off — the noscript link applies, in place`);
    check(once(r.hits['/full.css']),
          `${tag} loader: scripts off — the stylesheet is requested once`);
  }
}

console.log(failures ? `${failures} FAILURE(S)` : 'ALL PASS');
process.exit(failures ? 1 : 0);
