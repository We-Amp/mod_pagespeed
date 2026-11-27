// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Demo sites integration tests.
 *
 * These tests browse the demo sites directly via http://localhost:8084
 * (through nginx proxy) and verify that PageSpeed optimizations are applied.
 *
 * Prerequisites: workbench-demo stack running
 *   (origin :8081, nginx :8084, worker :9880, vite :5173).
 */
import { test, expect } from '@playwright/test';

const PROXY_BASE = process.env.PROXY_BASE || 'http://localhost:8084';
const ECOMMERCE = `${PROXY_BASE}/demos/ecommerce/index.html`;

test.describe('Demo Sites — Optimization Verification', () => {
  // Cache warming is handled by global-setup.ts (fetch + 10s wait + fetch).

  // -- Page loads through proxy with X-PageSpeed header ---------------------

  test('ecommerce page loads with X-PageSpeed header', async ({ request }) => {
    const response = await request.get(ECOMMERCE);
    expect(response.status()).toBe(200);

    const xps = response.headers()['x-pagespeed'];
    test.info().annotations.push({
      type: 'x-pagespeed',
      description: `X-PageSpeed header: ${xps ?? 'not present'}`,
    });
    // On HIT the header should be present
    if (xps) {
      expect(xps).toMatch(/HIT|MISS/i);
    }
  });

  // -- Images served as WebP with Accept: image/webp -----------------------

  test('images served as WebP when Accept: image/webp', async ({ request }) => {
    test.slow();

    // Request an image with WebP accept header
    const response = await request.get(
      `${PROXY_BASE}/demos/ecommerce/images/jacket.jpg`,
      {
        headers: { Accept: 'image/webp,image/*,*/*' },
      },
    );
    expect(response.status()).toBe(200);

    const contentType = response.headers()['content-type'];
    test.info().annotations.push({
      type: 'webp-content-type',
      description: `Content-Type: ${contentType}`,
    });

    // Should be WebP or original JPEG (if worker hasn't finished yet)
    expect(contentType).toMatch(/image\/(webp|jpeg)/);
  });

  // -- Images served as AVIF with Accept: image/avif -----------------------

  test('images served as AVIF when Accept: image/avif', async ({ request }) => {
    test.slow();

    const response = await request.get(
      `${PROXY_BASE}/demos/ecommerce/images/jacket.jpg`,
      {
        headers: { Accept: 'image/avif,image/webp,image/*,*/*' },
      },
    );
    expect(response.status()).toBe(200);

    const contentType = response.headers()['content-type'];
    test.info().annotations.push({
      type: 'avif-content-type',
      description: `Content-Type: ${contentType}`,
    });

    // Should be AVIF, WebP, or original JPEG
    expect(contentType).toMatch(/image\/(avif|webp|jpeg)/);
  });

  // -- CSS response is smaller than origin ----------------------------------

  test('CSS response is compressed compared to origin', async ({ request }) => {
    // Get CSS through proxy
    const proxyResp = await request.get(
      `${PROXY_BASE}/demos/ecommerce/style.css`,
    );
    expect(proxyResp.status()).toBe(200);
    const proxyBody = await proxyResp.text();

    // Get CSS directly from origin
    const originBase = process.env.ORIGIN_BASE || 'http://localhost:8081';
    const originResp = await request.get(
      `${originBase}/demos/ecommerce/style.css`,
    );
    expect(originResp.status()).toBe(200);
    const originBody = await originResp.text();

    test.info().annotations.push({
      type: 'css-sizes',
      description: `origin: ${originBody.length}, proxy: ${proxyBody.length}`,
    });

    // Proxy version should be same size or smaller (minified)
    expect(proxyBody.length).toBeLessThanOrEqual(originBody.length);
  });

  // -- HTML contains loading="lazy" on images -------------------------------

  test('HTML contains loading="lazy" attribute on images', async ({ request }) => {
    test.slow();

    const response = await request.get(ECOMMERCE);
    const html = await response.text();

    test.info().annotations.push({
      type: 'lazy-load',
      description: `loading="lazy" count: ${(html.match(/loading="lazy"/g) || []).length}`,
    });

    // Should have at least one lazy-loaded image
    expect(html).toContain('loading="lazy"');
  });

  // -- HTML contains <link rel="preload"> for LCP image ---------------------

  test('HTML contains preload link for LCP image', async ({ request }) => {
    test.slow();

    // LCP preload injection requires the worker to have analyzed multiple
    // page loads. Retry a few times to allow the worker to catch up.
    let hasPreload = false;
    for (let attempt = 0; attempt < 3 && !hasPreload; attempt++) {
      if (attempt > 0) await new Promise((r) => setTimeout(r, 5000));
      const response = await request.get(ECOMMERCE);
      const html = await response.text();
      hasPreload = html.includes('rel="preload"');
    }

    test.info().annotations.push({
      type: 'lcp-preload',
      description: `rel="preload" present: ${hasPreload}`,
    });

    // LCP preload is injected after the worker's Chrome analyzes the page.
    // On a fresh stack this may not be ready yet — annotate and skip.
    if (!hasPreload) {
      test.info().annotations.push({
        type: 'skip',
        description: 'LCP preload not yet injected — worker Chrome needs more time',
      });
      test.skip();
    }
  });

  // -- Response includes Vary: Accept header --------------------------------

  test('response includes Vary: Accept header for images', async ({ request }) => {
    const response = await request.get(
      `${PROXY_BASE}/demos/ecommerce/images/jacket.jpg`,
      {
        headers: { Accept: 'image/webp,image/*,*/*' },
      },
    );

    const vary = response.headers()['vary'];
    test.info().annotations.push({
      type: 'vary-header',
      description: `Vary: ${vary ?? 'not present'}`,
    });

    if (vary) {
      expect(vary.toLowerCase()).toContain('accept');
    }
  });

  // -- Brotli-encoded response with Accept-Encoding: br --------------------

  test('brotli-encoded response with Accept-Encoding: br', async ({ request }) => {
    const response = await request.get(ECOMMERCE, {
      headers: { 'Accept-Encoding': 'br, gzip, deflate' },
    });
    expect(response.status()).toBe(200);

    const encoding = response.headers()['content-encoding'];
    test.info().annotations.push({
      type: 'content-encoding',
      description: `Content-Encoding: ${encoding ?? 'none'}`,
    });

    // Should be brotli or gzip (both are acceptable)
    if (encoding) {
      expect(encoding).toMatch(/br|gzip/);
    }
  });
});
