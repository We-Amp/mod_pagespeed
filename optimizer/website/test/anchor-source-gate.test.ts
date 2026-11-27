// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// anchor-source-gate.test.ts — the CI gate for the deep-link anchors that
// external redirects depend on. Computes, from the markdown SOURCE, the set
// of ids each converged page's headings and raw anchor tags will expose once
// built, using the same heading-id mechanism the site's markdown pipeline
// uses: an explicit `{#id}` (remark-custom-heading-id) is read literally;
// a heading with no explicit id falls back to `github-slugger` — the exact
// package @astrojs/markdown-remark's own heading-id collector
// (rehype-collect-headings.js) imports, called once per file in document
// order so its dedupe state matches. Raw `<a id="…">` tags (e.g. the table
// anchors in the filter group pages) pass through the pipeline unchanged, so
// their id is read literally too. The id computation lives in
// test/lib/markdown-ids.ts, shared with the reference gates.
//
// This needs no build step, so it runs wherever `npx vitest run` runs,
// including CI (whose unit-test job does not build the site first). The
// built-HTML counterpart that reads the actual production output is
// anchor-gate.test.ts — it skips itself when dist/ is absent rather than
// gating CI, and is the stronger, catch-anything check to run before a
// deploy.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { describe, it, expect } from 'vitest';
import { loadAnchorFixture, groupBySlug } from './lib/anchor-fixture';
import { idsInSource, stripFrontmatter } from './lib/markdown-ids';
import { FILTERS } from '../src/data/filters';

const WEBSITE_ROOT = fileURLToPath(new URL('..', import.meta.url));
const FIXTURE_PATH = path.join(WEBSITE_ROOT, 'test/fixtures/legacy-doc-anchors.txt');
const DOCS_SRC_DIR = path.join(WEBSITE_ROOT, 'src/content/docs');

const ANCHOR_ROWS = loadAnchorFixture(FIXTURE_PATH);

describe('legacy doc anchors resolve at their new /docs/ location (source)', () => {
  it('fixture carries all 66 rows across the converged pages', () => {
    expect(ANCHOR_ROWS.length).toBe(66);
  });

  const bySlug = groupBySlug(ANCHOR_ROWS);

  for (const [slug, rows] of bySlug) {
    describe(`src/content/docs/${slug}.md`, () => {
      const mdPath = path.join(DOCS_SRC_DIR, `${slug}.md`);
      const ids = idsInSource(stripFrontmatter(readFileSync(mdPath, 'utf8')));

      for (const { anchor } of rows) {
        it(`will expose id="${anchor}" once built`, () => {
          expect(ids.has(anchor), `expected an id "${anchor}" among ${slug}.md's headings`).toBe(
            true,
          );
        });
      }
    });
  }
});

// The filter table (src/data/filters.ts) deep-links each row into a
// per-category docs page. Four of those categories were re-pointed from the
// legacy /1.1/docs/ route to /docs/; every such href's anchor must resolve
// in that page's docs/ SOURCE, the same way the legacy-redirect fixture
// above is checked.
describe('src/data/filters.ts anchors resolve at their /docs/ location (source)', () => {
  const DOCS_HREF = /^\/docs\/([a-z0-9-]+)\/#([a-zA-Z0-9_-]+)$/;
  const allHrefs = FILTERS.map((f) => f.href);
  const docsRows = [...new Set(allHrefs)]
    .map((href) => {
      const m = DOCS_HREF.exec(href);
      return m ? { href, slug: m[1], anchor: m[2] } : null;
    })
    .filter((row): row is { href: string; slug: string; anchor: string } => row !== null);

  it('found /docs/ rows for the re-pointed filter categories', () => {
    expect(docsRows.length).toBeGreaterThan(0);
  });

  const bySlug = new Map<string, typeof docsRows>();
  for (const row of docsRows) {
    const list = bySlug.get(row.slug) ?? [];
    list.push(row);
    bySlug.set(row.slug, list);
  }

  for (const [slug, rows] of bySlug) {
    describe(`src/content/docs/${slug}.md`, () => {
      const mdPath = path.join(DOCS_SRC_DIR, `${slug}.md`);
      const ids = idsInSource(stripFrontmatter(readFileSync(mdPath, 'utf8')));

      for (const { href, anchor } of rows) {
        it(`${href} resolves to id="${anchor}"`, () => {
          expect(ids.has(anchor), `expected an id "${anchor}" among ${slug}.md's headings`).toBe(
            true,
          );
        });
      }
    });
  }
});
