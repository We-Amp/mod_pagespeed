// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect, type Page } from '@playwright/test';
import { stubUmami, trackedEvents } from './helpers/umami';

// The shared scan interface ships behind a flag: v2 is the default, `?ui=v1`
// opts out (and sticks), `?ui=v2` opts back in. These specs pin the flag, the
// double-execution guard and the tile states of the v2 shell.

const PSI_BODY = (score: number) => ({
  lighthouseResult: {
    categories: { performance: { score } },
    audits: {
      'render-blocking-resources': {
        id: 'render-blocking-resources',
        title: 'Eliminate render-blocking resources',
        score: 0.4,
      },
    },
  },
});

const SCAN_BODY = {
  cached: false,
  id: 'abc123',
  permalink: 'https://modpagespeed.com/ai-readability/r/abc123',
  report: {
    url: 'https://example.com/',
    grade: 'C',
    score: 64,
    categories: {},
    preConsentLeak: { status: 'ok', verdict: 'leaks', trackerCount: 3 },
    scriptInventory: { status: 'ok', verdict: 'clean' },
  },
};

interface Calls {
  mobile: number;
  desktop: number;
  scan: number;
}

async function mockBackends(
  page: Page,
  opts: {
    mobile?: number;
    desktop?: number;
    psiStatus?: number;
    scanStatus?: number;
    scanAbort?: boolean;
    delayMs?: number;
  } = {},
): Promise<Calls> {
  const calls: Calls = { mobile: 0, desktop: 0, scan: 0 };
  const delay = () => new Promise((r) => setTimeout(r, opts.delayMs ?? 0));
  await page.route('**/psi/v5/runPagespeed**', async (route) => {
    const strategy = new URL(route.request().url()).searchParams.get('strategy');
    if (strategy === 'mobile') calls.mobile += 1;
    if (strategy === 'desktop') calls.desktop += 1;
    await delay();
    if (opts.psiStatus && opts.psiStatus !== 200) {
      await route.fulfill({
        status: opts.psiStatus,
        contentType: 'application/json',
        body: JSON.stringify({ error: { message: 'PSI rate limit' } }),
      });
      return;
    }
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify(
        PSI_BODY(strategy === 'mobile' ? (opts.mobile ?? 0.4) : (opts.desktop ?? 0.95)),
      ),
    });
  });
  await page.route('**/ai-readability/api/scan**', async (route) => {
    calls.scan += 1;
    await delay();
    if (opts.scanAbort) {
      await route.abort();
      return;
    }
    const status = opts.scanStatus ?? 200;
    await route.fulfill({
      status,
      contentType: 'application/json',
      body: JSON.stringify(status === 200 ? SCAN_BODY : { error: 'scan failed' }),
    });
  });
  return calls;
}

// A cold dev server re-optimizes its module graph on the first hit of a page
// and reloads once; do that outside the measured visit.
async function warm(page: Page, path: string) {
  await page.goto(path);
  await page.waitForLoadState('networkidle');
}

const PAGES = [
  { path: '/analyze/', v1Form: '#psi-form' },
  { path: '/ai-readability/', v1Form: '#ar-form' },
];

test.describe('scan interface flag', () => {
  for (const { path, v1Form } of PAGES) {
    test(`${path}: v2 is the default`, async ({ page }) => {
      await page.goto(path);
      await expect(page.locator('html')).toHaveAttribute('data-scan-ui', 'v2');
      await expect(page.locator('form[data-scan-box]')).toBeVisible();
      await expect(page.locator(v1Form)).toBeHidden();
    });

    test(`${path}: ?ui=v1 shows v1, sticks after a reload, and ?ui=v2 returns to v2`, async ({
      page,
    }) => {
      await page.goto(`${path}?ui=v1`);
      await expect(page.locator('html')).toHaveAttribute('data-scan-ui', 'v1');
      await expect(page.locator(v1Form)).toBeVisible();
      await expect(page.locator('form[data-scan-box]')).toBeHidden();
      await expect(page.locator('[data-scan-results]')).toBeHidden();

      await page.goto(path);
      await expect(page.locator('html')).toHaveAttribute('data-scan-ui', 'v1');
      await expect(page.locator(v1Form)).toBeVisible();

      await page.goto(`${path}?ui=v2`);
      await expect(page.locator('html')).toHaveAttribute('data-scan-ui', 'v2');
      await expect(page.locator('form[data-scan-box]')).toBeVisible();
      await expect(page.locator(v1Form)).toBeHidden();

      await page.goto(path);
      await expect(page.locator('html')).toHaveAttribute('data-scan-ui', 'v2');
      await expect(page.locator('form[data-scan-box]')).toBeVisible();
      await expect(page.locator(v1Form)).toBeHidden();

      await page.goto(`${path}?ui=v1`);
      await expect(page.locator('html')).toHaveAttribute('data-scan-ui', 'v1');
      await expect(page.locator(v1Form)).toBeVisible();
      await expect(page.locator('form[data-scan-box]')).toBeHidden();

      await page.goto(path);
      await expect(page.locator('html')).toHaveAttribute('data-scan-ui', 'v1');
    });
  }

  test('the v2 hero keeps one disclaimer and one privacy note', async ({ page }) => {
    await page.goto('/analyze/?ui=v2');
    await expect(page.locator('p:visible', { hasText: 'This tool calls Google' })).toHaveCount(1);
    await expect(page.getByRole('link', { name: 'Privacy policy' }).first()).toBeVisible();
  });
});

test.describe('deep link runs each request once', () => {
  for (const ui of ['v1', 'v2']) {
    test(`/analyze/?url= under ${ui} calls PSI once per strategy`, async ({ page }) => {
      const calls = await mockBackends(page);
      await warm(page, `/analyze/?ui=${ui}`);
      calls.mobile = calls.desktop = calls.scan = 0;
      await page.goto(`/analyze/?ui=${ui}&url=https://example.com`);
      if (ui === 'v1') {
        await expect(page.locator('#psi-results')).toBeVisible({ timeout: 15000 });
      } else {
        await expect(page.locator('[data-scan-tile="speed"] [data-scan-status]')).not.toHaveText(
          'Checking…',
          { timeout: 15000 },
        );
      }
      await page.waitForTimeout(500);
      expect(calls.mobile).toBe(1);
      expect(calls.desktop).toBe(1);
      expect(calls.scan).toBe(ui === 'v2' ? 1 : 0);
    });
  }

  test('/ai-readability/?url= under v2 runs all three requests once', async ({ page }) => {
    const calls = await mockBackends(page);
    await warm(page, '/ai-readability/?ui=v2');
    calls.mobile = calls.desktop = calls.scan = 0;
    await page.goto('/ai-readability/?ui=v2&url=https://example.com');
    await expect(page.locator('[data-scan-tile="airead"] [data-scan-status]')).not.toHaveText(
      'Checking…',
      { timeout: 15000 },
    );
    await page.waitForTimeout(500);
    expect(calls).toEqual({ mobile: 1, desktop: 1, scan: 1 });
  });
});

test.describe('v2 shell tile states', () => {
  test('tiles start as Checking and settle independently', async ({ page }) => {
    await mockBackends(page, { mobile: 0.4, desktop: 0.95 });
    await warm(page, '/analyze/?ui=v2');
    await page.fill('#scan-url', 'example.com');
    await page.click('#scan-submit');

    const speed = page.locator('[data-scan-tile="speed"]');
    const airead = page.locator('[data-scan-tile="airead"]');
    const risk = page.locator('[data-scan-tile="risk"]');
    await expect(page.locator('[data-scan-results]')).toBeVisible();
    await expect(speed.locator('[data-scan-status]')).toHaveText('Poor', { timeout: 15000 });
    await expect(speed.locator('[data-scan-value]')).toHaveText('Mobile 40 · Desktop 95');
    await expect(airead.locator('[data-scan-status]')).toHaveText('Needs work');
    await expect(airead.locator('[data-scan-value]')).toHaveText('Grade C · 64/100');
    await expect(risk.locator('[data-scan-status]')).toHaveText('1 flagged');
    await expect(risk.locator('[data-scan-value]')).toHaveText('1 need attention');
    await expect(page.locator('[data-scan-health]')).toHaveText(
      '3 of 3 areas checked need attention.',
    );
    await expect(page.locator('[data-scan-live]')).not.toBeEmpty();
  });

  test('a scanner error keeps the tile value to one line and explains in the panel', async ({
    page,
  }) => {
    await mockBackends(page, { scanStatus: 503 });
    await warm(page, '/ai-readability/?ui=v2');
    await page.fill('#scan-url', 'https://example.com');
    await page.click('#scan-submit');
    for (const p of ['airead', 'risk']) {
      const tile = page.locator(`[data-scan-tile="${p}"]`);
      await expect(tile.locator('[data-scan-status]')).toHaveText('Not measured', {
        timeout: 15000,
      });
      await expect(tile.locator('[data-scan-value]')).toContainText(
        'The scan did not complete. Try again in a moment.',
      );
      // The reason is one truncated line: the value box keeps its 28 px height.
      const box = await tile.locator('[data-scan-value]').boundingBox();
      expect(Math.round(box!.height)).toBe(28);
      await tile.click();
      const panel = page.locator(`#scan-panel-${p}`);
      await expect(panel).toContainText('The scan did not complete. Try again in a moment.');
      await expect(panel.getByRole('button', { name: 'Try again' })).toBeVisible();
    }
  });

  test('a not-measured tile is exactly as tall as a measured one', async ({ page }) => {
    await mockBackends(page, { scanStatus: 503 });
    await warm(page, '/analyze/?ui=v2');
    await page.fill('#scan-url', 'https://example.com');
    await page.click('#scan-submit');
    await expect(page.locator('[data-scan-tile="airead"] [data-scan-status]')).toHaveText(
      'Not measured',
      { timeout: 15000 },
    );
    const heights = await page
      .locator('[data-scan-tile]')
      .evaluateAll((els) => els.map((e) => Math.round(e.getBoundingClientRect().height)));
    expect(new Set(heights).size).toBe(1);
    const values = await page
      .locator('[data-scan-value]')
      .evaluateAll((els) => els.map((e) => Math.round(e.getBoundingClientRect().height)));
    expect(new Set(values).size).toBe(1);
  });

  test('each tile shows a visible Details / Hide control that follows aria-expanded', async ({
    page,
  }) => {
    await mockBackends(page);
    await warm(page, '/analyze/?ui=v2');
    await page.fill('#scan-url', 'https://example.com');
    await page.click('#scan-submit');
    const speed = page.locator('[data-scan-tile="speed"]');
    await expect(speed.getByText('Details ▾')).toBeVisible();
    await expect(speed.getByText('Hide ▴')).toBeHidden();
    const closed = await speed.boundingBox();
    await speed.click();
    await expect(speed.getByText('Hide ▴')).toBeVisible();
    await expect(speed.getByText('Details ▾')).toBeHidden();
    // A non-colour cue: the open tile carries an inset ring on top of its border.
    expect(await speed.evaluate((e) => getComputedStyle(e).boxShadow)).not.toBe('none');
    expect((await speed.boundingBox())!.height).toBe(closed!.height);
    // The accessible name stays on the button, not on the visible control.
    await expect(speed).toHaveAttribute('aria-label', 'Hide Speed details');
  });

  test('Try again re-runs only the source behind that tile', async ({ page }) => {
    const calls = await mockBackends(page, { scanStatus: 503 });
    await warm(page, '/analyze/?ui=v2');
    await page.fill('#scan-url', 'https://example.com');
    await page.click('#scan-submit');
    await expect(page.locator('[data-scan-tile="airead"] [data-scan-status]')).toHaveText(
      'Not measured',
      { timeout: 15000 },
    );
    await expect(page.locator('[data-scan-tile="speed"] [data-scan-status]')).toHaveText('Poor');
    const before = { ...calls };
    await page.locator('[data-scan-tile="airead"]').click();
    await page.locator('#scan-panel-airead').getByRole('button', { name: 'Try again' }).click();
    await expect(page.locator('[data-scan-tile="airead"] [data-scan-status]')).toHaveText(
      'Not measured',
    );
    await page.waitForTimeout(300);
    expect(calls.scan).toBe(before.scan + 1);
    expect(calls.mobile).toBe(before.mobile);
    expect(calls.desktop).toBe(before.desktop);
  });

  test('an invalid URL is rejected before any request', async ({ page }) => {
    const calls = await mockBackends(page);
    await warm(page, '/analyze/?ui=v2');
    await page.fill('#scan-url', 'ftp://example.com');
    await page.click('#scan-submit');
    await expect(page.locator('#scan-form-error')).toHaveText(
      'Only http:// and https:// URLs are supported.',
    );
    await expect(page.locator('[data-scan-results]')).toBeHidden();
    expect(calls).toEqual({ mobile: 0, desktop: 0, scan: 0 });
  });

  test('tiles are disclosure buttons and only one panel is open', async ({ page }) => {
    await mockBackends(page);
    await warm(page, '/analyze/?ui=v2');
    await page.fill('#scan-url', 'https://example.com');
    await page.click('#scan-submit');
    const speed = page.locator('[data-scan-tile="speed"]');
    const airead = page.locator('[data-scan-tile="airead"]');
    await expect(speed).toHaveAttribute('aria-expanded', 'false');
    await expect(speed).toHaveAttribute('aria-controls', 'scan-panel-speed');
    await speed.click();
    await expect(speed).toHaveAttribute('aria-expanded', 'true');
    await expect(speed).toHaveAttribute('aria-label', 'Hide Speed details');
    await expect(page.locator('#scan-panel-speed')).toBeVisible();
    await airead.click();
    await expect(speed).toHaveAttribute('aria-expanded', 'false');
    await expect(airead).toHaveAttribute('aria-expanded', 'true');
    await expect(page.locator('#scan-panel-speed')).toBeHidden();
    await expect(page.locator('#scan-panel-airead')).toBeVisible();
    await airead.click();
    await expect(page.locator('#scan-panel-airead')).toBeHidden();
  });

  test('the fixture hook paints every tile without a network', async ({ page }) => {
    await warm(page, '/ai-readability/?ui=v2');
    await page.goto('/ai-readability/?ui=v2&fixture=full');
    await expect(page.locator('[data-scan-tile="speed"] [data-scan-status]')).toHaveText(
      'Needs work',
    );
    await expect(page.locator('[data-scan-tile="risk"] [data-scan-status]')).toHaveText(
      '2 flagged',
    );
  });
});

test.describe('v2 shell details', () => {
  test('tiles open and close with Enter and Space', async ({ page }) => {
    await warm(page, '/analyze/?ui=v2');
    await page.goto('/analyze/?ui=v2&fixture=full');
    const speed = page.locator('[data-scan-tile="speed"]');
    await expect(speed).toBeVisible();
    await speed.focus();
    await page.keyboard.press('Enter');
    await expect(speed).toHaveAttribute('aria-expanded', 'true');
    await page.keyboard.press('Space');
    await expect(speed).toHaveAttribute('aria-expanded', 'false');
    await page.keyboard.press('Space');
    await expect(speed).toHaveAttribute('aria-expanded', 'true');
  });

  test('the panel region is named by the tile name', async ({ page }) => {
    await warm(page, '/analyze/?ui=v2');
    await page.goto('/analyze/?ui=v2&fixture=full');
    await page.locator('[data-scan-tile="speed"]').click();
    await expect(page.getByRole('region', { name: 'Speed', exact: true })).toBeVisible();
  });

  test('the request form note starts empty', async ({ page }) => {
    await page.goto('/analyze/?ui=v2');
    await expect(page.locator('#scan-lead-note')).toHaveValue('');
  });

  for (const path of ['/analyze/', '/ai-readability/']) {
    for (const ui of ['v1', 'v2']) {
      test(`${path}?ui=${ui} has no duplicate ids`, async ({ page }) => {
        await page.goto(`${path}?ui=${ui}`);
        const dupes = await page.evaluate(() => {
          const seen = new Map<string, number>();
          for (const el of document.querySelectorAll('[id]')) {
            seen.set(el.id, (seen.get(el.id) ?? 0) + 1);
          }
          return [...seen].filter(([, n]) => n > 1).map(([id]) => id);
        });
        expect(dupes).toEqual([]);
      });

      test(`${path}?ui=${ui} counts one scan-ui page view`, async ({ page }) => {
        await stubUmami(page);
        await page.goto(`${path}?ui=${ui}`);
        await page.waitForLoadState('networkidle');
        const events = (await trackedEvents(page)).filter((e) => e.name === 'scan-ui');
        expect(events).toEqual([{ name: 'scan-ui', data: { variant: ui } }]);
      });
    }
  }

  test('/ai-readability/?url= under v1 makes no request of its own', async ({ page }) => {
    const calls = await mockBackends(page);
    await warm(page, '/ai-readability/?ui=v1');
    await page.goto('/ai-readability/?ui=v1&url=https://example.com');
    await page.waitForTimeout(800);
    expect(calls).toEqual({ mobile: 0, desktop: 0, scan: 0 });
  });
});

test.describe('v2 layout', () => {
  test('one tile column on a phone, three on a desktop, no horizontal scroll', async ({ page }) => {
    await warm(page, '/analyze/?ui=v2');
    for (const [width, columns] of [
      [390, 1],
      [1280, 3],
    ] as const) {
      await page.setViewportSize({ width, height: 844 });
      await page.goto('/analyze/?ui=v2&fixture=full');
      await expect(page.locator('[data-scan-results]')).toBeVisible();
      const lefts = await page
        .locator('[data-scan-tile]')
        .evaluateAll((els) => els.map((e) => Math.round(e.getBoundingClientRect().left)));
      expect(new Set(lefts).size).toBe(columns);
      const overflow = await page.evaluate(
        () => document.documentElement.scrollWidth - document.documentElement.clientWidth,
      );
      expect(overflow).toBeLessThanOrEqual(0);
      const box = await page.locator('[data-scan-tile="speed"]').boundingBox();
      expect(box!.height).toBeGreaterThanOrEqual(44);
    }
  });

  for (const path of ['/analyze/', '/ai-readability/']) {
    test(`${path}: the skeleton and the tile swap shift the layout by at most 0.02`, async ({
      page,
    }) => {
      await page.addInitScript(() => {
        const w = window as unknown as { __cls: number };
        w.__cls = 0;
        new PerformanceObserver((list) => {
          for (const entry of list.getEntries()) {
            const shift = entry as PerformanceEntry & { hadRecentInput?: boolean; value?: number };
            if (!shift.hadRecentInput) w.__cls += shift.value ?? 0;
          }
        }).observe({ type: 'layout-shift', buffered: true });
      });
      await mockBackends(page, { delayMs: 1500 });
      await warm(page, `${path}?ui=v2`);
      await page.goto(`${path}?ui=v2`);
      await page.fill('#scan-url', 'https://example.com');
      await page.click('#scan-submit');
      await expect(page.locator('[data-scan-tile="speed"] [data-scan-status]')).toHaveText('Poor', {
        timeout: 15000,
      });
      await expect(page.locator('[data-scan-tile="risk"] [data-scan-status]')).toHaveText(
        '1 flagged',
      );
      await page.waitForTimeout(500);
      const cls = await page.evaluate(() => (window as unknown as { __cls: number }).__cls);
      expect(cls).toBeLessThanOrEqual(0.02);
    });
  }

  test('/ai-readability/ results use the full tool width with a 16 px gutter on a phone', async ({
    page,
  }) => {
    await warm(page, '/ai-readability/?ui=v2');
    await page.setViewportSize({ width: 390, height: 844 });
    await page.goto('/ai-readability/?ui=v2&fixture=full');
    await expect(page.locator('[data-scan-results]')).toBeVisible();
    const tile = await page.locator('[data-scan-tile="speed"]').boundingBox();
    expect(Math.round(tile!.x)).toBe(16);
    await page.setViewportSize({ width: 1280, height: 800 });
    await page.goto('/ai-readability/?ui=v2&fixture=full');
    await expect(page.locator('[data-scan-results]')).toBeVisible();
    const grid = await page.locator('[data-scan-tiles]').boundingBox();
    expect(grid!.width).toBeGreaterThan(900);
  });

  for (const path of ['/analyze/', '/ai-readability/']) {
    test(`${path}: the error path shifts the layout by at most 0.02 at 390 px`, async ({
      page,
    }) => {
      await page.addInitScript(() => {
        const w = window as unknown as { __cls: number };
        w.__cls = 0;
        new PerformanceObserver((list) => {
          for (const entry of list.getEntries()) {
            const shift = entry as PerformanceEntry & { hadRecentInput?: boolean; value?: number };
            if (!shift.hadRecentInput) w.__cls += shift.value ?? 0;
          }
        }).observe({ type: 'layout-shift', buffered: true });
      });
      await page.setViewportSize({ width: 390, height: 844 });
      await mockBackends(page, { psiStatus: 429, scanAbort: true, delayMs: 1500 });
      await warm(page, `${path}?ui=v2`);
      await page.goto(`${path}?ui=v2`);
      await page.fill('#scan-url', 'https://example.com');
      await page.click('#scan-submit');
      for (const p of ['speed', 'airead', 'risk']) {
        await expect(page.locator(`[data-scan-tile="${p}"] [data-scan-status]`)).toHaveText(
          'Not measured',
          { timeout: 15000 },
        );
      }
      await page.waitForTimeout(500);
      const cls = await page.evaluate(() => (window as unknown as { __cls: number }).__cls);
      expect(cls).toBeLessThanOrEqual(0.02);
    });
  }
});
