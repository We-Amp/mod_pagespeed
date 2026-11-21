// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { expect, type Page } from '@playwright/test';
import { readFileSync } from 'node:fs';
import { stubUmami } from './umami';

export const readFixture = (name: string) =>
  readFileSync(new URL(`../fixtures/scan/${name}.json`, import.meta.url), 'utf8');

export type Fixture =
  | 'full'
  | 'hosted'
  | 'unknown'
  | 'clean'
  | 'blocked'
  | 'sample-builder'
  | 'sample-static'
  | 'sample-product';

const GOOD_PSI = JSON.stringify({
  lighthouseResult: { categories: { performance: { score: 0.96 } }, audits: {} },
});

/**
 * Mock the scanner, PageSpeed Insights and the contact endpoint. `speed`
 * picks the PageSpeed Insights answer: the recorded fixtures (a poor mobile
 * score) or a good score with no flagged audit.
 */
export async function mockScan(
  page: Page,
  fixture: Fixture,
  opts: { speed?: 'fixture' | 'good'; scanDelay?: number; psiDelay?: number } = {},
) {
  const wait = (ms?: number) => (ms ? new Promise((done) => setTimeout(done, ms)) : null);
  await page.route('**/psi/v5/runPagespeed**', async (route) => {
    await wait(opts.psiDelay);
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body:
        opts.speed === 'good'
          ? GOOD_PSI
          : readFixture(
              route.request().url().includes('strategy=desktop') ? 'psi-desktop' : 'psi-mobile',
            ),
    });
  });
  await page.route('**/ai-readability/api/scan**', async (route) => {
    await wait(opts.scanDelay);
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: readFixture(`report-${fixture}`),
    });
  });
  await page.route('**/ai-readability/api/contact', (route) =>
    route.fulfill({ status: 200, contentType: 'application/json', body: '{"ok":true}' }),
  );
}

/** Load the page and start a scan; `settle` waits until all three tiles have a result. */
export async function startScan(page: Page, settle = true) {
  await stubUmami(page);
  await page.goto('/ai-readability/?ui=v2');
  await page.waitForLoadState('networkidle');
  await page.fill('#scan-url', 'studio.example.org');
  await page.click('#scan-submit');
  if (!settle) return;
  for (const tile of ['speed', 'airead', 'risk']) {
    await expect(page.locator(`[data-scan-tile="${tile}"]`)).not.toHaveAttribute(
      'data-state',
      'checking',
      { timeout: 15000 },
    );
  }
  await expect(page.locator('[data-scan-chip="agents"]')).toBeVisible();
}
