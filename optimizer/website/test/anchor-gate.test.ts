// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// anchor-gate.test.ts — pre-deploy gate for the deep-link anchors that
// external redirects depend on. Reads the production BUILD output
// (dist/client), not the markdown source, so it catches anything that could
// drop an id between content and render: a heading rename, a dropped `{#id}`
// custom id, a removed `<a id>` anchor, or a markdown-pipeline change.
//
// This is the STRONGER, run-after-a-build check — not the CI gate. CI's
// unit-test job runs `npx vitest run` without a preceding `npm run build`,
// and dist/ is gitignored, so this test SKIPS itself (never throws) when
// dist/client/docs does not exist, rather than redding every PR. The
// always-on, no-build-required counterpart that DOES run in CI is
// anchor-source-gate.test.ts. Run this one explicitly after a build with
// `npm run test:anchors`, or as part of `npm run build && npm run test:unit`.
//
// Extend the shared fixture (test/fixtures/legacy-doc-anchors.txt, never
// remove a row) whenever a page with deep-link anchors moves into /docs/.

import { readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { describe, it, expect, beforeAll } from 'vitest';
import { loadAnchorFixture, groupBySlug } from './lib/anchor-fixture';

const WEBSITE_ROOT = fileURLToPath(new URL('..', import.meta.url));
const FIXTURE_PATH = path.join(WEBSITE_ROOT, 'test/fixtures/legacy-doc-anchors.txt');
const DIST_DOCS_DIR = path.join(WEBSITE_ROOT, 'dist/client/docs');

const ANCHOR_ROWS = loadAnchorFixture(FIXTURE_PATH);
const BUILT = existsSync(DIST_DOCS_DIR);

if (!BUILT) {
  // Visible in the test run's output either way — a console.log, not a
  // silently-vanishing skip.
  // eslint-disable-next-line no-console
  console.log(
    `[anchor-gate] built output not found at ${DIST_DOCS_DIR} — skipping the built-HTML ` +
      'anchor check. Run "npm run build" first, then "npm run test:anchors", to exercise it. ' +
      'The always-on source check (anchor-source-gate.test.ts) still ran.',
  );
}

describe.skipIf(!BUILT)(
  'legacy doc anchors resolve at their new /docs/ location (built html)',
  () => {
    // Fixture sanity: this is the pre-deploy gate, so a shrinking fixture
    // must fail loudly rather than silently pass with fewer checks.
    it('fixture carries all 66 rows across the converged pages', () => {
      expect(ANCHOR_ROWS.length).toBe(66);
    });

    if (!BUILT) return; // belt-and-braces: no file I/O below when unbuilt.

    const bySlug = groupBySlug(ANCHOR_ROWS);

    for (const [slug, rows] of bySlug) {
      describe(`/docs/${slug}/`, () => {
        const htmlPath = path.join(DIST_DOCS_DIR, slug, 'index.html');
        let html: string;

        beforeAll(() => {
          if (!existsSync(htmlPath)) {
            throw new Error(`${htmlPath} does not exist — was /docs/${slug}/ built?`);
          }
          html = readFileSync(htmlPath, 'utf8');
        });

        for (const { anchor } of rows) {
          it(`has an id="${anchor}"`, () => {
            const idPattern = new RegExp(`id=["']${anchor}["']`);
            expect(html, `expected id="${anchor}" in built /docs/${slug}/`).toMatch(idPattern);
          });
        }
      });
    }
  },
);
