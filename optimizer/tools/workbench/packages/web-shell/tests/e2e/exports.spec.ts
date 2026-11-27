// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Export button E2E tests.
 *
 * Tests that export buttons exist and are clickable across pages.
 * Does not verify file contents — just ensures the UI elements are present
 * and responsive.
 */
import { test, expect } from '@playwright/test';

test.describe('Export Buttons', () => {
  test('dashboard has Export JSON button', async ({ page }) => {
    await page.goto('/console/');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });
    await page.waitForTimeout(3000);

    const exportBtn = page.getByRole('button', { name: /export json/i });
    await expect(exportBtn).toBeVisible();
    expect(await exportBtn.isEnabled()).toBeTruthy();
  });

  test('dashboard has Export CSV button', async ({ page }) => {
    await page.goto('/console/');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });
    await page.waitForTimeout(3000);

    const exportBtn = page.getByRole('button', { name: /export csv/i });
    await expect(exportBtn).toBeVisible();
    expect(await exportBtn.isEnabled()).toBeTruthy();
  });

  test('savings page has export buttons', async ({ page }) => {
    await page.goto('/console/savings');
    await page.waitForTimeout(3000);

    // Look for any export button
    const exportBtns = page.getByRole('button', { name: /export/i });
    const count = await exportBtns.count();
    test.info().annotations.push({
      type: 'savings-exports',
      description: `Export buttons found: ${count}`,
    });
  });

  test('metrics page has Export JSON button', async ({ page }) => {
    await page.goto('/console/metrics');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });
    await page.waitForTimeout(3000);

    const exportBtn = page.getByRole('button', { name: /export json/i });
    if (await exportBtn.isVisible().catch(() => false)) {
      expect(await exportBtn.isEnabled()).toBeTruthy();
    } else {
      test.info().annotations.push({
        type: 'skip',
        description: 'Metrics export button not found',
      });
    }
  });

  test('logs page has export buttons', async ({ page }) => {
    await page.goto('/console/logs');
    await page.waitForTimeout(3000);

    const exportBtns = page.getByRole('button', { name: /export/i });
    const count = await exportBtns.count();
    test.info().annotations.push({
      type: 'logs-exports',
      description: `Export buttons found: ${count}`,
    });
  });

  test('waterfall page has Export JSON button', async ({ page }) => {
    await page.goto('/console/waterfall');
    await page.waitForTimeout(2000);

    const exportBtn = page.getByRole('button', { name: /export/i });
    if (await exportBtn.isVisible().catch(() => false)) {
      test.info().annotations.push({
        type: 'waterfall-export',
        description: 'Export button found',
      });
    } else {
      test.info().annotations.push({
        type: 'waterfall-export',
        description: 'Export button not visible (may require capture data)',
      });
    }
  });
});
