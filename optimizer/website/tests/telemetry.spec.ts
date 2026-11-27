// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { readFileSync } from 'node:fs';
import { test, expect } from '@playwright/test';
import { expectErrorPage } from './helpers/server-mode';

// Expected fallback values come from the committed capture file; the live
// upgrade is tested with a fictitious header value (9.9.9-test), never a real
// or guessed production one.
const capture = JSON.parse(
  readFileSync(new URL('../src/data/telemetry-capture.json', import.meta.url), 'utf8'),
) as {
  capturedAt: string;
  paths: Record<string, { wireBytes: number; headers: Record<string, string | null> }>;
};
const capturedDate = capture.capturedAt.slice(0, 10);
const capturedXmps = capture.paths['/'].headers['x-mod-pagespeed'] ?? 'not present';

const STRIP = '[data-ui="telemetry-strip"]';

test.describe('Telemetry strip', () => {
  test('with JS disabled it shows the captured values and their date', async ({ browser }) => {
    const context = await browser.newContext({ javaScriptEnabled: false });
    const page = await context.newPage();
    await page.goto('/');
    const strip = page.locator(STRIP);
    await expect(strip).toBeVisible();
    await expect(strip.locator('[data-t="state"]').first()).toHaveText(`captured ${capturedDate}`);
    await expect(strip.locator('[data-t="xmps"]')).toHaveText(capturedXmps);
    // A real captured byte value, not a dash (the root path is in the capture).
    await expect(strip.locator('[data-t="wire"]')).toHaveText(/\d[\d,]*\sB/);
    await context.close();
  });

  test('live upgrade shows the HEAD response header and the live state', async ({ page }) => {
    // The strip issues the page's only HEAD request; give it a fictitious
    // x-mod-pagespeed value.
    await page.route('**/*', (route) =>
      route.request().method() === 'HEAD'
        ? route.fulfill({
            status: 200,
            headers: { 'x-mod-pagespeed': '9.9.9-test', vary: 'Save-Data,Accept-Encoding' },
          })
        : route.continue(),
    );
    await page.goto('/');
    const strip = page.locator(STRIP);
    await expect(strip.locator('[data-t="state"]').first()).toHaveText('this view · live');
    await expect(strip.locator('[data-t="xmps"]')).toHaveText('9.9.9-test');
    // The state link is named for its state and its destination; the visible
    // text stays the state alone.
    await expect(
      strip.getByRole('link', {
        name: 'this view · live How to read these response measurements',
        exact: true,
      }),
    ).toHaveAttribute('href', '/pagespeed-markers/');
    await expect(strip.locator('[data-t="vary"]')).toHaveText('Save-Data,Accept-Encoding');
  });

  test('live ttfb is the HEAD probe resource timing, not connection-inclusive', async ({
    page,
  }) => {
    // Hold the HEAD for 300 ms. The probe's responseStart − requestStart is
    // then ~300 ms whatever connection the probe rides, because requestStart
    // is when the request left the browser; a definition that includes
    // connection setup (the probe's total duration, or a navigation entry's
    // responseStart) would read higher by the fresh connection's setup.
    await page.route('**/*', (route) => {
      if (route.request().method() !== 'HEAD') return route.continue();
      return new Promise((r) => setTimeout(r, 300)).then(() =>
        route.fulfill({ status: 200, headers: { 'x-mod-pagespeed': '9.9.9-test' } }),
      );
    });
    await page.goto('/');
    const strip = page.locator(STRIP);
    // ttfb is set before the state label flips to live.
    await expect(strip.locator('[data-t="state"]').first()).toHaveText('this view · live');
    const shown = parseInt(
      (await strip.locator('[data-t="ttfb"]').first().textContent())?.replace(/\D/g, '') ?? '',
      10,
    );
    // Within tolerance of the routed delay (route interception adds its CDP
    // round trip on top of the 300 ms sleep, so the ceiling is generous)…
    expect(shown).toBeGreaterThanOrEqual(280);
    expect(shown).toBeLessThanOrEqual(600);
    // …and equal to the probe's own Resource Timing value (within rounding),
    // which excludes connection setup by definition: a value inflated by the
    // new connection cannot pass this check.
    const probeTtfb = await page.evaluate(() => {
      // Resource Timing entries are named by the absolute URL.
      const href = new URL(location.pathname + location.search, location.href).href;
      const entry = (performance.getEntriesByName(href, 'resource') as PerformanceResourceTiming[])
        .filter((e) => e.initiatorType === 'fetch')
        .reduce<PerformanceResourceTiming | undefined>(
          (latest, e) => (latest === undefined || e.startTime >= latest.startTime ? e : latest),
          undefined,
        );
      return entry && entry.requestStart > 0
        ? Math.round(entry.responseStart - entry.requestStart)
        : null;
    });
    expect(probeTtfb, 'the HEAD must be observable through Resource Timing').not.toBeNull();
    expect(Math.abs(shown - (probeTtfb as number))).toBeLessThanOrEqual(1);
  });

  test('live ttfb is responseStart minus requestStart, not a connection-inclusive span', async ({
    page,
  }) => {
    // Feed the strip a timing entry whose phases all differ: only
    // responseStart − requestStart gives 123; duration (530), responseStart −
    // startTime (523) and responseStart − connectStart (513) all fail.
    await page.addInitScript(() => {
      const perf = performance as unknown as {
        getEntriesByName: (name: string, type?: string) => PerformanceEntry[];
      };
      const orig = perf.getEntriesByName.bind(performance);
      perf.getEntriesByName = (name, type) => {
        const real = orig(name, type);
        if (
          type !== 'resource' ||
          !real.some((e) => (e as PerformanceResourceTiming).initiatorType === 'fetch')
        )
          return real;
        return [
          {
            name,
            entryType: 'resource',
            initiatorType: 'fetch',
            startTime: 1000,
            fetchStart: 1000,
            connectStart: 1010,
            connectEnd: 1200,
            requestStart: 1400,
            responseStart: 1523,
            responseEnd: 1530,
            duration: 530,
          } as unknown as PerformanceEntry,
        ];
      };
    });
    await page.goto('/');
    const strip = page.locator(STRIP);
    await expect(strip.locator('[data-t="state"]').first()).toHaveText('this view · live');
    await expect(strip.locator('[data-t="ttfb"]').first()).toHaveText('123 ms');
  });

  test('a non-2xx probe spells the status in the state text', async ({ browser }) => {
    // Below 480px the LED does not render, so the status must read in the
    // state text itself. 390px is inside that range.
    const context = await browser.newContext({ viewport: { width: 390, height: 844 } });
    const page = await context.newPage();
    await page.route('**/*', (route) =>
      route.request().method() === 'HEAD'
        ? route.fulfill({ status: 405, headers: { 'x-mod-pagespeed': '9.9.9-test' } })
        : route.continue(),
    );
    await page.goto('/');
    const strip = page.locator(STRIP);
    await expect(strip.locator('[data-t="state"]').first()).toHaveText('this view · HTTP 405');
    await expect(strip.locator('[data-t="xmps"]')).toHaveText('9.9.9-test');
    await context.close();
  });

  test('a failed probe keeps the captured fallback', async ({ page }) => {
    await page.route('**/*', (route) =>
      route.request().method() === 'HEAD' ? route.abort() : route.continue(),
    );
    const headSeen = page.waitForRequest((req) => req.method() === 'HEAD');
    await page.goto('/');
    await headSeen;
    // Give the rejected fetch a beat in which it must NOT apply anything.
    await page.waitForTimeout(500);
    const strip = page.locator(STRIP);
    await expect(strip.locator('[data-t="state"]').first()).toHaveText(`captured ${capturedDate}`);
    await expect(strip.locator('[data-t="xmps"]')).toHaveText(capturedXmps);
  });

  test('the strip and every field keep their box through the live upgrade', async ({ page }) => {
    // Hold the HEAD response back so the first measurement is the SSR fallback.
    // The mock answers without a vary header, which also exercises the
    // "not present" path for that field.
    await page.route('**/*', async (route) => {
      if (route.request().method() === 'HEAD') {
        await new Promise((r) => setTimeout(r, 1200));
        return route.fulfill({ status: 200, headers: { 'x-mod-pagespeed': '9.9.9-test' } });
      }
      return route.continue();
    });
    await page.goto('/');
    const strip = page.locator(STRIP);
    await expect(strip).toBeVisible();
    const boxes = async () => [
      await strip.boundingBox(),
      // The state label, every field and the "what these headers mean" link:
      // the upgrade may replace text but must never move a box.
      ...(await Promise.all(
        (await page.locator(`${STRIP} .t-state, ${STRIP} .t-f, ${STRIP} .t-more`).all()).map(
          (part) => part.boundingBox(),
        ),
      )),
    ];
    const before = await boxes();
    await expect(strip.locator('[data-t="state"]').first()).toHaveText('this view · live');
    const after = await boxes();
    expect(after).toEqual(before);
  });

  test('no field value is clipped at 360, 390, 414 or 1440', async ({ browser }) => {
    const failures: string[] = [];
    for (const width of [360, 390, 414, 1440]) {
      const context = await browser.newContext({ viewport: { width, height: 844 } });
      // Keep the strip in its captured state: a failed probe applies nothing.
      await context.route('**/*', (route) =>
        route.request().method() === 'HEAD' ? route.abort() : route.continue(),
      );
      const page = await context.newPage();
      await page.goto('/');
      const strip = page.locator(STRIP);
      await expect(strip).toBeVisible();
      // The dev server serves scoped styles via JS; wait until they land.
      await expect
        .poll(() =>
          page.evaluate(
            () =>
              getComputedStyle(document.querySelector('[data-ui="telemetry-strip"]')!).blockSize,
          ),
        )
        .toBe('32px');
      // Measure in the self-hosted mono font, not whatever fallback is
      // rendering while it loads — fallback metrics are wider and differ
      // between platforms.
      await page.evaluate(() => document.fonts.ready);
      const problems = await page.evaluate(() => {
        const el0 = document.querySelector<HTMLElement>('[data-ui="telemetry-strip"]')!;
        const out: string[] = [];
        if (el0.scrollWidth > el0.clientWidth)
          out.push(
            `strip overflow: scrollWidth ${el0.scrollWidth} > clientWidth ${el0.clientWidth}`,
          );
        if (document.documentElement.scrollWidth > window.innerWidth)
          out.push(
            `page horizontal overflow: ${document.documentElement.scrollWidth} > ${window.innerWidth}`,
          );
        // The row clips what overflows it, so scrollWidth alone cannot see a
        // value that runs past its field: also compare each value's rendered
        // right edge with the right edges of its field and of the row.
        const rowRight = el0
          .querySelector<HTMLElement>('.telemetry-row')!
          .getBoundingClientRect().right;
        el0.querySelectorAll<HTMLElement>('[data-t]').forEach((el) => {
          if (el.getClientRects().length === 0) return; // not rendered at this width
          if (el.scrollWidth > el.clientWidth)
            out.push(
              `[data-t="${el.dataset.t}"] clipped: scrollWidth ${el.scrollWidth} > clientWidth ${el.clientWidth} ("${el.textContent}")`,
            );
          const range = document.createRange();
          range.selectNodeContents(el);
          const right = range.getBoundingClientRect().right;
          const field = el.closest<HTMLElement>('.t-f, .t-state');
          if (field && right > field.getBoundingClientRect().right + 0.5)
            out.push(
              `[data-t="${el.dataset.t}"] past its field: content ends ${right.toFixed(1)} > field right ${field.getBoundingClientRect().right.toFixed(1)} ("${el.textContent}")`,
            );
          if (right > rowRight + 0.5)
            out.push(
              `[data-t="${el.dataset.t}"] past the row: content ends ${right.toFixed(1)} > row right ${rowRight.toFixed(1)} ("${el.textContent}")`,
            );
        });
        return out;
      });
      failures.push(...problems.map((p) => `${width}px: ${p}`));
      await context.close();
    }
    expect(failures).toEqual([]);
  });

  test('the strip is 32px tall at first paint with the CSS sheet blocked', async ({ page }) => {
    // The scoped and global rules ship separately from the inline critical
    // guard: on production as the deferred /_astro/*.css sheet (aborted
    // below), in dev as style-module requests plus inline
    // <style data-vite-dev-id> tags that the dev server injects into the
    // document. Strip those tags from the HTML as well, or dev keeps the
    // strip at 32px without the guard and the test proves nothing.
    await page.route(
      (url) => /\.css(\?|$)|[?&]type=style/.test(url.href),
      (route) => route.abort(),
    );
    await page.route(/\/$/, async (route) => {
      // Only the document is rewritten. The strip's own same-origin HEAD
      // request also ends in "/" and can still be in flight when the test
      // ends, so it must not wait on a fetch here.
      if (!route.request().isNavigationRequest()) return route.continue();
      const response = await route.fetch();
      const body = (await response.text()).replace(
        /<style data-vite-dev-id=[^>]*>[\s\S]*?<\/style>/g,
        '',
      );
      await route.fulfill({ response, body });
    });
    await page.goto('/');
    const strip = page.locator(STRIP);
    await expect(strip).toBeVisible();
    const height = await strip.evaluate((el) => el.getBoundingClientRect().height);
    expect(height).toBe(32);
  });

  test('the skip link is still the first tab stop', async ({ page }) => {
    await page.goto('/');
    await page.locator(STRIP).waitFor();
    await page.keyboard.press('Tab');
    await expect
      .poll(() =>
        page.evaluate(() => (document.activeElement as HTMLElement | null)?.getAttribute('href')),
      )
      .toBe('#main-content');
  });

  test('the strip is not rendered on the 404 page', async ({ page }) => {
    await page.goto('/404/');
    // Assert on the real page, so a stub without a strip cannot pass.
    await expectErrorPage(page, '404');
    await expect(page.locator(STRIP)).toHaveCount(0);
  });
});
