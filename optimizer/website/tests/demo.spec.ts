// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

test.describe('Demo page', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/demo/');
  });

  test('renders 4 demo site tabs', async ({ page }) => {
    await expect(page.locator('#tab-ecommerce')).toBeVisible();
    await expect(page.locator('#tab-blog')).toBeVisible();
    await expect(page.locator('#tab-news')).toBeVisible();
    await expect(page.locator('#tab-portfolio')).toBeVisible();
  });

  test('first tab is selected by default', async ({ page }) => {
    await expect(page.locator('#tab-ecommerce')).toHaveAttribute('aria-selected', 'true');
    await expect(page.locator('#tab-blog')).toHaveAttribute('aria-selected', 'false');
  });

  test('clicking tab shows corresponding panel', async ({ page }) => {
    await page.locator('#tab-blog').click();

    await expect(page.locator('#preview-blog')).toBeVisible();
    await expect(page.locator('#preview-ecommerce')).toBeHidden();

    await expect(page.locator('#tab-blog')).toHaveAttribute('aria-selected', 'true');
    await expect(page.locator('#tab-ecommerce')).toHaveAttribute('aria-selected', 'false');
  });

  test('tabs use ARIA tablist/tab/tabpanel roles', async ({ page }) => {
    await expect(page.locator('#demo-tabs')).toHaveAttribute('role', 'tablist');
    const tabs = page.locator('[role="tab"]');
    expect(await tabs.count()).toBe(4);

    const panels = page.locator('[role="tabpanel"]');
    expect(await panels.count()).toBe(4);
  });

  test('demo iframe loads for selected tab', async ({ page }) => {
    const iframe = page.locator('#preview-ecommerce iframe');
    await expect(iframe).toBeAttached();
    const src = await iframe.getAttribute('src');
    expect(src).toBe('/demos/ecommerce/index.html');
  });

  test('URL bar updates when switching tabs', async ({ page }) => {
    await expect(page.locator('#demo-url-bar')).toHaveText(/ecommerce/);

    await page.locator('#tab-blog').click();
    await expect(page.locator('#demo-url-bar')).toHaveText(/blog/);
  });

  test('open link updates when switching tabs', async ({ page }) => {
    await page.locator('#tab-news').click();
    await expect(page.locator('#demo-open-link')).toHaveAttribute('href', '/demos/news/index.html');
  });

  test('site picker buttons are visible', async ({ page }) => {
    const picker = page.locator('#site-picker');
    const buttons = picker.locator('button[data-site]');
    expect(await buttons.count()).toBe(4);
  });

  test('clicking site picker shows analysis panel', async ({ page }) => {
    const blogBtn = page.locator('#site-picker button[data-site="blog"]');
    await blogBtn.click();

    await expect(page.locator('#panel-blog')).toBeVisible();
    await expect(page.locator('#panel-ecommerce')).toBeHidden();
  });

  test('resource breakdown shows before/after byte deltas', async ({ page }) => {
    // The first panel (ecommerce) shows the per-resource breakdown as
    // ByteDelta plates. The base page's table row labels — "CSS (minified)"
    // and "JavaScript (minified)" — are kept verbatim as the plate headings,
    // so the resource each plate measures stays named. The Total plate leads,
    // carrying the table's former Total row (the per-site reduction that
    // backs the lede's 48-69% claim).
    const panel = page.locator('#panel-ecommerce');
    await expect(panel.locator('h3:has-text("Resource breakdown")')).toBeVisible();
    await expect(panel.locator('[data-ui="byte-delta"]')).toHaveCount(5);
    await expect(panel.getByText('Total', { exact: true })).toBeVisible();
    await expect(panel.getByText('CSS (minified)')).toBeVisible();
    await expect(panel.getByText('JavaScript (minified)')).toBeVisible();
  });

  test('CSS minification example is visible', async ({ page }) => {
    await expect(
      page.locator('#panel-ecommerce h3:has-text("CSS minification example")'),
    ).toBeVisible();
    // Should have before and after code blocks
    const codeBlocks = page.locator('#panel-ecommerce pre code');
    expect(await codeBlocks.count()).toBeGreaterThanOrEqual(2);
  });

  test('all tab panels have content', async ({ page }) => {
    const tabIds = ['ecommerce', 'blog', 'news', 'portfolio'];
    for (const id of tabIds) {
      await page.locator(`#tab-${id}`).click();
      const panel = page.locator(`#preview-${id}`);
      await expect(panel).toBeVisible();
      const iframe = panel.locator('iframe');
      await expect(iframe).toBeAttached();
    }
  });

  test('methodology section is visible', async ({ page }) => {
    await expect(page.locator('h2:has-text("Methodology")')).toBeVisible();
  });

  test('CTA section at bottom links to /download/', async ({ page }) => {
    // Download-first funnel — the bottom CTA reads "Download & run". The CTA
    // section background is the brand interactive token (bg-interactive), not
    // the retired bg-blue-700 utility.
    await expect(page.locator('section.bg-interactive a:has-text("Download")')).toHaveAttribute(
      'href',
      '/download/',
    );
  });
});
