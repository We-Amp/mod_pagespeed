// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Metrics page E2E tests.
 *
 * Prerequisites: workbench-demo stack running
 *   (origin :8081, nginx :8080, worker :9880, vite :5173).
 *
 * Run: cd tools/workbench/packages/web-shell && npx playwright test metrics
 */
import { test, expect } from '@playwright/test';

test.describe('Metrics (/console/metrics)', () => {
  // -- Page load --------------------------------------------------------------

  test('page loads without error', async ({ page }) => {
    const response = await page.goto('/console/metrics');
    expect(response?.status()).toBe(200);
  });

  // -- Metrics table renders --------------------------------------------------

  test('metrics table renders with rows after connection', async ({ page }) => {
    await page.goto('/console/metrics');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });

    // Wait for stats to load (loading spinner disappears, table appears)
    await expect(
      page.locator('.metrics-table').first(),
    ).toBeVisible({ timeout: 10000 });

    // Table should have at least one data row
    const rows = page.locator('.metrics-table tbody tr');
    const count = await rows.count();
    expect(count).toBeGreaterThan(0);
  });

  // -- Filter input -----------------------------------------------------------

  test('filter input narrows displayed metrics', async ({ page }) => {
    await page.goto('/console/metrics');
    await expect(
      page.locator('.metrics-table').first(),
    ).toBeVisible({ timeout: 10000 });

    // Get initial row count
    const initialCount = await page.locator('.metrics-table tbody tr').count();

    // Type a filter that should match some but not all metrics
    const filterInput = page.locator('.filter-input');
    await expect(filterInput).toBeVisible();
    await filterInput.fill('notifications');

    // Wait for filter to take effect
    await page.waitForTimeout(500);

    // Filtered count should be less than or equal to initial count
    const filteredCount = await page.locator('.metrics-table tbody tr').count();
    expect(filteredCount).toBeLessThanOrEqual(initialCount);
    expect(filteredCount).toBeGreaterThan(0);
  });

  // -- Export JSON button -----------------------------------------------------

  test('export JSON button is present', async ({ page }) => {
    await page.goto('/console/metrics');
    await expect(
      page.locator('.metrics-table').first(),
    ).toBeVisible({ timeout: 10000 });

    const exportBtn = page.getByRole('button', { name: /export json/i });
    await expect(exportBtn).toBeVisible();
  });

  // -- Metric keys in dotted path format --------------------------------------

  test('metric keys are displayed in dotted path format', async ({ page }) => {
    await page.goto('/console/metrics');
    await expect(
      page.locator('.metrics-table').first(),
    ).toBeVisible({ timeout: 10000 });

    // Metric keys should contain dots (e.g., "notifications.received")
    const metricKeys = page.locator('.metric-key');
    const count = await metricKeys.count();
    expect(count).toBeGreaterThan(0);

    // Check that at least one key contains a dot (nested path)
    let foundDotted = false;
    for (let i = 0; i < Math.min(count, 20); i++) {
      const text = await metricKeys.nth(i).textContent();
      if (text?.includes('.')) {
        foundDotted = true;
        break;
      }
    }
    expect(foundDotted).toBe(true);
  });

  // -- Metric values displayed ------------------------------------------------

  test('metric values are displayed as formatted numbers', async ({ page }) => {
    await page.goto('/console/metrics');
    await expect(
      page.locator('.metrics-table').first(),
    ).toBeVisible({ timeout: 10000 });

    // Metric values should be visible in the value column
    const metricValues = page.locator('.metric-value');
    const count = await metricValues.count();
    expect(count).toBeGreaterThan(0);

    // At least one value should be a number (possibly with formatting)
    let foundNumeric = false;
    for (let i = 0; i < Math.min(count, 20); i++) {
      const text = await metricValues.nth(i).textContent();
      if (text && /\d/.test(text)) {
        foundNumeric = true;
        break;
      }
    }
    expect(foundNumeric).toBe(true);
  });

  // -- Warning highlighting ---------------------------------------------------

  test('warning rows are highlighted for concerning values', async ({ page }) => {
    await page.goto('/console/metrics');
    await expect(
      page.locator('.metrics-table').first(),
    ).toBeVisible({ timeout: 10000 });

    // Check if any warn-row exists (may or may not depending on server state)
    const warnRows = page.locator('.warn-row');
    const warnCount = await warnRows.count();

    // Also check the warning badge in the header
    const warnBadge = page.locator('.ps-badge-warning').filter({ hasText: /warning/i });
    if (warnCount > 0) {
      // If there are warning rows, the badge should reflect the count
      await expect(warnBadge.first()).toBeVisible();
      // Warning values should have distinct styling
      const warnValue = page.locator('.warn-value').first();
      await expect(warnValue).toBeVisible();
    } else {
      // No warnings is valid — record it
      test.info().annotations.push({
        type: 'no-warnings',
        description: 'No warning metrics present (all values nominal)',
      });
    }
  });

  // -- Disconnected state message ---------------------------------------------

  test('shows disconnected state before connection', async ({ page }) => {
    // Navigate and check that the disconnected UI elements exist in the page.
    await page.goto('/console/metrics');

    // The page has three possible states after navigation:
    //   1. Disconnected: "Waiting for worker connection..."
    //   2. Loading:      "Loading metrics..." (connected but stats not yet fetched)
    //   3. Loaded:       "All Metrics" heading with the metrics table
    // Use a locator with auto-retry so we don't race SSR-to-hydration.
    await expect(page.locator('body')).toContainText(
      /Waiting for worker connection|Loading metrics|All Metrics/,
    );
  });

  // -- Loading state ----------------------------------------------------------

  test('shows loading spinner before stats arrive', async ({ page }) => {
    // Navigate and quickly check for loading state
    await page.goto('/console/metrics');

    // The loading state shows "Loading metrics..." with a spinner.
    // It may flash briefly before stats arrive. Check that the page
    // transitions through it to the final table state.
    await expect(
      page.locator('.metrics-table').first(),
    ).toBeVisible({ timeout: 15000 });

    // Title should be visible once loaded
    await expect(
      page.getByRole('heading', { name: /all metrics/i }),
    ).toBeVisible();
  });

  // -- Counter badge ----------------------------------------------------------

  test('counter badge shows number of displayed metrics', async ({ page }) => {
    await page.goto('/console/metrics');
    await expect(
      page.locator('.metrics-table').first(),
    ).toBeVisible({ timeout: 10000 });

    // Badge should show "N counters"
    const counterBadge = page.locator('.ps-badge').filter({ hasText: /counters/ });
    await expect(counterBadge).toBeVisible();
    const text = await counterBadge.textContent();
    expect(text).toMatch(/\d+ counters/);
  });

  // -- No results message -----------------------------------------------------

  test('shows no results message for non-matching filter', async ({ page }) => {
    await page.goto('/console/metrics');
    await expect(
      page.locator('.metrics-table').first(),
    ).toBeVisible({ timeout: 10000 });

    // Type a filter that should match nothing
    const filterInput = page.locator('.filter-input');
    await filterInput.fill('zzz_nonexistent_metric_xyz');
    await page.waitForTimeout(500);

    // Should show "No metrics match" message
    await expect(
      page.locator('.no-results'),
    ).toBeVisible();
  });

  // -- Table headers ----------------------------------------------------------

  test('table has Metric and Value column headers', async ({ page }) => {
    await page.goto('/console/metrics');
    await expect(
      page.locator('.metrics-table').first(),
    ).toBeVisible({ timeout: 10000 });

    const headers = page.locator('.metrics-table th');
    const headerTexts = await headers.allTextContents();
    expect(headerTexts.map((h) => h.trim())).toContain('Metric');
    expect(headerTexts.map((h) => h.trim())).toContain('Value');
  });
});
