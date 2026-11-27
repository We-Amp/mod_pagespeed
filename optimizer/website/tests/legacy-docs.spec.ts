// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

test.describe('Legacy docs (1.0)', () => {
  test('/1.0/ loads legacy index page', async ({ page }) => {
    await page.goto('/1.0/');
    // Should not trigger a download — content should be visible
    await expect(page.locator('body')).toBeVisible();
    const content = await page.content();
    expect(content.length).toBeGreaterThan(100);
  });

  test('/1.0/doc/index.html loads without download', async ({ request }) => {
    const response = await request.get('/1.0/doc/index.html');
    expect(response.status()).toBe(200);
    const contentType = response.headers()['content-type'] ?? '';
    expect(contentType).toMatch(/text\/html/);
  });

  test('legacy pages have valid HTML content', async ({ page }) => {
    await page.goto('/1.0/');
    const html = await page.content();
    expect(html).toContain('<html');
    expect(html).toContain('</html>');
  });
});
