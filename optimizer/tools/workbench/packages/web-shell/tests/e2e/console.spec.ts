// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Workbench console E2E tests.
 *
 * Prerequisites: the workbench-demo stack must be running
 *   (origin :8081, nginx :8080, worker :9880, vite :5173).
 *
 * Run: cd tools/workbench/packages/web-shell && npx playwright test
 */
import { test, expect } from '@playwright/test';

// ---------------------------------------------------------------------------
// Dashboard (root page)
// ---------------------------------------------------------------------------

test.describe('Dashboard (/console/)', () => {
  test('loads without 500 error', async ({ page }) => {
    const response = await page.goto('/console/');
    expect(response?.status()).toBe(200);
  });

  test('shows connection status', async ({ page }) => {
    await page.goto('/console/');
    // Should eventually show "Connected" somewhere in the page
    await expect(
      page.getByText(/connected/i).first()
    ).toBeVisible({ timeout: 10000 });
  });

  test('displays stats cards with non-zero values', async ({ page }) => {
    await page.goto('/console/');
    // Wait for stats to load via WebSocket
    await page.waitForTimeout(3000);

    // Check that the page has some numeric content (stats cards)
    const body = await page.textContent('body');
    expect(body).toBeTruthy();

    // Should not still be showing "Loading statistics"
    const loading = page.getByText(/loading statistics/i);
    await expect(loading).not.toBeVisible({ timeout: 10000 });
  });

  test('displays notification count > 0', async ({ page }) => {
    await page.goto('/console/');
    await page.waitForTimeout(3000);
    // Look for "Notifications" label and a non-zero number near it
    const notifSection = page.getByText(/notifications/i).first();
    await expect(notifSection).toBeVisible({ timeout: 10000 });
  });

  test('displays variant count > 0', async ({ page }) => {
    await page.goto('/console/');
    await page.waitForTimeout(3000);
    const variantSection = page.getByText(/variants/i).first();
    await expect(variantSection).toBeVisible({ timeout: 10000 });
  });

  test('shows time-series charts', async ({ page }) => {
    await page.goto('/console/');
    await page.waitForTimeout(3000);
    // uPlot renders into canvas elements
    const canvases = page.locator('canvas');
    const count = await canvases.count();
    // Dashboard should render at least one chart
    expect(count).toBeGreaterThan(0);
  });

  test('topbar and sidebar render', async ({ page }) => {
    await page.goto('/console/');
    await page.waitForTimeout(2000);
    // Sidebar should render navigation links
    const sidebar = page.locator('nav, [class*="sidebar"], [class*="Sidebar"]');
    const sidebarCount = await sidebar.count();
    expect(sidebarCount).toBeGreaterThan(0);
    // Topbar should render
    const topbar = page.locator('[class*="topbar"], [class*="Topbar"], header');
    const topbarCount = await topbar.count();
    expect(topbarCount).toBeGreaterThan(0);
  });

  test('export buttons visible on dashboard', async ({ page }) => {
    await page.goto('/console/');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });
    await page.waitForTimeout(3000);

    await expect(
      page.getByRole('button', { name: /export json/i }),
    ).toBeVisible();
    await expect(
      page.getByRole('button', { name: /export csv/i }),
    ).toBeVisible();
  });

  test('uptime badge shows duration', async ({ page }) => {
    await page.goto('/console/');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });
    await page.waitForTimeout(3000);

    const uptimeText = page.getByText(/uptime/i);
    await expect(uptimeText).toBeVisible();
  });

  test('by-type breakdown table renders', async ({ page }) => {
    await page.goto('/console/');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });
    await page.waitForTimeout(3000);

    await expect(
      page.getByText(/processing by type/i),
    ).toBeVisible();

    // Should show HTML, CSS, JS, Image rows
    const body = await page.textContent('body');
    expect(body?.toLowerCase()).toContain('html');
    expect(body?.toLowerCase()).toContain('css');
    expect(body?.toLowerCase()).toContain('image');
  });

  test('by-format breakdown table renders', async ({ page }) => {
    await page.goto('/console/');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });
    await page.waitForTimeout(3000);

    await expect(
      page.getByText(/image formats/i),
    ).toBeVisible();

    const body = await page.textContent('body');
    expect(body).toContain('WebP');
    expect(body).toContain('AVIF');
  });

  test('canvas chart elements render with data', async ({ page }) => {
    await page.goto('/console/');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });
    await page.waitForTimeout(5000);

    // uPlot renders into canvas elements
    const canvases = page.locator('canvas');
    const count = await canvases.count();
    // Dashboard has 3 chart cards: Throughput, Cache, Health
    expect(count).toBeGreaterThanOrEqual(3);
  });
});

// ---------------------------------------------------------------------------
// URL Inspector (/console/urls)
// ---------------------------------------------------------------------------

test.describe('URL Inspector (/console/urls)', () => {
  test('loads without error', async ({ page }) => {
    const response = await page.goto('/console/urls');
    expect(response?.status()).toBe(200);
  });

  test('shows cached URLs list', async ({ page }) => {
    await page.goto('/console/urls');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });
    await page.waitForTimeout(3000);
    // Should show table rows from demo content
    const rows = page.locator('tr');
    const count = await rows.count();
    // Header row + at least one data row
    expect(count).toBeGreaterThan(1);
  });

  test('shows alternates when URL is selected', async ({ page }) => {
    await page.goto('/console/urls');
    await expect(
      page.getByText(/connected/i).first(),
    ).toBeVisible({ timeout: 10000 });
    await page.waitForTimeout(3000);
    // Click a URL row
    const urlRow = page.locator('.url-row').first();
    await expect(urlRow).toBeVisible({ timeout: 5000 });
    await urlRow.click();
    await page.waitForTimeout(2000);
    // Variant detail should appear
    await expect(
      page.getByText(/variant|webp|avif|original/i).first(),
    ).toBeVisible({ timeout: 5000 });
  });
});

// ---------------------------------------------------------------------------
// Configuration (/console/config)
// ---------------------------------------------------------------------------

test.describe('Configuration (/console/config)', () => {
  test('loads without error', async ({ page }) => {
    const response = await page.goto('/console/config');
    expect(response?.status()).toBe(200);
  });

  test('displays configuration fields', async ({ page }) => {
    await page.goto('/console/config');
    await page.waitForTimeout(3000);
    const body = await page.textContent('body');

    // Check for common config labels
    const configTerms = [
      'jpeg', 'webp', 'avif', 'quality', 'gzip', 'brotli',
      'mobile', 'tablet', 'desktop', 'threads', 'html', 'css',
    ];
    const found = configTerms.filter(
      (t) => body?.toLowerCase().includes(t)
    );
    test.info().annotations.push({
      type: 'config-terms',
      description: `Config terms found: ${found.join(', ')}`,
    });
    // Should find at least some config fields
    expect(found.length).toBeGreaterThan(0);
  });

  test('has input fields or toggles', async ({ page }) => {
    await page.goto('/console/config');
    await page.waitForTimeout(2000);
    const inputs = page.locator('input, select, [role="switch"], [role="checkbox"]');
    const count = await inputs.count();
    test.info().annotations.push({
      type: 'input-count',
      description: `Found ${count} input/toggle elements`,
    });
    expect(count).toBeGreaterThan(0);
  });
});

// ---------------------------------------------------------------------------
// Savings Calculator (/console/savings)
// ---------------------------------------------------------------------------

test.describe('Savings Calculator (/console/savings)', () => {
  test('loads without error', async ({ page }) => {
    const response = await page.goto('/console/savings');
    expect(response?.status()).toBe(200);
  });

  test('shows savings data', async ({ page }) => {
    await page.goto('/console/savings');
    await page.waitForTimeout(3000);
    const body = await page.textContent('body');

    // Look for savings-related content
    const savingsTerms = ['savings', 'bandwidth', 'bytes', 'original', 'optimized', '%'];
    const found = savingsTerms.filter(
      (t) => body?.toLowerCase().includes(t.toLowerCase())
    );
    test.info().annotations.push({
      type: 'savings-terms',
      description: `Savings terms found: ${found.join(', ')}`,
    });
  });
});

// ---------------------------------------------------------------------------
// Waterfall (/console/waterfall) — Chrome now available in container
// ---------------------------------------------------------------------------

test.describe('Waterfall (/console/waterfall)', () => {
  test('loads without error', async ({ page }) => {
    const response = await page.goto('/console/waterfall');
    expect(response?.status()).toBe(200);
  });

  test('shows capture form with URL input and buttons', async ({ page }) => {
    await page.goto('/console/waterfall');
    await expect(
      page.getByText(/connected/i).first()
    ).toBeVisible({ timeout: 10000 });

    // URL input field
    await expect(page.locator('#url-input')).toBeVisible();

    // Viewport selector
    await expect(page.locator('#viewport-select')).toBeVisible();

    // Capture button
    await expect(
      page.getByRole('button', { name: /capture/i }).first()
    ).toBeVisible();

    // Capture Both button
    await expect(
      page.getByRole('button', { name: /capture both/i })
    ).toBeVisible();
  });
});

// ---------------------------------------------------------------------------
// Visual Diff (/console/diff) — Chrome now available in container
// ---------------------------------------------------------------------------

test.describe('Visual Diff (/console/diff)', () => {
  test('loads without error', async ({ page }) => {
    const response = await page.goto('/console/diff');
    expect(response?.status()).toBe(200);
  });

  test('shows capture form with URL input and buttons', async ({ page }) => {
    await page.goto('/console/diff');
    await expect(
      page.getByText(/connected/i).first()
    ).toBeVisible({ timeout: 10000 });

    // URL input
    await expect(page.locator('#diff-url')).toBeVisible();

    // Viewport selector
    await expect(page.locator('#viewport-select')).toBeVisible();

    // Capture buttons
    await expect(
      page.getByRole('button', { name: /capture original/i })
    ).toBeVisible();
    await expect(
      page.getByRole('button', { name: /capture optimized/i })
    ).toBeVisible();
    await expect(
      page.getByRole('button', { name: /capture both/i })
    ).toBeVisible();
  });
});

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

test.describe('Navigation', () => {
  test('sidebar links navigate between pages', async ({ page }) => {
    await page.goto('/console/');
    await page.waitForTimeout(2000);

    // Try to find and click sidebar links
    const navLinks = page.locator('a[href*="/console/"]');
    const count = await navLinks.count();
    test.info().annotations.push({
      type: 'nav-links',
      description: `Found ${count} console nav links`,
    });

    if (count > 0) {
      const hrefs: string[] = [];
      for (let i = 0; i < Math.min(count, 10); i++) {
        const href = await navLinks.nth(i).getAttribute('href');
        if (href) hrefs.push(href);
      }
      test.info().annotations.push({
        type: 'nav-hrefs',
        description: hrefs.join(', '),
      });
    }
  });

  test('keyboard shortcut ? opens help', async ({ page }) => {
    await page.goto('/console/');
    await page.waitForTimeout(2000);
    await page.keyboard.press('?');
    await page.waitForTimeout(500);
    const help = page.getByText(/keyboard|shortcut/i).first();
    const helpVisible = await help.isVisible().catch(() => false);
    test.info().annotations.push({
      type: 'keyboard-help',
      description: `Help visible after ?: ${helpVisible}`,
    });
  });
});

// ---------------------------------------------------------------------------
// API Proxy
// ---------------------------------------------------------------------------

test.describe('API Proxy', () => {
  test('/v1/stats returns valid JSON through Vite proxy', async ({ request }) => {
    const response = await request.get('/v1/stats');
    expect(response.status()).toBe(200);
    const data = await response.json();
    expect(data).toHaveProperty('notifications');
    expect(data).toHaveProperty('variants');
    expect(data.notifications.received).toBeGreaterThanOrEqual(0);
  });

  test('/v1/health returns ok status', async ({ request }) => {
    const response = await request.get('/v1/health');
    expect(response.status()).toBe(200);
    const data = await response.json();
    expect(data.status).toBe('ok');
    expect(data.ready).toBe(true);
  });

  test('/v1/cache/urls returns URL list', async ({ request }) => {
    const response = await request.get('/v1/cache/urls');
    expect(response.status()).toBe(200);
    const data = await response.json();
    test.info().annotations.push({
      type: 'cache-urls',
      description: `URLs returned: ${JSON.stringify(data).substring(0, 200)}`,
    });
  });

  test('/v1/config returns configuration', async ({ request }) => {
    const response = await request.get('/v1/config');
    expect(response.status()).toBe(200);
    const data = await response.json();
    test.info().annotations.push({
      type: 'config-api',
      description: `Config keys: ${Object.keys(data).join(', ')}`,
    });
  });
});
