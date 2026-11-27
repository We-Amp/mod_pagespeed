// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Docs headings get a "#" permalink inside them. The permalink's label must
// not become part of the heading's accessible name, or a screen reader's
// headings list reads every entry twice; the permalink itself stays a named,
// working link. Checked against Chromium's accessibility tree.

import { test, expect } from '@playwright/test';
import { axQuery, gotoSettled } from './helpers/ax-tree';

const PAGES = ['/docs/getting-started/', '/docs/configuration/', '/how-it-works/metadata-cache/'];

for (const path of PAGES) {
  test(`${path}: heading names are the heading text alone`, async ({ page }) => {
    await gotoSettled(page, path);
    await expect(page.locator('article .heading-anchor').first()).toBeAttached();

    const expected = await page.locator('article :is(h2, h3, h4)[id]').evaluateAll((els) =>
      els
        .filter((el) => el.getClientRects().length > 0)
        .map((el) => {
          const copy = el.cloneNode(true) as HTMLElement;
          copy.querySelectorAll('.heading-anchor').forEach((a) => a.remove());
          return (copy.textContent ?? '').replace(/\s+/g, ' ').trim();
        }),
    );
    expect(expected.length).toBeGreaterThan(2);

    const headings = (await axQuery(page, 'article', 'heading')).map((h) => h.name);
    expect(headings.filter((n) => n.includes('Link to'))).toEqual([]);
    for (const text of expected) expect(headings).toContain(text);

    // Every permalink is still a link with a name that says where it goes.
    const links = (await axQuery(page, 'article', 'link'))
      .map((l) => l.name)
      .filter((n) => n.startsWith('Link to'));
    expect(links.length).toBe(expected.length);
    expect(links[0]).toBe(`Link to “${expected[0]}”`);
  });
}

test('a heading permalink is reachable by keyboard and moves to its section', async ({ page }) => {
  await gotoSettled(page, '/docs/getting-started/');
  const anchor = page.locator('article .heading-anchor').first();
  await expect(anchor).toBeAttached();
  const href = await anchor.getAttribute('href');
  await anchor.focus();
  await expect(anchor).toBeFocused();
  await expect(anchor).toHaveCSS('opacity', '1');
  await page.keyboard.press('Enter');
  await expect(page).toHaveURL(new RegExp(`${href}$`));
});

test.describe('without JavaScript', () => {
  test.use({ javaScriptEnabled: false });

  // The permalinks are in the HTML as served (rendered at build time), so they
  // work with scripts off and after an optimizer has rewritten the page, and
  // the heading's name and text are the same either way.
  test('the permalinks are in the served HTML', async ({ page }) => {
    await page.goto('/docs/migrate-from-ngx-pagespeed/');
    const heading = page.locator('article h2#roll-back');
    await expect(heading.locator('> a.heading-anchor')).toHaveAttribute('href', '#roll-back');
    expect(await heading.evaluate((h) => h.textContent)).toBe('Roll back');
    const names = (await axQuery(page, 'article', 'heading')).map((h) => h.name);
    expect(names).toContain('Roll back');
  });
});
