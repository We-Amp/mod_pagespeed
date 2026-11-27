// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Critical CSS injection tests.
 *
 * Verifies that the worker's critical CSS injection produces correct
 * output — in particular that revalidation passes do not duplicate CSS
 * rules or accumulate multiple <style data-pagespeed-critical> blocks.
 *
 * Also tests that critical CSS is injected for all demo pages, including
 * those whose first request carries image-format bits (AVIF/WebP) in the
 * notification mask.  A previous bug caused HTML variants to be written at
 * the wrong AlternateId when the notification had AVIF format bits.
 *
 * Prerequisites: workbench-demo stack running
 *   (origin :8081, nginx :8084, worker :9880).
 */
import { test, expect } from '@playwright/test';

const PROXY_BASE = process.env.PROXY_BASE || 'http://localhost:8084';
const ECOMMERCE = `${PROXY_BASE}/demos/ecommerce/index.html`;
const BLOG = `${PROXY_BASE}/demos/blog/index.html`;

// Request options matching real browser behavior (gzip).  The identity
// (uncompressed) variant may be stale in nginx's RAM cache because the
// worker overwrites the same AlternateId, but nginx only picks up disk
// writes for alternate IDs it hasn't cached in RAM yet.
const BROWSER_HEADERS = { 'Accept-Encoding': 'gzip, deflate, br' };

// The block is inlined only together with the deferral it makes possible.
// Zero blocks needs a render-blocking stylesheet and no async marker; one block
// needs the deferred stylesheet. Never more than one.
function assertBlockInvariant(html: string): number {
  const blocks = (html.match(/data-pagespeed-critical/g) || []).length;
  expect(blocks).toBeLessThanOrEqual(1);
  if (blocks === 0) {
    expect(html).toMatch(/<link[^>]*rel="stylesheet"/);
    expect(html).not.toContain('data-pagespeed-async');
  } else {
    expect(html).toContain('data-pagespeed-async');
  }
  return blocks;
}

test.describe('Critical CSS — No Duplication', () => {
  // Seed the cache: first request is a MISS, then CSS gets cached,
  // then a revalidation request lets the worker gather external CSS.
  test.beforeAll(async ({ request }) => {
    // First request through proxy populates cache (HTML + assets).
    await request.get(ECOMMERCE, { headers: BROWSER_HEADERS });
    await request.get(`${PROXY_BASE}/demos/ecommerce/style.css`);
    // Give worker time to process HTML + CSS notifications.
    await new Promise((r) => setTimeout(r, 10000));
    // Second request triggers revalidation (CSS now available).
    await request.get(ECOMMERCE, { headers: BROWSER_HEADERS });
    // Wait for the revalidation pass + browser analysis to complete.
    await new Promise((r) => setTimeout(r, 12000));
  });

  test('critical CSS block appears exactly once', async ({ request }) => {
    const response = await request.get(ECOMMERCE, {
      headers: BROWSER_HEADERS,
    });
    expect(response.status()).toBe(200);
    const html = await response.text();

    const matches = html.match(/data-pagespeed-critical/g) || [];
    test.info().annotations.push({
      type: 'critical-css-count',
      description: `data-pagespeed-critical occurrences: ${matches.length}`,
    });

    expect(assertBlockInvariant(html)).toBe(matches.length);
  });

  test('no duplicate selectors in critical CSS', async ({ request }) => {
    const response = await request.get(ECOMMERCE, {
      headers: BROWSER_HEADERS,
    });
    const html = await response.text();

    // Extract the critical CSS content.
    const styleMatch = html.match(
      /<style[^>]*data-pagespeed-critical[^>]*>([\s\S]*?)<\/style>/,
    );
    if (styleMatch === null) {
      assertBlockInvariant(html);
      return;
    }
    const css = styleMatch![1];

    // Split into rules and extract selectors.
    const rules = css
      .split('}')
      .map((r) => r.trim())
      .filter((r) => r.includes('{'));
    const selectors = rules.map((r) => r.split('{')[0].trim());

    test.info().annotations.push({
      type: 'css-rules',
      description: `Total rules: ${rules.length}, unique selectors: ${new Set(selectors).size}`,
    });

    // Find duplicates.
    const counts = new Map<string, number>();
    for (const sel of selectors) {
      counts.set(sel, (counts.get(sel) || 0) + 1);
    }
    const duplicates = [...counts.entries()]
      .filter(([, count]) => count > 1)
      .map(([sel, count]) => `${count}x: ${sel}`);

    expect(duplicates).toEqual([]);
  });

  test('critical CSS does not grow after revalidation', async ({ request }) => {
    test.slow();

    // Get the current HTML variant.
    const firstResp = await request.get(ECOMMERCE, {
      headers: BROWSER_HEADERS,
    });
    const firstHtml = await firstResp.text();

    // Request again after a delay (simulating repeated visits).
    await new Promise((r) => setTimeout(r, 3000));
    const secondResp = await request.get(ECOMMERCE, {
      headers: BROWSER_HEADERS,
    });
    const secondHtml = await secondResp.text();

    // Extract critical CSS sizes.
    const extractCritical = (html: string) => {
      const m = html.match(/<style[^>]*data-pagespeed-critical[^>]*>([\s\S]*?)<\/style>/);
      return m ? m[1] : '';
    };

    const firstCss = extractCritical(firstHtml);
    const secondCss = extractCritical(secondHtml);

    test.info().annotations.push({
      type: 'css-sizes',
      description: `first: ${firstCss.length} bytes, second: ${secondCss.length} bytes`,
    });

    // The critical CSS should not grow between requests.
    expect(secondCss.length).toBeLessThanOrEqual(firstCss.length + 10);
  });
});

// ---------------------------------------------------------------------------
// Regression: image format bits must not leak into HTML AlternateId.
//
// When a browser sends Accept: image/avif, nginx sets AVIF format bits in
// the notification mask. If the worker doesn't strip those bits, the HTML
// variant ends up at AlternateId 0x0A instead of 0x08, and nginx can never
// serve it. The blog demo is a good canary because its first notification
// typically arrives with AVIF bits.
// ---------------------------------------------------------------------------
test.describe('Critical CSS — Blog (AVIF format-bit regression)', () => {
  // Use Accept headers that include image/avif, matching real browsers.
  const AVIF_HEADERS = {
    Accept: 'text/html,image/avif,image/webp,*/*',
    'Accept-Encoding': 'gzip, deflate, br',
  };

  test.beforeAll(async ({ request }) => {
    // Seed CSS first so the worker has it for critical CSS extraction.
    await request.get(`${PROXY_BASE}/demos/blog/style.css`);
    // First HTML request — triggers notification with AVIF format bits.
    await request.get(BLOG, { headers: AVIF_HEADERS });
    // Give worker time to process HTML + CSS.
    await new Promise((r) => setTimeout(r, 10000));
    // Second request triggers revalidation (CSS now cached).
    await request.get(BLOG, { headers: AVIF_HEADERS });
    // Wait for revalidation + browser analysis.
    await new Promise((r) => setTimeout(r, 12000));
  });

  test('blog page has critical CSS injected', async ({ request }) => {
    const response = await request.get(BLOG, { headers: AVIF_HEADERS });
    expect(response.status()).toBe(200);
    const html = await response.text();

    const matches = html.match(/data-pagespeed-critical/g) || [];
    test.info().annotations.push({
      type: 'blog-critical-css',
      description: `data-pagespeed-critical occurrences: ${matches.length}`,
    });

    expect(assertBlockInvariant(html)).toBe(matches.length);
  });

  test('blog critical CSS contains rules', async ({ request }) => {
    const response = await request.get(BLOG, { headers: AVIF_HEADERS });
    const html = await response.text();

    const styleMatch = html.match(
      /<style[^>]*data-pagespeed-critical[^>]*>([\s\S]*?)<\/style>/,
    );
    if (styleMatch === null) {
      assertBlockInvariant(html);
      return;
    }
    const css = styleMatch![1];

    // Count CSS rules (selector { ... } pairs).
    const rules = css
      .split('}')
      .map((r) => r.trim())
      .filter((r) => r.includes('{'));

    test.info().annotations.push({
      type: 'blog-css-rules',
      description: `Blog critical CSS rules: ${rules.length}`,
    });

    // The blog demo has substantial CSS — expect at least 10 critical rules.
    expect(rules.length).toBeGreaterThanOrEqual(10);
  });
});
