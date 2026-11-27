// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

test.describe('Static pages', () => {
  const pages = [
    {
      path: '/contact/',
      title: /Contact/i,
      content: 'info@we-amp.com',
    },
    {
      path: '/security/',
      title: /Security/i,
      content: 'security@modpagespeed.com',
    },
    { path: '/license/', title: /License/i, content: 'Apache' },
    { path: '/terms/', title: /Terms/i, content: '' },
  ];

  for (const p of pages) {
    test(`${p.path} renders with correct title`, async ({ page }) => {
      await page.goto(p.path);
      await expect(page).toHaveTitle(p.title);
    });
  }

  for (const p of pages) {
    test(`${p.path} uses BaseLayout (header + footer)`, async ({ page }) => {
      await page.goto(p.path);
      await expect(page.locator('header nav')).toBeVisible();
      await expect(page.locator('footer')).toBeVisible();
    });
  }

  test('/contact/ renders contact information', async ({ page }) => {
    await page.goto('/contact/');
    // The contact page leads with a form; the one commercial address is
    // info@we-amp.com and security disclosures go to a separate channel. The
    // support subscription is a Topic option on the form (the old
    // enterprise and sales addresses are retired).
    await expect(page.locator('a[href="mailto:info@we-amp.com"]').first()).toBeVisible();
    await expect(page.locator('a[href="mailto:security@modpagespeed.com"]')).toBeVisible();
    await expect(page.locator('#contact-topic option[value="support"]')).toBeAttached();
  });

  // Retired pages forward to the software-license pages (astro.config.mjs
  // `redirects`): receipts, portal mails and old bookmarks keep resolving.
  const retired: Array<[string, string]> = [
    ['/claim-license/', '/license/'],
    ['/community-license/', '/license/'],
    ['/docs/license-activation/', '/docs/license/'],
  ];
  for (const [from, to] of retired) {
    test(`${from} is a 301 to ${to}`, async ({ request, baseURL }) => {
      const res = await request.get(from, { maxRedirects: 0 });
      expect(res.status()).toBe(301);
      expect(new URL(res.headers()['location'], baseURL).pathname).toBe(to);
    });
    test(`${from} lands on ${to} in a browser`, async ({ page }) => {
      await page.goto(from);
      expect(new URL(page.url()).pathname).toBe(to);
      await expect(page.locator('h1').first()).toBeVisible();
    });
  }

  test('/features/ renders all sections', async ({ page }) => {
    await page.goto('/features/');
    // Scope to the page hero h1 — the embedded console-demo preview adds its
    // own h1 nodes, so a bare `h1` locator is no longer strict-mode safe.
    await expect(page.locator('h1.page-hero-title')).toBeVisible();
    await expect(page.locator('header nav')).toBeVisible();
    await expect(page.locator('footer')).toBeVisible();
  });

  // Four product-level pages retired after their unique content was migrated
  // to their replacement pages. The server-side 301s live in a separate repo
  // (nginx), so these paths are simply gone here — a resurrected route file
  // would flip this test red.
  const retiredProductPages = ['/1.1/', '/1.1/apache/', '/1.1/cpanel/', '/1.1/migrate/'];
  for (const path of retiredProductPages) {
    test(`${path} is not served`, async ({ page }) => {
      const response = await page.goto(path);
      expect(response?.status()).toBe(404);
    });
  }

  test('/docs/cpanel/ renders with its H1', async ({ page }) => {
    await page.goto('/docs/cpanel/');
    await expect(page.locator('h1').first()).toBeVisible();
  });
});
