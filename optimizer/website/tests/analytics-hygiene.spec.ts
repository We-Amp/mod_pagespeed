// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

// The analytics tracker is restricted to the production hostnames, and a URL
// switch sets or clears the tracker's built-in opt-out flag. The tracker
// script itself is never loaded here (it is blocked), so only the tag and the
// inline switch are exercised.
test.describe('Analytics hygiene', () => {
  test.beforeEach(async ({ page }) => {
    await page.route('**/umami.we-amp.com/**', (route) => route.abort());
  });

  for (const url of ['/', '/docs/getting-started/', '/error/404/']) {
    test(`tracker tag is limited to the production hostnames on ${url}`, async ({ page }) => {
      await page.goto(url);
      const tag = page.locator('script[data-website-id]');
      await expect(tag).toHaveCount(1);
      await expect(tag).toHaveAttribute('data-domains', 'modpagespeed.com,www.modpagespeed.com');
    });
  }

  test('the opt-out script comes before the tracker tag', async ({ page }) => {
    await page.goto('/');
    const order = await page.evaluate(() => {
      const tracker = document.querySelector('script[data-website-id]');
      const optOut = [...document.querySelectorAll('script:not([src])')].find((s) =>
        (s.textContent ?? '').includes('umami-opt-out'),
      );
      if (!tracker || !optOut) return null;
      return Boolean(optOut.compareDocumentPosition(tracker) & Node.DOCUMENT_POSITION_FOLLOWING);
    });
    expect(order).toBe(true);
  });

  test('?umami-opt-out=1 sets the flag and ?umami-opt-out=0 clears it', async ({ page }) => {
    const flag = () => page.evaluate(() => localStorage.getItem('umami.disabled'));
    await page.goto('/');
    expect(await flag()).toBeNull();

    await page.goto('/?umami-opt-out=1');
    expect(await flag()).toBe('1');
    // The flag survives a normal navigation.
    await page.goto('/docs/getting-started/');
    expect(await flag()).toBe('1');

    await page.goto('/?umami-opt-out=0');
    expect(await flag()).toBeNull();
  });

  test('other values leave the flag alone', async ({ page }) => {
    await page.goto('/?umami-opt-out=1');
    await page.goto('/?umami-opt-out=maybe');
    expect(await page.evaluate(() => localStorage.getItem('umami.disabled'))).toBe('1');
  });

  test('unavailable storage does not break the page', async ({ page }) => {
    const errors: string[] = [];
    page.on('pageerror', (e) => errors.push(e.message));
    // Writes and removals throw, as in a private window or with a full quota.
    await page.addInitScript(() => {
      const deny = () => {
        throw new DOMException('denied', 'SecurityError');
      };
      Storage.prototype.setItem = deny;
      Storage.prototype.removeItem = deny;
    });
    await page.goto('/?umami-opt-out=0');
    await page.goto('/?umami-opt-out=1');
    await expect(page.locator('h1').first()).toBeVisible();
    expect(errors).toEqual([]);
  });
});
