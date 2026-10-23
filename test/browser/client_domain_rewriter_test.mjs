// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Browser integration tests for the client_domain_rewriter client JS.
//
// Drives the SHIPPED assets, the compiled bytes embedded in
// net/instaweb/rewriter/generated/client_domain_rewriter_opt.cc and its
// debug twin client_domain_rewriter.cc, in headless Chromium against markup
// shaped like DomainRewriteFilter's output: the asset followed by
// pagespeed.clientDomainRewriterInit(["<mapped domain>", ...]) at the end
// of the body:
//
//   1. A click on a link into a mapped domain navigates to the same path on
//      the page's own host instead.
//   2. A click on an element nested inside such a link is handled the same
//      way (the listener walks up to the enclosing <a>).
//   3. A click on a link outside the mapped domains navigates normally.
//   4. A non-Enter keypress on a mapped link does not navigate.
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
  ['opt', gen('client_domain_rewriter_opt.cc')],
  ['dbg', gen('client_domain_rewriter.cc')],
];

const testPage = (js) => `<!DOCTYPE html><html><body>
  <a id="mapped" href="http://cdn.example/a/b.html?q=1">mapped</a>
  <a id="nested" href="http://cdn.example/c.html"><span id="inner">in</span></a>
  <a id="other" href="http://elsewhere.example/d.html">other</a>
  <script>${js}pagespeed.clientDomainRewriterInit(["http://cdn.example/"]);<\/script>
</body></html>`;

let failures = 0;
function check(ok, label) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${label}`);
  if (!ok) failures++;
}

const browser = await chromium.launch();

for (const [name, js] of variants) {
  const run = async (action) => {
    const page = await browser.newPage();
    const requested = [];
    await page.route('**/*', (route) => {
      const url = route.request().url();
      requested.push(url);
      route.fulfill({
        contentType: 'text/html',
        body: url === 'http://test/' ? testPage(js)
                                     : '<!DOCTYPE html><p>landed</p>',
      });
    });
    await page.goto('http://test/');
    await page.waitForLoadState('load');
    const hasApi = await page.evaluate(
        () => typeof window.pagespeed.clientDomainRewriter);
    await action(page);
    const url = page.url();
    await page.close();
    return { url, requested, hasApi };
  };

  {
    const r = await run(async (page) => {
      await Promise.all([page.waitForURL((u) => u.pathname !== '/'),
                         page.click('#mapped')]);
    });
    check(r.hasApi === 'object',
          `${name}: init stores pagespeed.clientDomainRewriter`);
    check(r.url === 'http://test/a/b.html?q=1',
          `${name}: mapped link rewritten to own host (got ${r.url})`);
    check(!r.requested.some((u) => u.startsWith('http://cdn.example/')),
          `${name}: mapped domain never requested`);
  }

  {
    const r = await run(async (page) => {
      await Promise.all([page.waitForURL((u) => u.pathname !== '/'),
                         page.click('#inner')]);
    });
    check(r.url === 'http://test/c.html',
          `${name}: click inside a mapped link is rewritten (got ${r.url})`);
  }

  {
    const r = await run(async (page) => {
      await Promise.all([page.waitForURL((u) => u.pathname !== '/'),
                         page.click('#other')]);
    });
    check(r.url === 'http://elsewhere.example/d.html',
          `${name}: unmapped link navigates normally (got ${r.url})`);
  }

  {
    const r = await run(async (page) => {
      await page.focus('#mapped');
      await page.keyboard.press('a');
      await page.waitForTimeout(300);
    });
    check(r.url === 'http://test/',
          `${name}: non-Enter keypress does not navigate (got ${r.url})`);
  }
}

await browser.close();
console.log(failures ? `${failures} FAILURE(S)` : 'ALL PASS');
process.exit(failures ? 1 : 0);
