// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { readFileSync } from 'node:fs';
import { test, expect, type Page } from '@playwright/test';
import { stubUmami } from './helpers/umami';

// Where focus goes, and what the live region says, while a scan runs. Focus
// moves to the results heading when the scan starts. A result that lands later
// is announced, and moves focus to the heading only when focus is on nothing. The scanner and PageSpeed Insights are mocked and held back by
// the test; the real services are never called.

const report = readFileSync(new URL('./fixtures/scan/report-full.json', import.meta.url), 'utf8');

interface Gates {
  releaseScan: () => void;
  releaseSpeed: () => void;
}

async function start(page: Page): Promise<Gates> {
  let releaseScan!: () => void;
  let releaseSpeed!: () => void;
  const scanGate = new Promise<void>((r) => (releaseScan = r));
  const speedGate = new Promise<void>((r) => (releaseSpeed = r));
  await page.route('**/psi/v5/runPagespeed**', async (route) => {
    await speedGate;
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({ lighthouseResult: { categories: { performance: { score: 0.9 } } } }),
    });
  });
  await page.route('**/ai-readability/api/scan**', async (route) => {
    await scanGate;
    await route.fulfill({ status: 200, contentType: 'application/json', body: report });
  });
  await stubUmami(page);
  await page.goto('/ai-readability/');
  await page.waitForLoadState('networkidle');
  await page.fill('#scan-url', 'shop.example.com');
  await page.click('#scan-submit');
  await expect(page.locator('[data-scan-results]')).toBeVisible();
  return { releaseScan, releaseSpeed };
}

const heading = (page: Page) => page.locator('[data-scan-heading]');
const live = (page: Page) => page.locator('[data-scan-live]');
const aiStatus = (page: Page) => page.locator('[data-scan-tile="airead"] [data-scan-status]');

test.describe('focus and announcements during a scan', () => {
  test('a started scan moves focus to the results heading and says what it is checking', async ({
    page,
  }) => {
    const gates = await start(page);
    await expect(heading(page)).toBeFocused();
    await expect(live(page)).toHaveText(
      'Checking shop.example.com. Usually takes 20 to 40 seconds.',
    );
    // The scan box and the result give the same duration.
    await expect(page.locator('form[data-scan-box]')).toContainText(
      'usually takes 20 to 40 seconds',
    );
    await expect(page.locator('[data-scan-hint]')).toHaveText('Usually takes 20 to 40 seconds.');
    gates.releaseScan();
    gates.releaseSpeed();
  });

  test('focus stays in the email field when a result lands while the visitor is typing', async ({
    page,
  }) => {
    const gates = await start(page);
    const email = page.locator('form[data-scan-lead] input[name="email"]');
    await expect(email).toBeVisible();
    await email.click();
    await page.keyboard.type('ops@exam');
    await expect(email).toBeFocused();

    gates.releaseScan();
    await expect(aiStatus(page)).not.toHaveText('Checking…', { timeout: 15000 });
    await expect(live(page)).toHaveText('AI readability ready: grade C, 64 of 100.');
    await expect(email).toBeFocused();
    await page.keyboard.type('ple.com');
    await expect(email).toHaveValue('ops@example.com');

    gates.releaseSpeed();
    await expect(page.locator('[data-scan-tile="speed"] [data-scan-status]')).not.toHaveText(
      'Checking…',
      { timeout: 15000 },
    );
    await expect(email).toBeFocused();
  });

  test('focus stays on a tile the visitor moved to', async ({ page }) => {
    const gates = await start(page);
    const tile = page.locator('[data-scan-tile="risk"]');
    await tile.focus();
    gates.releaseScan();
    await expect(aiStatus(page)).not.toHaveText('Checking…', { timeout: 15000 });
    await expect(tile).toBeFocused();
    gates.releaseSpeed();
  });

  test('focus stays on the scan button when the visitor went back to it', async ({ page }) => {
    const gates = await start(page);
    await expect(heading(page)).toBeFocused();
    await page.locator('#scan-submit').focus();
    gates.releaseScan();
    await expect(aiStatus(page)).not.toHaveText('Checking…', { timeout: 15000 });
    await expect(page.locator('#scan-submit')).toBeFocused();
    gates.releaseSpeed();
  });

  test('the first result moves focus to the heading when focus is on nothing', async ({ page }) => {
    const gates = await start(page);
    await page.evaluate(() => (document.activeElement as HTMLElement | null)?.blur());
    expect(await page.evaluate(() => document.activeElement === document.body)).toBe(true);
    gates.releaseScan();
    await expect(aiStatus(page)).not.toHaveText('Checking…', { timeout: 15000 });
    await expect(heading(page)).toBeFocused();
    gates.releaseSpeed();
  });

  test('the live region is filled after the results are shown, not in the same step', async ({
    page,
  }) => {
    // Record what the live region held at each change, from before the scan.
    await page.addInitScript(() => {
      const seen: string[] = [];
      Object.defineProperty(window, '__live', { value: seen });
      document.addEventListener('DOMContentLoaded', () => {
        const region = document.querySelector('[data-scan-live]')!;
        const results = document.querySelector<HTMLElement>('[data-scan-results]')!;
        new MutationObserver(() => {
          if (!results.hidden) seen.push(region.textContent ?? '');
        }).observe(results, { attributes: true, attributeFilter: ['hidden'] });
        new MutationObserver(() => seen.push(region.textContent ?? '')).observe(region, {
          childList: true,
          characterData: true,
          subtree: true,
        });
      });
    });
    const gates = await start(page);
    await expect(live(page)).toHaveText(
      'Checking shop.example.com. Usually takes 20 to 40 seconds.',
    );
    const seen = await page.evaluate(() => (window as unknown as { __live: string[] }).__live);
    // Empty when the section appeared; the text came in a later step.
    expect(seen[0]).toBe('');
    expect(seen[seen.length - 1]).toBe(
      'Checking shop.example.com. Usually takes 20 to 40 seconds.',
    );
    gates.releaseScan();
    gates.releaseSpeed();
  });
});
