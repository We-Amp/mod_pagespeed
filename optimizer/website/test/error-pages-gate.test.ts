// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// error-pages-gate.test.ts — pre-deploy gate for the prerendered error copies
// at dist/client/error/404/ and dist/client/error/500/. nginx serves those
// copies (via error_page) for any failing URL, so their build-time HTML must
// never present the copy's own path as the request: the GET line renders
// blank and is filled from location.pathname on load, and the 500 copy reads
// 5xx because it also serves 502/503/504. For the same reason the copies carry
// no canonical link and no og:url: either would name /error/404/, a URL that
// answers 200, as the canonical page for every missing URL.
//
// Like anchor-gate.test.ts, this reads the BUILD output and SKIPS (never
// throws) when dist/client/error does not exist, because CI's unit-test job
// runs `npx vitest run` without a preceding build and dist/ is gitignored.
// Run it after a build with `npm run build && npx vitest run`.

import { readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { describe, it, expect } from 'vitest';

const WEBSITE_ROOT = fileURLToPath(new URL('..', import.meta.url));
const DIST_ERROR_DIR = path.join(WEBSITE_ROOT, 'dist/client/error');
const BUILT = existsSync(DIST_ERROR_DIR);

if (!BUILT) {
  // eslint-disable-next-line no-console
  console.log(
    `[error-pages-gate] built output not found at ${DIST_ERROR_DIR} — skipping the ` +
      'built-HTML check. Run "npm run build" first to exercise it.',
  );
}

const readCopy = (name: string) =>
  readFileSync(path.join(DIST_ERROR_DIR, name, 'index.html'), 'utf8');

describe.runIf(BUILT)('prerendered error copies (built output)', () => {
  it('the 404 copy never shows its own path as the request line', () => {
    expect(readCopy('404')).not.toContain('GET /error/');
  });

  it('the 500 copy never shows its own path as the request line', () => {
    expect(readCopy('500')).not.toContain('GET /error/');
  });

  it('the 500 copy hedges its status: it also serves 502/503/504', () => {
    const html = readCopy('500');
    expect(html).toContain('5xx');
    expect(html).not.toContain('→ 500');
    // The arrow sits in its own aria-hidden span, so check the status text too.
    expect(html).not.toContain('</span>500');
  });

  it('the copies carry no canonical link and no og:url', () => {
    for (const name of ['404', '500']) {
      expect(readCopy(name)).not.toMatch(/<link[^>]*rel="canonical"/);
      expect(readCopy(name)).not.toContain('property="og:url"');
    }
  });

  it('the request line is filled from location.pathname on load', () => {
    for (const name of ['404', '500']) {
      const html = readCopy(name);
      expect(html).toContain('data-error-readout');
      expect(html).toContain('location.pathname');
    }
  });
});

// Source check, runs without a build: every error route opts out of the
// canonical link the layout emits by default.
describe('error routes opt out of the canonical link (source)', () => {
  const routes = ['404.astro', '500.astro', 'error/404.astro', 'error/500.astro'];
  for (const route of routes) {
    it(`src/pages/${route} passes canonical={false}`, () => {
      const src = readFileSync(path.join(WEBSITE_ROOT, 'src/pages', route), 'utf8');
      expect(src).toContain('canonical={false}');
    });
  }
});

// The top-level 404.html and 500.html are what the built server
// (node dist/server/entry.mjs) answers with. They must be the branded pages,
// not a meta-refresh stub that sends the browser to /404 and back again.
const DIST_CLIENT_DIR = path.join(WEBSITE_ROOT, 'dist/client');

describe.runIf(BUILT)('top-level error pages (built output)', () => {
  for (const code of ['404', '500']) {
    it(`${code}.html is the branded page, not a redirect stub`, () => {
      const html = readFileSync(path.join(DIST_CLIENT_DIR, `${code}.html`), 'utf8');
      expect(html).not.toMatch(/http-equiv=["']?refresh/i);
      expect(html).toContain('id="error-heading"');
      expect(html).toContain('data-error-readout');
    });
  }
});
