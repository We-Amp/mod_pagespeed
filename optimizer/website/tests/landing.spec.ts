// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

test.describe('Landing page', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/');
  });

  test('renders hero section with headline', async ({ page }) => {
    const h1 = page.locator('main h1').first();
    await expect(h1).toContainText('Lighthouse 56 to 90.');
    await expect(h1).toContainText('On your own servers.');
  });

  test('hero CTA buttons link correctly', async ({ page }) => {
    // Primary CTA names the action the click delivers ("Download & run") instead
    // of promising a trial start that actually happens in the admin console post-install.
    const hero = page.locator('main section').first();
    const startTrial = hero.locator('a:has-text("Download")').first();
    await expect(startTrial).toHaveAttribute('href', '/download/');

    const seeDemo = hero.locator('a:has-text("See the demo")').first();
    await expect(seeDemo).toHaveAttribute('href', '/demo/');
  });

  test('feature cards are visible (6 cards)', async ({ page }) => {
    await expect(
      page.locator('h3:has-text("One decode pass, up to 37 variants out")'),
    ).toBeVisible();
    await expect(page.locator('h3:has-text("Render-blocking CSS eliminated")')).toBeVisible();
    await expect(
      page.locator('h3:has-text("Cache hits are a pointer, not a pipeline")'),
    ).toBeVisible();
    await expect(page.locator('h3:has-text("Self-hosted by design")')).toBeVisible();
    await expect(page.locator('h3:has-text("Deliberately conservative")')).toBeVisible();
    await expect(page.locator('h3:has-text("The right bytes for every client")')).toBeVisible();
  });

  test('how-it-works section renders 3 steps', async ({ page }) => {
    await expect(page.locator('h2:has-text("Install. Configure. Verify.")')).toBeVisible();
    await expect(page.locator('h3:has-text("Install")')).toBeVisible();
    await expect(page.locator('h3:has-text("Tell it what to optimize")')).toBeVisible();
    await expect(page.locator('h3:has-text("Check the response headers")')).toBeVisible();
  });

  test('bottom CTA section is visible', async ({ page }) => {
    // Bottom CTA mirrors hero verb — "Download & run" + "Run the numbers".
    // The band's full-cyan background is retired (one cyan fill per viewport);
    // the CTA now lives in a card-featured panel labelled by #s-cta.
    const ctaSection = page.locator('section[aria-labelledby="s-cta"]');
    await expect(ctaSection.locator('h2')).toBeVisible();
    await expect(ctaSection.locator('a:has-text("Download")')).toHaveAttribute(
      'href',
      '/download/',
    );
    await expect(ctaSection.locator('a:has-text("Run the numbers")')).toHaveAttribute(
      'href',
      '/calculator/',
    );
  });

  test('all CTA links have valid hrefs', async ({ page }) => {
    const ctaLinks = page.locator('main a[href^="/"]');
    const hrefs = await ctaLinks.evaluateAll((els) => els.map((el) => el.getAttribute('href')));
    for (const href of hrefs) {
      expect(href).toBeTruthy();
      expect(href).toMatch(/^\//);
    }
  });

  test('page has correct title', async ({ page }) => {
    await expect(page).toHaveTitle(/PageSpeed module for nginx and Apache: mod_pagespeed$/i);
  });

  test('page has correct meta description', async ({ page }) => {
    const desc = page.locator('meta[name="description"]');
    await expect(desc).toHaveAttribute('content', /maintained again/i);
  });

  test('running-on-our-own-sites section shows the live console', async ({ page }) => {
    const section = page.locator('section[data-live-console]');
    await expect(section.locator('h2')).toHaveText('Running on our own sites');
    const img = section.locator('img[src="/images/live-console-overview.webp"]');
    await expect(img).toHaveAttribute('width', '1196');
    await expect(img).toHaveAttribute('height', '306');
    await expect(img).toHaveAttribute('loading', 'lazy');
    await expect(img).toHaveAttribute(
      'alt',
      /^Admin console Overview\. Module: .* Optimizer: running/,
    );
    await expect(section.locator('figure:has(img) figcaption')).toContainText('public on purpose');
    const link = section.locator('a[href="https://we-amp.com/pagespeed_global_admin/"]');
    await expect(link).toHaveText('Open the live console');
  });
});
