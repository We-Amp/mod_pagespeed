// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// related-links.test.ts: the "Related" blocks (src/data/related.ts) only key
// real pages, only point at real pages, never link a page to itself, and never
// stack under a page's own "See also" list.

import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { describe, it, expect } from 'vitest';
import { relatedLinks, relatedFor } from '../src/data/related';
import { pillars } from '../src/data/cwv-pillars';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function resolves(href: string): boolean {
  const segs = href.split('/').filter(Boolean);
  if (segs[0] === 'docs' && segs.length === 2) {
    const docs = readdirSync(path.join(root, 'src/content/docs'));
    if (docs.some((f) => f.replace(/\.mdx?$/, '') === segs[1])) return true;
  }
  if (pillars.some((p) => p.slug === href)) return true;
  const rel = segs.join('/');
  return (
    existsSync(path.join(root, 'src/pages', `${rel}.astro`)) ||
    existsSync(path.join(root, 'src/pages', rel, 'index.astro'))
  );
}

describe('related links', () => {
  const entries = Object.entries(relatedLinks);

  it('covers the landing pages', () => {
    expect(entries.length).toBeGreaterThanOrEqual(7);
  });

  it.each(entries)(
    '%s is a real page with two to four real, distinct neighbours',
    (page, links) => {
      expect(page.startsWith('/') && page.endsWith('/')).toBe(true);
      expect(resolves(page)).toBe(true);
      expect(links.length).toBeGreaterThanOrEqual(2);
      expect(links.length).toBeLessThanOrEqual(4);
      const hrefs = links.map((l) => l.href);
      expect(new Set(hrefs).size).toBe(hrefs.length);
      expect(hrefs).not.toContain(page);
      for (const l of links) {
        expect(resolves(l.href), l.href).toBe(true);
        expect(l.label.trim().length).toBeGreaterThan(0);
      }
    },
  );

  // The page's source: the .astro file, or the template behind a dynamic route.
  function source(page: string): string {
    const rel = page.split('/').filter(Boolean).join('/');
    const direct = [`${rel}.astro`, `${rel}/index.astro`]
      .map((f) => path.join(root, 'src/pages', f))
      .find((f) => existsSync(f));
    if (direct) return readFileSync(direct, 'utf8');
    const dir = path.join(root, 'src/pages', path.dirname(rel));
    const dynamic = readdirSync(dir).find((f) => /^\[.+\]\.astro$/.test(f));
    if (!dynamic) throw new Error(`no source for ${page}`);
    return readFileSync(path.join(dir, dynamic), 'utf8');
  }

  it.each(entries.map(([page]) => page))('%s has no "See also" list of its own', (page) => {
    expect(source(page)).not.toMatch(/see also/i);
  });

  it('matches a path with or without its trailing slash', () => {
    expect(relatedFor('/psol')).toEqual(relatedFor('/psol/'));
    expect(relatedFor('/')).toEqual([]);
  });
});
