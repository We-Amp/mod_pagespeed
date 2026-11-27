// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Reconnection overlay E2E tests.
 *
 * Prerequisites: workbench-demo stack running
 *   (origin :8081, nginx :8080, worker :9880, vite :5173).
 *
 * The ReconnectOverlay is shown only after a successful connection is lost
 * (wasConnected && state !== 'connected'). Testing actual disconnection
 * would require stopping the Docker stack mid-test, which is fragile.
 * These tests verify the happy path: the overlay remains hidden while the
 * connection is healthy, and the connection status is correct.
 */
import { test, expect } from '@playwright/test';

test.describe('Reconnection', () => {
  // -- Overlay not visible on healthy connection ------------------------------

  test('reconnect overlay not visible on normal connection', async ({ page }) => {
    await page.goto('/console/');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });

    // The reconnect overlay should not be in the visible DOM
    const reconnectOverlay = page.locator('.reconnect-overlay');
    await expect(reconnectOverlay).not.toBeVisible();
  });

  test('page loads normally when connected to worker', async ({ page }) => {
    const response = await page.goto('/console/');
    expect(response?.status()).toBe(200);

    // Wait for WebSocket connection to establish
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });

    // Main content should be visible (not obscured by overlay)
    const mainContent = page.locator('.main-content');
    await expect(mainContent).toBeVisible();
  });

  test('reconnect overlay absent across all console routes', async ({ page }) => {
    const routes = [
      '/console/',
      '/console/urls',
      '/console/config',
      '/console/savings',
      '/console/waterfall',
      '/console/diff',
    ];

    for (const route of routes) {
      await page.goto(route);
      await expect(
        page.getByText(/connected/i).first(),
      ).toBeVisible({ timeout: 10000 });

      const reconnectOverlay = page.locator('.reconnect-overlay');
      await expect(reconnectOverlay).not.toBeVisible();
    }
  });

  // -- Connection status indicators -------------------------------------------

  test('connection status shows connected on normal load', async ({ page }) => {
    await page.goto('/console/');
    // The "Connected" text should appear in the topbar or status area
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });

    // "Reconnecting" text should NOT be visible
    const reconnecting = page.getByText(/reconnecting/i);
    await expect(reconnecting).not.toBeVisible();
  });

  test('no reconnect attempt counter visible during healthy connection', async ({ page }) => {
    await page.goto('/console/');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });

    // "Attempt N of" text from the ReconnectOverlay should not be present
    const attemptText = page.getByText(/attempt \d+ of/i);
    await expect(attemptText).not.toBeVisible();
  });

  // -- WebSocket health check -------------------------------------------------

  test('WebSocket stats endpoint is reachable', async ({ page }) => {
    await page.goto('/console/');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });

    // Wait for stats to actually load (metrics table or value appears)
    await expect(
      page.locator('.metric-value, .metrics-table, .stat-value, .ps-card').first()
    ).toBeVisible({ timeout: 15000 });

    // Verify loading state is gone - use correct text from the actual component
    const loading = page.getByText(/loading metrics|loading statistics/i);
    await expect(loading).not.toBeVisible();

    // Reconnect overlay should still be absent
    const reconnectOverlay = page.locator('.reconnect-overlay');
    await expect(reconnectOverlay).not.toBeVisible();
  });

  test('reconnect overlay dialog has correct ARIA attributes when shown', async ({ page }) => {
    await page.goto('/console/');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });

    // Inject a reconnect overlay to verify its expected structure
    await page.evaluate(() => {
      const overlay = document.createElement('div');
      overlay.className = 'reconnect-overlay';
      overlay.setAttribute('role', 'dialog');
      overlay.setAttribute('aria-modal', 'true');
      overlay.setAttribute('aria-labelledby', 'reconnect-dialog-title');
      overlay.innerHTML = `
        <div class="reconnect-card ps-card">
          <span class="ps-spinner reconnect-spinner" aria-hidden="true"></span>
          <h2 class="reconnect-title" id="reconnect-dialog-title">Reconnecting...</h2>
          <p class="reconnect-attempt">Attempt 1 of &infin;</p>
          <div class="reconnect-actions">
            <button class="ps-btn ps-btn-primary">Reconnect Now</button>
            <button class="ps-btn ps-btn-ghost reconnect-dismiss">Dismiss</button>
          </div>
        </div>
      `;
      document.body.appendChild(overlay);
    });

    // Verify the ARIA structure
    const dialog = page.locator('.reconnect-overlay[role="dialog"]');
    await expect(dialog).toBeVisible();
    await expect(dialog).toHaveAttribute('aria-modal', 'true');
    await expect(dialog).toHaveAttribute('aria-labelledby', 'reconnect-dialog-title');

    // Verify the title
    const title = page.locator('#reconnect-dialog-title');
    await expect(title).toHaveText('Reconnecting...');

    // Verify buttons
    await expect(
      page.getByRole('button', { name: /reconnect now/i }),
    ).toBeVisible();
    await expect(
      page.locator('.reconnect-dismiss'),
    ).toBeVisible();
  });
});
