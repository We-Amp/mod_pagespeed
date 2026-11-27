// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// An example page's measured figures and source diff, as a screen reader is
// given them (Chromium's accessibility tree). The requests tile is a level-3
// heading like the byte plates, and its struck-through figure is announced as
// the original count rather than relying on the strikethrough. The scrollable
// source diff is a region named by its heading, reachable from the keyboard.
// The back link does not read its arrow glyph.

import { test, expect, type Page } from '@playwright/test';
import { axQuery, gotoSettled } from './helpers/ax-tree';

const PAGES = ['/examples/inline_css/', '/examples/combine_css/'];

async function tile(page: Page) {
  const root = 'div:has(> h3 + dl)';
  const headings = await axQuery(page, root, 'heading');
  const terms = await axQuery(page, root, 'term');
  const defs = await axQuery(page, root, 'definition');
  return { headings, terms, defs };
}

for (const path of PAGES) {
  test.describe(`example impact semantics on ${path}`, () => {
    test('requests tile: heading, then original and optimized figures', async ({ page }) => {
      await gotoSettled(page, path);
      const { headings, terms, defs } = await tile(page);
      // The label is drawn in capitals (CSS), which Chromium carries into the name.
      expect(headings.map((h) => h.name.toLowerCase())).toEqual(['http requests']);
      const level = await page
        .locator('h3', { hasText: 'HTTP requests' })
        .evaluate((el) => el.tagName);
      expect(level).toBe('H3');
      expect(terms).toHaveLength(2);
      // Read in order: the term, then its figure.
      const text = (await axQuery(page, 'div:has(> h3 + dl) dl', 'StaticText')).map((t) => t.name);
      expect(text).toHaveLength(4);
      expect([text[0], text[2]]).toEqual(['original', 'optimized']);
      expect(Number(text[1])).toBeGreaterThan(Number(text[3]));
      expect(defs).toHaveLength(2);
      const before = await page.locator('div:has(> h3 + dl) dd').first();
      await expect(before).toHaveCSS('text-decoration-line', 'line-through');
    });

    test('source diff: a named region that takes keyboard focus', async ({ page }) => {
      await gotoSettled(page, path);
      const regions = await axQuery(page, 'main', 'region', 'What changed in the source');
      expect(regions).toHaveLength(1);
      const scroller = page.locator('[role="region"][aria-labelledby="source-diff-title"]');
      await expect(scroller).toHaveAttribute('tabindex', '0');
      await scroller.focus();
      await expect(scroller).toBeFocused();
      // Arrow keys scroll it sideways when the diff is wider than the box.
      const wide = await scroller.evaluate((el) => el.scrollWidth > el.clientWidth);
      if (wide) {
        await page.keyboard.press('ArrowRight');
        await expect.poll(() => scroller.evaluate((el) => el.scrollLeft)).toBeGreaterThan(0);
      }
    });

    test('back link is named without its arrow', async ({ page }) => {
      await gotoSettled(page, path);
      const names = (await axQuery(page, 'main', 'link')).map((l) => l.name);
      expect(names.filter((n) => n.includes('All optimization examples'))).toEqual([
        'All optimization examples',
      ]);
    });
  });
}
