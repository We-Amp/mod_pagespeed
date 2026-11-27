// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

test.describe('No downloads (P0 regression)', () => {
  test('header nav links return HTML, not a download', async ({ page }) => {
    await page.goto('/');
    const navLinks = page.locator('header nav a[href^="/"]');
    const hrefs = await navLinks.evaluateAll((els) =>
      els.map((el) => el.getAttribute('href')).filter(Boolean),
    );
    const unique = [...new Set(hrefs)];
    expect(unique.length).toBeGreaterThan(0);

    for (const href of unique) {
      const response = await page.request.get(href!);
      const contentType = response.headers()['content-type'] ?? '';
      expect.soft(contentType, `${href} should return HTML`).toMatch(/text\/html/);
      const disposition = response.headers()['content-disposition'] ?? '';
      expect.soft(disposition, `${href} should not trigger download`).not.toMatch(/attachment/);
    }
  });

  test('footer links return HTML, not a download', async ({ page }) => {
    await page.goto('/');
    const footerLinks = page.locator('footer a[href^="/"]');
    const hrefs = await footerLinks.evaluateAll((els) =>
      els.map((el) => el.getAttribute('href')).filter(Boolean),
    );
    const unique = [...new Set(hrefs)];

    for (const href of unique) {
      // Skip legacy static paths not served by Astro dev server
      if (href!.startsWith('/1.0/') || href!.startsWith('/1.1/')) continue;
      const response = await page.request.get(href!);
      const contentType = response.headers()['content-type'] ?? '';
      // RSS/Atom feeds (e.g. /blog/rss.xml) are correctly served as
      // application/xml; everything else must be HTML. Neither must trigger a
      // download — that's the actual P0 regression this test guards.
      const isFeed = href!.endsWith('.xml');
      const expectedType = isFeed ? /application\/(xml|rss\+xml|atom\+xml)/ : /text\/html/;
      expect
        .soft(contentType, `${href} should return ${isFeed ? 'XML' : 'HTML'}`)
        .toMatch(expectedType);
      const disposition = response.headers()['content-disposition'] ?? '';
      expect.soft(disposition, `${href} should not trigger download`).not.toMatch(/attachment/);
    }
  });

  test('landing page CTA links return HTML', async ({ page }) => {
    await page.goto('/');
    const ctaLinks = page.locator('main a[href^="/"]');
    const hrefs = await ctaLinks.evaluateAll((els) =>
      els.map((el) => el.getAttribute('href')).filter(Boolean),
    );
    const unique = [...new Set(hrefs)];

    for (const href of unique) {
      const response = await page.request.get(href!);
      const contentType = response.headers()['content-type'] ?? '';
      expect.soft(contentType, `${href} should return HTML`).toMatch(/text\/html/);
    }
  });

  test('all internal routes serve text/html', async ({ request }) => {
    const routes = [
      '/',
      '/features/',
      '/pricing/',
      '/docs/',
      '/blog/',
      '/demo/',
      '/calculator/',
      '/contact/',
      '/security/',
      '/license/',
      '/terms/',
    ];

    for (const route of routes) {
      const response = await request.get(route);
      expect(response.status(), `${route} should return 200`).toBe(200);
      const contentType = response.headers()['content-type'] ?? '';
      expect.soft(contentType, `${route} should return HTML`).toMatch(/text\/html/);
    }
  });

  test('legacy docs serve HTML content', async ({ request }) => {
    // Legacy 1.0 docs are static files not served by Astro dev server.
    // This test validates them in production only.
    const routes = ['/1.0/', '/1.0/doc/index.html'];

    for (const route of routes) {
      const response = await request.get(route);
      // In dev mode, legacy static paths may 404 — skip gracefully
      if (response.status() === 404) continue;
      const contentType = response.headers()['content-type'] ?? '';
      expect.soft(contentType, `${route} should return HTML`).toMatch(/text\/html/);
    }
  });
});
