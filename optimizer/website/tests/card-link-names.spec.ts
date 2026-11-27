// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The download page's integration cards are clickable as a whole, but only
// the card title is the link (a stretched link). A link wrapped around the
// card was named with every word on it, 40 to 80 words read out for one
// link. Checked against Chromium's accessibility tree, plus the click target
// and the keyboard focus ring that the card keeps.

import { test, expect } from '@playwright/test';
import { axQuery, gotoSettled } from './helpers/ax-tree';

test('integration card links are named by the card title alone', async ({ page }) => {
  await gotoSettled(page, '/download/');
  const titles = await page
    .locator('#integrations .card-stretch h3')
    .evaluateAll((els) => els.map((el) => (el.textContent ?? '').replace(/\s+/g, ' ').trim()));
  expect(titles.length).toBe(7);

  const cardLinks = await page
    .locator('#integrations .card-stretch-link')
    .evaluateAll((els) => els.map((el) => el.getAttribute('href')));
  expect(cardLinks).toHaveLength(titles.length);

  const links = (await axQuery(page, '#integrations', 'link')).map((l) => l.name);
  for (const title of titles) expect(links).toContain(title);
  // No link in the grid reads the card body any more.
  for (const name of links) expect(name.split(/\s+/).length).toBeLessThan(6);

  // The titles are still level-3 headings with the same names.
  const headings = (await axQuery(page, '#integrations', 'heading')).map((h) => h.name);
  expect(headings).toEqual(expect.arrayContaining(titles));
});

test('a click anywhere on an integration card follows its link', async ({ page }) => {
  await gotoSettled(page, '/download/');
  const card = page.locator('#integrations .card#nginx');
  const href = await card.locator('.card-stretch-link').getAttribute('href');
  expect(href).toBeTruthy();
  // Click the call to action at the bottom of the card, away from the title:
  // the point is covered by the title link's stretched hit area.
  const cta = card.getByText('Install on nginx');
  await cta.scrollIntoViewIfNeeded();
  const box = (await cta.boundingBox())!;
  const x = box.x + box.width / 2;
  const y = box.y + box.height / 2;
  const hit = await page.evaluate(
    ([px, py]) => document.elementFromPoint(px, py)?.closest('a')?.className ?? '',
    [x, y],
  );
  expect(hit).toContain('card-stretch-link');
  await page.mouse.click(x, y);
  await expect(page).toHaveURL(new RegExp(`${href!.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}$`));
});

test('keyboard focus on a card link rings the whole card', async ({ page }) => {
  await gotoSettled(page, '/download/');
  const card = page.locator('#integrations .card#apache');
  const link = card.locator('.card-stretch-link');
  // Reach it with Tab from the link just before the grid.
  await page.locator('#integrations a[href="/docs/release-notes/"]').focus();
  await page.keyboard.press('Tab');
  await expect(link).toBeFocused();
  await expect(card).toHaveCSS('outline-style', 'solid');
  await expect(card).toHaveCSS('outline-width', '2px');
  await expect(link).toHaveCSS('outline-style', 'none');
});
