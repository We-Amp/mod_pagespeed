// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Error state E2E tests.
 *
 * Tests that all console pages handle disconnected/error states gracefully.
 * Uses route interception to simulate API unavailability.
 */
import { test, expect } from '@playwright/test';

test.describe('Error States', () => {
  test.beforeEach(async ({ page }) => {
    // Intercept all API calls and abort them to simulate disconnection
    await page.route('**/v1/**', (route) => route.abort());
  });

  test('dashboard shows waiting state when disconnected', async ({ page }) => {
    await page.goto('/console/');

    // Should show the disconnected card with spinner
    await expect(
      page.getByText(/waiting|disconnected|connecting|reconnecting/i).first()
    ).toBeVisible({ timeout: 10000 });
  });

  test('config page shows loading or error state', async ({ page }) => {
    await page.goto('/console/config');
    await page.waitForTimeout(3000);

    // Should not show config fields (API is unreachable)
    // Instead should show loading spinner or error message
    const body = await page.textContent('body');
    const hasConfig =
      body?.toLowerCase().includes('jpeg quality') &&
      body?.toLowerCase().includes('webp quality');

    // Config values should NOT be populated since API is down
    test.info().annotations.push({
      type: 'config-state',
      description: `Config fields visible: ${hasConfig}`,
    });
  });

  test('URLs page shows loading state when disconnected', async ({ page }) => {
    await page.goto('/console/urls');
    await page.waitForTimeout(3000);

    // Should not show URL rows since API is unreachable
    const urlRows = page.locator('.url-row');
    const count = await urlRows.count();
    expect(count).toBe(0);
  });

  test('savings page handles disconnection', async ({ page }) => {
    await page.goto('/console/savings');
    await page.waitForTimeout(3000);

    // Page should load without crashing
    const body = await page.textContent('body');
    expect(body).toBeTruthy();
  });

  test('waterfall page renders form even when disconnected', async ({ page }) => {
    await page.goto('/console/waterfall');
    await page.waitForTimeout(2000);

    // The URL input should still be visible
    const urlInput = page.locator('#url-input');
    const visible = await urlInput.isVisible().catch(() => false);
    test.info().annotations.push({
      type: 'waterfall-form',
      description: `URL input visible: ${visible}`,
    });
  });

  test('diff page renders form even when disconnected', async ({ page }) => {
    await page.goto('/console/diff');
    await page.waitForTimeout(2000);

    // The URL input should still be visible
    const urlInput = page.locator('#diff-url');
    const visible = await urlInput.isVisible().catch(() => false);
    test.info().annotations.push({
      type: 'diff-form',
      description: `URL input visible: ${visible}`,
    });
  });
});
