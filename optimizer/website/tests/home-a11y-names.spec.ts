// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// What a screen reader is given on the home page (Chromium's accessibility
// tree): the hero heading is read as two sentences, the phone-width diagram
// carries the same summary as the wide one, link names do not read arrow
// glyphs, citations open in the same tab, the page has a few named regions
// rather than one per section, and the further-reading cards are links named
// by their title while the whole card stays clickable.

import { test, expect } from '@playwright/test';
import { axQuery, gotoSettled } from './helpers/ax-tree';

const squash = (s: string) => s.replace(/\s+/g, ' ').trim();

test.describe('home page accessible names', () => {
  test('the hero heading is read as two sentences', async ({ page }) => {
    await gotoSettled(page, '/');
    const [h1] = await axQuery(page, 'main', 'heading');
    // Chromium puts a space around the hidden (out-of-flow) full stop.
    expect(squash(h1.name)).toMatch(
      /^mod_pagespeed, the PageSpeed module for nginx and Apache ?\. Lighthouse 56 to 90\. On your own servers\.$/,
    );
  });

  test('the phone-width diagram has the same name as the wide one', async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await gotoSettled(page, '/');
    const images = (await axQuery(page, 'main', 'image')).map((i) => squash(i.name));
    const diagram = images.filter((n) => n.startsWith('mod_pagespeed architecture'));
    expect(diagram).toEqual([
      'mod_pagespeed architecture: request, module and cache lookup, HIT serves from mmap, MISS serves origin while an async worker writes the optimized variant.',
    ]);
  });

  test('no link in the page body reads an arrow glyph', async ({ page }) => {
    await page.setViewportSize({ width: 1440, height: 900 });
    await gotoSettled(page, '/');
    const names = (await axQuery(page, 'main', 'link')).map((l) => l.name);
    expect(names.length).toBeGreaterThan(10);
    expect(names.filter((n) => /[←-⇿⤀-⥿]/.test(n))).toEqual([]);
  });

  test('citation links open in the same tab', async ({ page }) => {
    await gotoSettled(page, '/');
    const blank = await page
      .locator('main a[target="_blank"]')
      .evaluateAll((els) => els.map((e) => e.getAttribute('href')));
    expect(blank).toEqual([]);
    await expect(page.locator('main a[href="https://we-amp.com/"]')).toHaveCount(1);
    expect(await page.locator('main a[href*="builtwith.com"]').count()).toBeGreaterThan(0);
  });

  test('a few named regions, not one per section', async ({ page }) => {
    await gotoSettled(page, '/');
    const regions = (await axQuery(page, 'main', 'region')).map((r) => squash(r.name));
    // Four sections keep a name; the fifth region is the scrollable headers
    // terminal, which is named so its keyboard focus is announced.
    expect(regions).toEqual([
      'Install. Configure. Verify.',
      'Running on our own sites',
      'Captured response headers of the modpagespeed.com homepage',
      'Before it goes in front of production traffic.',
      'Free to install and run. Start today.',
    ]);
  });
});

test.describe('home further-reading cards', () => {
  const GRID = 'section.home-reading';

  test('each card link is named by the card title', async ({ page }) => {
    await gotoSettled(page, '/');
    const titles = await page
      .locator(`${GRID} .card-stretch h3`)
      .evaluateAll((els) => els.map((el) => (el.textContent ?? '').replace(/\s+/g, ' ').trim()));
    expect(titles).toHaveLength(4);
    const links = (await axQuery(page, GRID, 'link')).map((l) => squash(l.name));
    expect(links).toEqual(titles);
    const headings = (await axQuery(page, GRID, 'heading', undefined)).map((h) => squash(h.name));
    expect(headings).toEqual(expect.arrayContaining(titles));
  });

  test('a click on the card body follows the card link', async ({ page }) => {
    await page.setViewportSize({ width: 1440, height: 900 });
    await gotoSettled(page, '/');
    const card = page.locator(`${GRID} .card-stretch`).first();
    const href = await card.locator('.card-stretch-link').getAttribute('href');
    const cta = card.getByText('Read the docs');
    await cta.scrollIntoViewIfNeeded();
    const box = (await cta.boundingBox())!;
    await page.mouse.click(box.x + box.width / 2, box.y + box.height / 2);
    await expect(page).toHaveURL(new RegExp(`${href!.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}$`));
  });

  test('keyboard focus on a card link rings the whole card', async ({ page }) => {
    await page.setViewportSize({ width: 1440, height: 900 });
    await gotoSettled(page, '/');
    const cards = page.locator(`${GRID} .card-stretch`);
    const links = page.locator(`${GRID} .card-stretch-link`);
    await links.first().focus();
    // Tab walks the four card links in order, nothing in between.
    for (let i = 0; i < 4; i++) {
      if (i > 0) await page.keyboard.press('Tab');
      await expect(links.nth(i)).toBeFocused();
      await expect(cards.nth(i)).toHaveCSS('outline-style', 'solid');
      await expect(links.nth(i)).toHaveCSS('outline-style', 'none');
    }
  });
});
