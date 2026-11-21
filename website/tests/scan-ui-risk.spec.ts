// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { readFileSync } from 'node:fs';
import { test, expect, type Page } from '@playwright/test';
import { stubUmami } from './helpers/umami';

// The Risk & SEO panel: four rows, a state chip each, one verdict sentence,
// and the "what is planned" sentence and link only when the lens's gate
// fires. The scanner and PageSpeed Insights are mocked.

type Report = Record<string, unknown>;

const fixture = (name: string): Report =>
  JSON.parse(readFileSync(`tests/fixtures/scan/report-${name}.json`, 'utf8')).report;

const PSI = {
  lighthouseResult: { categories: { performance: { score: 0.9 } }, audits: {} },
};

interface Mock {
  scan: number;
  psi: number;
  body: () => { status: number; json: unknown };
}

async function mock(page: Page, report: Report | null, error?: { status: number }): Promise<Mock> {
  const m: Mock = {
    scan: 0,
    psi: 0,
    body: () =>
      error
        ? { status: error.status, json: { error: 'scanner unavailable' } }
        : { status: 200, json: { cached: false, id: 'x', permalink: '', report } },
  };
  await page.route('**/psi/v5/runPagespeed**', async (route) => {
    m.psi += 1;
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify(PSI),
    });
  });
  await page.route('**/ai-readability/api/scan**', async (route) => {
    m.scan += 1;
    const { status, json } = m.body();
    await route.fulfill({ status, contentType: 'application/json', body: JSON.stringify(json) });
  });
  return m;
}

async function scan(page: Page) {
  await page.goto('/ai-readability/');
  await page.waitForLoadState('networkidle');
  await page.fill('#scan-url', 'example.com');
  await page.click('#scan-submit');
  await expect(page.locator('[data-scan-tile="risk"] [data-scan-status]')).not.toHaveText(
    'Checking…',
    { timeout: 15000 },
  );
  await page.click('[data-scan-tile="risk"]');
  return page.locator('[data-scan-panel="risk"]');
}

const row = (panel: ReturnType<Page['locator']>, key: string) =>
  panel.locator(`[data-risk-row="${key}"]`);

test.beforeEach(async ({ page }) => {
  await stubUmami(page);
});

test.describe('Risk & SEO panel', () => {
  test('attention: four rows in order, chips, sentences, gated sentences and link events', async ({
    page,
  }) => {
    await mock(page, fixture('full'));
    const panel = await scan(page);
    await expect(page.locator('[data-scan-tile="risk"] [data-scan-value]')).toHaveText(
      '4 need attention',
    );
    await expect(panel.locator('[data-risk-row] h3')).toHaveText([
      'Pre-consent leak',
      'Script inventory',
      'SEO defects',
      'Response exposure',
    ]);
    for (const key of ['preConsentLeak', 'scriptInventory', 'seoDefects', 'responseExposure']) {
      await expect(row(panel, key).locator('.scan-risk-chip')).toHaveText('Attention');
      await expect(row(panel, key).locator('[data-risk-sentence]')).toHaveCount(1);
      await expect(row(panel, key).locator('a')).toHaveCount(1);
    }
    await expect(row(panel, 'preConsentLeak').locator('[data-risk-building]')).toHaveText(
      'Consent enforcement at the origin is planned: third-party tags would stay inert at the server until the consent cookie grants them.',
    );
    await expect(row(panel, 'seoDefects').locator('[data-risk-building]')).toHaveText(
      'Issues like these are the kind a rule on the server can correct in the HTML it serves, without a CMS release. The free edge-SEO pack for mod_pagespeed is a template for writing those rules, starting in report mode.',
    );
    await expect(row(panel, 'seoDefects').locator('[data-risk-sentence]')).toContainText(
      '3 issues: canonical 1, title 1, structured data 1.',
    );
    await expect(row(panel, 'responseExposure').locator('[data-risk-building]')).toContainText(
      'A response-firewall pack for mod_pagespeed is planned.',
    );
    // One verdict sentence per row: no counts, host lists, CSP or server lines.
    const text = await panel.locator('[data-risk-sentence]').allTextContents();
    expect(text.join('\n')).not.toMatch(
      /script\(s\)|from other hosts|CSP:|Server header|CDN headers|jquery|google-analytics|…/,
    );
    await expect(row(panel, 'scriptInventory').locator('[data-risk-sentence]')).toContainText(
      'Some scripts on this page need attention.',
    );
    const detail = panel.locator('[data-risk-detail]');
    await expect(detail).toHaveCount(1);
    await expect(row(panel, 'scriptInventory').locator('[data-risk-detail]')).toHaveText(
      /^Flagged: /,
    );
    const links = {
      preConsentLeak: ['/platform/consent/', 'airead-cta-consent'],
      scriptInventory: ['/platform/', 'airead-cta-page-integrity'],
      seoDefects: ['/platform/edge-seo/', 'airead-cta-edge-seo'],
      responseExposure: ['/platform/#pack-response-firewall', 'airead-cta-response-firewall'],
    };
    for (const [key, [href, event]] of Object.entries(links)) {
      const a = row(panel, key).locator('a');
      await expect(a).toHaveAttribute('href', href);
      await expect(a).toHaveAttribute('data-umami-event', event);
    }
  });

  test('desktop: the secondary text is plain paragraphs, no disclosure', async ({ page }) => {
    await mock(page, fixture('full'));
    const panel = await scan(page);
    await expect(panel.locator('details')).toHaveCount(0);
    await expect(panel.locator('[data-risk-building]').first()).toBeVisible();
  });

  test('mobile: each row with secondary text has one closed disclosure', async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await mock(page, fixture('full'));
    const panel = await scan(page);
    const details = panel.locator('details');
    const withText = panel.locator('[data-risk-row]:has([data-risk-building])');
    const rows = await withText.count();
    expect(rows).toBeGreaterThan(0);
    await expect(details).toHaveCount(rows);
    await expect(details.locator('summary')).toContainText(Array(rows).fill('What is planned'));
    for (let i = 0; i < rows; i++) {
      await expect(details.nth(i)).not.toHaveAttribute('open', /.*/);
      await expect(details.nth(i).locator('[data-risk-building]').first()).toBeHidden();
    }
    // The verdict and the Flagged line stay visible, and the link stays outside.
    await expect(panel.locator('[data-risk-sentence]').first()).toBeVisible();
    await expect(row(panel, 'scriptInventory').locator('[data-risk-detail]')).toBeVisible();
    await expect(row(panel, 'seoDefects').locator('a')).toBeVisible();
    // Two paragraphs on the script-inventory row share one disclosure.
    await expect(row(panel, 'scriptInventory').locator('details [data-risk-building]')).toHaveCount(
      2,
    );
    await expect(details.locator('summary')).toContainText(Array(rows).fill('▾'));
    const box = await details.nth(0).locator('summary').boundingBox();
    expect(box!.height).toBeGreaterThanOrEqual(44);
    await details.nth(0).locator('summary').click();
    await expect(details.nth(0).locator('summary')).toContainText('▴');
    await expect(details.nth(0).locator('[data-risk-building]').first()).toBeVisible();
  });

  test('clean: four clean rows with no gated sentence and no link', async ({ page }) => {
    await mock(page, fixture('clean'));
    const panel = await scan(page);
    await expect(page.locator('[data-scan-tile="risk"] [data-scan-status]')).toHaveText(
      'Nothing flagged',
    );
    await expect(panel.locator('.scan-risk-chip')).toHaveText([
      'Nothing flagged',
      'Nothing flagged',
      'Nothing flagged',
      'Nothing flagged',
    ]);
    await expect(panel.locator('a')).toHaveCount(0);
    await expect(panel.locator('[data-risk-building]')).toHaveCount(0);
    await expect(panel.locator('[data-risk-detail]')).toHaveCount(0);
  });

  test('blocked and error lenses are muted not-measured rows, k shrinks', async ({ page }) => {
    await mock(page, fixture('blocked'));
    const panel = await scan(page);
    await expect(page.locator('[data-scan-tile="risk"] [data-scan-status]')).toHaveText(
      'Not measured',
    );
    await expect(panel.locator('.scan-risk-chip')).toHaveText([
      'Not measured',
      'Not measured',
      'Not measured',
      'Not measured',
    ]);
    // the response-exposure error shape has a reason and no note: the error default shows
    await expect(row(panel, 'responseExposure').locator('[data-risk-sentence]')).toHaveText(
      'Not measured: the scanner could not complete this check.',
    );
    await expect(panel.locator('a')).toHaveCount(0);
    await expect(panel.getByRole('button', { name: 'Try again' })).toBeVisible();
  });

  test('a blocked lens with a note shows the note and the others still count', async ({ page }) => {
    const report = fixture('clean');
    report.seoDefects = {
      status: 'blocked',
      verdict: 'unknown',
      note: 'Blocked by a challenge page.',
    };
    await mock(page, report);
    const panel = await scan(page);
    await expect(page.locator('[data-scan-tile="risk"] [data-scan-value]')).toHaveText(
      '0 of 3 checks flagged',
    );
    await expect(row(panel, 'seoDefects').locator('[data-risk-sentence]')).toHaveText(
      'Blocked by a challenge page.',
    );
  });

  test('a disabled lens and a missing key have no row', async ({ page }) => {
    const report = fixture('full');
    report.scriptInventory = { status: 'disabled', verdict: 'unknown' };
    delete report.responseExposure;
    await mock(page, report);
    const panel = await scan(page);
    await expect(panel.locator('[data-risk-row]')).toHaveCount(2);
    await expect(row(panel, 'scriptInventory')).toHaveCount(0);
    await expect(row(panel, 'responseExposure')).toHaveCount(0);
    await expect(page.locator('[data-scan-tile="risk"] [data-scan-value]')).toHaveText(
      '2 need attention',
    );
  });

  test('an unknown verdict shows the could-not-render sentence and no link', async ({ page }) => {
    const report = fixture('full');
    report.preConsentLeak = { status: 'ok', verdict: 'unknown' };
    await mock(page, report);
    const panel = await scan(page);
    const r = row(panel, 'preConsentLeak');
    await expect(r.locator('.scan-risk-chip')).toHaveText('Not measured');
    await expect(r.locator('[data-risk-sentence]')).toHaveText(
      'We could not render this page, so we could not observe which third parties it contacts before consent.',
    );
    await expect(r.locator('a')).toHaveCount(0);
    await expect(page.locator('[data-scan-tile="risk"] [data-scan-value]')).toHaveText(
      '3 need attention',
    );
  });

  test('links appear only for the lenses whose gate fires', async ({ page }) => {
    const report = fixture('clean');
    // end-of-life only: the response gate fires; version tells alone would not
    report.responseExposure = {
      status: 'ok',
      verdict: 'attention',
      reasons: ['end-of-life'],
      leaks: [],
      tells: [{ product: 'php', source: 'x-powered-by', version: '7.4.33' }],
      endOfLife: [{ product: 'php', line: '7.4', basis: 'eol-date', date: '2022-11-28' }],
      headers: { missing: [] },
    };
    await mock(page, report);
    const panel = await scan(page);
    await expect(panel.locator('a')).toHaveCount(1);
    await expect(row(panel, 'responseExposure').locator('a')).toHaveAttribute(
      'data-umami-event',
      'airead-cta-response-firewall',
    );
  });

  test('version tells and missing headers alone flag the row without a link', async ({ page }) => {
    const report = fixture('clean');
    report.responseExposure = {
      status: 'ok',
      verdict: 'attention',
      reasons: ['version-tell', 'missing-headers'],
      leaks: [],
      tells: [{ product: 'apache', source: 'server', version: '2.4.41' }],
      endOfLife: [],
      headers: { missing: ['hsts'] },
    };
    await mock(page, report);
    const panel = await scan(page);
    await expect(row(panel, 'responseExposure').locator('.scan-risk-chip')).toHaveText('Attention');
    await expect(panel.locator('a')).toHaveCount(0);
  });

  test('a failed scan shows the reason and Try again re-runs only the scanner', async ({
    page,
  }) => {
    const m = await mock(page, null, { status: 503 });
    const panel = await scan(page);
    await expect(page.locator('[data-scan-tile="risk"] [data-scan-status]')).toHaveText(
      'Not measured',
    );
    await expect(panel).toContainText('The scan did not complete.');
    await expect(panel).not.toContainText('scanner unavailable');
    const psiBefore = m.psi;
    m.body = () => ({
      status: 200,
      json: { cached: false, id: 'x', permalink: '', report: fixture('clean') },
    });
    await panel.getByRole('button', { name: 'Try again' }).click();
    await expect(page.locator('[data-scan-tile="risk"] [data-scan-status]')).toHaveText(
      'Nothing flagged',
    );
    expect(m.scan).toBe(2);
    expect(m.psi).toBe(psiBefore);
    await expect(panel.locator('[data-risk-row]')).toHaveCount(4);
  });
});
