// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// layout-import-order.test.ts — pins the order of the styles in the built
// <head> on a few representative pages. Astro places a page's stylesheets by
// the position of each import in its importer's whole import list, styled or
// not, so reordering any import in BaseLayout.astro (even an unstyled
// component or a plain module) can move the site sheet before or after a
// page's own styles. Checking the source cannot see that; the built output
// can. Each page's signature is the sequence of <style> blocks (S) and
// stylesheet links (L:<bundle name>) inside <head>.
//
// Like error-pages-gate.test.ts, this reads the BUILD output and SKIPS (never
// throws) when dist/client does not exist, because CI's unit-test job runs
// `npx vitest run` before the build; CI runs it again after `npm run build`.
// Run it locally with `npm run build && npx vitest run`.

import { readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { describe, it, expect } from 'vitest';

const WEBSITE_ROOT = fileURLToPath(new URL('..', import.meta.url));
const DIST = path.join(WEBSITE_ROOT, 'dist/client');
const BUILT = existsSync(path.join(DIST, 'index.html'));

if (!BUILT) {
  // eslint-disable-next-line no-console
  console.log(
    `[layout-import-order] built output not found at ${DIST} — skipping the ` +
      'head style order check. Run "npm run build" first to exercise it.',
  );
}

// Page -> expected head signature. The layout's own inline styles come first,
// then the site sheet (BaseLayout.*.css), then page styles, except on pages
// whose inline styles Astro hoists above the sheet (the alternatives hub).
const EXPECTED: Record<string, string> = {
  '': 'S S S L:BaseLayout S L:index',
  'docs/getting-started/': 'S S S L:BaseLayout S',
  'pricing/': 'S S S L:BaseLayout',
  'alternatives/': 'S S S L:BaseLayout',
  'alternatives/shortpixel/': 'S S S L:BaseLayout S',
  'ai-readability/': 'S S S L:BaseLayout S',
};

function headStyleSignature(html: string): string {
  const end = html.indexOf('</head>');
  const head = end === -1 ? html : html.slice(0, end);
  return [...head.matchAll(/<style\b[^>]*>|<link\b[^>]*\brel=["']?stylesheet\b[^>]*>/gi)]
    .map((m) => {
      if (/^<style/i.test(m[0])) return 'S';
      const name = /href=["']?[^"'\s>]*\/([A-Za-z0-9_-]+)\.[^"'\s>/]*\.css/i.exec(m[0]);
      return `L:${name ? name[1] : '?'}`;
    })
    .join(' ');
}

describe.runIf(BUILT)('head style order (built output)', () => {
  for (const [page, expected] of Object.entries(EXPECTED)) {
    it(`/${page} keeps its style order: ${expected}`, () => {
      const html = readFileSync(path.join(DIST, page, 'index.html'), 'utf8');
      expect(headStyleSignature(html)).toBe(expected);
    });
  }
});
