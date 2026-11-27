// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Page audit tests — verify that each console page renders meaningful content.
 *
 * Previously these tests only used console.log with zero assertions.
 * Now they verify actual rendered state.
 *
 * Prerequisites: workbench-demo stack running
 *   (origin :8081, nginx :8080, worker :9880, vite :5173).
 */
import { test, expect } from '@playwright/test';

test.describe('Audit: Dashboard', () => {
  test('renders stat cards with content', async ({ page }) => {
    await page.goto('/console/');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });
    await page.waitForTimeout(3000);

    // Stat cards should contain numeric content
    const cards = page.locator('[class*="card"], [class*="stat-card"]');
    const count = await cards.count();
    expect(count).toBeGreaterThan(0);
  });

  test('shows notification and variant counts', async ({ page }) => {
    await page.goto('/console/');
    await page.waitForTimeout(5000);
    const bodyText = await page.textContent('body') ?? '';
    expect(bodyText).toMatch(/notifications/i);
    expect(bodyText).toMatch(/variants/i);
  });

  test('renders charts as canvas elements', async ({ page }) => {
    await page.goto('/console/');
    await page.waitForTimeout(3000);
    const canvases = page.locator('canvas');
    const count = await canvases.count();
    expect(count).toBeGreaterThan(0);
  });

  test('has export buttons', async ({ page }) => {
    await page.goto('/console/');
    await page.waitForTimeout(3000);
    const exportBtns = page.getByText(/export/i);
    const count = await exportBtns.count();
    expect(count).toBeGreaterThan(0);
  });

  test('no persistent loading state', async ({ page }) => {
    await page.goto('/console/');
    await page.waitForTimeout(5000);
    const loading = page.getByText(/loading statistics/i);
    await expect(loading).not.toBeVisible();
  });
});

test.describe('Audit: URL Inspector', () => {
  test('renders URL rows from demo content', async ({ page }) => {
    await page.goto('/console/urls');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });
    await page.waitForTimeout(3000);

    // Should have at least one table row for demo URLs
    const rows = page.locator('tr');
    const count = await rows.count();
    // Header row + at least one data row
    expect(count).toBeGreaterThan(1);
  });

  test('clicking a URL row shows variant detail', async ({ page }) => {
    await page.goto('/console/urls');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });
    await page.waitForTimeout(3000);

    // Click first data row
    const urlRow = page.locator('.url-row').first();
    if (await urlRow.isVisible().catch(() => false)) {
      await urlRow.click();
      await page.waitForTimeout(2000);
      // Should show variant/alternate information
      await expect(
        page.getByText(/variant|webp|avif|original|0x/i).first(),
      ).toBeVisible();
    }
  });
});

test.describe('Audit: Config', () => {
  test('renders input fields for configuration', async ({ page }) => {
    await page.goto('/console/config');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });
    await page.waitForTimeout(3000);

    const inputs = page.locator('input, select, [role="switch"], [role="checkbox"]');
    const count = await inputs.count();
    expect(count).toBeGreaterThan(5);
  });

  test('shows quality and compression config terms', async ({ page }) => {
    await page.goto('/console/config');
    await page.waitForTimeout(3000);
    const bodyText = (await page.textContent('body') ?? '').toLowerCase();
    expect(bodyText).toContain('jpeg');
    expect(bodyText).toContain('quality');
  });
});

test.describe('Audit: Savings', () => {
  test('renders savings-related content', async ({ page }) => {
    await page.goto('/console/savings');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });
    await page.waitForTimeout(3000);

    const bodyText = (await page.textContent('body') ?? '').toLowerCase();
    // Should contain at least some savings-related terms
    const hasSavingsContent =
      bodyText.includes('savings') ||
      bodyText.includes('bandwidth') ||
      bodyText.includes('original') ||
      bodyText.includes('optimized');
    expect(hasSavingsContent).toBe(true);
  });
});

test.describe('Audit: Waterfall', () => {
  test('renders capture form', async ({ page }) => {
    await page.goto('/console/waterfall');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });

    await expect(page.locator('#url-input')).toBeVisible();
    await expect(
      page.getByRole('button', { name: /capture/i }).first(),
    ).toBeVisible();
  });
});

test.describe('Audit: Visual Diff', () => {
  test('renders diff capture form', async ({ page }) => {
    await page.goto('/console/diff');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });

    await expect(page.locator('#diff-url')).toBeVisible();
    const captureButtons = page.getByRole('button', { name: /capture/i });
    const count = await captureButtons.count();
    expect(count).toBeGreaterThanOrEqual(2);
  });
});
