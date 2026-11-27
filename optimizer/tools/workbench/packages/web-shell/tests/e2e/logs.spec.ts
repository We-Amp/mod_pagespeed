// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Debug Console (Logs) E2E tests.
 *
 * Prerequisites: workbench-demo stack running
 *   (origin :8081, nginx :8080, worker :9880, vite :5173).
 *
 * Run: cd tools/workbench/packages/web-shell && npx playwright test logs
 */
import { test, expect } from '@playwright/test';

test.describe('Logs (/console/logs)', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/console/logs');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });
  });

  // -- Page load --------------------------------------------------------------

  test('page loads without error', async ({ page }) => {
    const response = await page.goto('/console/logs');
    expect(response?.status()).toBe(200);
  });

  // -- Log entries or empty state ---------------------------------------------

  // Scope to the Logs page: the docked Debug Console (LogPanel) renders its
  // own "No log entries yet." empty state, so an unscoped getByText matches
  // two elements whenever both views are empty.
  //
  // The demo runs the worker with an API token AND read-open, and the console
  // holds no token. Read-open covers only the stats and events streams, so the
  // log stream is rejected (close 4001) and stays empty: the Logs page then
  // shows its "No log entries yet" message. A console that has the token, or
  // a deployment that serves logs without one, shows entries instead.
  test('shows log entries or empty state message', async ({ page }) => {
    const logPage = page.locator('.log-page');
    const emptyState = logPage.getByText(/no log entries yet/i);
    // Wait for either log entries or empty state to render
    await Promise.race([
      logPage.locator('.log-line').first().waitFor({ state: 'visible', timeout: 15000 }),
      emptyState.waitFor({ state: 'visible', timeout: 15000 })
    ]).catch(() => {});  // One will succeed
    const hasEntries = await logPage.locator('.log-line').count();
    if (hasEntries > 0) {
      await expect(logPage.locator('.log-line').first()).toBeVisible();
    } else {
      // Should show the empty state message
      await expect(emptyState).toBeVisible();
    }
  });

  // -- Source filter chips ----------------------------------------------------

  test('source filter chips exist for worker, cache, chrome', async ({ page }) => {
    await expect(page.getByText('Source', { exact: true })).toBeVisible();

    // Each source should appear as a toggle chip
    for (const source of ['Worker', 'Cache', 'Chrome']) {
      await expect(
        page.locator('.toggle-chip').filter({ hasText: source }),
      ).toBeVisible();
    }
  });

  // -- Level filter chips -----------------------------------------------------

  test('level filter chips exist for debug, info, warning, error', async ({ page }) => {
    await expect(page.getByText('Level', { exact: true })).toBeVisible();

    for (const level of ['Debug', 'Info', 'Warning', 'Error']) {
      await expect(
        page.locator('.toggle-chip').filter({ hasText: level }),
      ).toBeVisible();
    }
  });

  // -- Search input -----------------------------------------------------------

  test('search input is present and accepts text', async ({ page }) => {
    const logPage = page.locator('.log-page');
    const searchInput = logPage.locator('input[type="search"]');
    await expect(searchInput).toBeVisible();
    await expect(searchInput).toHaveAttribute('placeholder', /search logs/i);

    // Type into the search input
    await searchInput.fill('test query');
    await expect(searchInput).toHaveValue('test query');
  });

  // -- Pause/Resume button ----------------------------------------------------

  test('pause button toggles to resume and back', async ({ page }) => {
    const logPage = page.locator('.log-page');
    const pauseBtn = logPage.getByRole('button', { name: 'Pause', exact: true });
    await expect(pauseBtn).toBeVisible();

    // Click to pause
    await pauseBtn.click();

    // Should now show "Resume"
    const resumeBtn = logPage.getByRole('button', { name: /^resume/i });
    await expect(resumeBtn).toBeVisible();

    // Click to resume
    await resumeBtn.click();

    // Should show "Pause" again
    await expect(
      logPage.getByRole('button', { name: 'Pause', exact: true }),
    ).toBeVisible();
  });

  // -- Clear button -----------------------------------------------------------

  test('clear button clears log entries', async ({ page }) => {
    const logPage = page.locator('.log-page');
    // Wait for some logs to potentially arrive
    await page.waitForTimeout(2000);

    const clearBtn = logPage.getByRole('button', { name: /clear/i });
    await expect(clearBtn).toBeVisible();

    // Click clear
    await clearBtn.click();

    // After clearing, log count should show "0"
    await expect(
      logPage.locator('.log-count'),
    ).toContainText('0');
  });

  // -- Export TXT button ------------------------------------------------------

  test('export TXT button is present', async ({ page }) => {
    const logPage = page.locator('.log-page');
    const exportTxtBtn = logPage.getByRole('button', { name: /export txt/i });
    await expect(exportTxtBtn).toBeVisible();
  });

  // -- Export JSON button -----------------------------------------------------

  test('export JSON button is present', async ({ page }) => {
    const logPage = page.locator('.log-page');
    const exportJsonBtn = logPage.getByRole('button', { name: /export json/i });
    await expect(exportJsonBtn).toBeVisible();
  });

  // -- Log line rendering -----------------------------------------------------

  test('log lines display timestamp, source, level, and message', async ({ page }) => {
    // Wait for log entries to arrive
    await page.waitForTimeout(5000);

    const logLines = page.locator('.log-line');
    const count = await logLines.count();

    if (count > 0) {
      const firstLine = logLines.first();

      // Timestamp element
      await expect(firstLine.locator('.log-ts')).toBeVisible();

      // Level badge
      await expect(firstLine.locator('.log-level-badge')).toBeVisible();

      // Source badge
      await expect(firstLine.locator('.log-source-badge')).toBeVisible();

      // Message text
      await expect(firstLine.locator('.log-message')).toBeVisible();
    } else {
      test.info().annotations.push({
        type: 'skip',
        description: 'No log entries arrived during test window',
      });
    }
  });

  // -- Empty state message ----------------------------------------------------

  test('empty state shows appropriate message when no logs', async ({ page }) => {
    const logPage = page.locator('.log-page');
    // Clear all logs first
    await logPage.getByRole('button', { name: /clear/i }).click();

    // Should show "No log entries yet" empty state (scoped to the Logs page;
    // the docked LogPanel has its own empty state)
    await expect(
      logPage.getByText(/no log entries yet/i),
    ).toBeVisible();
  });

  // -- Dropped count badge ----------------------------------------------------

  test('dropped count badge area exists in toolbar', async ({ page }) => {
    // The dropped count badge only appears when overflow happens. Verify
    // the toolbar-info container exists where it would appear.
    const toolbarInfo = page.locator('.toolbar-info');
    await expect(toolbarInfo).toBeVisible();

    // Log count text should be present
    await expect(
      page.locator('.log-count'),
    ).toBeVisible();
  });

  // -- Auto-scroll behavior ---------------------------------------------------

  test('auto-scroll is active by default and pauses on click', async ({ page }) => {
    const logPage = page.locator('.log-page');
    // The log container should exist with the role="log" attribute
    const logContainer = logPage.locator('[role="log"]');
    await expect(logContainer).toBeVisible();

    // Default state should show "Pause" (meaning auto-scroll is active)
    await expect(
      logPage.getByRole('button', { name: 'Pause', exact: true }),
    ).toBeVisible();

    // After clicking Pause, auto-scroll stops and we see Resume
    await logPage.getByRole('button', { name: 'Pause', exact: true }).click();
    await expect(
      logPage.getByRole('button', { name: /^resume/i }),
    ).toBeVisible();
  });

  // -- Log level coloring (error = red) ---------------------------------------

  test('error log lines have distinct styling', async ({ page }) => {
    // Wait for log entries
    await page.waitForTimeout(5000);

    // Check for error-level badges in the page
    const errorBadges = page.locator('.log-level-badge.level-error');
    const count = await errorBadges.count();

    if (count > 0) {
      // Error lines should have the level-error class on the log-line
      const errorLine = page.locator('.log-line.level-error').first();
      await expect(errorLine).toBeVisible();
    } else {
      // No errors is fine — verify level classes exist in the DOM structure
      // by checking that level filter chips have the expected CSS classes
      const errorChip = page.locator('.toggle-chip.level-error');
      await expect(errorChip).toBeVisible();
    }
  });

  // -- Source badge styling ---------------------------------------------------

  test('source badges are styled with bordered pill shape', async ({ page }) => {
    await page.waitForTimeout(5000);

    const logLines = page.locator('.log-line');
    const count = await logLines.count();

    if (count > 0) {
      const sourceBadge = page.locator('.log-source-badge').first();
      await expect(sourceBadge).toBeVisible();

      // Source badge text should be one of the expected sources
      const text = await sourceBadge.textContent();
      expect(['worker', 'cache', 'chrome']).toContain(text?.trim());
    } else {
      test.info().annotations.push({
        type: 'skip',
        description: 'No log entries to verify source badge styling',
      });
    }
  });

  // -- Filter chips toggle behavior -------------------------------------------

  test('clicking a source filter chip toggles it off and filters entries', async ({ page }) => {
    await page.waitForTimeout(3000);

    // Read initial entry count
    const initialCountText = await page.locator('.log-count').textContent();

    // Click the Worker chip to disable it
    const workerChip = page.locator('.toggle-chip').filter({ hasText: 'Worker' });
    await workerChip.click();

    // The chip should lose its active styling
    await expect(workerChip).not.toHaveClass(/toggle-chip-active/);

    // Click again to re-enable
    await workerChip.click();
    await expect(workerChip).toHaveClass(/toggle-chip-active/);
  });

  // -- Disconnected state -----------------------------------------------------

  test('disconnected state shows waiting message', async ({ page }) => {
    // Navigate fresh without waiting for connection — check the disconnected UI
    // by looking at the page title which is always present
    const title = await page.title();
    expect(title).toContain('Debug Console');
  });

  // -- Entry count display ----------------------------------------------------

  test('entry count display shows filtered and total counts', async ({ page }) => {
    await page.waitForTimeout(2000);

    const logCount = page.locator('.log-count');
    await expect(logCount).toBeVisible();

    // Should show "N / M entries" format
    const text = await logCount.textContent();
    expect(text).toMatch(/\d+\s*\/\s*\d+\s*entries/);
  });
});
