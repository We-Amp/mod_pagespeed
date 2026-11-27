// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect, type Page } from '@playwright/test';

// Input states of the shared scan box: an invalid host never reaches a
// backend, a submit before the script loaded still ends in "Enter a URL",
// a valid run puts its URL in the address bar, and a progress hint shows
// while the checks run.

const PSI_OK = { lighthouseResult: { categories: { performance: { score: 0.9 } }, audits: {} } };

async function trackBackends(page: Page) {
  const calls = { psi: 0, scan: 0 };
  await page.route('**/psi/v5/runPagespeed**', async (route) => {
    calls.psi += 1;
    await new Promise((r) => setTimeout(r, 400));
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify(PSI_OK),
    });
  });
  await page.route('**/ai-readability/api/scan**', async (route) => {
    calls.scan += 1;
    await new Promise((r) => setTimeout(r, 400));
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({ report: { grade: 'C', score: 64, categories: {} } }),
    });
  });
  return calls;
}

// A cold dev server reloads once after the first hit; do that outside the test.
async function open(page: Page, url: string) {
  await page.goto(url);
  await page.waitForLoadState('networkidle');
  // Fingerprint: this checkout's form carries a GET fallback action.
  await expect(page.locator('form[data-scan-box]')).toHaveAttribute('method', 'get');
}

for (const path of ['/analyze/', '/ai-readability/']) {
  test.describe(path, () => {
    test('an invalid host shows an error and makes no backend call', async ({ page }) => {
      const calls = await trackBackends(page);
      await open(page, path);
      const input = page.locator('form[data-scan-box] input[name="url"]');
      for (const bad of ['foo', 'https://a_b.example.com', 'https://-bad.example.com']) {
        await input.fill(bad);
        await input.press('Enter');
        await expect(page.locator('form[data-scan-box] [role="alert"]')).toContainText(
          'real website address',
        );
        await expect(input).toHaveAttribute('aria-invalid', 'true');
      }
      await page.waitForTimeout(300);
      expect(calls).toEqual({ psi: 0, scan: 0 });
      expect(new URL(page.url()).searchParams.has('url')).toBe(false);
    });

    test('a submit before the script loaded shows "Enter a URL"', async ({ page }) => {
      // Hold back the interface script (a hashed chunk in the build, the source
      // module under the dev server) so the bare form is all the page has.
      await page.route(/\/(_astro\/app\.[^/]+\.js|src\/scripts\/scan\/app\.ts)/, (route) =>
        route.abort(),
      );
      await page.goto(path);
      await page.waitForLoadState('load');
      await expect(page.locator('form[data-scan-box]')).toHaveAttribute('method', 'get');
      await Promise.all([
        page.waitForURL(/[?&]url=(&|$)/),
        page.locator('form[data-scan-box] button[type="submit"]').click(),
      ]);
      expect(new URL(page.url()).pathname).toBe(path);
      // The page it lands on, with the script, says what is missing.
      await page.unroute(/\/(_astro\/app\.[^/]+\.js|src\/scripts\/scan\/app\.ts)/);
      await page.reload();
      await expect(page.locator('form[data-scan-box] [role="alert"]')).toHaveText(
        'Enter a URL to check.',
      );
    });

    test('the hint keeps its height busy and idle at 390 px', async ({ page }) => {
      await trackBackends(page);
      await page.setViewportSize({ width: 390, height: 844 });
      await open(page, path);
      await page.locator('form[data-scan-box] input[name="url"]').fill('example.com');
      await page.locator('form[data-scan-box] button[type="submit"]').click();
      const hint = page.locator('[data-scan-hint]');
      await expect(hint).toContainText('20 to 40 seconds');
      const busy = (await hint.boundingBox())!.height;
      await expect(hint).toHaveText('');
      const idle = (await hint.boundingBox())!.height;
      expect(idle).toBe(busy);
    });

    test('a valid run writes ?url= and shows the progress hint while it runs', async ({ page }) => {
      const calls = await trackBackends(page);
      await open(page, path);
      const hint = page.locator('[data-scan-hint]');
      await expect(hint).toHaveText('');
      await page.locator('form[data-scan-box] input[name="url"]').fill('example.com');
      await page.locator('form[data-scan-box] button[type="submit"]').click();
      await expect(hint).toContainText('20 to 40 seconds');
      await expect
        .poll(() => new URL(page.url()).searchParams.get('url'))
        .toBe('https://example.com/');
      await expect(hint).toHaveText('');
      expect(calls.psi).toBeGreaterThan(0);
      expect(calls.scan).toBe(1);
    });
  });
}
