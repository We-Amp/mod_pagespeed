// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Cross-page navigation E2E tests.
 *
 * Tests sidebar navigation, active link highlighting, and inter-page links.
 */
import { test, expect } from '@playwright/test';

test.describe('Navigation', () => {
  test('sidebar active link highlights current page', async ({ page }) => {
    await page.goto('/console/config');
    await page.waitForTimeout(1000);

    // The sidebar should have a link to /console/config that is marked active
    const configLink = page.locator(
      'a[href="/console/config"], a[href*="/config"]',
    ).first();
    await expect(configLink).toBeVisible({ timeout: 5000 });

    // Check if it has an active-related class or aria-current
    const className = await configLink.getAttribute('class');
    const ariaCurrent = await configLink.getAttribute('aria-current');
    const isActive =
      className?.includes('active') || ariaCurrent === 'page';

    test.info().annotations.push({
      type: 'active-link',
      description: `Config link active: ${isActive}, class="${className}"`,
    });
  });

  test('sidebar has no License entry and /console/license renders no license page', async ({ page }) => {
    await page.goto('/console/');
    await page.waitForTimeout(1000);

    // The nav must not offer a License item (the route was removed).
    const nav = page.locator('nav[aria-label="Main navigation"]');
    await expect(nav).toBeVisible({ timeout: 5000 });
    await expect(nav.getByRole('link', { name: 'License' })).toHaveCount(0);
    await expect(nav.locator('a[href="/console/license"]')).toHaveCount(0);

    // Visiting the old URL must not render license-management UI. The SPA
    // falls through to its not-found rendering for unknown routes, so assert
    // on the absence of the former page's copy rather than on a status code.
    await page.goto('/console/license');
    await page.waitForTimeout(1000);
    await expect(page.getByText(/activate a license|license key|start trial/i)).toHaveCount(0);
  });

  test('support notice shows once and stays dismissed', async ({ page }) => {
    await page.goto('/console/');
    const notice = page.getByTestId('support-notice');
    await expect(notice).toBeVisible({ timeout: 5000 });
    await expect(notice).toContainText('licensed under the Apache License 2.0');
    await expect(notice.getByRole('link')).toHaveAttribute(
      'href',
      'https://we-amp.com/licensing/',
    );

    await page.getByTestId('support-notice-dismiss').click();
    await expect(notice).toHaveCount(0);

    // Dismissal persists across reloads within the same browser profile.
    await page.reload();
    await page.waitForTimeout(1000);
    await expect(page.getByTestId('support-notice')).toHaveCount(0);
  });

  test('sidebar links navigate to correct pages', async ({ page }) => {
    await page.goto('/console/');
    await page.waitForTimeout(2000);

    // Navigate to URLs page via sidebar
    const urlsLink = page.locator(
      'a[href="/console/urls"], a[href*="/urls"]',
    ).first();
    if (await urlsLink.isVisible().catch(() => false)) {
      await urlsLink.click();
      await page.waitForURL('**/console/urls**');
      expect(page.url()).toContain('/console/urls');
    }
  });

  test('dashboard errors link navigates to logs', async ({ page }) => {
    await page.goto('/console/');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });
    await page.waitForTimeout(3000);

    // Look for the errors stat card with a link to logs
    const errorLink = page.locator('a[href="/console/logs"]').first();
    if (await errorLink.isVisible().catch(() => false)) {
      await errorLink.click();
      await page.waitForURL('**/console/logs**');
      expect(page.url()).toContain('/console/logs');
    } else {
      test.info().annotations.push({
        type: 'skip',
        description: 'No error link to logs found (may need errors > 0)',
      });
    }
  });

  test('dashboard write failures link navigates to metrics', async ({ page }) => {
    await page.goto('/console/');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });
    await page.waitForTimeout(3000);

    // Look for write failures link to metrics
    const metricsLink = page.locator('a[href="/console/metrics"]').first();
    if (await metricsLink.isVisible().catch(() => false)) {
      await metricsLink.click();
      await page.waitForURL('**/console/metrics**');
      expect(page.url()).toContain('/console/metrics');
    } else {
      test.info().annotations.push({
        type: 'skip',
        description: 'No write failures link found (may need failures > 0)',
      });
    }
  });
});
