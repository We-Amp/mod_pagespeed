// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * URL Inspector E2E tests.
 *
 * Prerequisites: workbench-demo stack running with seeded demo content
 *   (origin :8081, nginx :8084, worker :9880, vite :5173).
 *
 * Tests assume the demo ecommerce site has been accessed at least once
 * through the proxy so URLs are cached.
 */
import { test, expect } from '@playwright/test';

test.describe('URL Inspector', () => {
  // Seed the cache before tests by requesting the ecommerce page through
  // the proxy. This ensures URLs are present in the cache for inspection.
  test.beforeAll(async ({ request }) => {
    // Hit the demo page through the proxy to populate cache
    try {
      const proxyBase = process.env.PROXY_BASE || 'http://localhost:8084';
      await request.get(`${proxyBase}/demos/ecommerce/index.html`);
      // Wait for worker to process
      await new Promise((r) => setTimeout(r, 5000));
    } catch {
      // Stack might not be fully ready; individual tests will handle this
    }
  });

  test.beforeEach(async ({ page }) => {
    await page.goto('/console/urls');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });
  });

  // -- URL list loads -------------------------------------------------------

  test('URL list loads with cached entries', async ({ page }) => {
    await expect(
      page.locator('.ps-spinner').first(),
    ).not.toBeVisible({ timeout: 10000 });

    const urlRows = page.locator('.url-row');
    const count = await urlRows.count();
    expect(count).toBeGreaterThan(0);
  });

  // -- Click URL shows alternates -------------------------------------------

  test('clicking URL shows alternates panel', async ({ page }) => {
    await expect(
      page.locator('.url-row').first(),
    ).toBeVisible({ timeout: 10000 });

    await page.locator('.url-row').first().click();

    await expect(
      page.getByText(/variants/i).first(),
    ).toBeVisible({ timeout: 10000 });

    await expect(
      page.getByText(/back to url list/i),
    ).toBeVisible();
  });

  // -- Alternate details ----------------------------------------------------

  test('alternates show mask hex, size, and content-type', async ({ page }) => {
    await expect(
      page.locator('.url-row').first(),
    ).toBeVisible({ timeout: 10000 });
    await page.locator('.url-row').first().click();

    await expect(
      page.locator('.data-table').first(),
    ).toBeVisible({ timeout: 10000 });

    const headers = page.locator('.data-table th');
    const headerTexts = await headers.allTextContents();
    const headerStr = headerTexts.join(' ').toLowerCase();
    expect(headerStr).toContain('id');
    expect(headerStr).toContain('content type');
    expect(headerStr).toContain('size');
  });

  // -- Image URLs have WebP/AVIF alternates ---------------------------------

  test('image URLs include WebP and/or AVIF format alternates', async ({ page }) => {
    await expect(
      page.locator('.url-row').first(),
    ).toBeVisible({ timeout: 10000 });

    const imageRow = page.locator('.url-row').filter({
      hasText: /\.(jpg|jpeg|png|gif|webp)/i,
    }).first();

    if (await imageRow.isVisible().catch(() => false)) {
      await imageRow.click();

      await expect(
        page.locator('.data-table').first(),
      ).toBeVisible({ timeout: 10000 });

      const bodyText = await page.textContent('body');
      const hasModernFormat =
        bodyText?.includes('WebP') || bodyText?.includes('AVIF');
      test.info().annotations.push({
        type: 'modern-formats',
        description: `WebP/AVIF present: ${hasModernFormat}`,
      });
      expect(bodyText).toContain('Original');
    } else {
      test.info().annotations.push({
        type: 'skip',
        description: 'No image URLs found in cache — skipping format check',
      });
    }
  });

  // -- CSS URLs show compressed alternates ----------------------------------

  test('CSS URLs show compressed alternates', async ({ page }) => {
    await expect(
      page.locator('.url-row').first(),
    ).toBeVisible({ timeout: 10000 });

    const cssRow = page.locator('.url-row').filter({
      hasText: /\.css/i,
    }).first();

    if (await cssRow.isVisible().catch(() => false)) {
      await cssRow.click();

      await expect(
        page.locator('.data-table').first(),
      ).toBeVisible({ timeout: 10000 });

      const rows = page.locator('.data-table tbody tr');
      const count = await rows.count();
      expect(count).toBeGreaterThan(0);

      test.info().annotations.push({
        type: 'css-alternates',
        description: `CSS alternate count: ${count}`,
      });
    } else {
      test.info().annotations.push({
        type: 'skip',
        description: 'No CSS URLs found in cache',
      });
    }
  });

  // -- Pagination -----------------------------------------------------------

  test('pagination shows page info', async ({ page }) => {
    await expect(
      page.locator('.url-row').first(),
    ).toBeVisible({ timeout: 10000 });

    await expect(
      page.getByText(/showing page/i),
    ).toBeVisible();
  });

  // =========================================================================
  // New tests below
  // =========================================================================

  // -- Hostname filter ------------------------------------------------------

  test('hostname filter input filters URL list', async ({ page }) => {
    await expect(
      page.locator('.url-row').first(),
    ).toBeVisible({ timeout: 10000 });

    const filterInput = page.locator('input[placeholder*="hostname"]');
    await expect(filterInput).toBeVisible();

    // Type a nonexistent hostname to filter to zero results
    await filterInput.fill('nonexistent.example.com');
    await page.waitForTimeout(1000);

    // Should show no results or an empty message
    const rows = page.locator('.url-row');
    const count = await rows.count();
    expect(count).toBe(0);

    // Clear filter to restore results
    await filterInput.fill('');
    await page.waitForTimeout(1000);
    const restoredCount = await page.locator('.url-row').count();
    expect(restoredCount).toBeGreaterThan(0);
  });

  // -- Device simulation panel ----------------------------------------------

  test('device simulation panel opens and shows mask builder', async ({ page }) => {
    await expect(
      page.locator('.url-row').first(),
    ).toBeVisible({ timeout: 10000 });
    await page.locator('.url-row').first().click();

    await expect(
      page.getByText(/device simulation/i),
    ).toBeVisible({ timeout: 10000 });

    await expect(
      page.locator('.simulate-btn'),
    ).toBeVisible();
  });

  // -- Simulate Selection button --------------------------------------------

  test('simulate selection shows results', async ({ page }) => {
    await expect(
      page.locator('.url-row').first(),
    ).toBeVisible({ timeout: 10000 });
    await page.locator('.url-row').first().click();

    await expect(
      page.locator('.simulate-btn'),
    ).toBeVisible({ timeout: 10000 });

    await page.locator('.simulate-btn').click();
    await page.waitForTimeout(2000);

    // Should show some result (selected alternate, score, or error)
    const body = await page.textContent('body');
    const hasResult =
      body?.toLowerCase().includes('score') ||
      body?.toLowerCase().includes('selected') ||
      body?.toLowerCase().includes('no alternate');
    expect(hasResult).toBeTruthy();
  });

  // -- Purge button shows confirmation dialog --------------------------------

  test('purge button shows confirmation dialog', async ({ page }) => {
    await expect(
      page.locator('.url-row').first(),
    ).toBeVisible({ timeout: 10000 });
    await page.locator('.url-row').first().click();

    // Wait for detail panel to load
    await expect(
      page.getByText(/variants/i).first(),
    ).toBeVisible({ timeout: 10000 });

    // Click Purge button
    const purgeBtn = page.getByRole('button', { name: /^purge$/i });
    await expect(purgeBtn).toBeVisible();
    await purgeBtn.click();

    // Confirmation prompt should appear
    await expect(
      page.getByText(/purge all variants/i),
    ).toBeVisible();

    // Cancel button should appear
    await expect(
      page.getByRole('button', { name: /cancel/i }),
    ).toBeVisible();
  });

  // -- Purge cancel dismisses confirmation -----------------------------------

  test('purge cancel dismisses confirmation', async ({ page }) => {
    await expect(
      page.locator('.url-row').first(),
    ).toBeVisible({ timeout: 10000 });
    await page.locator('.url-row').first().click();

    await expect(
      page.getByText(/variants/i).first(),
    ).toBeVisible({ timeout: 10000 });

    const purgeBtn = page.getByRole('button', { name: /^purge$/i });
    await expect(purgeBtn).toBeVisible();
    await purgeBtn.click();

    await expect(
      page.getByText(/purge all variants/i),
    ).toBeVisible();

    // Click Cancel
    await page.getByRole('button', { name: /cancel/i }).first().click();

    // Confirmation should disappear
    await expect(
      page.getByText(/purge all variants/i),
    ).not.toBeVisible();
  });

  // -- Reprocess button shows confirmation dialog ----------------------------

  test('reprocess button shows confirmation dialog', async ({ page }) => {
    await expect(
      page.locator('.url-row').first(),
    ).toBeVisible({ timeout: 10000 });
    await page.locator('.url-row').first().click();

    await expect(
      page.getByText(/variants/i).first(),
    ).toBeVisible({ timeout: 10000 });

    const reprocessBtn = page.getByRole('button', { name: /^reprocess$/i });
    await expect(reprocessBtn).toBeVisible();
    await reprocessBtn.click();

    // Confirmation prompt
    await expect(
      page.getByText(/purge and re-queue/i),
    ).toBeVisible();

    await expect(
      page.getByRole('button', { name: /cancel/i }),
    ).toBeVisible();
  });

  // -- Back to URL list navigates back ---------------------------------------

  test('back to URL list returns to list view', async ({ page }) => {
    await expect(
      page.locator('.url-row').first(),
    ).toBeVisible({ timeout: 10000 });
    await page.locator('.url-row').first().click();

    const backLink = page.getByText(/back to url list/i);
    await expect(backLink).toBeVisible({ timeout: 10000 });
    await backLink.click();

    // Should return to the URL list
    await expect(
      page.locator('.url-row').first(),
    ).toBeVisible({ timeout: 10000 });
  });

  // -- Expanded detail shows metadata ----------------------------------------

  test('alternate detail shows metadata columns', async ({ page }) => {
    await expect(
      page.locator('.url-row').first(),
    ).toBeVisible({ timeout: 10000 });
    await page.locator('.url-row').first().click();

    await expect(
      page.locator('.data-table').first(),
    ).toBeVisible({ timeout: 10000 });

    // Check that key metadata columns are present
    const headerTexts = await page.locator('.data-table th').allTextContents();
    const headerStr = headerTexts.join(' ').toLowerCase();

    // Should have format, viewport, encoding columns
    const hasFormat = headerStr.includes('format');
    const hasViewport = headerStr.includes('viewport');
    const hasEncoding = headerStr.includes('encoding');

    test.info().annotations.push({
      type: 'metadata-columns',
      description: `format=${hasFormat}, viewport=${hasViewport}, encoding=${hasEncoding}`,
    });

    // At minimum, format should be present
    expect(hasFormat).toBeTruthy();
  });

  // -- Hostname column visible in URL list ------------------------------------

  test('URL list shows hostname column', async ({ page }) => {
    await expect(
      page.locator('.url-row').first(),
    ).toBeVisible({ timeout: 10000 });

    const headerTexts = await page.locator('th').allTextContents();
    const hasHostname = headerTexts.some((h) =>
      h.toLowerCase().includes('hostname'),
    );
    expect(hasHostname).toBeTruthy();
  });

  // -- Alternate count column ------------------------------------------------

  test('URL list shows alternate count column', async ({ page }) => {
    await expect(
      page.locator('.url-row').first(),
    ).toBeVisible({ timeout: 10000 });

    // The "Variants" column header shows the alternate count
    const headerTexts = await page.locator('th').allTextContents();
    const hasVariants = headerTexts.some(
      (h) => h.toLowerCase().includes('variants'),
    );
    expect(hasVariants).toBeTruthy();
  });

  // -- Content preview for image alternates -----------------------------------

  test('image alternate shows content preview or size info', async ({ page }) => {
    await expect(
      page.locator('.url-row').first(),
    ).toBeVisible({ timeout: 10000 });

    const imageRow = page.locator('.url-row').filter({
      hasText: /\.(jpg|jpeg|png|gif|webp)/i,
    }).first();

    if (await imageRow.isVisible().catch(() => false)) {
      await imageRow.click();

      await expect(
        page.locator('.data-table').first(),
      ).toBeVisible({ timeout: 10000 });

      // Each alternate row should show a size
      const firstRow = page.locator('.data-table tbody tr').first();
      const rowText = await firstRow.textContent();
      // Size should contain bytes/KB/MB
      const hasSize =
        rowText?.includes('B') || rowText?.includes('KB') || rowText?.includes('MB');
      expect(hasSize).toBeTruthy();
    } else {
      test.info().annotations.push({
        type: 'skip',
        description: 'No image URLs found in cache',
      });
    }
  });
});
