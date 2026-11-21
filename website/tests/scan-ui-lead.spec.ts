// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { readFileSync } from 'node:fs';
import { test, expect, type Page } from '@playwright/test';
import { stubUmami, trackedEvents } from './helpers/umami';

// The v2 lead form. The scanner, PageSpeed Insights and the contact endpoint
// are mocked; the real services are never called.

const fixture = (name: string) =>
  readFileSync(new URL(`./fixtures/scan/report-${name}.json`, import.meta.url), 'utf8');

type Posted = Array<Record<string, unknown>>;

async function mock(
  page: Page,
  opts: { report?: 'full' | 'clean' | object; psi?: number; failTopics?: string[] } = {},
): Promise<{ posted: Posted; failTopics: string[] }> {
  const posted: Posted = [];
  const failTopics = [...(opts.failTopics ?? [])];
  await page.route('**/psi/v5/runPagespeed**', (route) =>
    route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({
        lighthouseResult: { categories: { performance: { score: opts.psi ?? 0.9 } } },
      }),
    }),
  );
  await page.route('**/ai-readability/api/scan**', (route) =>
    route.fulfill({
      status: 200,
      contentType: 'application/json',
      body:
        typeof opts.report === 'object'
          ? JSON.stringify(opts.report)
          : fixture(opts.report ?? 'full'),
    }),
  );
  await page.route('**/ai-readability/api/contact', async (route) => {
    const body = route.request().postDataJSON() as Record<string, unknown>;
    posted.push(body);
    const fail = failTopics.includes(String(body.topic));
    await route.fulfill({
      status: fail ? 500 : 200,
      contentType: 'application/json',
      body: fail ? '{"error":"nope"}' : '{"ok":true}',
    });
  });
  return { posted, failTopics };
}

async function scanV2(page: Page, path = '/ai-readability/?ui=v2') {
  await stubUmami(page);
  await page.goto(path);
  await page.waitForLoadState('networkidle');
  await page.fill('#scan-url', 'shop.example.com');
  await page.click('#scan-submit');
  await expect(page.locator('[data-scan-tile="speed"] [data-scan-status]')).not.toHaveText(
    'Checking…',
    { timeout: 15000 },
  );
  await expect(page.locator('[data-scan-tile="airead"] [data-scan-status]')).not.toHaveText(
    'Checking…',
    { timeout: 15000 },
  );
}

const form = (page: Page) => page.locator('form[data-scan-lead]');
const chip = (page: Page, id: string) => page.locator(`[data-scan-chip="${id}"] input`);
const chipIds = (page: Page) =>
  page
    .locator('[data-scan-chip]')
    .evaluateAll((els) => els.map((e) => (e as HTMLElement).dataset.scanChip));
const checkedIds = (page: Page) =>
  page
    .locator('[data-scan-chip] input:checked')
    .evaluateAll((els) => els.map((e) => (e as HTMLInputElement).value));

const ALL = [
  'tollbooth',
  'agentpass',
  'compliancefix',
  'consent-enforcement',
  'page-integrity',
  'third-party-freeze',
  'edge-seo',
  'response-firewall',
  'markdown-origin',
  'consulting',
  'agents',
  'audit',
];

test.describe('the one lead form', () => {
  test('shows every gated chip on the full report, marked as found and none selected', async ({
    page,
  }) => {
    await mock(page);
    await scanV2(page);
    await expect(form(page)).toBeVisible();
    await expect(form(page).locator('fieldset legend')).toHaveText('Topics');
    expect(await chipIds(page)).toEqual(ALL);
    expect(await checkedIds(page)).toEqual([]);
    // The nine chips whose check fired come first and say so; the marker is
    // part of the checkbox's accessible name.
    for (const id of ALL.slice(0, 9)) {
      await expect(page.locator(`[data-scan-chip="${id}"]`)).toContainText(
        '(flagged by your scan)',
      );
    }
    for (const id of ALL.slice(9)) {
      await expect(page.locator(`[data-scan-chip="${id}"]`)).not.toContainText(
        'flagged by your scan',
      );
    }
    await expect(
      form(page).getByRole('checkbox', {
        name: 'Accessibility remediation (flagged by your scan)',
        exact: true,
      }),
    ).toHaveCount(1);
    await expect(form(page)).toContainText(
      'Pick what you want to talk about, or leave the topics empty for a general question.',
    );
    await expect(form(page)).not.toContainText('pre-selected');
    await expect(form(page).locator('input[type="checkbox"]')).toHaveCount(12);
    await expect(page.locator('[data-scan-live]')).toHaveCount(1);
    await expect(
      page.locator('#scan-results [aria-live], #scan-results [role="status"]'),
    ).toHaveCount(1);
  });

  test('shows only the ungated chips on a clean report, none selected', async ({ page }) => {
    await mock(page, { report: 'clean' });
    await scanV2(page);
    expect(await chipIds(page)).toEqual(['consulting', 'agents', 'audit']);
    expect(await checkedIds(page)).toEqual([]);
  });

  test('keeps the audit line hidden while a scan runs and again on a rescan', async ({ page }) => {
    await mock(page);
    let release!: () => void;
    let releaseSecond!: () => void;
    const gate = new Promise<void>((r) => (release = r));
    const gateSecond = new Promise<void>((r) => (releaseSecond = r));
    let scans = 0;
    await page.route('**/ai-readability/api/scan**', async (route) => {
      scans += 1;
      await (scans === 1 ? gate : gateSecond);
      await route.fulfill({ status: 200, contentType: 'application/json', body: fixture('full') });
    });
    await stubUmami(page);
    await page.goto('/ai-readability/?ui=v2');
    await page.waitForLoadState('networkidle');
    await page.fill('#scan-url', 'shop.example.com');
    await page.click('#scan-submit');
    await expect(form(page)).toBeVisible({ timeout: 15000 });
    await expect(page.locator('[data-scan-audit]')).toBeHidden();
    release();
    await expect(page.locator('[data-scan-audit]')).toBeVisible({ timeout: 15000 });
    await page.fill('#scan-url', 'other.example.com');
    await page.click('#scan-submit');
    await expect(page.locator('[data-scan-audit]')).toBeHidden();
    releaseSecond();
    await expect(page.locator('[data-scan-audit]')).toBeVisible({ timeout: 15000 });
  });

  test('pre-selects the speed-help chip only when Speed is Poor', async ({ page }) => {
    await mock(page, { report: 'clean', psi: 0.3 });
    await scanV2(page);
    expect(await checkedIds(page)).toEqual(['consulting']);
  });

  test('a Poor speed result selects the speed-help chip and no gated chip, on both pages', async ({
    page,
  }) => {
    await mock(page, { psi: 0.3 });
    await scanV2(page, '/analyze/?ui=v2');
    expect(await chipIds(page)).toEqual(ALL);
    expect(await checkedIds(page)).toEqual(['consulting']);
    await mock(page, { psi: 0.3 });
    await scanV2(page);
    expect(await checkedIds(page)).toEqual(['consulting']);
    // A good speed result selects nothing at all.
    await mock(page);
    await scanV2(page, '/analyze/?ui=v2');
    expect(await checkedIds(page)).toEqual([]);
  });

  test('shows the report button once a result is in', async ({ page }) => {
    await mock(page, { report: 'clean' });
    await scanV2(page);
    await expect(page.getByRole('button', { name: 'Download this report' })).toBeVisible();
  });

  test('the report button stays hidden while nothing has arrived', async ({ page }) => {
    await mock(page);
    await page.route('**/ai-readability/api/scan**', () => {});
    await page.route('**/psi/v5/runPagespeed**', () => {});
    await stubUmami(page);
    await page.goto('/ai-readability/?ui=v2');
    await page.waitForLoadState('networkidle');
    await page.fill('#scan-url', 'shop.example.com');
    await page.click('#scan-submit');
    await expect(page.locator('[data-scan-tile="speed"] [data-scan-status]')).toHaveText(
      'Checking…',
    );
    await expect(page.getByRole('button', { name: 'Download this report' })).toBeHidden();
  });

  test('still renders with the ungated chips when the scan fails', async ({ page }) => {
    await mock(page);
    await page.route('**/ai-readability/api/scan**', (route) => route.abort());
    await scanV2(page);
    await expect(form(page)).toBeVisible();
    expect(await chipIds(page)).toEqual(['consulting', 'agents']);
    await expect(page.locator('[data-scan-audit]')).toBeHidden();
  });

  test('an untouched form sends one general question', async ({ page }) => {
    const { posted } = await mock(page);
    await scanV2(page);
    expect(await checkedIds(page)).toEqual([]);
    await page.fill('input[name="email"]', 'ops@example.com');
    await page.getByLabel('Anything we should know? (optional)').fill('Where do I start?');
    await page.getByRole('button', { name: 'Talk to us' }).click();
    await expect(
      page.getByText(
        'Thanks, your message is in. An engineer replies within one business day (CET).',
        { exact: true },
      ),
    ).toBeVisible();
    // The visitor ticked nothing, so the confirmation names no topic.
    await expect(page.getByText('Your question was sent.', { exact: true })).toBeVisible();
    await expect(page.getByText(/Topics sent/)).toHaveCount(0);
    expect(posted).toHaveLength(1);
    expect(posted[0]).toMatchObject({
      topic: 'agents',
      wedge: '',
      name: '',
      email: 'ops@example.com',
      url: 'https://shop.example.com/',
    });
    expect(String(posted[0].message)).toContain('[agents] request from the scan result');
    expect(String(posted[0].message)).toContain('Note: Where do I start?');
    expect(posted[0].lensSignal).toBeUndefined();

    const events = await trackedEvents(page);
    expect(events.filter((e) => e.name === 'lead_submit').map((e) => e.data)).toEqual([
      { channel: 'scan', topic: 'agents', wedge: '', source_path: '/ai-readability/' },
    ]);
    expect(events.filter((e) => e.name.startsWith('airead-next-'))).toHaveLength(0);
  });

  test('an untouched form still asks for an email address first', async ({ page }) => {
    const { posted } = await mock(page, { report: 'clean' });
    await scanV2(page);
    await page.getByRole('button', { name: 'Talk to us' }).click();
    await expect(page.locator('[data-scan-lead-msg]')).toHaveText('');
    await expect(form(page)).toBeVisible();
    expect(posted).toHaveLength(0);
  });

  test('a general question that fails names no topic, and can be sent again', async ({ page }) => {
    const mocked = await mock(page, { failTopics: ['agents'] });
    await scanV2(page, '/analyze/?ui=v2');
    await page.fill('input[name="email"]', 'ops@example.com');
    await page.getByRole('button', { name: 'Talk to us' }).click();
    await expect(page.locator('[data-scan-lead-msg]')).toHaveText(
      'We could not send your question. Try again.',
    );
    await expect(form(page)).toBeVisible();
    expect(await checkedIds(page)).toEqual([]);
    mocked.failTopics.length = 0;
    await page.getByRole('button', { name: 'Talk to us' }).click();
    await expect(page.getByText('Your question was sent.', { exact: true })).toBeVisible();
    expect(mocked.posted.map((p) => p.topic)).toEqual(['agents', 'agents']);
  });

  test('a ticked general topic is confirmed by name', async ({ page }) => {
    const { posted } = await mock(page, { report: 'clean' });
    await scanV2(page);
    await chip(page, 'agents').check();
    await page.fill('input[name="email"]', 'ops@example.com');
    await page.getByRole('button', { name: 'Talk to us' }).click();
    await expect(page.getByText(/^Topics sent: /)).toHaveText(
      'Topics sent: AI agents and crawlers.',
    );
    expect(posted.map((p) => p.topic)).toEqual(['agents']);
  });

  test('clears the failure line once the failed topic is unticked', async ({ page }) => {
    const mocked = await mock(page, { failTopics: ['agentpass'] });
    await scanV2(page);
    await chip(page, 'tollbooth').check();
    await chip(page, 'agentpass').check();
    await page.fill('input[name="email"]', 'ops@example.com');
    await page.getByRole('button', { name: 'Talk to us' }).click();
    const msg = page.locator('[data-scan-lead-msg]');
    await expect(msg).toContainText('We could not send: Signed-agent verification');
    // Another topic changing leaves the line alone while the failed one is ticked.
    await chip(page, 'edge-seo').check();
    await expect(msg).toContainText('We could not send: Signed-agent verification');
    await chip(page, 'agentpass').uncheck();
    await expect(msg).toHaveText('');
    await chip(page, 'edge-seo').uncheck();
    // Nothing is left to send: the confirmation lists what went through.
    await page.getByRole('button', { name: 'Talk to us' }).click();
    await expect(page.getByText(/^Topics sent: /)).toHaveText(
      'Topics sent: Origin controls (who reads / verifies).',
    );
    expect(mocked.posted.map((p) => p.topic)).toEqual(['tollbooth', 'agentpass']);
  });

  test('asks for at most four topics', async ({ page }) => {
    const { posted } = await mock(page);
    await scanV2(page);
    for (const id of ALL.slice(0, 4)) await chip(page, id).check();
    await expect(page.locator('[data-scan-lead-msg]')).toHaveText('');
    await chip(page, 'edge-seo').check();
    await expect(page.locator('[data-scan-lead-msg]')).toHaveText('Pick up to four topics.');
    await page.fill('input[name="email"]', 'ops@example.com');
    await page.getByRole('button', { name: 'Talk to us' }).click();
    await expect(page.locator('[data-scan-lead-msg]')).toHaveText('Pick up to four topics.');
    expect(posted).toHaveLength(0);
    await chip(page, 'tollbooth').uncheck();
    await expect(page.locator('[data-scan-lead-msg]')).toHaveText('');
  });

  test('shows the sites field only for the SEO and legacy-site topics', async ({ page }) => {
    await mock(page);
    await scanV2(page);
    const sites = page.getByLabel('Sites you run (optional)');
    await expect(sites).toBeHidden();
    await chip(page, 'edge-seo').check();
    await expect(sites).toBeVisible();
    await chip(page, 'edge-seo').uncheck();
    await expect(sites).toBeHidden();
    await chip(page, 'response-firewall').check();
    await expect(sites).toBeVisible();
  });

  test('sends one lead per topic, in order, with the right wedge, note and events', async ({
    page,
  }) => {
    const { posted } = await mock(page);
    await scanV2(page);
    await chip(page, 'agentpass').check();
    await chip(page, 'edge-seo').check();
    await chip(page, 'markdown-origin').check();
    await chip(page, 'consulting').check();
    await page.fill('input[name="email"]', 'ops@example.com');
    await page.getByLabel('Sites you run (optional)').fill('12');
    await page.getByLabel('Anything we should know? (optional)').fill('WordPress');
    await page.getByRole('button', { name: 'Talk to us' }).click();

    await expect(
      page.getByText(
        'Thanks, your message is in. An engineer replies within one business day (CET).',
        { exact: true },
      ),
    ).toBeVisible();
    await expect(page.getByText(/^Topics sent: /)).toContainText('SEO fixes at the server');
    await expect(form(page)).toBeHidden();
    expect(posted.map((p) => p.topic)).toEqual([
      'agentpass',
      'edge-seo',
      'markdown-origin',
      'consulting',
    ]);
    expect(posted.map((p) => p.wedge)).toEqual(['agentpass', 'edge-seo', 'markdown-origin', '']);
    expect(posted[2].lensSignal).toEqual({
      verdict: 'attention',
      negotiated: false,
      httpStatus: 200,
      contentType: 'text/html; charset=utf-8',
      varyAccept: false,
    });
    for (const p of posted) expect(typeof p.t).toBe('number');
    expect(String(posted[1].message)).toContain(
      'Operator note / crawl volume: sites: 12; WordPress',
    );
    expect(String(posted[0].message)).toContain('Operator note / crawl volume: WordPress');
    expect(String(posted[2].message)).toContain('[markdown-origin] lead from RenderPeek result');
    expect(String(posted[0].message)).not.toContain('sites: 12');
    expect(posted[3]).toMatchObject({
      topic: 'consulting',
      wedge: '',
      name: '',
      email: 'ops@example.com',
      url: 'https://shop.example.com/',
    });
    expect(String(posted[3].message)).toContain('[consulting] request from the scan result');

    const events = await trackedEvents(page);
    const leads = events.filter((e) => e.name === 'lead_submit');
    expect(leads.map((e) => e.data)).toEqual(
      ['agentpass', 'edge-seo', 'markdown-origin', 'consulting'].map((t) => ({
        channel: 'scan',
        topic: t,
        wedge: t === 'consulting' ? '' : t,
        source_path: '/ai-readability/',
      })),
    );
    const next = events.filter((e) => e.name.startsWith('airead-next-')).map((e) => e.name);
    expect(next).toEqual([
      'airead-next-agentpass',
      'airead-next-edge-seo',
      'airead-next-markdown-origin',
    ]);
  });

  test('retries only the topics that failed, without counting twice', async ({ page }) => {
    const mocked = await mock(page, { failTopics: ['agentpass'] });
    await scanV2(page);
    for (const id of ALL.slice(0, 4)) await chip(page, id).check();
    await page.fill('input[name="email"]', 'ops@example.com');
    await page.getByRole('button', { name: 'Talk to us' }).click();
    await expect(page.locator('[data-scan-lead-msg]')).toHaveText(
      'Sent: Origin controls (who reads / verifies), Accessibility remediation, Consent enforcement. We could not send: Signed-agent verification (prove which agent is which). Try again.',
    );
    await expect(form(page)).toBeVisible();
    expect(await checkedIds(page)).toEqual(['agentpass']);
    expect(mocked.posted.map((p) => p.topic)).toEqual([
      'tollbooth',
      'agentpass',
      'compliancefix',
      'consent-enforcement',
    ]);

    mocked.failTopics.length = 0;
    mocked.posted.length = 0;
    await page.getByRole('button', { name: 'Talk to us' }).click();
    await expect(
      page.getByText(
        'Thanks, your message is in. An engineer replies within one business day (CET).',
        { exact: true },
      ),
    ).toBeVisible();
    expect(mocked.posted.map((p) => p.topic)).toEqual(['agentpass']);
    await expect(page.getByText(/^Topics sent: /)).toContainText('Origin controls');
    const events = await trackedEvents(page);
    expect(events.filter((e) => e.name === 'lead_submit')).toHaveLength(4);
    expect(events.filter((e) => e.name === 'airead-next-agentpass')).toHaveLength(1);
  });

  // The speed result is held back until the test releases it.
  async function scanWithLateSpeed(page: Page, score: number): Promise<() => void> {
    let release!: () => void;
    const gate = new Promise<void>((r) => (release = r));
    await page.route('**/psi/v5/runPagespeed**', async (route) => {
      await gate;
      await route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ lighthouseResult: { categories: { performance: { score } } } }),
      });
    });
    await stubUmami(page);
    await page.goto('/ai-readability/?ui=v2');
    await page.waitForLoadState('networkidle');
    await page.fill('#scan-url', 'shop.example.com');
    await page.click('#scan-submit');
    await expect(form(page)).toBeVisible({ timeout: 15000 });
    await expect.poll(() => chipIds(page)).toEqual(['agents', 'audit']);
    return release;
  }

  test('a late Poor speed result adds the speed-help chip ticked while the topics are untouched', async ({
    page,
  }) => {
    await mock(page, { report: 'clean' });
    const release = await scanWithLateSpeed(page, 0.2);
    release();
    await expect(chip(page, 'consulting')).toBeChecked();
    expect(await chipIds(page)).toEqual(['consulting', 'agents', 'audit']);
  });

  test('a late Poor speed result adds the speed-help chip unticked once the visitor chose topics', async ({
    page,
  }) => {
    await mock(page, { report: 'clean' });
    const release = await scanWithLateSpeed(page, 0.2);
    await chip(page, 'agents').check();
    release();
    await expect.poll(() => chipIds(page)).toEqual(['consulting', 'agents', 'audit']);
    await expect(chip(page, 'consulting')).not.toBeChecked();
    await expect(chip(page, 'agents')).toBeChecked();
  });

  test('ticking and unticking everything still sends the general question after a late Poor speed result', async ({
    page,
  }) => {
    const { posted } = await mock(page, { report: 'clean' });
    const release = await scanWithLateSpeed(page, 0.2);
    await chip(page, 'agents').check();
    await chip(page, 'audit').check();
    await chip(page, 'agents').uncheck();
    await chip(page, 'audit').uncheck();
    release();
    await expect.poll(() => chipIds(page)).toEqual(['consulting', 'agents', 'audit']);
    expect(await checkedIds(page)).toEqual([]);
    await page.fill('input[name="email"]', 'ops@example.com');
    await page.getByRole('button', { name: 'Talk to us' }).click();
    await expect(page.getByText('Your question was sent.', { exact: true })).toBeVisible();
    expect(posted.map((p) => p.topic)).toEqual(['agents']);
  });
});

test.describe('next steps and share', () => {
  test('carry the surface’s events, the permalink and the extended report', async ({ page }) => {
    await mock(page, { report: JSON.parse(fixture('full')) });
    await scanV2(page);
    const next = page.locator('[data-scan-next]');
    await expect(next.getByRole('link', { name: /Download & run/ })).toHaveAttribute(
      'data-umami-event',
      'airead-cta-optimize',
    );
    await expect(next.getByRole('link', { name: /Support pricing/ })).toHaveAttribute(
      'data-umami-event',
      'airead-cta-buy',
    );
    const audit = page.locator('[data-scan-audit]');
    await expect(audit).toBeVisible();
    await expect(audit).toContainText(
      'Agent-readiness audit, €950 per site, excl. VAT: a written report and one review call. It lists the configuration change for each finding; applying the changes is not included. It does not measure visibility in AI answers.',
    );
    const auditLink = audit.getByRole('link', { name: 'Ask for an audit' });
    await expect(auditLink).toHaveAttribute('href', '/contact/?topic=audit');
    await expect(auditLink).toHaveAttribute('data-umami-event', 'cta_commercial');
    await expect(auditLink).toHaveAttribute('data-umami-event-offer', 'audit');
    await expect(auditLink).toHaveAttribute('data-umami-event-position', 'scan_result');
    await expect(auditLink).toHaveAttribute('data-umami-event-surface', 'airead');
    await expect(page.locator('[data-scan-permalink]')).toHaveText(
      'https://modpagespeed.com/ai-readability/api/r/a1b2c3d4e5f6',
    );
    await expect(page.locator('[data-scan-share]')).toContainText(
      'The link works for up to 24 hours.',
    );
    await expect(page.getByRole('button', { name: 'Copy link to this result' })).toBeVisible();

    const [download] = await Promise.all([
      page.waitForEvent('download'),
      page.getByRole('button', { name: 'Download this report' }).click(),
    ]);
    expect(download.suggestedFilename()).toBe('pagespeed-report-shop.example.com.md');
    const body = readFileSync((await download.path())!, 'utf8');
    expect(body).toContain('AI readability: grade C (64/100)');
    expect(body).toContain('- Pre-consent leak: Attention');
    const events = await trackedEvents(page);
    expect(events.filter((e) => e.name === 'analyze_report_download')).toHaveLength(1);
  });

  test('use the analyzer’s events on the analyzer page', async ({ page }) => {
    await mock(page);
    await scanV2(page, '/analyze/?ui=v2');
    const next = page.locator('[data-scan-next]');
    await expect(next.getByRole('link', { name: /Download & run/ })).toHaveAttribute(
      'data-umami-event',
      'psi-result-install-cta',
    );
    const support = next.getByRole('link', { name: /Support pricing/ });
    await expect(support).toHaveAttribute('data-umami-event', 'cta_commercial');
    await expect(support).toHaveAttribute('data-umami-event-surface', 'analyze');
    const auditLink = page.locator('[data-scan-audit]').getByRole('link', {
      name: 'Ask for an audit',
    });
    await expect(auditLink).toHaveAttribute('href', '/contact/?topic=audit');
    await expect(auditLink).toHaveAttribute('data-umami-event-offer', 'audit');
    await expect(auditLink).toHaveAttribute('data-umami-event-surface', 'analyze');
    await expect(page.locator('[data-scan-audit]')).toBeVisible();
  });
});

test.describe('v1 and v2 send the same lead', () => {
  // Every wedge the original page offers, on the full report. Each case fills
  // the original form, then the new one with only that topic ticked, and the
  // two POST bodies must be identical.
  const cases: Array<{ wedge: string; sites?: string; note?: string }> = [
    { wedge: 'tollbooth' },
    { wedge: 'agentpass' },
    { wedge: 'compliancefix' },
    { wedge: 'consent-enforcement' },
    { wedge: 'page-integrity' },
    { wedge: 'third-party-freeze' },
    { wedge: 'edge-seo', sites: '12' },
    { wedge: 'edge-seo', sites: '12', note: 'WordPress' },
    { wedge: 'edge-seo', note: 'WordPress' },
  ];

  for (const c of cases) {
    const name = `${c.wedge}${c.sites ? ' with sites' : ''}${c.note ? ' with a note' : ' and an empty note'}`;
    test(`${name}: the same body in both`, async ({ page }) => {
      const v1 = await mock(page);
      await stubUmami(page);
      await page.goto('/ai-readability/?ui=v1');
      await page.fill('#ar-url', 'shop.example.com');
      await page.click('#ar-go');
      const email = 'operator@example.com';
      if (c.wedge === 'edge-seo') {
        await page.fill('#ar-seo-email', email);
        if (c.sites) await page.fill('#ar-seo-sites', c.sites);
        if (c.note) await page.fill('#ar-seo-note', c.note);
        await page.click('#ar-seo-form button[type="submit"]');
      } else if (c.wedge === 'consent-enforcement') {
        await page.fill('#ar-consent-email', email);
        await page.click('#ar-consent-form button[type="submit"]');
      } else if (c.wedge === 'page-integrity' || c.wedge === 'third-party-freeze') {
        await page.fill('#ar-scripts-email', email);
        await page.click(`#ar-scripts-form button[data-wedge="${c.wedge}"]`);
      } else {
        await page.fill('#ar-next-email', email);
        await page.check(`input[name="ar-topic"][value="${c.wedge}"]`);
        await page.click('#ar-next-form button[type="submit"]');
      }
      await expect.poll(() => v1.posted.length, { timeout: 15000 }).toBe(1);

      const p2 = await page.context().newPage();
      const v2 = await mock(p2);
      await scanV2(p2);
      expect(await checkedIds(p2)).toEqual([]);
      await chip(p2, c.wedge).check();
      await p2.fill('input[name="email"]', email);
      if (c.sites) await p2.getByLabel('Sites you run (optional)').fill(c.sites);
      if (c.note) await p2.getByLabel('Anything we should know? (optional)').fill(c.note);
      await p2.getByRole('button', { name: 'Talk to us' }).click();
      await expect(
        p2.getByText(
          'Thanks, your message is in. An engineer replies within one business day (CET).',
          { exact: true },
        ),
      ).toBeVisible();
      expect(v2.posted).toHaveLength(1);
      // The page-load timestamp differs per page; the rest of the body is the same.
      expect(typeof v1.posted[0].t).toBe('number');
      expect(typeof v2.posted[0].t).toBe('number');
      const { t: _t1, ...body1 } = v1.posted[0];
      const { t: _t2, ...body2 } = v2.posted[0];
      expect(body2).toEqual(body1);
    });
  }
});
