// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// nav-docs-links.test.ts: every /docs/<slug>/ link in the header Docs menu
// (src/data/nav.ts, `docsSections`) must resolve to a docs-collection page or
// a hand-written docs route, so the menu can never point at a 404.

import { existsSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { describe, it, expect } from 'vitest';
import { docsSections } from '../src/data/nav';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

const hrefs = docsSections
  .flatMap((section) => section.links.map((link) => link.href))
  .filter((href) => href.startsWith('/'));

function resolves(href: string): boolean {
  const segs = href.split('/').filter(Boolean);
  if (segs[0] === 'docs' && segs.length === 2) {
    const slug = segs[1];
    const docs = readdirSync(path.join(root, 'src/content/docs'));
    if (docs.some((f) => f.replace(/\.mdx?$/, '') === slug)) return true;
    return (
      existsSync(path.join(root, 'src/pages/docs', `${slug}.astro`)) ||
      existsSync(path.join(root, 'src/pages/docs', slug, 'index.astro'))
    );
  }
  const rel = segs.join('/');
  return (
    existsSync(path.join(root, 'src/pages', `${rel}.astro`)) ||
    existsSync(path.join(root, 'src/pages', rel, 'index.astro'))
  );
}

describe('header Docs menu links', () => {
  it('finds the menu links', () => {
    expect(hrefs.length).toBeGreaterThanOrEqual(25);
  });
  for (const href of hrefs) {
    it(`${href} resolves`, () => {
      expect(resolves(href)).toBe(true);
    });
  }
});
