// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Visual suite for the scanner pages. Opt-in: VISUAL=1 npx playwright test --project=visual
// Writes test-output/<run>/visual/{page}-{NN}-{state}-{viewport}.png for review; it asserts
// nothing about pixels, so it never gates CI.

import { mkdirSync } from 'node:fs';
import { readFileSync } from 'node:fs';
import { expect, test, type Page } from '@playwright/test';
import { installMocks, makeDeterministic, settle, type MockOptions, type Mocks } from './mocks';

const OUT_DIR = `${process.env.PW_RUN_DIR ?? 'test-output/local'}/visual`;

const PAGES = [
  { name: 'analyze', path: '/analyze/' },
  { name: 'ai-readability', path: '/ai-readability/' },
] as const;

const VIEWPORTS = [
  { name: 'desktop', width: 1280, height: 800 },
  { name: 'mobile', width: 390, height: 844 },
] as const;

type PageDef = (typeof PAGES)[number];
type ViewportDef = (typeof VIEWPORTS)[number];

interface Ctx {
  page: Page;
  mocks: Mocks;
  target: PageDef;
  viewport: ViewportDef;
  /** Writes test-output/<run>/visual/{page}-{state}-{viewport}.png. */
  shot: (state: string, options?: { fullPage?: boolean }) => Promise<void>;
}

// v2 is the default; ?ui=v2 pins it explicitly.
const V2 = '?ui=v2';
const URL_TO_SCAN = 'shop.example.com';

const fixtureText = (name: string) =>
  readFileSync(new URL(`../fixtures/scan/${name}`, import.meta.url), 'utf8');

/** A PSI response in which every audit passes: all scores 1, no savings. */
function cleanPsi(name: 'psi-mobile.json' | 'psi-desktop.json'): unknown {
  const body = JSON.parse(fixtureText(name));
  const lr = body.lighthouseResult;
  lr.categories.performance.score = 0.98;
  for (const audit of Object.values<Record<string, any>>(lr.audits)) {
    if (typeof audit.score === 'number') audit.score = 1;
    if (audit.score == null && audit.details) {
      delete audit.details.overallSavingsMs;
      delete audit.details.overallSavingsBytes;
    }
  }
  return body;
}

const tile = (page: Page, id: 'speed' | 'airead' | 'risk') =>
  page.locator(`[data-scan-tile="${id}"]`);
const status = (page: Page, id: 'speed' | 'airead' | 'risk') =>
  tile(page, id).locator('[data-scan-status]');
const health = (page: Page) => page.locator('[data-scan-health]');
const chipInput = (page: Page, id: string) => page.locator(`[data-scan-chip="${id}"] input`);
const leadMsg = (page: Page) => page.locator('[data-scan-lead-msg]');

/** Fill the scan box and submit. */
async function submitScan(page: Page): Promise<void> {
  await page.fill('#scan-url', URL_TO_SCAN);
  await page.click('#scan-submit');
}

/** Wait until none of the three tiles says Checking and the form is live. */
async function resultsSettled(page: Page): Promise<void> {
  for (const id of ['speed', 'airead', 'risk'] as const) {
    await expect(status(page, id)).not.toHaveText('Checking…', { timeout: 15000 });
  }
  await expect(page.locator('form[data-scan-lead] button[type="submit"]')).toBeEnabled();
}

/** Tab from `from` until the focused element matches `selector`. */
async function tabTo(page: Page, from: string, selector: string): Promise<void> {
  await page.locator(from).focus();
  for (let i = 0; i < 40; i++) {
    await page.keyboard.press('Tab');
    if (await page.evaluate((sel) => document.activeElement?.matches(sel) ?? false, selector)) {
      return;
    }
  }
  throw new Error(`Tab never reached ${selector}`);
}

/** Bring the lead form into view. */
const toForm = (page: Page) => page.locator('form[data-scan-lead]').scrollIntoViewIfNeeded();

interface State {
  /** State number from the plan; also the filename prefix. */
  n: number;
  slug: string;
  /** Filename prefix when it is not the zero-padded number (a variant such as 13b). */
  file?: string;
  title: string;
  /** Mock timing the state needs; the state body reads this. */
  mock: string;
  /** Network behaviour for the state; default is every source answering at once with its fixture. */
  opts?: MockOptions;
  run: (ctx: Ctx) => Promise<void>;
}

const pad = (n: number) => String(n).padStart(2, '0');

const STATES: State[] = [
  {
    n: 1,
    slug: 'landing',
    title: 'landing',
    mock: 'No requests; the page as first loaded, above the fold.',
    run: async ({ shot }) => {
      await shot('landing');
    },
  },
  {
    n: 2,
    slug: 'loading',
    title: 'loading, all three Checking',
    mock: 'scan, psiMobile and psiDesktop all gated (never released); capture after submit.',
    opts: { scan: { gate: true }, psiMobile: { gate: true }, psiDesktop: { gate: true } },
    run: async ({ page, mocks, shot }) => {
      await submitScan(page);
      await expect(health(page)).toHaveText('Checking 3 of 3 areas…');
      for (const id of ['speed', 'airead', 'risk'] as const) {
        await expect(status(page, id)).toHaveText('Checking…');
      }
      await shot('loading', { fullPage: true });
    },
  },
  {
    n: 3,
    slug: 'partial',
    title: 'partial, scan in and PSI pending',
    mock: 'scan fixture full answers at 0 ms; psiMobile and psiDesktop gated.',
    opts: { psiMobile: { gate: true }, psiDesktop: { gate: true } },
    run: async ({ page, mocks, shot }) => {
      await submitScan(page);
      await expect(status(page, 'airead')).not.toHaveText('Checking…', { timeout: 15000 });
      await expect(status(page, 'speed')).toHaveText('Checking…');
      await expect(health(page)).toContainText('Checking 1 of 3 areas');
      await shot('partial', { fullPage: true });
    },
  },
  {
    n: 4,
    slug: 'results-collapsed',
    title: 'results, all panels collapsed',
    mock: 'scan full and both PSI answer at 0 ms; capture once all three tiles settle.',
    run: async ({ page, mocks, shot }) => {
      await submitScan(page);
      await resultsSettled(page);
      await expect(health(page)).toContainText('areas checked need attention');
      for (const id of ['speed', 'airead', 'risk'] as const) {
        await expect(tile(page, id)).toHaveAttribute('aria-expanded', 'false');
      }
      await shot('results-collapsed', { fullPage: true });
    },
  },
  {
    n: 5,
    slug: 'panel-speed',
    title: 'Speed panel open',
    mock: 'As state 4, then open the Speed panel.',
    run: async ({ page, mocks, shot }) => {
      await submitScan(page);
      await resultsSettled(page);
      await tile(page, 'speed').click();
      await expect(tile(page, 'speed')).toHaveAttribute('aria-expanded', 'true');
      await expect(page.locator('[data-scan-panel="speed"]')).toBeVisible();
      await shot('panel-speed', { fullPage: true });
    },
  },
  {
    n: 6,
    slug: 'panel-ai',
    title: 'AI readability panel open',
    mock: 'As state 4, then open the AI readability panel.',
    run: async ({ page, mocks, shot }) => {
      await submitScan(page);
      await resultsSettled(page);
      await tile(page, 'airead').click();
      await expect(tile(page, 'airead')).toHaveAttribute('aria-expanded', 'true');
      await expect(page.locator('[data-scan-panel="airead"]')).toBeVisible();
      await shot('panel-ai', { fullPage: true });
    },
  },
  {
    n: 7,
    slug: 'panel-risk',
    title: 'Risk and SEO panel open',
    mock: 'As state 4, then open the Risk and SEO panel.',
    run: async ({ page, mocks, shot }) => {
      await submitScan(page);
      await resultsSettled(page);
      await tile(page, 'risk').click();
      await expect(tile(page, 'risk')).toHaveAttribute('aria-expanded', 'true');
      await expect(page.locator('[data-scan-panel="risk"]')).toBeVisible();
      await shot('panel-risk', { fullPage: true });
    },
  },
  {
    n: 8,
    slug: 'form-topics',
    title: 'form with the topic chips',
    mock: 'As state 4; scroll to the form; the chips from the full report are marked as found and none is selected.',
    run: async ({ page, mocks, shot }) => {
      await submitScan(page);
      await resultsSettled(page);
      await toForm(page);
      await expect(page.locator('[data-scan-chip] input:checked')).toHaveCount(0);
      await shot('form-topics', { fullPage: true });
    },
  },
  {
    n: 9,
    slug: 'form-sites-field',
    title: 'form with the sites field',
    mock: 'As state 8, then select SEO fixes or Legacy sites so the sites field shows.',
    run: async ({ page, mocks, shot }) => {
      await submitScan(page);
      await resultsSettled(page);
      await toForm(page);
      await chipInput(page, 'edge-seo').check();
      await expect(page.locator('[data-scan-sites]')).toBeVisible();
      await shot('form-sites-field', { fullPage: true });
    },
  },
  {
    n: 10,
    slug: 'form-cap-message',
    title: 'topic cap message',
    mock: 'As state 8, then select a fifth chip so the "Pick up to four topics." message shows.',
    run: async ({ page, mocks, shot }) => {
      await submitScan(page);
      await resultsSettled(page);
      await toForm(page);
      await chipInput(page, 'page-integrity').check();
      await expect(leadMsg(page)).toHaveText('Pick up to four topics.');
      await shot('form-cap-message', { fullPage: true });
    },
  },
  {
    n: 11,
    slug: 'submitted',
    title: 'submitted',
    mock: 'As state 8; contact answers 200 at 0 ms; fill email, submit, capture the confirmation.',
    run: async ({ page, mocks, shot }) => {
      await submitScan(page);
      await resultsSettled(page);
      await toForm(page);
      await page.fill('input[name="email"]', 'reviewer@example.com');
      await page.click('form[data-scan-lead] button[type="submit"]');
      await expect(
        page.getByText(
          'Sent. An engineer at We-Amp replies to reviewer@example.com within one business day.',
          { exact: true },
        ),
      ).toBeVisible();
      expect(mocks.calls.contact).toBe(1);
      await shot('submitted', { fullPage: true });
    },
  },
  {
    n: 12,
    slug: 'form-send-failure',
    title: 'form send failure, the request was not sent',
    mock: 'As state 8; contact status 500; capture the "We could not send your request" message with the form still filled in.',
    opts: { contact: { status: 500 } },
    run: async ({ page, mocks, shot }) => {
      await submitScan(page);
      await resultsSettled(page);
      await toForm(page);
      await page.fill('input[name="email"]', 'reviewer@example.com');
      await page.click('form[data-scan-lead] button[type="submit"]');
      await expect(leadMsg(page)).toHaveText('We could not send your request. Try again.');
      expect(mocks.calls.contact).toBe(1);
      await shot('form-send-failure', { fullPage: true });
    },
  },
  {
    n: 13,
    slug: 'scan-error',
    title: 'scan error',
    mock: 'scan status 503 with an error body (0 ms); both PSI answer 200.',
    opts: { scan: { status: 503, body: { error: 'busy — try again in a moment' } } },
    run: async ({ page, mocks, shot }) => {
      await submitScan(page);
      await expect(status(page, 'airead')).toHaveText('Not measured', { timeout: 15000 });
      await expect(status(page, 'speed')).not.toHaveText('Checking…');
      await shot('scan-error', { fullPage: true });
    },
  },
  {
    n: 14,
    slug: 'psi-rate-limited',
    title: 'PSI rate-limited',
    mock: 'both PSI status 429 with the default nginx HTML 429 page as body; scan fixture full at 0 ms.',
    opts: { psiMobile: { status: 429 }, psiDesktop: { status: 429 } },
    run: async ({ page, mocks, shot }) => {
      await submitScan(page);
      await expect(status(page, 'airead')).not.toHaveText('Checking…', { timeout: 15000 });
      await expect(status(page, 'speed')).toHaveText('Not measured', { timeout: 15000 });
      await shot('psi-rate-limited', { fullPage: true });
    },
  },
  {
    n: 15,
    slug: 'clean',
    title: 'clean report and all-clean speed',
    mock: 'scan fixture clean; both PSI answer 200 with every audit passing (body override).',
    opts: {
      scan: { fixture: 'clean' },
      psiMobile: { body: cleanPsi('psi-mobile.json') },
      psiDesktop: { body: cleanPsi('psi-desktop.json') },
    },
    run: async ({ page, mocks, shot }) => {
      await submitScan(page);
      await resultsSettled(page);
      await expect(health(page)).toHaveText('None of the 3 areas checked needs attention.');
      await shot('clean', { fullPage: true });
    },
  },
  {
    n: 4,
    slug: 'results-keyboard',
    title: 'results, keyboard focus on a tile',
    mock: 'As state 4; Tab from the results heading to the first tile.',
    run: async ({ page, shot }) => {
      await submitScan(page);
      await resultsSettled(page);
      await tabTo(page, '[data-scan-heading]', '[data-scan-tile]');
      await expect(tile(page, 'speed')).toBeFocused();
      await shot('results-keyboard', { fullPage: true });
    },
  },
  {
    n: 8,
    slug: 'form-keyboard',
    title: 'form, keyboard focus on a chip',
    mock: 'As state 8; Tab from the last tile to the first chip.',
    run: async ({ page, shot }) => {
      await submitScan(page);
      await resultsSettled(page);
      await tabTo(page, '[data-scan-tile="risk"]', '[data-scan-chip] input');
      await expect(page.locator('[data-scan-chip] input').first()).toBeFocused();
      await shot('form-keyboard', { fullPage: true });
    },
  },
  {
    n: 13,
    file: '13b',
    slug: 'scan-error-panel',
    title: 'scan error, AI readability panel open',
    mock: 'As state 13; open the AI readability tile.',
    opts: { scan: { status: 503, body: { error: 'busy — try again in a moment' } } },
    run: async ({ page, shot }) => {
      await submitScan(page);
      await expect(status(page, 'airead')).toHaveText('Not measured', { timeout: 15000 });
      await expect(status(page, 'speed')).not.toHaveText('Checking…');
      await tile(page, 'airead').click();
      await expect(tile(page, 'airead')).toHaveAttribute('aria-expanded', 'true');
      await expect(page.locator('[data-scan-panel="airead"]')).toBeVisible();
      await shot('scan-error-panel', { fullPage: true });
    },
  },
  {
    n: 14,
    file: '14b',
    slug: 'psi-rate-limited-panel',
    title: 'PSI rate-limited, Speed panel open',
    mock: 'As state 14; open the Speed tile.',
    opts: { psiMobile: { status: 429 }, psiDesktop: { status: 429 } },
    run: async ({ page, shot }) => {
      await submitScan(page);
      await expect(status(page, 'airead')).not.toHaveText('Checking…', { timeout: 15000 });
      await expect(status(page, 'speed')).toHaveText('Not measured', { timeout: 15000 });
      await tile(page, 'speed').click();
      await expect(tile(page, 'speed')).toHaveAttribute('aria-expanded', 'true');
      await expect(page.locator('[data-scan-panel="speed"]')).toBeVisible();
      await shot('psi-rate-limited-panel', { fullPage: true });
    },
  },
];

test.describe('scan UI visual states', () => {
  test.beforeAll(() => {
    mkdirSync(OUT_DIR, { recursive: true });
  });

  for (const target of PAGES) {
    for (const viewport of VIEWPORTS) {
      for (const state of STATES) {
        const name = `${state.file ?? pad(state.n)} ${state.title} [${target.name}, ${viewport.name}]`;
        const run = state.run;
        test(name, async ({ page }) => {
          await page.setViewportSize({ width: viewport.width, height: viewport.height });
          await makeDeterministic(page);
          const mocks = await installMocks(page, state.opts);
          // Warm-up visit: the dev server re-optimises its module graph on the
          // first hit of a page and reloads once; do that outside the capture.
          await page.goto(target.path + V2);
          await page.waitForLoadState('networkidle');
          await page.goto(target.path + V2);
          await page.waitForLoadState('networkidle');
          await settle(page);
          await run({
            page,
            mocks,
            target,
            viewport,
            shot: async (slug, options) => {
              await settle(page);
              // A full-page capture would otherwise paint the sticky site header
              // at the scroll offset, mid-page; and the content below the scan
              // results is not part of this suite.
              let clip: { x: number; y: number; width: number; height: number } | undefined;
              if (options?.fullPage) {
                await page.evaluate(() => {
                  for (const el of document.querySelectorAll<HTMLElement>('body *')) {
                    const pos = getComputedStyle(el).position;
                    if (pos === 'fixed' || pos === 'sticky') el.style.position = 'static';
                  }
                });
                await page.evaluate(() => window.scrollTo(0, 0));
                const box = await page.evaluate(() => {
                  const r = document.querySelector('[data-scan-results]');
                  return r ? r.getBoundingClientRect().bottom + window.scrollY : 0;
                });
                if (box > 0)
                  clip = { x: 0, y: 0, width: viewport.width, height: Math.ceil(box) + 24 };
              }
              const found = STATES.find((s) => s.slug === slug);
              const prefix = found?.file ?? pad(found?.n ?? 0);
              await page.screenshot({
                // The telemetry strip shows live timings; mask it for stable runs.
                mask: [page.locator('[data-ui="telemetry-strip"]')],
                maskColor: '#070809',
                path: `${OUT_DIR}/${target.name}-${prefix}-${slug}-${viewport.name}.png`,
                animations: 'disabled',
                caret: 'hide',
                fullPage: options?.fullPage ?? false,
                clip,
              });
            },
          });
        });
      }
    }
  }
});
