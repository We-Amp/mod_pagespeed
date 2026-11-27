// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

// /ai-readability/hosted-builders/: the guide for sites on Wix, Squarespace
// or Shopify. Static copy; nothing here calls the scanner or a vendor site.

const PATH = '/ai-readability/hosted-builders/';
const VENDOR_HOSTS = [
  'support.wix.com',
  'support.squarespace.com',
  'help.shopify.com',
  'shopify.dev',
];

test.describe('/ai-readability/hosted-builders/', () => {
  test('has its heading, a description within the length gate and a canonical URL', async ({
    page,
  }) => {
    await page.goto(PATH);
    await expect(page.locator('main h1')).toHaveText(
      'AI readability on Wix, Squarespace and Shopify: what you can change',
    );
    const description = await page.locator('meta[name="description"]').getAttribute('content');
    expect(description!.length).toBeGreaterThanOrEqual(120);
    expect(description!.length).toBeLessThanOrEqual(155);
    await expect(page.locator('link[rel="canonical"]')).toHaveAttribute(
      'href',
      'https://modpagespeed.com/ai-readability/hosted-builders/',
    );
  });

  test('says plainly that mod_pagespeed cannot be installed on a hosted builder', async ({
    page,
  }) => {
    await page.goto(PATH);
    const scope = page.locator('[data-hosted-scope]');
    await expect(scope).toContainText('This page sells nothing.');
    await expect(scope).toContainText('cannot be installed on Wix, Squarespace or Shopify');
  });

  test('every question in the FAQPage structured data is on the page with its answer', async ({
    page,
  }) => {
    await page.goto(PATH);
    const blocks = await page.locator('script[type="application/ld+json"]').allTextContents();
    const nodes = blocks.flatMap((b) => [JSON.parse(b)].flat());
    const faq = nodes.find((n) => n['@type'] === 'FAQPage');
    expect(faq).toBeTruthy();
    expect(faq.mainEntity.length).toBe(4);
    const visible = (await page.locator('main details').allTextContents()).map((t) =>
      t.replace(/\s+/g, ' ').trim(),
    );
    expect(visible.length).toBe(faq.mainEntity.length);
    for (const item of faq.mainEntity) {
      const match = visible.find((t) => t.startsWith(item.name));
      expect(match, item.name).toBeTruthy();
      expect(match).toBe(`${item.name} ${item.acceptedAnswer.text}`);
    }
  });

  test('the breadcrumb structured data leads back to the checker', async ({ page }) => {
    await page.goto(PATH);
    const blocks = await page.locator('script[type="application/ld+json"]').allTextContents();
    const nodes = blocks.flatMap((b) => [JSON.parse(b)].flat());
    const crumbs = nodes.find((n) => n['@type'] === 'BreadcrumbList');
    expect(crumbs.itemListElement.map((i: { name: string }) => i.name)).toEqual([
      'Home',
      'AI readability checker',
      'Hosted builders',
    ]);
    await expect(page.locator('main nav[aria-label="Breadcrumb"]')).toHaveText(
      /^\s*AI readability checker\s*\/\s*Hosted builders\s*$/,
    );
    expect(crumbs.itemListElement.map((i: { item: string }) => i.item)).toEqual([
      'https://modpagespeed.com/',
      'https://modpagespeed.com/ai-readability/',
      'https://modpagespeed.com/ai-readability/hosted-builders/',
    ]);
  });

  test('every external link opens in a new tab, says so, and goes to a vendor’s documentation', async ({
    page,
  }) => {
    await page.goto(PATH);
    const links = await page.locator('main a[href^="http"]').evaluateAll((anchors) =>
      anchors.map((a) => ({
        href: a.getAttribute('href') ?? '',
        target: a.getAttribute('target'),
        rel: a.getAttribute('rel'),
        // Not trimmed: a space at either end of a link would be underlined.
        text: a.textContent ?? '',
      })),
    );
    expect(links.length).toBe(10);
    for (const link of links) {
      expect(VENDOR_HOSTS, link.href).toContain(new URL(link.href).host);
      expect(link.target, link.href).toBe('_blank');
      expect(link.rel, link.href).toBe('noopener noreferrer');
      expect(link.text, link.href).toMatch(/^\S.*\S \(opens in new tab\)$/);
    }
  });

  test('each platform section carries a source link for every statement', async ({ page }) => {
    await page.goto(PATH);
    for (const [id, count] of [
      ['wix', 4],
      ['squarespace', 3],
      ['shopify', 3],
    ] as const) {
      const section = page.locator(`[data-hosted-platform="${id}"]`);
      await expect(section.locator('dt')).toHaveCount(count);
      await expect(section.locator('dd a[target="_blank"]')).toHaveCount(count);
    }
  });

  test('links to the checker and the download page, and the checker links here once', async ({
    page,
  }) => {
    await page.goto(PATH);
    // No underlined space at the end of a link in the running text.
    const texts = await page.locator('main article p a, main article nav a').allTextContents();
    for (const text of texts) expect(text, text).toBe(text.trim());
    await expect(page.locator('main p a[href="/ai-readability/"]')).toHaveCount(1);
    await expect(page.locator('main p a[href="/download/"]')).toHaveCount(1);
    await page.goto('/ai-readability/');
    await expect(page.locator(`main a[href="${PATH}"]`)).toHaveCount(1);
  });
});
