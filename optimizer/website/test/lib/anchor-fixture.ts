// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Shared fixture loader for the two anchor gates (anchor-source-gate.test.ts,
// the always-on CI check, and anchor-gate.test.ts, the built-HTML pre-deploy
// check). Kept out of `test/**/*.test.ts` on purpose so vitest never tries to
// run it as its own test file.

import { readFileSync } from 'node:fs';

export type AnchorRow = { legacyPage: string; slug: string; anchor: string };

// A handful of legacy pages folded as a SECTION into a /docs/ page with a
// different slug (rather than migrating whole under their own slug). The
// fixture's "page" column stays the legacy /1.1/docs/ URL for provenance;
// this table is where the resulting /docs/ slug is looked up when it
// differs from the legacy one.
const SLUG_OVERRIDES: Record<string, string> = {
  'caching-url-filters': 'cache-control',
  caching: 'cache-modes',
};

export function loadAnchorFixture(fixturePath: string): AnchorRow[] {
  const raw = readFileSync(fixturePath, 'utf8');
  const lines = raw.split('\n').filter((l) => l.trim().length > 0);
  // First line is a header ("page\tanchor"); skip it.
  const [header, ...rows] = lines;
  if (!header.startsWith('page\t')) {
    throw new Error(`${fixturePath}: expected a "page\\tanchor" header row, got: ${header}`);
  }
  return rows.map((line) => {
    const [legacyPage, anchor] = line.split('\t');
    if (!legacyPage || !anchor) {
      throw new Error(`${fixturePath}: malformed row: ${JSON.stringify(line)}`);
    }
    // legacyPage looks like "/1.1/docs/css-filters/" — the same page now also
    // lives at "/docs/css-filters/".
    const m = legacyPage.match(/^\/1\.1\/docs\/([a-z0-9-]+)\/$/);
    if (!m) {
      throw new Error(`${fixturePath}: unrecognized page format: ${legacyPage}`);
    }
    const legacySlug = m[1];
    const slug = SLUG_OVERRIDES[legacySlug] ?? legacySlug;
    return { legacyPage, slug, anchor };
  });
}

export function groupBySlug(rows: AnchorRow[]): Map<string, AnchorRow[]> {
  const bySlug = new Map<string, AnchorRow[]>();
  for (const row of rows) {
    const list = bySlug.get(row.slug) ?? [];
    list.push(row);
    bySlug.set(row.slug, list);
  }
  return bySlug;
}
