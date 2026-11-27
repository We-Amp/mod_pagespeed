// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Sitemap dates for the example pages (src/lib/page-lastmod.mjs): each
// /examples/<slug>/ page dates from its own catalog entry and capture, so a
// commit that re-measures a few examples re-dates only those. The history is
// injected, so nothing here needs git.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';
import {
  exampleCatalogRecords,
  exampleDataRecords,
  exampleLastmods,
  recordDates,
} from '../src/lib/page-lastmod.mjs';
import { EXAMPLES } from '../src/data/examples';

const ROOT = fileURLToPath(new URL('..', import.meta.url));

const capture = (records: Record<string, unknown>) =>
  JSON.stringify({ generated_at: 'x', examples: records });
const entry = (slug: string, title: string) =>
  `  {\n    slug: '${slug}',\n    title: '${title}',\n  },`;
const catalog = (...entries: string[]) => `export const EXAMPLES = [\n${entries.join('\n')}\n];\n`;

describe('example page dates', () => {
  it('a commit that re-measures one example re-dates that example only', () => {
    const history = [
      { date: '2026-11-12', text: capture({ a: { bytes: 2 }, b: { bytes: 1 } }) },
      { date: '2026-10-01', text: capture({ a: { bytes: 1 }, b: { bytes: 1 } }) },
      { date: '2026-09-01', text: capture({ a: { bytes: 1 } }) },
    ];
    const dates = recordDates(history, exampleDataRecords);
    expect(dates.get('a')).toBe('2026-11-12');
    // b arrived on 10-01 and has not changed since.
    expect(dates.get('b')).toBe('2026-10-01');
  });

  it('a record that changes and changes back dates from the change back', () => {
    const history = [
      { date: '2026-03-01', text: capture({ a: 1 }) },
      { date: '2026-02-01', text: capture({ a: 2 }) },
      { date: '2026-01-01', text: capture({ a: 1 }) },
    ];
    expect(recordDates(history, exampleDataRecords).get('a')).toBe('2026-03-01');
  });

  it('an unparsable version counts as having no records', () => {
    const history = [
      { date: '2026-03-01', text: capture({ a: 1 }) },
      { date: '2026-02-01', text: '{ not json' },
    ];
    expect(recordDates(history, exampleDataRecords).get('a')).toBe('2026-03-01');
  });

  it('each page takes the latest of its entry, its capture and the template', () => {
    const files: Record<string, Array<{ date: string; text: string }>> = {
      'examples-data.json': [
        { date: '2026-11-12', text: capture({ a: { bytes: 2 }, b: { bytes: 1 } }) },
        { date: '2026-10-01', text: capture({ a: { bytes: 1 }, b: { bytes: 1 } }) },
      ],
      'examples.ts': [
        { date: '2026-10-20', text: catalog(entry('a', 'A'), entry('b', 'B2')) },
        { date: '2026-09-01', text: catalog(entry('a', 'A'), entry('b', 'B')) },
      ],
    };
    const historyOf = (f: string) => files[f.split('/').pop()!] ?? null;
    const dateOf = (f: string) => (f.endsWith('[slug].astro') ? '2026-09-15' : null);
    const map = exampleLastmods('/site', dateOf, historyOf);
    expect(map.get('/examples/a/')).toBe('2026-11-12T00:00:00.000Z');
    expect(map.get('/examples/b/')).toBe('2026-10-20T00:00:00.000Z');
  });

  it('no usable history gives no per-page dates (the family date stays the fallback)', () => {
    expect(
      exampleLastmods(
        '/site',
        () => null,
        () => null,
      ).size,
    ).toBe(0);
  });

  it('the record readers find every example in the real catalog and capture', () => {
    const slugs = EXAMPLES.map((e) => e.slug).sort();
    const cat = exampleCatalogRecords(readFileSync(`${ROOT}src/data/examples.ts`, 'utf8'));
    expect([...cat.keys()].sort()).toEqual(slugs);
    for (const [slug, text] of cat) expect(text, slug).toContain(`slug: '${slug}'`);
    const data = exampleDataRecords(readFileSync(`${ROOT}src/data/examples-data.json`, 'utf8'));
    for (const slug of data.keys()) expect(slugs, slug).toContain(slug);
  });
});
