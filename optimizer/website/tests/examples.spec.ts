// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

// The before/after iframes point at the live 1.1 demo backend (an external
// origin), so these tests assert page structure and iframe URLs rather than
// loading the framed content.

test.describe('Examples gallery index', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/examples/');
  });

  test('renders the hero and all category sections', async ({ page }) => {
    // Scope to the in-page h1 — the Astro dev toolbar injects its own h1 nodes
    // (outside <main>), so a bare `h1` locator is not strict-mode safe in dev.
    await expect(page.locator('main h1')).toHaveText(/Optimization examples/);
    for (const id of ['images', 'css', 'javascript', 'html', 'caching', 'resources']) {
      await expect(page.locator(`section#${id}`)).toBeVisible();
    }
  });

  test('lists every example as a card linking to its detail page', async ({ page }) => {
    const cards = page.locator('a[href^="/examples/"][href$="/"]');
    // 49 catalog entries (plus the category jump-links are #anchors, excluded).
    expect(await cards.count()).toBeGreaterThanOrEqual(40);
    await expect(page.locator('a[href="/examples/combine_css/"]')).toBeVisible();
  });

  test('category jump-links are present', async ({ page }) => {
    await expect(
      page.locator('nav[aria-label="Example categories"] a[href="#images"]'),
    ).toBeVisible();
  });
});

test.describe('Example detail page', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/examples/combine_css/');
  });

  test('shows the title, category and breadcrumb', async ({ page }) => {
    // Scope to the in-page h1 — the embedded console-demo preview adds its
    // own h1 nodes, so a bare `h1` locator is not strict-mode safe. The hero
    // h1 renders through SectionHeader now (no h1-hero class).
    await expect(page.locator('main h1')).toHaveText(/Combine CSS/);
    await expect(page.locator('nav[aria-label="Breadcrumb"] a[href="/examples/"]')).toBeVisible();
  });

  test('frames the live before and after with correct URLs', async ({ page }) => {
    const frames = page.locator('iframe');
    expect(await frames.count()).toBe(2);

    const before = await frames.nth(0).getAttribute('src');
    const after = await frames.nth(1).getAttribute('src');
    expect(before).toContain('/mod_pagespeed_example/combine_css.html?PageSpeed=off');
    expect(after).toContain('/mod_pagespeed_example/combine_css.html?PageSpeedFilters=combine_css');
    expect(before).toContain('demo-httpd-1.1.modpagespeed.com');
  });

  test('labels the frames Original and Optimized', async ({ page }) => {
    await expect(page.locator('text=Original').first()).toBeVisible();
    await expect(page.locator('text=Optimized').first()).toBeVisible();
  });

  test('states the measured result in words', async ({ page }) => {
    await expect(page.locator('[data-measured-text]')).toContainText('5 HTTP requests become 2');
    await expect(page).toHaveTitle('Combine CSS live demo: before and after — mod_pagespeed');
  });

  test('links to the relevant filter docs', async ({ page }) => {
    await expect(page.locator('a[href="/docs/css-filters/"]')).toBeVisible();
  });
});

test.describe('An example page follows its capture status', () => {
  test('a beacon capture shows a note and no figures, and is noindex', async ({ page }) => {
    await page.goto('/examples/prioritize_critical_images/');
    await expect(page.locator('[data-not-applied]')).toContainText('beacon script');
    await expect(page.locator('[data-measured-text]')).toHaveCount(0);
    await expect(page.locator('meta[name="robots"]')).toHaveAttribute('content', /noindex/);
  });

  test('a header capture quotes the hint header', async ({ page }) => {
    await page.goto('/examples/hint_preload_subresources/');
    await expect(page.locator('[data-hint-header]')).toContainText('rel=preload');
    await expect(page.locator('meta[name="robots"]')).toHaveAttribute('content', /noindex/);
  });

  test('a capture settled as a browser is indexable and says so', async ({ page }) => {
    await page.goto('/examples/defer_javascript/');
    await expect(page.locator('[data-not-applied]')).toHaveCount(0);
    await expect(page.locator('[data-browser-only]')).toContainText('as a browser');
    await expect(page.locator('meta[name="robots"]')).toHaveCount(0);
  });
});

test.describe('Examples in site navigation', () => {
  test('header nav links to the gallery', async ({ page }) => {
    await page.goto('/');
    await expect(page.locator('nav a[href="/examples/"]').first()).toBeAttached();
  });
});

// The preview frames are eager and tiny, so one can finish loading before the
// inline script under them runs and attaches its `load` listener -- a hard
// reload makes that likelier, because the page's own blocking resources are
// re-fetched while the frame comes off a warm connection. The listener then
// misses an event that already happened, the 12s timeout fires, and the page
// hides a frame that loaded and claims the demo host did not respond. These
// tests pin the two halves of the fix.
test.describe('Example preview frames: a load that lands before the settle script', () => {
  const PAGE = '/examples/insert_speculation_rules/';

  test('records the load even when nothing is listening on the frame itself', async ({ page }) => {
    await page.goto(PAGE);
    const stamped = await page.evaluate(() => {
      const frame = document.querySelector('[data-iframe]');
      if (!frame) return 'no frame';
      frame.removeAttribute('data-loaded');
      // `load` does not bubble; only a capture-phase listener registered before
      // the frames were parsed can see this.
      frame.dispatchEvent(new Event('load'));
      return frame.hasAttribute('data-loaded') ? 'recorded' : 'lost';
    });
    expect(stamped).toBe('recorded');
  });

  test('settles a frame that had already finished, instead of reporting it unavailable', async ({
    page,
  }) => {
    // Hold the demo host so no real load event can arrive during the window
    // below: the stamp must be what settles the frame, not a second load.
    await page.route('**/demo-httpd-1.1.modpagespeed.com/**', () => {});
    await page.route(`**${PAGE}`, async (route) => {
      // Only the document itself carries the stamp. The page also sends a HEAD
      // request to its own URL (the response-header strip); that one goes to the
      // server untouched, so no intercepted fetch is still in flight when the
      // test ends and its page closes.
      if (!route.request().isNavigationRequest()) return route.fallback();
      const response = await route.fetch();
      const html = (await response.text()).replace(
        /(<iframe\b[^>]*?)data-iframe/g,
        '$1data-iframe data-loaded',
      );
      await route.fulfill({ response, body: html });
    });
    // Record what the first preview looks like at DOMContentLoaded of every
    // document. The settle script runs at parse time, so by then it has run;
    // the 12s fallback timer cannot have fired yet. Recording per document
    // also keeps the check on the document that is actually shown: a cold dev
    // server reloads the page once after it optimizes its dependencies.
    await page.addInitScript(() => {
      document.addEventListener('DOMContentLoaded', () => {
        const frame = document.querySelector('[data-frame]');
        const shown = (sel: string) => {
          const el = frame?.querySelector(sel);
          if (!el || !el.checkVisibility({ visibilityProperty: true })) return false;
          const box = el.getBoundingClientRect();
          return box.width > 0 && box.height > 0;
        };
        (window as unknown as { __settledAtDcl: object }).__settledAtDcl = {
          skeleton: shown('[data-skeleton]'),
          fallback: shown('[data-fallback]'),
          iframe: shown('[data-iframe]'),
        };
      });
    });
    // The held frame requests keep the document's own `load` event pending, so
    // wait for the DOM instead of the load event.
    await page.goto(PAGE, { waitUntil: 'domcontentloaded' });
    // Before the fix the skeleton stayed up for the full 12s timeout and the
    // fallback then replaced a frame that had loaded.
    await expect
      .poll(() =>
        page
          .evaluate(() => (window as unknown as { __settledAtDcl?: object }).__settledAtDcl)
          // A document being replaced has no context to evaluate in; read the
          // next one.
          .catch(() => undefined),
      )
      .toEqual({ skeleton: false, fallback: false, iframe: true });
    await page.unrouteAll({ behavior: 'ignoreErrors' });
  });
});
