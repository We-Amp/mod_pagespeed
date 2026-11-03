// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Real-browser checks that a page a framework hydrates survives the module.
//
// Without HYDRATION_ORIGIN (the plain browser-suite run), the fixtures in
// hydration/out are served as built and then with the edits a rewriter makes,
// applied to the HTML in flight. That pins what each framework version does
// with each edit, and proves the oracle below notices a failed hydration.
//
// With HYDRATION_ORIGIN=http://host (hydration/run_module_check.sh), the same
// fixtures come through a server running the module. Each one is visited at
// the default filters and with single filters forced by query parameter. It
// fails if hydration fails, if an original and a rewritten copy of one resource
// are both fetched, or, on a page expected to be detected, if a stylesheet URL
// or inline style changed or defer_javascript deferred a script or iframe.
//
// HYDRATION_EXPECT=off is for a server with DetectHydratedPages off. Its pages
// are rewritten as before, so it reports instead of failing, except that it
// fails if nothing was rewritten or deferred: then the run above would have
// proved nothing.

import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const OUT = process.env.HYDRATION_FIXTURES || path.join(HERE, 'hydration', 'out');
const ORIGIN = process.env.HYDRATION_ORIGIN || '';
const EXPECT_OFF = process.env.HYDRATION_EXPECT === 'off';

// detected: the module is expected to recognize the page from its response.
// A page with detected: false is a documented miss. It has no marker for the
// module to see, so it behaves as before detection existed.
const FIXTURES = [
  { name: 'react19-root', detected: true },
  { name: 'react19-doc', detected: true },
  { name: 'react18-root', detected: true },
  { name: 'react17-root', detected: true },
  { name: 'vue', detected: true },
  { name: 'svelte', detected: true },
  { name: 'next', detected: true, path: 'next/', full: true },
  { name: 'nuxt', detected: true, path: 'nuxt/', full: true },
  { name: 'react19-root-plain', detected: false },
  { name: 'react18-root-plain', detected: false },
  { name: 'react19-doc-plain', detected: false },
  { name: 'vue-plain', detected: false },
];

// The filters a stylesheet, script or image URL can change under. '' is the
// default filter set of the server.
const FILTER_SCENARIOS = [
  '',
  'rewrite_css',
  'flatten_css_imports,rewrite_css',
  'inline_css',
  'extend_cache_css',
  'combine_css',
  'inline_import_to_link',
  'rewrite_style_attributes',
  'rewrite_images',
  'move_css_to_head',
  'outline_css',
  'rewrite_javascript',
  'extend_cache_scripts',
  'combine_javascript',
  'inline_javascript',
  'outline_javascript',
  'remove_comments',
  'collapse_whitespace',
  'defer_javascript',
];

// Edits applied in flight in the no-server run, as a rewriter would make them.
const EDITS = {
  none: (html) => html,
  // rewrite_css on an inline style inside or outside the hydrated tree.
  inline_style_text: (html) => html.replace(
      /<style([^>]*)>([^<]*)<\/style>/g,
      (m, attrs, css) => `<style${attrs}>${css.replace(/\s+/g, '')}</style>`),
  // rewrite_css, extend_cache_css on a linked stylesheet.
  stylesheet_href: (html) => html.replace(
      /(<link[^>]*rel="stylesheet"[^>]*href=")([^"]*)(\.css")/g,
      (m, a, url, c) => `${a}${url}.css.pagespeed.cf.0123456789.css"`),
  // inline_css.
  stylesheet_inlined: (html) => html.replace(
      /<link[^>]*rel="stylesheet"[^>]*href="\/a\.css"[^>]*>/,
      '<style>body{margin:0}.box{display:block}</style>'),
  // rewrite_images with hint stripping: the image preload React 19 renders
  // is removed, while the image keeps its place in the tree.
  image_preload_removed: (html) => html.replace(
      /<link[^>]*rel="preload"[^>]*as="image"[^>]*>/g, ''),
  // rewrite_javascript, extend_cache_scripts on an async script.
  async_script_src: (html) => html.replace(
      /(<script[^>]*src=")([^"]*)(\.js"[^>]*async)/g,
      (m, a, url, c) => `${a}${url}.js.pagespeed.jm.0123456789.js"${c.slice(4)}`),
};

// The no-server run pins this: fixture -> edits that fail hydration or fetch a
// resource twice. React 18 and later fall back to rendering on the client after
// a mismatch inside the tree; React 17 and Svelte's {@html} keep the server's
// DOM. When React owns the document it finds stylesheets and async scripts by
// URL, so a changed URL loads a second copy.
const EXPECTED_FAILURES = JSON.parse(fs.readFileSync(
    path.join(HERE, 'hydration', 'expected_edits.json'), 'utf8'));

let failures = 0;
function check(ok, label) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${label}`);
  if (!ok) failures++;
}

const TRACK_PROBE = `
  (() => {
    window.__consoleProblems = [];
    const observer = new MutationObserver(() => {
      const probe = document.getElementById('probe');
      if (probe && !window.__probe) {
        window.__probe = probe;
        observer.disconnect();
      }
    });
    observer.observe(document, { childList: true, subtree: true });
  })();`;

const MISMATCH = /hydrat|did not match|mismatch/i;

// The server withholds UA-dependent rewrites such as defer_javascript from a
// headless browser's user agent, so speak as the desktop browser it is.
const desktopAgent = (browser) => 'Mozilla/5.0 (X11; Linux x86_64) ' +
    `AppleWebKit/537.36 (KHTML, like Gecko) Chrome/${browser.version()} Safari/537.36`;

// What one visit shows. Original URL -> fetched as original and/or as rewritten.
async function visit(browser, url, routeHtml) {
  const context = await browser.newContext({ userAgent: desktopAgent(browser) });
  const page = await context.newPage();
  await page.addInitScript(TRACK_PROBE);
  const problems = [];
  page.on('console', (m) => {
    if (['error', 'warning'].includes(m.type()) && MISMATCH.test(m.text())) {
      problems.push(m.text().slice(0, 160));
    }
  });
  page.on('pageerror', (e) => problems.push(`pageerror: ${String(e).slice(0, 160)}`));
  const fetched = [];
  page.on('request', (r) => fetched.push(new URL(r.url()).pathname));
  if (routeHtml) await routeHtml(page);
  const response = await page.goto(url, { waitUntil: 'load' });
  const served = response ? await response.text() : '';
  let hydrated = true;
  try {
    await page.waitForFunction(() => window.__hydrated === true, null, { timeout: 8000 });
  } catch (e) {
    hydrated = false;
  }
  await page.waitForTimeout(300);
  const state = await page.evaluate(() => ({
    kept: !!window.__probe && window.__probe.isConnected &&
        window.__probe === document.getElementById('probe'),
    recoverable: window.__recoverable || [],
    asyncRuns: window.__asyncRuns || 0,
  }));
  let clicked = false;
  if (hydrated) {
    await page.click('#inc');
    clicked = (await page.textContent('#count')) === '1';
  }
  const out = { page, context, hydrated, clicked, problems, fetched, served, ...state };
  return out;
}

// /x/a.css.pagespeed.cf.HASH.css -> a.css; /A.a.css.pagespeed... -> a.css;
// a combined leaf lists its inputs joined by +. An image leaf starts with its
// dimensions code: xbig.png.pagespeed.ic.HASH.png or 160x160xbig.png... -> big.png.
function originalsOf(pathname) {
  const leaf = pathname.split('/').pop();
  const m = leaf.match(/^(.*)\.pagespeed\.(?:[a-z]\.)?([a-z]{2})\.[^/]*$/);
  if (!m) return null;
  const names = m[1].replace(/^[A-Z]\./, '').split('+').map((p) => p.replace(/^[A-Z]\./, ''));
  if (m[2] !== 'ic') return names;
  return names.map((p) => p.replace(/^(?:(?:\d+|N)x(?:\d+|N))?[xwvm]/, ''));
}

function duplicates(fetched) {
  const originals = new Set();
  const rewrittenFrom = new Set();
  for (const pathname of fetched) {
    const from = originalsOf(pathname);
    if (from) from.forEach((f) => rewrittenFrom.add(f));
    else originals.add(pathname.split('/').pop());
  }
  return [...rewrittenFrom].filter((f) => originals.has(f));
}

function failureOf(result) {
  const reasons = [];
  if (!result.hydrated) reasons.push('did not hydrate');
  if (result.hydrated && !result.clicked) reasons.push('not interactive');
  if (!result.kept) reasons.push('server DOM replaced');
  if (result.recoverable.length) reasons.push('recoverable error');
  if (result.problems.length) reasons.push(`console: ${result.problems[0]}`);
  const dups = duplicates(result.fetched);
  if (dups.length) reasons.push(`fetched twice: ${dups.join(',')}`);
  if (result.asyncRuns > 1) reasons.push(`async script ran ${result.asyncRuns} times`);
  return reasons;
}

function serveFixtures(root) {
  const types = { '.html': 'text/html', '.css': 'text/css', '.js': 'text/javascript',
    '.png': 'image/png', '.json': 'application/json', '.txt': 'text/plain' };
  return http.createServer((req, res) => {
    let pathname = decodeURIComponent(new URL(req.url, 'http://x').pathname);
    // The in-flight edits name a rewritten URL: answer it from the original.
    pathname = pathname.replace(/\.pagespeed\.[a-z]{2}\.[0-9]+\.(css|js)$/, '');
    if (pathname.endsWith('/')) pathname += 'index.html';
    const file = path.join(root, pathname);
    if (!file.startsWith(root) || !fs.existsSync(file)) {
      res.writeHead(404);
      res.end();
      return;
    }
    res.writeHead(200, { 'content-type': types[path.extname(file)] || 'text/plain' });
    res.end(fs.readFileSync(file));
  });
}

function fixtureAvailable(fixture) {
  return fs.existsSync(path.join(OUT, fixture.path || `${fixture.name}/`, 'index.html'));
}

async function runEditsWithoutServer(browser) {
  const server = serveFixtures(OUT);
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const base = `http://127.0.0.1:${server.address().port}`;
  for (const fixture of FIXTURES) {
    if (!fixtureAvailable(fixture)) {
      if (!fixture.full) check(false, `${fixture.name}: fixture not built`);
      continue;
    }
    const url = `${base}/${fixture.path || `${fixture.name}/`}`;
    const expected = EXPECTED_FAILURES[fixture.name] || [];
    for (const [editName, edit] of Object.entries(EDITS)) {
      const result = await visit(browser, url, (page) => page.route(url, async (route) => {
        const response = await route.fetch();
        await route.fulfill({ response, body: edit(await response.text()) });
      }));
      const reasons = failureOf(result);
      await result.context.close();
      const shouldFail = expected.includes(editName);
      check(shouldFail === (reasons.length > 0),
            `${fixture.name} ${editName}: expected to ${shouldFail ? 'break' : 'survive'}` +
            (reasons.length ? ` (${reasons.join('; ')})` : ''));
    }
  }
  server.close();
}

// What the browser sends for a page. The server keeps a separate rewritten
// copy per user agent class and image format support, so settle that one.
const pageHeaders = (browser) => ({
  'user-agent': desktopAgent(browser),
  accept: 'text/html,application/xhtml+xml,application/xml;q=0.9,' +
      'image/avif,image/webp,image/apng,*/*;q=0.8',
});

// The rewrites are asynchronous: ask until the same answer comes twice.
async function settledHtml(browser, url) {
  let previous = null;
  let body = '';
  for (let i = 0; i < 40; i++) {
    const response = await fetch(url, { headers: pageHeaders(browser) });
    body = await response.text();
    if (body === previous && i >= 3) return body;
    previous = body;
    await new Promise((resolve) => setTimeout(resolve, 250));
  }
  return body;
}

const stylesheetHrefs = (html) => [...html.matchAll(/<link\b[^>]*>/g)]
    .map((m) => m[0])
    .filter((tag) => /rel="?stylesheet/.test(tag))
    .map((tag) => (tag.match(/href="?([^"\s>]*)/) || [])[1]);
// defer_javascript retypes scripts and renames iframes in the HTML it serves.
const deferred = (html) => /type="?text\/psajs|<pagespeed_iframe/.test(html);
const inlineStyles = (html) => [...html.matchAll(/<style\b[^>]*>([^<]*)<\/style>/g)]
    .map((m) => m[1]);

async function runAgainstServer(browser) {
  let rewrittenCount = 0;
  let deferredCount = 0;
  const table = [];
  for (const fixture of FIXTURES) {
    if (!fixtureAvailable(fixture)) {
      check(false, `${fixture.name}: fixture not built`);
      continue;
    }
    const rel = fixture.path || `${fixture.name}/`;
    const source = fs.readFileSync(path.join(OUT, rel, 'index.html'), 'utf8');
    for (const filters of FILTER_SCENARIOS) {
      const query = filters
        ? `?PageSpeedRewriteLevel=PassThrough&PageSpeedFilters=${filters}`
        : '';
      const url = `${ORIGIN}/${rel}${query}`;
      const html = await settledHtml(browser, url);
      const hrefsSame = JSON.stringify(stylesheetHrefs(html)) ===
          JSON.stringify(stylesheetHrefs(source));
      const stylesSame = JSON.stringify(inlineStyles(html)) ===
          JSON.stringify(inlineStyles(source));
      if (!hrefsSame || !stylesSame) rewrittenCount++;
      const result = await visit(browser, url);
      const reasons = failureOf(result);
      const wasDeferred = deferred(result.served);
      if (wasDeferred) deferredCount++;
      await result.context.close();
      table.push({ fixture: fixture.name, filters: filters || 'default', hrefsSame,
        stylesSame, deferred: wasDeferred, reasons });
      const label = `${fixture.name} [${filters || 'default'}]`;
      const detail = reasons.length ? ` (${reasons.join('; ')})` : '';
      if (EXPECT_OFF || !fixture.detected) {
        console.log(`info ${label}: hrefs ${hrefsSame ? 'kept' : 'changed'}, ` +
            `styles ${stylesSame ? 'kept' : 'changed'}, ` +
            `scripts ${wasDeferred ? 'deferred' : 'kept'}${detail}`);
        continue;
      }
      check(hrefsSame, `${label}: stylesheet URLs unchanged`);
      check(stylesSame, `${label}: inline styles unchanged`);
      check(!wasDeferred, `${label}: scripts and iframes not deferred`);
      check(reasons.length === 0, `${label}: hydrates${detail}`);
    }
  }
  if (EXPECT_OFF) {
    check(rewrittenCount > 0, 'negative control: pages were rewritten');
    check(deferredCount > 0, 'negative control: defer_javascript deferred scripts');
  }
  if (process.env.HYDRATION_TABLE) {
    fs.writeFileSync(process.env.HYDRATION_TABLE, JSON.stringify(table, null, 1));
  }

  // Client navigation: the payload names the original stylesheet URLs, so any
  // rewritten link in the server HTML would load the stylesheet a second time.
  if (fixtureAvailable({ path: 'next/' })) {
    await settledHtml(browser, `${ORIGIN}/next/`);
    const result = await visit(browser, `${ORIGIN}/next/`);
    await result.page.click('#nav');
    let arrived = true;
    try {
      await result.page.waitForSelector('#second', { timeout: 8000 });
      await result.page.waitForTimeout(500);
    } catch (e) {
      arrived = false;
    }
    const reasons = failureOf(result);
    const links = await result.page.evaluate(() => [...document.querySelectorAll(
        'link[rel="stylesheet"]')].map((l) => new URL(l.href).pathname));
    await result.context.close();
    const leaves = links.map((l) => originalsOf(l) || [l.split('/').pop()]).flat();
    const repeated = leaves.filter((l, i) => leaves.indexOf(l) !== i);
    const label = 'next client navigation';
    if (EXPECT_OFF) {
      console.log(`info ${label}: arrived ${arrived}, stylesheet listed twice ` +
          `[${repeated}] ${reasons.join('; ')}`);
    } else {
      check(arrived, `${label}: reaches the second page`);
      check(repeated.length === 0, `${label}: no stylesheet listed twice [${repeated}]`);
      check(reasons.length === 0, `${label}: clean (${reasons.join('; ')})`);
    }
  }
}

const browser = await chromium.launch();
if (ORIGIN) await runAgainstServer(browser);
else await runEditsWithoutServer(browser);
await browser.close();
if (failures) {
  console.error(`${failures} failure(s)`);
  process.exit(1);
}
console.log('hydration_test: all passed');
