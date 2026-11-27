// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

// /buy/ retired with the support tiers: support is bought on /pricing/, so the
// checkout placeholder is gone and its URL is a 301 to /pricing/. Production
// serves the redirect from the nginx map; astro.config.mjs `redirects` keeps
// dev and preview in step. Receipts and old portal mails link here with a
// ?product= query, so that form must land on /pricing/ too.
test.describe('/buy/ — retired, redirects to /pricing/', () => {
  test('/buy/ is a 301 to /pricing/', async ({ request, baseURL }) => {
    const res = await request.get('/buy/', { maxRedirects: 0 });
    expect(res.status()).toBe(301);
    expect(new URL(res.headers()['location'], baseURL).pathname).toBe('/pricing/');
  });

  test('/buy/ lands on the pricing page in a browser', async ({ page }) => {
    await page.goto('/buy/');
    expect(new URL(page.url()).pathname).toBe('/pricing/');
    await expect(page.locator('h1').first()).toContainText('Run it on any number of servers.');
  });

  test('legacy SKU deep-links land on /pricing/ as well', async ({ page }) => {
    await page.goto('/buy/?product=business-site-monthly');
    expect(new URL(page.url()).pathname).toBe('/pricing/');
  });
});
