// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Behavioural tests for the async-CSS loader (src/worker/async_css_loader.h).
//
// The C++ suite can only assert that certain SUBSTRINGS appear in the loader
// body. That is not protection: a loader whose idempotency guard is inverted
// (`!==` -> `===`) contains every one of those substrings and is a total
// no-op — every deferred stylesheet downloads and is never applied, leaving a
// permanently unstyled page for every visitor with JavaScript. This file exists
// because that mutation must not be able to ship green.
//
// So: extract the real bytes out of the header, EXECUTE them, and assert what
// they do. Node's built-in test runner and `node:vm` are used deliberately —
// no package.json, no lockfile, no network install for a harness whose entire
// job is to run ~500 bytes of ES5.
//
// Run: node --test tools/async-css-loader/

import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { test, describe } from 'node:test';

import { deferredLink, FakeLink, runLoader } from './dom-stub.mjs';
import {
  ASYNC_CSS_LOADER_HEADER,
  asyncCssLoaderJs,
  asyncCssLoaderPathFromSource,
  fnv1a32,
} from './extract.mjs';

const LOADER = asyncCssLoaderJs();

describe('extraction', () => {
  test('pulls a plausible loader out of the header', () => {
    assert.ok(LOADER.length > 100, `suspiciously short loader: ${LOADER}`);
    assert.ok(
      LOADER.startsWith('(function(){') && LOADER.endsWith('})();'),
      `loader is not the expected IIFE: ${LOADER}`,
    );
    assert.ok(
      LOADER.includes('data-pagespeed-async'),
      'loader does not mention the marker it selects on',
    );
  });

  test('agrees byte-for-byte with the Python twin', () => {
    // The pytest harnesses recompute the SERVED PATH from the same header with
    // tools/common/async_css_loader_path.py. If these two extractions ever
    // disagree, this suite would be testing bytes nobody serves — green, and
    // worthless. Skipped rather than failed where python3 is absent: the tie is
    // a cross-check, not a reason to make the JS suite unrunnable.
    let fromPython;
    try {
      fromPython = execFileSync(
        'python3',
        [
          '-c',
          'import sys; sys.path.insert(0, "tools");' +
            'from common.async_css_loader_path import async_css_loader_js_bytes;' +
            'sys.stdout.buffer.write(async_css_loader_js_bytes())',
        ],
        { cwd: new URL('../..', import.meta.url).pathname, encoding: 'utf8' },
      );
    } catch (err) {
      if (err.code === 'ENOENT') return; // no python3 here; the cross-check is optional
      throw err;
    }
    assert.equal(
      fromPython,
      LOADER,
      'the JS and Python extractions of kAsyncCssLoaderJs disagree',
    );
  });

  test('FNV-1a 32 matches published vectors and yields a well-formed path', () => {
    // Pin the digest itself, so a broken hash cannot make the path assertion
    // below pass by accident.
    assert.equal(fnv1a32(''), 0x811c9dc5);
    assert.equal(fnv1a32('a'), 0xe40c292c);
    assert.equal(fnv1a32('foobar'), 0xbf9cf968);
    assert.match(
      asyncCssLoaderPathFromSource(),
      /^\/pagespeed_static\/async_css\.[0-9a-f]{8}\.js$/,
    );
  });

  test('fails loudly rather than silently returning nothing', () => {
    assert.throws(
      () => asyncCssLoaderJs(`${ASYNC_CSS_LOADER_HEADER}.does-not-exist`),
      /ENOENT/,
    );
  });
});

describe('the deferred sheet is applied', () => {
  test('(a) the link load event flips rel and restores the recorded media', () => {
    const link = deferredLink('/a.css', 'screen');
    runLoader(LOADER, [link]);

    // Before the trigger it is still a preload: that IS the deferral.
    assert.equal(link.rel, 'preload');

    link.dispatch('load');

    assert.equal(link.rel, 'stylesheet');
    assert.equal(link.media, 'screen');
    // `as` deliberately stays: see the header comment. Removing it before the
    // flip would break the preload/consumer match.
    assert.equal(link.as, 'style');
  });

  test('(b) a Resource Timing entry applies it without any event at all', () => {
    // The sheet finished downloading BEFORE this deferred script ran, so its
    // load event already fired and will never fire again. Without this path the
    // sheet would hang at rel="preload" until window load.
    const link = deferredLink('/b.css', 'screen');
    runLoader(LOADER, [link], { resourceTimingHrefs: ['/b.css'] });

    assert.equal(link.rel, 'stylesheet', 'timing-entry path did not apply');
    assert.equal(link.media, 'screen');
  });

  test('(c) window load is the backstop when neither of the above fires', () => {
    // The resource timing buffer is finite and drops entries on a large page.
    // A sheet that finished early AND lost its entry has no other rescue: if
    // this backstop is removed the page stays unstyled forever.
    const link = deferredLink('/c.css', 'print, screen');
    const run = runLoader(LOADER, [link], { resourceTimingHrefs: [] });

    assert.equal(link.rel, 'preload', 'precondition: nothing has fired yet');
    assert.ok(
      run.windowLoadHandlerCount > 0,
      'no window load handler registered — the backstop is gone',
    );

    run.fireWindowLoad();

    assert.equal(link.rel, 'stylesheet', 'window-load backstop did not apply');
    assert.equal(link.media, 'print, screen');
  });

  test('a link with no recorded media falls back to all', () => {
    const link = new FakeLink({
      href: '/d.css',
      rel: 'preload',
      as: 'style',
      attrs: { 'data-pagespeed-async': '' },
    });
    runLoader(LOADER, [link]);
    link.dispatch('load');

    assert.equal(link.rel, 'stylesheet');
    assert.equal(link.media, 'all');
  });

  test('the error path still hands the sheet to the stylesheet machinery', () => {
    // The preload failed. Flipping rel is the recovery hook — it is what gives
    // the browser the chance to surface the sheet at all.
    const link = deferredLink('/e.css', 'screen');
    runLoader(LOADER, [link]);
    link.dispatch('error');

    assert.equal(link.rel, 'stylesheet');
    assert.equal(link.media, 'screen');
  });

  test('registers load AND error, both once-only', () => {
    const link = deferredLink('/f.css');
    runLoader(LOADER, [link]);
    const types = link.listenerLog.map((e) => e.type).sort();
    assert.deepEqual(types, ['error', 'load']);
    assert.ok(
      link.listenerLog.every((e) => e.once),
      'listeners must be {once:true} so they cannot pile up',
    );
  });

  test('applies every deferred sheet on the page, not just the first', () => {
    const links = [deferredLink('/1.css'), deferredLink('/2.css'), deferredLink('/3.css')];
    const run = runLoader(LOADER, links);
    run.fireWindowLoad();
    for (const l of links) assert.equal(l.rel, 'stylesheet', l.href);
  });
});

describe('idempotency guard', () => {
  // THE mutation this file exists for. A loader whose guard is inverted or
  // deleted re-runs the swap on a link that is already a stylesheet, and — far
  // worse when inverted — never runs it on one that is still a preload.
  test('(d) an already-applied link is left completely alone', () => {
    // The recorded media deliberately DIFFERS from the live media, so a loader
    // that re-runs the swap is caught changing something it must not touch.
    const link = new FakeLink({
      href: '/already.css',
      rel: 'stylesheet',
      as: 'style',
      media: 'screen',
      attrs: {
        'data-pagespeed-async': '',
        'data-pagespeed-media': 'only print',
      },
    });

    const run = runLoader(LOADER, [link], { resourceTimingHrefs: ['/already.css'] });
    link.dispatch('load');
    run.fireWindowLoad();

    assert.equal(link.rel, 'stylesheet');
    assert.equal(
      link.media,
      'screen',
      'the guard let the swap re-run over a live stylesheet',
    );
  });

  test('(d) all three triggers together still apply exactly once', () => {
    const link = deferredLink('/once.css', 'screen');
    const run = runLoader(LOADER, [link], { resourceTimingHrefs: ['/once.css'] });
    // Timing path already applied it; now let the other two fire on top.
    assert.equal(link.rel, 'stylesheet');
    link.media = 'tampered';
    link.dispatch('load');
    run.fireWindowLoad();
    assert.equal(
      link.media,
      'tampered',
      'a later trigger re-applied the swap over an applied sheet',
    );
  });
});

describe('scope', () => {
  test('(e) a preload without our marker is untouched', () => {
    // Author preloads, our own LCP/font hints — the page is full of them.
    const foreign = new FakeLink({
      href: '/theirs.css',
      rel: 'preload',
      as: 'style',
      attrs: { 'data-pagespeed-media': 'screen' },
    });
    const image = new FakeLink({
      href: '/hero.jpg',
      rel: 'preload',
      as: 'image',
      attrs: { 'data-pagespeed-hint': '' },
    });

    const run = runLoader(LOADER, [foreign, image], {
      resourceTimingHrefs: ['/theirs.css', '/hero.jpg'],
    });
    run.fireWindowLoad();

    assert.equal(foreign.rel, 'preload', 'hijacked a preload that is not ours');
    assert.equal(image.rel, 'preload', 'hijacked an image preload');
    assert.equal(foreign.listenerLog.length, 0);
  });

  test('selects on the marker attribute, not on rel', () => {
    const run = runLoader(LOADER, [deferredLink('/x.css')]);
    assert.deepEqual(run.selectorsSeen, ['link[data-pagespeed-async]']);
  });

  test('never creates a second link for the same sheet', () => {
    // dom-stub's createElement throws; reaching it means the loader built a new
    // element instead of flipping the preload, which double-downloads the sheet.
    const link = deferredLink('/single.css');
    const run = runLoader(LOADER, [link], { resourceTimingHrefs: ['/single.css'] });
    link.dispatch('load');
    run.fireWindowLoad();
    assert.equal(link.rel, 'stylesheet');
  });
});
