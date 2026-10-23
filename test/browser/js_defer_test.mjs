// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Browser integration tests for the js_defer (defer_javascript) client JS.
//
// Drives the SHIPPED asset, the compiled bytes embedded in
// net/instaweb/rewriter/generated/js_defer_opt.cc, in headless Chromium
// against markup shaped like JsDisableFilter + JsDeferDisabledFilter output:
// every deferrable script re-typed to text/psajs (or text/prioritypsajs)
// with data-pagespeed-orig-type/-orig-index, element onload handlers moved
// to data-pagespeed-onload behind kElementOnloadCode, data-pagespeed-no-defer
// scripts untouched, and <script src=".../js_defer.js"> at the end of body.
// The asset self-starts (deferInit + startDeferJs on DOMContentLoaded/load):
//
//   1. Ordering: no-defer scripts run during parse; deferred inline and
//      external scripts run afterwards in document order, even when an
//      earlier external script is slow.
//   2. Events: DOMContentLoaded/load listeners and window.onload set by
//      deferred scripts fire after all deferred scripts, DOMContentLoaded
//      first; document.readyState is overridden while scripts run (reads
//      'interactive' for text/psajs scripts today) and restored afterwards.
//   3. document.write / writeln: written HTML lands at the script's
//      position, and written inline and external scripts run in order
//      before the next deferred script.
//   4. Parse-time emulation: getElementById/getElementsByTagName hide
//      elements that follow the running script.
//   5. Error paths: a throwing script, a syntax error and a 404 external
//      script do not stop later scripts or onload.
//   6. Element onload handlers (data-pagespeed-onload) run at deferred
//      onload; addAfterDeferRunFunctions callbacks run; scriptsAreDone().
//   7. Dynamically inserted scripts delay onload until they load.
//   8. text/prioritypsajs scripts run before text/psajs scripts.
//
// Run via run_browser_tests.sh (installs playwright + chromium on demand).

import { chromium } from 'playwright';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadCompiledAsset } from './lib.mjs';

const repoRoot = join(dirname(fileURLToPath(import.meta.url)), '..', '..');

const deferJs = loadCompiledAsset(
    join(repoRoot, 'net', 'instaweb', 'rewriter', 'generated',
         'js_defer_opt.cc'));

// Mirrors JsDisableFilter::kElementOnloadCode.
const ELEMENT_ONLOAD = 'var elem=this;if (this==window) elem=document.body;' +
    "elem.setAttribute('data-pagespeed-loaded', 1)";

// 1x1 transparent GIF.
const GIF = Buffer.from(
    'R0lGODlhAQABAIAAAP///wAAACH5BAEAAAAALAAAAAABAAEAAAICRAEAOw==',
    'base64');

// Deferred-script markup as JsDisableFilter emits it.
let index = 0;
function deferred(body, { src = '', type = 'text/psajs' } = {}) {
  const srcAttr = src ? ` src="${src}"` : '';
  return `<script${srcAttr} type="${type}" ` +
         `data-pagespeed-orig-index="${index++}">${body}<\/script>`;
}

function page(bodyHtml, headHtml = '') {
  return `<!DOCTYPE html><html><head>
    <script data-pagespeed-no-defer>window.log = [];<\/script>
    ${headHtml}
  </head><body>
    ${bodyHtml}
    <script type="text/javascript"
            src="http://test/static/js_defer.js"><\/script>
  </body></html>`;
}

// External scripts: /js/<name>.js pushes <name>; slow-*.js are delayed.
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
let currentHtml = '';

let failures = 0;
function check(ok, label) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${label}`);
  if (!ok) failures++;
}

const browser = await chromium.launch();

async function run(html, { waitFor = 'window.__done === true' } = {}) {
  currentHtml = html;
  const p = await browser.newPage();
  const errors = [];
  p.on('pageerror', (e) => errors.push(e.message));
  await p.route('http://test/**', async (route) => {
    const url = new URL(route.request().url());
    if (url.pathname === '/static/js_defer.js') {
      return route.fulfill({ contentType: 'application/javascript',
                             body: deferJs });
    }
    if (url.pathname.startsWith('/img/')) {
      return route.fulfill({ contentType: 'image/gif', body: GIF });
    }
    const m = url.pathname.match(/^\/js\/([\w-]+)\.js$/);
    if (m) {
      if (m[1].startsWith('missing')) {
        return route.fulfill({ status: 404, body: 'not found' });
      }
      if (m[1].startsWith('slow')) await sleep(400);
      return route.fulfill({ contentType: 'application/javascript',
                             body: `log.push(${JSON.stringify(m[1])});` });
    }
    return route.fulfill({ contentType: 'text/html', body: currentHtml });
  });
  await p.goto('http://test/');
  let finished = true;
  try {
    await p.waitForFunction(waitFor, null, { timeout: 8000 });
  } catch {
    finished = false;
  }
  const log = await p.evaluate(() => window.log);
  return { p, log, errors, finished };
}

const eq = (a, b) => JSON.stringify(a) === JSON.stringify(b);
// Marks the end of a scenario once window load has run in deferred land.
const DONE = `window.addEventListener('load', function() {
                setTimeout(function() { window.__done = true; }, 50);
              });`;

// --- 1-2. Ordering and events --------------------------------------------
{
  index = 0;
  const { p, log, finished } = await run(page(`
    ${deferred(`log.push('i0:' + document.readyState);
      document.addEventListener('DOMContentLoaded', function(e) {
        log.push('dcl:' + e.type + ':' + document.readyState);
      });
      window.addEventListener('load', function(e) { log.push('load:' + e.type); });
      window.onload = function() { log.push('window.onload'); };
      ${DONE}`)}
    ${deferred('', { src: 'http://test/js/slow-e1.js' })}
    <script data-pagespeed-no-defer>log.push('nodefer');<\/script>
    ${deferred(`log.push('i2');`)}
    ${deferred('', { src: 'http://test/js/e3.js' })}
    ${deferred(`log.push('i4');`)}
  `));
  check(finished, 'ordering: deferred scripts completed and onload fired');
  check(log[0] === 'nodefer', `ordering: no-defer script runs first (got ${log[0]})`);
  // KNOWN-QUIRK: 'i0:interactive' pins existing behaviour, not a contract
  // (see the readyState note below); a fix may change it
  // and should then update this assertion deliberately.
  check(eq(log.slice(1, 6), ['i0:interactive', 'slow-e1', 'i2', 'e3', 'i4']),
        `ordering: deferred scripts in document order despite slow external ` +
        `(got ${log.slice(1, 6).join(',')})`);
  // KNOWN-QUIRK: this pins existing behaviour, not a contract. A
  // fix ('loading' during psajs scripts) should update these assertions
  // deliberately.
  // Current behaviour: the readyState getter installed by setUp() belongs to
  // the high-priority instance, which has already finished by the time the
  // (low-priority) text/psajs scripts run, so they see 'interactive', not
  // 'loading'.
  const rest = log.slice(6);
  check(rest[0] === 'dcl:DOMContentLoaded:interactive',
        `events: DOMContentLoaded fires after scripts, readyState interactive ` +
        `(got ${rest[0]})`);
  check(rest.includes('load:load'), 'events: load listener fires');
  check(rest.includes('window.onload'), 'events: window.onload assignment fires');
  check(rest.indexOf('dcl:DOMContentLoaded:interactive') <
        rest.indexOf('load:load'), 'events: DOMContentLoaded before load');
  check(rest.filter((x) => x === 'load:load').length === 1,
        'events: load listener fires exactly once');
  const state = await p.evaluate(() => ({
    done: pagespeed.deferJs.scriptsAreDone(),
    readyState: document.readyState,
    psanodes: document.getElementsByTagName('psanode').length,
  }));
  check(state.done, 'events: scriptsAreDone() true at the end');
  check(state.readyState === 'complete',
        `events: real readyState restored (got ${state.readyState})`);
  check(state.psanodes === 0, 'events: psanode markers removed');
  await p.close();
}

// --- 3. document.write / writeln -----------------------------------------
{
  index = 0;
  const { p, log, finished } = await run(page(`
    <div id="before"></div>
    ${deferred(`document.write('<p id="w1">written</p>' +
        '<script>log.push("dw-inline")<\\/script>' +
        '<script src="http://test/js/dw-ext.js"><\\/script>');
      document.writeln('<span id="w2">line</span>');
      log.push('writer');`)}
    <div id="after"></div>
    ${deferred(`log.push('next:' + !!document.getElementById('w1'));
      ${DONE}`)}
  `));
  check(finished, 'document.write: page completed');
  check(eq(log, ['writer', 'dw-inline', 'dw-ext', 'next:true']),
        `document.write: written scripts run in order before the next ` +
        `deferred script (got ${log.join(',')})`);
  const pos = await p.evaluate(() => {
    const w1 = document.getElementById('w1');
    const w2 = document.getElementById('w2');
    const before = document.getElementById('before');
    const after = document.getElementById('after');
    const follows = (a, b) =>
      !!(a && b && (a.compareDocumentPosition(b) &
                    Node.DOCUMENT_POSITION_FOLLOWING));
    return { w1: !!w1, w2: !!w2, order: follows(before, w1) && follows(w1, after),
             w2text: w2 && w2.nextSibling && w2.nextSibling.nodeValue };
  });
  check(pos.w1 && pos.w2, 'document.write: written elements inserted');
  check(pos.order, 'document.write: output lands at the writing script position');
  check(pos.w2text === '\n', 'document.writeln appends a newline');
  await p.close();
}

// --- 4. Parse-time DOM emulation ------------------------------------------
{
  index = 0;
  const { p, log, finished } = await run(page(`
    <div id="early"></div>
    ${deferred(`log.push('early:' + !!document.getElementById('early'));
      log.push('late:' + !!document.getElementById('late'));
      log.push('divs:' + document.getElementsByTagName('div').length);`)}
    <div id="late"></div>
    ${deferred(`log.push('late2:' + !!document.getElementById('late'));
      log.push('divs2:' + document.getElementsByTagName('div').length);
      ${DONE}`)}
  `));
  check(finished, 'dom emulation: page completed');
  check(eq(log, ['early:true', 'late:false', 'divs:1', 'late2:true', 'divs2:2']),
        `dom emulation: elements after the running script are hidden ` +
        `(got ${log.join(',')})`);
  await p.close();
}

// --- 5. Error paths -------------------------------------------------------
{
  index = 0;
  const { p, log, finished, errors } = await run(page(`
    ${deferred(`log.push('a'); throw new Error('boom');`)}
    ${deferred(`log.push('b'); this is not javascript(`)}
    ${deferred('', { src: 'http://test/js/missing.js' })}
    ${deferred(`log.push('c');
      window.addEventListener('load', function() { log.push('onload'); });
      ${DONE}`)}
  `));
  check(finished, 'errors: page still completes');
  check(eq(log, ['a', 'c', 'onload']),
        `errors: later scripts and onload still run (got ${log.join(',')})`);
  check(errors.some((e) => /boom/.test(e)),
        'errors: thrown exception surfaces as a page error');
  await p.close();
}

// --- 6. Element onload, after-defer callbacks ------------------------------
{
  index = 0;
  const { p, log, finished } = await run(page(`
    <script data-pagespeed-no-defer>
      pagespeed = window.pagespeed || {};
    <\/script>
    <img id="img" src="http://test/img/x.gif" onload="${ELEMENT_ONLOAD}"
         data-pagespeed-onload="log.push('img-onload:' + this.id)">
    ${deferred(`log.push('s');
      pagespeed.deferJs.addAfterDeferRunFunctions(function() {
        log.push('after');
      });
      ${DONE}`)}
  `));
  check(finished, 'element onload: page completed');
  check(log[0] === 's', 'element onload: deferred script ran first');
  check(log.includes('img-onload:img'),
        `element onload: data-pagespeed-onload handler ran with this=img ` +
        `(got ${log.join(',')})`);
  check(log.includes('after') &&
        log.indexOf('after') > log.indexOf('img-onload:img'),
        'element onload: afterDeferRun callback runs after onload handlers');
  await p.close();
}

// --- 7. Dynamically inserted scripts delay onload ---------------------------
{
  index = 0;
  const { p, log, finished } = await run(page(`
    ${deferred(`var s = document.createElement('script');
      s.src = 'http://test/js/slow-dyn.js';
      document.body.appendChild(s);
      log.push('inserted');
      window.addEventListener('load', function() { log.push('onload'); });
      ${DONE}`)}
  `));
  check(finished, 'dynamic script: page completed');
  check(eq(log, ['inserted', 'slow-dyn', 'onload']),
        `dynamic script: onload waits for the inserted script ` +
        `(got ${log.join(',')})`);
  await p.close();
}

// --- 8. Priority scripts run first ----------------------------------------
{
  index = 0;
  const { p, log, finished } = await run(page(`
    ${deferred(`log.push('low1');`)}
    ${deferred(`log.push('high');`, { type: 'text/prioritypsajs' })}
    ${deferred(`log.push('low2'); ${DONE}`)}
  `));
  check(finished, 'priority: page completed');
  check(eq(log, ['high', 'low1', 'low2']),
        `priority: text/prioritypsajs before text/psajs (got ${log.join(',')})`);
  await p.close();
}

await browser.close();
console.log(failures ? `${failures} FAILURE(S)` : 'ALL PASS');
process.exit(failures ? 1 : 0);
