// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Global setup for Playwright E2E tests.
 *
 * Seeds the cache by requesting the ecommerce demo page through the proxy.
 * Runs once before all test files, replacing the duplicated beforeAll hooks
 * in demo-sites.spec.ts and urls.spec.ts.
 */
import { request } from '@playwright/test';

const PROXY_BASE = process.env.PROXY_BASE || 'http://localhost:8084';
const ECOMMERCE = `${PROXY_BASE}/demos/ecommerce/index.html`;

export default async function globalSetup() {
  const ctx = await request.newContext();
  try {
    // Wait for proxy to be reachable (may take a while in Docker on arm64)
    const deadline = Date.now() + 120_000;
    let seeded = false;
    while (Date.now() < deadline) {
      try {
        await ctx.get(ECOMMERCE, { timeout: 10_000 });
        seeded = true;
        break;
      } catch {
        await new Promise((r) => setTimeout(r, 3000));
      }
    }
    if (!seeded) {
      console.warn('Global setup: proxy not reachable after 120s — tests may flake');
      return;
    }
    // Give worker time to optimize
    await new Promise((r) => setTimeout(r, 10_000));
    // Second request should hit cache
    await ctx.get(ECOMMERCE, { timeout: 10_000 });
  } finally {
    await ctx.dispose();
  }
}
