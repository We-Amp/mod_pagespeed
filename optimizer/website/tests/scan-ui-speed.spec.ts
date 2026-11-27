// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { readFileSync } from 'node:fs';
import { test, expect, type Page } from '@playwright/test';
import { stubUmami } from './helpers/umami';

// The Speed pillar of the scan interface. PageSpeed Insights and the
// scanner are always mocked; nothing here reaches a real service.

const fixture = (name: string) =>
  readFileSync(new URL(`./fixtures/scan/${name}`, import.meta.url), 'utf8');
const PSI_MOBILE = fixture('psi-mobile.json');
const PSI_DESKTOP = fixture('psi-desktop.json');
const PSI_429 = fixture('psi-429.json');

const SCAN_OK = JSON.stringify({
  cached: false,
  id: 'abc123',
  permalink: 'https://modpagespeed.com/ai-readability/r/abc123',
  report: { url: 'https://example.com/', grade: 'B', score: 81, categories: {} },
});

type Reply = { status: number; body: string } | 'hang' | 'abort';
interface Mocks {
  mobile: Reply;
  desktop: Reply;
}
interface Calls {
  mobile: number;
  desktop: number;
  scan: number;
}

const ok = (body: string): Reply => ({ status: 200, body });
const errorBody = (status: number, message: string): Reply => ({
  status,
  body: JSON.stringify({ error: { code: status, message } }),
});

async function mock(page: Page, mocks: Mocks, scanDelayMs = 0): Promise<Calls> {
  const calls: Calls = { mobile: 0, desktop: 0, scan: 0 };
  await page.route('**/psi/v5/runPagespeed**', async (route) => {
    const strategy = new URL(route.request().url()).searchParams.get('strategy') as
      'mobile' | 'desktop';
    calls[strategy] += 1;
    const reply = mocks[strategy];
    if (reply === 'abort') return route.abort();
    if (reply === 'hang') return; // never answers; the client times out
    await route.fulfill({
      status: reply.status,
      contentType: 'application/json',
      body: reply.body,
    });
  });
  await page.route('**/ai-readability/api/scan**', async (route) => {
    calls.scan += 1;
    if (scanDelayMs) await new Promise((r) => setTimeout(r, scanDelayMs));
    await route.fulfill({ status: 200, contentType: 'application/json', body: SCAN_OK });
  });
  return calls;
}

async function warm(page: Page) {
  await page.goto('/analyze/');
  await page.waitForLoadState('networkidle');
}

async function scan(page: Page) {
  await page.goto('/analyze/');
  await page.locator('form[data-scan-box] input[name="url"]').fill('https://example.com');
  await page.locator('form[data-scan-box] button[type="submit"]').click();
}

const tile = (page: Page) => page.locator('[data-scan-tile="speed"]');
const panel = (page: Page) => page.locator('[data-scan-panel="speed"]');

async function openSpeed(page: Page) {
  await tile(page).click();
  await expect(panel(page)).toBeVisible();
}

test.describe('Speed panel', () => {
  test.beforeEach(async ({ page }) => {
    await stubUmami(page);
  });

  test('a non-JSON 429 reads as the classified title on the tile, never a raw status', async ({
    page,
  }) => {
    const html429 = { status: 429, body: '<html><body>429 Too Many Requests</body></html>' };
    await mock(page, { mobile: html429, desktop: html429 });
    await warm(page);
    await scan(page);
    await expect(tile(page)).toHaveAttribute('data-state', 'none', { timeout: 15000 });
    await expect(tile(page)).toContainText('Rate limit reached. Try again in a minute.');
    await expect(tile(page)).not.toContainText('HTTP 429');
    await openSpeed(page);
    await expect(panel(page)).toContainText('Rate limit reached. Try again in a minute.');
  });

  test('shows two plates, the sentences and the top five fixes', async ({ page }) => {
    await mock(page, { mobile: ok(PSI_MOBILE), desktop: ok(PSI_DESKTOP) });
    await warm(page);
    await scan(page);
    await expect(tile(page)).toHaveAttribute('data-state', 'poor', { timeout: 15000 });
    await openSpeed(page);

    const mobile = panel(page).locator('[data-speed-plate="mobile"]');
    await expect(mobile).toContainText('46');
    await expect(mobile).toContainText('Poor');
    await expect(panel(page).locator('[data-speed-plate="desktop"]')).toContainText('Desktop');

    await expect(panel(page)).toContainText(/mod_pagespeed addresses \d+ of \d+ flagged audits\./);
    await expect(panel(page)).toContainText(
      /Across the fixable audits PSI flagged, mod_pagespeed could recover/,
    );
    await expect(panel(page).getByRole('heading', { name: 'Fixes on your server' })).toBeVisible();

    // The cut blocks stay cut.
    await expect(panel(page)).not.toContainText('Which part you configure card');
    await expect(panel(page).locator('p', { hasText: /^Caveat:/ })).toHaveCount(0);

    const rows = panel(page).locator('[data-speed-fix]');
    await expect(rows).toHaveCount(5);
    await expect(rows.first()).toContainText(/Fully addressed|Partial fix/);

    const showAll = panel(page).getByRole('button', { name: /^Show all \d+$/ });
    const total = Number(((await showAll.textContent()) ?? '').replace(/\D/g, ''));
    expect(total).toBeGreaterThan(5);
    await showAll.click();
    await expect(rows).toHaveCount(total);
    await panel(page).getByRole('button', { name: 'Show fewer' }).click();
    await expect(rows).toHaveCount(5);
  });

  test('filter names are inline code buttons that keep their event', async ({ page, context }) => {
    await mock(page, { mobile: ok(PSI_MOBILE), desktop: ok(PSI_DESKTOP) });
    await warm(page);
    await scan(page);
    await expect(tile(page)).toHaveAttribute('data-state', 'poor', { timeout: 15000 });
    await openSpeed(page);
    const chip = panel(page).locator('[data-speed-fix] button[data-umami-event]').first();
    await expect(chip).toHaveAttribute('data-umami-event', 'psi-result-filter-click');
    const name = (await chip.textContent()) ?? '';
    await expect(chip).toHaveAttribute('data-umami-event-filter', name);
    const popup = context.waitForEvent('page');
    await chip.click();
    expect((await popup).url()).toContain('/docs/');
  });

  test('audits outside reach are one collapsed line with titles only', async ({ page }) => {
    await mock(page, { mobile: ok(PSI_MOBILE), desktop: ok(PSI_DESKTOP) });
    await warm(page);
    await scan(page);
    await expect(tile(page)).toHaveAttribute('data-state', 'poor', { timeout: 15000 });
    await openSpeed(page);
    const line = panel(page).getByText(/audits? mod_pagespeed does not address/);
    await expect(line).toBeVisible();
    const toggle = panel(page).getByRole('button', { name: 'Show them' });
    await expect(toggle).toHaveAttribute('aria-expanded', 'false');
    const list = panel(page).locator(`#${await toggle.getAttribute('aria-controls')}`);
    await expect(list).toBeHidden();
    await toggle.click();
    await expect(list).toBeVisible();
    await expect(list.locator('li').first()).not.toBeEmpty();
  });

  test('the configuration is collapsed; toggle and copy keep their events', async ({
    page,
    context,
  }) => {
    await context.grantPermissions(['clipboard-read', 'clipboard-write']);
    await mock(page, { mobile: ok(PSI_MOBILE), desktop: ok(PSI_DESKTOP) });
    await warm(page);
    await scan(page);
    await expect(tile(page)).toHaveAttribute('data-state', 'poor', { timeout: 15000 });
    await openSpeed(page);

    await expect(panel(page).locator('[data-speed-snippet]')).toHaveCount(0);
    await panel(page).getByRole('button', { name: 'Show the configuration' }).click();
    const snippet = panel(page).locator('[data-speed-snippet]');
    await expect(snippet).toContainText('pagespeed on;');
    await expect(snippet).toContainText('factory_worker');

    const worker = panel(page).getByRole('button', { name: 'Optimizer worker' });
    const module = panel(page).getByRole('button', { name: 'Module' });
    await expect(worker).toHaveAttribute('data-umami-event', 'psi-edition-toggle');
    await expect(module).toHaveAttribute('data-umami-event-edition', '1.1');
    await expect(worker).toHaveAttribute('aria-pressed', 'true');
    await module.click();
    await expect(module).toHaveAttribute('aria-pressed', 'true');
    await expect(panel(page).locator('[data-speed-snippet]')).toContainText(
      'pagespeed EnableFilters',
    );

    const copy = panel(page).locator('[data-umami-event="psi-result-config-copy"]');
    await expect(copy).toHaveAttribute('data-umami-event', 'psi-result-config-copy');
    await page.evaluate(() => {
      const w = window as unknown as { siteAnnounce: (_m: string) => void; __calls: string[] };
      const original = w.siteAnnounce;
      w.__calls = [];
      w.siteAnnounce = (m: string) => {
        w.__calls.push(m);
        original(m);
      };
    });
    await expect(copy).toHaveAccessibleName('Copy');
    // From the keyboard: focus the button and press Enter.
    await copy.focus();
    await page.keyboard.press('Enter');
    // Spoken once by the page's status message; the visible text says
    // "Copied" while the accessible name stays "Copy".
    await expect(copy).toHaveText('Copied');
    await expect(copy).toHaveAccessibleName('Copy');
    await expect(copy).toBeFocused();
    await expect
      .poll(() => page.evaluate(() => (window as unknown as { __calls: string[] }).__calls))
      .toEqual(['Copied to clipboard']);
    await expect(page.locator('#copy-status')).toHaveText('Copied to clipboard');
    const clip = await page.evaluate(() => navigator.clipboard.readText());
    expect(clip).toContain('pagespeed EnableFilters');
    // Focus stays on the button after the label goes back to "Copy".
    await expect(copy).toHaveText('Copy', { timeout: 4000 });
    await expect(copy).toBeFocused();

    await panel(page).getByRole('button', { name: 'Hide the configuration' }).click();
    await expect(panel(page).locator('[data-speed-snippet]')).toHaveCount(0);
  });

  test('all clean gives the one sentence', async ({ page }) => {
    const clean = JSON.stringify({
      lighthouseResult: { categories: { performance: { score: 0.97 } }, audits: {} },
    });
    await mock(page, { mobile: ok(clean), desktop: ok(clean) });
    await warm(page);
    await scan(page);
    await expect(tile(page)).toHaveAttribute('data-state', 'good', { timeout: 15000 });
    await openSpeed(page);
    await expect(panel(page)).toContainText(
      'PSI flagged no failing performance audits on this page.',
    );
    await expect(panel(page).getByRole('heading', { name: 'Fixes on your server' })).toHaveCount(0);
  });

  test('flagged audits nothing can fix give the cannot-fix sentence', async ({ page }) => {
    const odd = JSON.stringify({
      lighthouseResult: {
        categories: { performance: { score: 0.7 } },
        audits: { 'no-such-audit': { id: 'no-such-audit', title: 'Made up', score: 0.2 } },
      },
    });
    await mock(page, { mobile: ok(odd), desktop: ok(odd) });
    await warm(page);
    await scan(page);
    await expect(tile(page)).toHaveAttribute('data-state', 'needs-work', { timeout: 15000 });
    await openSpeed(page);
    await expect(panel(page)).toContainText(
      'PSI flagged 1 performance audit; mod_pagespeed cannot automatically fix any of them on this page.',
    );
  });

  const failures: Array<{ name: string; reply: Reply; title: string; body?: string }> = [
    {
      name: '429',
      reply: { status: 429, body: PSI_429 },
      title: 'Rate limit reached. Try again in a minute.',
      body: 'You’ve hit the per-visitor rate limit on this page.',
    },
    {
      name: '400',
      reply: errorBody(400, 'Bad URL'),
      title: 'PageSpeed Insights could not analyze that URL.',
      body: 'Bad URL Common causes',
    },
    {
      name: '5xx',
      reply: errorBody(503, 'down'),
      title: 'PageSpeed Insights returned an error. Try again later.',
      body: 'returned a server error',
    },
    {
      name: 'network',
      reply: 'abort',
      title: 'PageSpeed Insights did not answer.',
      body: 'Check your network',
    },
  ];
  for (const f of failures) {
    test(`${f.name}: the Speed tile is not measured and the others are not`, async ({ page }) => {
      await mock(page, { mobile: f.reply, desktop: f.reply });
      await warm(page);
      await scan(page);
      await expect(tile(page)).toHaveAttribute('data-state', 'none', { timeout: 15000 });
      await expect(tile(page)).toContainText('Not measured');
      await expect(page.locator('[data-scan-tile="airead"]')).toHaveAttribute('data-state', 'good');
      await openSpeed(page);
      await expect(panel(page)).toContainText(f.title);
      if (f.body) await expect(panel(page)).toContainText(f.body);
      await expect(panel(page).getByRole('button', { name: 'Try again' })).toBeVisible();
    });
  }

  test('a PSI timeout reads as not measured with its reason', async ({ page }) => {
    await page.clock.install();
    await mock(page, { mobile: 'hang', desktop: 'hang' });
    await warm(page);
    // The client arms its 60s timer when it issues each PSI request, which is
    // after the tile first reads 'checking'. Advance the fake clock only once
    // both requests are in flight; otherwise the jump lands before the timers
    // exist and nothing ever fires.
    const inFlight = ['mobile', 'desktop'].map((strategy) =>
      page.waitForRequest(
        (req) =>
          req.url().includes('/psi/v5/runPagespeed') &&
          new URL(req.url()).searchParams.get('strategy') === strategy,
      ),
    );
    await scan(page);
    await expect(tile(page)).toHaveAttribute('data-state', 'checking');
    await Promise.all(inFlight);
    await page.clock.fastForward(61_000);
    await expect(tile(page)).toHaveAttribute('data-state', 'none', { timeout: 15000 });
    await expect(tile(page)).toContainText('PageSpeed Insights did not answer.');
    await openSpeed(page);
    await expect(panel(page)).toContainText('PageSpeed Insights did not answer.');
    await expect(panel(page)).toContainText('PageSpeed Insights did not answer within a minute.');
    await expect(panel(page)).not.toContainText('Check your network');
  });

  test('one strategy failing shows what returned and marks the other', async ({ page }) => {
    await mock(page, { mobile: errorBody(503, 'down'), desktop: ok(PSI_DESKTOP) });
    await warm(page);
    await scan(page);
    await expect(tile(page)).not.toHaveAttribute('data-state', 'checking', { timeout: 15000 });
    await expect(tile(page)).not.toHaveAttribute('data-state', 'none');
    await expect(tile(page)).toContainText('Mobile —');
    await openSpeed(page);
    const mobile = panel(page).locator('[data-speed-plate="mobile"]');
    await expect(mobile).toContainText('Not measured');
    await expect(mobile).toContainText('—');
    await expect(panel(page).locator('[data-speed-partial]')).toContainText('Mobile not measured');
    await expect(panel(page).locator('[data-speed-plate="desktop"]')).not.toContainText(
      'Not measured',
    );
    await expect(panel(page).locator('[data-speed-fix]').first()).toBeVisible();
  });

  test('a mapping chunk failure is scoped to the Speed tile and needs a reload', async ({
    page,
  }) => {
    await mock(page, { mobile: ok(PSI_MOBILE), desktop: ok(PSI_DESKTOP) });
    await page.route('**/psi-mps-mapping*', (route) => route.abort());
    await warm(page);
    await scan(page);
    await expect(tile(page)).toHaveAttribute('data-state', 'none', { timeout: 15000 });
    await expect(page.locator('[data-scan-tile="airead"]')).toHaveAttribute('data-state', 'good');
    await openSpeed(page);
    await expect(panel(page)).toContainText('This page needs a reload.');
    await expect(panel(page)).toContainText('Reload the page');
    await expect(panel(page).getByRole('button', { name: 'Try again' })).toHaveCount(0);
    const visible = await page.locator('body').innerText();
    expect(visible).not.toContain('psi-mps-mapping');
    expect(visible).not.toContain('/_astro/');
  });

  test('a late chunk failure never counts toward the health line or the live region', async ({
    page,
  }) => {
    const calls = await mock(page, { mobile: ok(PSI_MOBILE), desktop: ok(PSI_DESKTOP) }, 2500);
    await page.route('**/psi-mps-mapping*', (route) => route.abort());
    await warm(page);
    await page.addInitScript(() => {
      const seen: string[] = [];
      Object.defineProperty(window, '__live', { value: seen });
      document.addEventListener('DOMContentLoaded', () => {
        const live = document.querySelector('[data-scan-live]')!;
        new MutationObserver(() => seen.push(live.textContent ?? '')).observe(live, {
          childList: true,
          characterData: true,
          subtree: true,
        });
      });
    });
    await scan(page);
    await expect(tile(page)).toHaveAttribute('data-state', 'none', { timeout: 15000 });
    await expect(page.locator('[data-scan-health]')).toContainText('Checking 2 of 3 areas');
    await expect(page.locator('[data-scan-tile="airead"]')).toHaveAttribute('data-state', 'good', {
      timeout: 15000,
    });
    await expect(page.locator('[data-scan-health]')).toHaveText(
      'The one area checked needs no attention.',
    );
    const seen = await page.evaluate(() => (window as unknown as { __live: string[] }).__live);
    expect(seen.join(' | ')).toContain('Speed not measured');
    expect(seen.join(' | ')).not.toContain('Speed ready');

    // A second run does not spend PSI calls: only a reload recovers.
    const before = calls.mobile + calls.desktop;
    await page.locator('form[data-scan-box] button[type="submit"]').click();
    await expect(tile(page)).toHaveAttribute('data-state', 'none');
    await expect(page.locator('[data-scan-tile="airead"]')).toHaveAttribute('data-state', 'good', {
      timeout: 15000,
    });
    expect(calls.mobile + calls.desktop).toBe(before);
  });

  test('Try again re-runs only PageSpeed Insights', async ({ page }) => {
    const mocks: Mocks = { mobile: errorBody(503, 'down'), desktop: errorBody(503, 'down') };
    const calls = await mock(page, mocks);
    await warm(page);
    await scan(page);
    await expect(tile(page)).toHaveAttribute('data-state', 'none', { timeout: 15000 });
    await expect(page.locator('[data-scan-tile="airead"]')).toHaveAttribute('data-state', 'good');
    expect(calls).toEqual({ mobile: 1, desktop: 1, scan: 1 });
    mocks.mobile = ok(PSI_MOBILE);
    mocks.desktop = ok(PSI_DESKTOP);
    await openSpeed(page);
    await panel(page).getByRole('button', { name: 'Try again' }).click();
    await expect(tile(page)).toHaveAttribute('data-state', 'poor', { timeout: 15000 });
    expect(calls).toEqual({ mobile: 2, desktop: 2, scan: 1 });
  });
});
