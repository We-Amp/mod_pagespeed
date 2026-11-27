// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Each example page shows the original and the optimized page side by side,
// each with an "Open in new tab" link. The two links go to different pages,
// so their accessible names must differ too; each starts with the frame's
// visible label and still contains the visible link text. Checked against
// Chromium's accessibility tree.

import { test, expect } from '@playwright/test';
import { axQuery, gotoSettled } from './helpers/ax-tree';

const ORIGINAL = 'Original Open in new tab';
const OPTIMIZED = 'Optimized Open in new tab';

for (const slug of ['inline_css', 'lazyload_images']) {
  test(`/examples/${slug}/: the two "Open in new tab" links have distinct names`, async ({
    page,
  }) => {
    await gotoSettled(page, `/examples/${slug}/`);
    const names = (await axQuery(page, 'main', 'link'))
      .map((l) => l.name)
      .filter((n) => n.includes('Open in new tab'));
    expect(names).toEqual([ORIGINAL, OPTIMIZED]);

    // Each name belongs to the link that opens that frame's page.
    const original = page.getByRole('link', { name: ORIGINAL, exact: true });
    const optimized = page.getByRole('link', { name: OPTIMIZED, exact: true });
    await expect(original).toHaveAttribute('href', /PageSpeed=off/);
    await expect(optimized).toHaveAttribute('href', new RegExp(`PageSpeedFilters=${slug}`));

    // The fallback links, shown when a preview does not load, follow suit.
    await page
      .locator('[data-fallback]')
      .evaluateAll((els) => els.forEach((el) => el.classList.remove('hidden')));
    const withFallback = (await axQuery(page, 'main', 'link'))
      .map((l) => l.name)
      .filter((n) => n.includes('Open in new tab'));
    expect(withFallback.sort()).toEqual([ORIGINAL, ORIGINAL, OPTIMIZED, OPTIMIZED].sort());
  });
}

test('the frame links are reached by keyboard in reading order', async ({ page }) => {
  await gotoSettled(page, '/examples/inline_css/');
  const original = page.getByRole('link', { name: ORIGINAL, exact: true });
  await original.focus();
  await expect(original).toBeFocused();
  // Tab moves through the original frame to the optimized frame's link.
  for (let i = 0; i < 6; i++) {
    await page.keyboard.press('Tab');
    const name = await page.evaluate(() => document.activeElement?.textContent?.trim() ?? '');
    if (name === 'Open in new tab') break;
  }
  await expect(page.getByRole('link', { name: OPTIMIZED, exact: true })).toBeFocused();
});
