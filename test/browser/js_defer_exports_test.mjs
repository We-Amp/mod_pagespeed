// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Browser integration test for the names the js_defer client JS exposes.
//
// Pages and server-emitted markup reach the script by name:
// pagespeed.deferJs (addBeforeDeferRunFunctions, addAfterDeferRunFunctions,
// registerScriptTags, execute, run, addStr, addUrl, scriptsAreDone,
// isExperimentalMode), pagespeed.highPriorityDeferJs and
// pagespeed.lowPriorityDeferJs, pagespeed.addOnload and
// pagespeed.deferJsNs.startDeferJs. A minifier that renames properties would
// break every one of them, so this test pins the surface in both shipped
// builds (the _opt and the _dbg asset):
//
//   1. The exported functions exist on the object pages see.
//   2. pagespeed.deferJs is one of the high and low priority instances (it
//      moves from the first to the second once the high priority scripts
//      have run), and the two are separate objects.
//   3. defer_js_experimental turns isExperimentalMode() on.
//   4. Loading the asset a second time keeps the first instance.
//
// Run via run_browser_tests.sh (installs playwright + chromium on demand).

import { chromium } from 'playwright';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadCompiledAsset } from './lib.mjs';

const repoRoot = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const generated = join(repoRoot, 'net', 'instaweb', 'rewriter', 'generated');

const METHODS = [
  'addStr', 'addUrl', 'execute', 'run', 'scriptsAreDone',
  'addBeforeDeferRunFunctions', 'addAfterDeferRunFunctions',
  'registerScriptTags', 'isExperimentalMode',
];

let failures = 0;
function check(ok, label) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${label}`);
  if (!ok) failures++;
}

const browser = await chromium.launch();

async function surface(js, preamble) {
  const p = await browser.newPage();
  const errors = [];
  p.on('pageerror', (e) => errors.push(e.message));
  await p.route('http://test/**', async (route) => {
    const url = new URL(route.request().url());
    if (url.pathname === '/static/js_defer.js') {
      return route.fulfill({ contentType: 'application/javascript',
                             body: js });
    }
    return route.fulfill({
      contentType: 'text/html',
      body: `<!DOCTYPE html><html><head><script>${preamble}<\/script></head>` +
            '<body><script src="http://test/static/js_defer.js"><\/script>' +
            '</body></html>' });
  });
  await p.goto('http://test/');
  await p.waitForFunction('document.readyState === "complete"');
  const info = await p.evaluate((methods) => {
    const ps = window.pagespeed;
    const dj = ps.deferJs;
    const out = {
      methods: {},
      addOnload: typeof ps.addOnload,
      startDeferJs: typeof (ps.deferJsNs && ps.deferJsNs.startDeferJs),
      current: ps.deferJs === ps.highPriorityDeferJs ||
               ps.deferJs === ps.lowPriorityDeferJs,
      lowDistinct: !!ps.lowPriorityDeferJs &&
                   ps.lowPriorityDeferJs !== ps.highPriorityDeferJs,
      experimental: dj.isExperimentalMode(),
    };
    for (const m of methods) out.methods[m] = typeof dj[m];
    window.__first = dj;
    return out;
  }, METHODS);
  return { p, info, errors };
}

for (const kind of ['opt', 'dbg']) {
  // The _dbg asset is embedded in js_defer.cc, the _opt one in js_defer_opt.cc.
  const js = loadCompiledAsset(
      join(generated, kind === 'opt' ? 'js_defer_opt.cc' : 'js_defer.cc'));
  {
    const { p, info, errors } = await surface(js, '');
    for (const m of METHODS) {
      check(info.methods[m] === 'function',
            `${kind}: deferJs.${m} is a function`);
    }
    check(info.addOnload === 'function',
          `${kind}: pagespeed.addOnload is a function`);
    check(info.startDeferJs === 'function',
          `${kind}: pagespeed.deferJsNs.startDeferJs is a function`);
    check(info.current,
          `${kind}: pagespeed.deferJs is one of the two priority instances`);
    check(info.lowDistinct,
          `${kind}: lowPriorityDeferJs is a separate instance`);
    check(info.experimental === false || info.experimental === undefined,
          `${kind}: isExperimentalMode is off by default`);
    check(errors.length === 0, `${kind}: no page errors (got ${errors})`);

    // Loading the asset again must not replace the existing instance.
    await p.addScriptTag({ content: js });
    const same = await p.evaluate(
        () => window.pagespeed.deferJs === window.__first);
    check(same, `${kind}: a second load keeps the first deferJs instance`);
    await p.close();
  }
  {
    const { p, info } = await surface(
        js, 'window.pagespeed = {defer_js_experimental: true};');
    check(!!info.experimental,
          `${kind}: defer_js_experimental enables isExperimentalMode`);
    await p.close();
  }
}

await browser.close();
console.log(failures === 0 ? 'ALL PASS' : `${failures} FAILED`);
process.exit(failures === 0 ? 0 : 1);
