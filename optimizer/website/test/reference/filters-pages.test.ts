// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// filters-pages.test.ts — every filter the module ships is documented once.
//
//   data   : src/data/reference/filters.json (generated from the module source)
//   view   : src/data/filters.ts (FILTERS, the table on /docs/filters/)
//   pages  : the group pages css-filters, image-filters, javascript-filters,
//            html-filters and cache-control (the caching filters' home)
//
// For every name in filters.json (the 96 gperf names plus the compound names):
//   - FILTERS carries it, so /docs/filters/ renders a row for it;
//   - exactly ONE group page exposes an id equal to the filter name, and the
//     row's href points at that page, so the deep link lands on the entry.
// The page set is checked from the markdown source (see test/lib/markdown-ids.ts),
// so this runs without a build.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';
import { describe, it, expect } from 'vitest';
import { idsInSource, stripFrontmatter } from '../lib/markdown-ids';
import { FILTERS, PAGE_BY_CATEGORY, CATEGORY_ORDER } from '../../src/data/filters';

const WEBSITE_ROOT = fileURLToPath(new URL('../..', import.meta.url));
const DOCS_DIR = resolve(WEBSITE_ROOT, 'src/content/docs');
const data = JSON.parse(
  readFileSync(resolve(WEBSITE_ROOT, 'src/data/reference/filters.json'), 'utf8'),
);

const GROUP_PAGES = [...new Set(Object.values(PAGE_BY_CATEGORY))];
const pageIds = new Map(
  GROUP_PAGES.map((slug) => [
    slug,
    idsInSource(stripFrontmatter(readFileSync(resolve(DOCS_DIR, `${slug}.md`), 'utf8'))),
  ]),
);

const allNames: string[] = [
  ...data.filters.map((f: { name: string }) => f.name),
  ...data.aliases.map((a: { name: string }) => a.name),
];

describe('FILTERS mirrors filters.json', () => {
  it('carries every generated name exactly once', () => {
    const names = FILTERS.map((f) => f.name);
    expect(new Set(names).size).toBe(names.length);
    expect([...names].sort()).toEqual([...allNames].sort());
  });

  it('is alphabetical, with a known category and a description for every row', () => {
    for (let i = 1; i < FILTERS.length; i++) {
      expect(FILTERS[i - 1].name.localeCompare(FILTERS[i].name, 'en')).toBeLessThan(0);
    }
    for (const f of FILTERS) {
      expect(CATEGORY_ORDER).toContain(f.category);
      expect(f.description.length).toBeGreaterThan(0);
    }
  });

  it('marks the compound names as aliases with members that are filters', () => {
    const single = new Set(data.filters.map((f: { name: string }) => f.name));
    for (const f of FILTERS.filter((x) => x.alias)) {
      expect(f.members.length).toBeGreaterThan(0);
      for (const m of f.members) expect(single.has(m), `${f.name} member ${m}`).toBe(true);
    }
  });
});

describe('every filter has a section on exactly one group page', () => {
  for (const f of FILTERS) {
    it(`${f.name} -> ${f.href}`, () => {
      const homes = GROUP_PAGES.filter((slug) => pageIds.get(slug)!.has(f.name));
      expect(homes, `pages exposing id="${f.name}"`).toEqual([PAGE_BY_CATEGORY[f.category]]);
      expect(f.href).toBe(`/docs/${PAGE_BY_CATEGORY[f.category]}/#${f.name}`);
    });
  }
});

describe('/docs/filters/ renders the table from FILTERS', () => {
  const page = readFileSync(resolve(WEBSITE_ROOT, 'src/pages/docs/filters.astro'), 'utf8');

  it('imports FILTERS and gives every row the filter name as its id', () => {
    expect(page).toMatch(/import \{[^}]*\bFILTERS\b[^}]*\} from '\.\.\/\.\.\/data\/filters'/);
    expect(page).toContain('id={f.name}');
  });
});
