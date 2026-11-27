// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { readFileSync } from 'node:fs';
import { test, expect, type Page } from '@playwright/test';
import { stubUmami, trackedEvents } from './helpers/umami';
import { lensSignalFor } from '../src/lib/scan/demand.mjs';

// The lead form. The scanner, PageSpeed Insights and the contact endpoint
// are mocked; the real services are never called. The form sends one request
// per submission, whatever the number of topics.

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

async function runScan(page: Page, path = '/ai-readability/') {
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

const sentLine = (page: Page, email = 'ops@example.com') =>
  page.getByText(`Sent. We usually reply to ${email} within one business day (CET).`, {
    exact: true,
  });
const topicsOf = (body: Record<string, unknown>) =>
  body.topics as Array<{ id: string; wedge: string | null; chosen: boolean }>;

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
    await runScan(page);
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
        name: 'Accessibility issues found (not offered yet) (flagged by your scan)',
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
    await runScan(page);
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
    await page.goto('/ai-readability/');
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
    await runScan(page);
    expect(await checkedIds(page)).toEqual(['consulting']);
  });

  test('a Poor speed result selects the speed-help chip and no gated chip, on both pages', async ({
    page,
  }) => {
    await mock(page, { psi: 0.3 });
    await runScan(page, '/analyze/');
    expect(await chipIds(page)).toEqual(ALL);
    expect(await checkedIds(page)).toEqual(['consulting']);
    await mock(page, { psi: 0.3 });
    await runScan(page);
    expect(await checkedIds(page)).toEqual(['consulting']);
    // A good speed result selects nothing at all.
    await mock(page);
    await runScan(page, '/analyze/');
    expect(await checkedIds(page)).toEqual([]);
  });

  test('shows the report button once a result is in', async ({ page }) => {
    await mock(page, { report: 'clean' });
    await runScan(page);
    await expect(page.getByRole('button', { name: 'Download this report' })).toBeVisible();
  });

  test('the report button stays hidden while nothing has arrived', async ({ page }) => {
    await mock(page);
    await page.route('**/ai-readability/api/scan**', () => {});
    await page.route('**/psi/v5/runPagespeed**', () => {});
    await stubUmami(page);
    await page.goto('/ai-readability/');
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
    await runScan(page);
    await expect(form(page)).toBeVisible();
    expect(await chipIds(page)).toEqual(['consulting', 'agents']);
    await expect(page.locator('[data-scan-audit]')).toBeHidden();
  });

  test('an untouched form sends one general question', async ({ page }) => {
    const { posted } = await mock(page);
    await runScan(page);
    expect(await checkedIds(page)).toEqual([]);
    await page.fill('input[name="email"]', 'ops@example.com');
    await page.getByLabel('Anything we should know? (optional)').fill('Where do I start?');
    await page.getByRole('button', { name: 'Send request' }).click();
    await expect(sentLine(page)).toBeVisible();
    // The visitor ticked nothing, so the confirmation names no topic.
    await expect(page.getByText('Your question was sent.', { exact: true })).toBeVisible();
    await expect(page.getByText(/Topics sent/)).toHaveCount(0);
    expect(posted).toHaveLength(1);
    expect(posted[0]).toMatchObject({
      topic: 'general',
      wedge: '',
      name: '',
      email: 'ops@example.com',
      url: 'https://shop.example.com/',
      origin: 'scan-result',
      message: 'Where do I start?',
      lensSignals: {},
    });
    // The general question is the one chosen topic; what the scan flagged follows.
    expect(topicsOf(posted[0])).toEqual([
      { id: 'general', wedge: null, chosen: true },
      ...ALL.slice(0, 5).map((id) => ({ id, wedge: id, chosen: false })),
    ]);
    expect(posted[0].lensSignal).toBeUndefined();

    const events = await trackedEvents(page);
    expect(events.filter((e) => e.name === 'lead_submit').map((e) => e.data)).toEqual([
      {
        channel: 'scan',
        topic: 'general',
        wedge: '',
        source_path: '/ai-readability/',
        topics_n: 1,
      },
    ]);
    expect(events.filter((e) => e.name.startsWith('airead-next-'))).toHaveLength(0);
  });

  test('an invalid email shows an inline error tied to the field', async ({ page }) => {
    const { posted } = await mock(page);
    await runScan(page);
    const email = page.locator('form[data-scan-lead] input[name="email"]');
    const error = page.locator('[data-scan-lead-email-error]');
    const topics = page.locator('[data-scan-topics]');
    await expect(error).toBeHidden();
    await expect(email).toHaveAttribute('aria-invalid', 'false');
    await expect(email).toHaveAttribute('aria-describedby', (await error.getAttribute('id'))!);
    // The topics group is described by the hint and the form's message line.
    await expect(topics).toHaveAttribute('aria-describedby', 'scan-lead-hint scan-lead-msg');
    await expect(page.locator('#scan-lead-hint')).toContainText('Pick what you want to talk about');

    // A missing address.
    await chip(page, 'edge-seo').check();
    await page.getByRole('button', { name: 'Send request' }).click();
    await expect(error).toHaveText('Enter your email address.');
    await expect(email).toHaveAttribute('aria-invalid', 'true');
    await expect(email).toBeFocused();
    await expect(email).toHaveAccessibleDescription('Enter your email address.');
    // Only the field that is wrong is marked.
    await expect(page.locator('form[data-scan-lead] [aria-invalid="true"]')).toHaveCount(1);
    await expect(topics).not.toHaveAttribute('aria-invalid');
    await expect(page.locator('[data-scan-lead-msg]')).toHaveText('');

    // A malformed address: the error stays while the visitor types.
    await email.fill('ops@');
    await page.getByRole('button', { name: 'Send request' }).click();
    await expect(error).toHaveText('Enter a valid email address, like name@example.com.');
    await email.fill('ops');
    await expect(error).toBeVisible();
    await expect(email).toHaveAttribute('aria-invalid', 'true');
    expect(posted).toHaveLength(0);
    expect(await checkedIds(page)).toEqual(['edge-seo']);

    // A valid address clears it.
    await email.fill('ops@example.com');
    await expect(error).toBeHidden();
    await expect(email).toHaveAttribute('aria-invalid', 'false');
    await page.getByRole('button', { name: 'Send request' }).click();
    await expect(sentLine(page)).toBeVisible();
    expect(posted).toHaveLength(1);
  });

  test('a topic message does not mark the email field', async ({ page }) => {
    await mock(page);
    await runScan(page);
    for (const id of ALL.slice(0, 5)) await chip(page, id).check();
    await expect(page.locator('[data-scan-lead-msg]')).toHaveText('Pick up to four topics.');
    await expect(page.locator('form[data-scan-lead] [aria-invalid="true"]')).toHaveCount(0);
  });

  test('a failed request leaves the whole form retryable and a retry sends one request with the same submission id', async ({
    page,
  }) => {
    const mocked = await mock(page, { failTopics: ['agentpass'] });
    await runScan(page);
    await chip(page, 'agentpass').check();
    await chip(page, 'edge-seo').check();
    await chip(page, 'audit').check();
    await page.fill('input[name="email"]', 'ops@example.com');
    await page.getByLabel('Sites you run (optional)').fill('3');
    await page.getByLabel('Anything we should know? (optional)').fill('Two shops');
    await page.getByRole('button', { name: 'Send request' }).click();
    await expect(page.locator('[data-scan-lead-msg]')).toHaveText(
      'We could not send your request. Try again.',
    );
    await expect(page.locator('[data-scan-live]')).toHaveText(
      'We could not send your request. Try again.',
    );
    // Nothing is cleared, disabled or marked as sent.
    await expect(form(page)).toBeVisible();
    expect(await checkedIds(page)).toEqual(['agentpass', 'edge-seo', 'audit']);
    await expect(form(page).locator('input:disabled')).toHaveCount(0);
    await expect(form(page).locator('input[name="email"]')).toHaveValue('ops@example.com');
    await expect(page.getByLabel('Sites you run (optional)')).toHaveValue('3');
    await expect(page.getByLabel('Anything we should know? (optional)')).toHaveValue('Two shops');
    await expect(page.getByRole('button', { name: 'Send request' })).toBeEnabled();
    expect(mocked.posted).toHaveLength(1);
    // A failed request counts nothing.
    let events = await trackedEvents(page);
    expect(events.filter((e) => e.name === 'lead_submit')).toHaveLength(0);
    expect(events.filter((e) => e.name.startsWith('airead-next-'))).toHaveLength(0);

    mocked.failTopics.length = 0;
    await page.getByRole('button', { name: 'Send request' }).click();
    await expect(sentLine(page)).toBeVisible();
    await expect(page.locator('[data-scan-lead-msg]')).toHaveText('');
    expect(mocked.posted).toHaveLength(2);
    const [first, retry] = mocked.posted;
    expect(String(first.submissionId)).toMatch(/^[A-Za-z0-9_-]{8,64}$/);
    expect(retry.submissionId).toBe(first.submissionId);
    expect(retry).toEqual(first);
    events = await trackedEvents(page);
    expect(events.filter((e) => e.name === 'lead_submit')).toHaveLength(1);
    expect(events.filter((e) => e.name.startsWith('airead-next-')).map((e) => e.name)).toEqual([
      'airead-next-agentpass',
      'airead-next-edge-seo',
    ]);
  });

  test('a request that cannot reach the server can be sent again', async ({ page }) => {
    const { posted } = await mock(page, { report: 'clean' });
    await runScan(page, '/analyze/');
    let offline = true;
    await page.route('**/ai-readability/api/contact', (route) =>
      offline ? route.abort() : route.fallback(),
    );
    await page.fill('input[name="email"]', 'ops@example.com');
    await page.getByRole('button', { name: 'Send request' }).click();
    await expect(page.locator('[data-scan-lead-msg]')).toHaveText(
      'We could not send your request. Try again.',
    );
    await expect(form(page)).toBeVisible();
    offline = false;
    await page.getByRole('button', { name: 'Send request' }).click();
    await expect(page.getByText('Your question was sent.', { exact: true })).toBeVisible();
    expect(posted.map((p) => p.topic)).toEqual(['general']);
  });

  test('a new submission after a sent one gets a new submission id', async ({ page }) => {
    const { posted } = await mock(page, { report: 'clean' });
    await runScan(page);
    await page.fill('input[name="email"]', 'ops@example.com');
    await page.getByRole('button', { name: 'Send request' }).click();
    await expect(sentLine(page)).toBeVisible();
    await page.fill('#scan-url', 'other.example.com');
    await page.click('#scan-submit');
    await expect(form(page)).toBeVisible({ timeout: 15000 });
    await expect(page.getByRole('button', { name: 'Send request' })).toBeEnabled();
    await page.getByRole('button', { name: 'Send request' }).click();
    await expect(sentLine(page)).toBeVisible();
    expect(posted).toHaveLength(2);
    expect(posted[1].submissionId).not.toBe(posted[0].submissionId);
  });

  test('a ticked general topic is confirmed by name', async ({ page }) => {
    const { posted } = await mock(page, { report: 'clean' });
    await runScan(page);
    await chip(page, 'agents').check();
    await page.fill('input[name="email"]', 'ops@example.com');
    await page.getByRole('button', { name: 'Send request' }).click();
    await expect(page.getByText(/^Topics sent: /)).toHaveText(
      'Topics sent: AI agents and crawlers.',
    );
    expect(posted.map((p) => p.topic)).toEqual(['agents']);
    expect(topicsOf(posted[0])).toEqual([{ id: 'agents', wedge: null, chosen: true }]);
  });

  test('asks for at most four topics', async ({ page }) => {
    const { posted } = await mock(page);
    await runScan(page);
    for (const id of ALL.slice(0, 4)) await chip(page, id).check();
    await expect(page.locator('[data-scan-lead-msg]')).toHaveText('');
    await chip(page, 'edge-seo').check();
    await expect(page.locator('[data-scan-lead-msg]')).toHaveText('Pick up to four topics.');
    await page.fill('input[name="email"]', 'ops@example.com');
    await page.getByRole('button', { name: 'Send request' }).click();
    await expect(page.locator('[data-scan-lead-msg]')).toHaveText('Pick up to four topics.');
    expect(posted).toHaveLength(0);
    await chip(page, 'tollbooth').uncheck();
    await expect(page.locator('[data-scan-lead-msg]')).toHaveText('');
  });

  test('always shows the optional sites field', async ({ page }) => {
    const { posted } = await mock(page);
    await runScan(page);
    const sites = page.getByLabel('Sites you run (optional)');
    await expect(sites).toBeVisible();
    await chip(page, 'edge-seo').check();
    await expect(sites).toBeVisible();
    await chip(page, 'edge-seo').uncheck();
    await expect(sites).toBeVisible();
    await sites.fill('12');
    await page.fill('input[name="email"]', 'ops@example.com');
    await page.getByRole('button', { name: 'Send request' }).click();
    await expect(sentLine(page)).toBeVisible();
    expect((posted[0].facts as Record<string, unknown>).sites).toBe(12);
    expect(posted[0].message).toBe('');
  });

  test('one submit is one request and one lead_submit event', async ({ page }) => {
    const { posted } = await mock(page);
    await runScan(page);
    // Ticked out of display order: the request lists them in display order.
    await chip(page, 'consulting').check();
    await chip(page, 'markdown-origin').check();
    await chip(page, 'agentpass').check();
    await chip(page, 'edge-seo').check();
    await page.fill('input[name="email"]', ' ops@example.com ');
    await page.getByLabel('Sites you run (optional)').fill('12');
    await page.getByLabel('Anything we should know? (optional)').fill('  WordPress, two shops ');
    await page.getByRole('button', { name: 'Send request' }).click();

    await expect(sentLine(page)).toBeVisible();
    await expect(page.getByText(/^Topics sent: /)).toHaveText(
      'Topics sent: Checking that a crawler is who it says it is (experimental), Fixing the flagged SEO tags on the server, Serving AI agents a Markdown version of the page (experimental), Help applying the speed fixes.',
    );
    await expect(form(page)).toBeHidden();

    expect(posted).toHaveLength(1);
    const body = posted[0];
    expect(Object.keys(body).sort()).toEqual(
      [
        'email',
        'facts',
        'lensSignals',
        'message',
        'name',
        'origin',
        'submissionId',
        't',
        'topic',
        'topics',
        'url',
        'wedge',
      ].sort(),
    );
    expect(body).toMatchObject({
      email: 'ops@example.com',
      name: '',
      url: 'https://shop.example.com/',
      origin: 'scan-result',
      topic: 'agentpass',
      wedge: 'agentpass',
      // The visitor's note and nothing else.
      message: 'WordPress, two shops',
      facts: { grade: 'C', score: 64, speedMobile: 90, speedDesktop: 90, sites: 12 },
    });
    expect(typeof body.t).toBe('number');
    expect(String(body.submissionId)).toMatch(/^[A-Za-z0-9_-]{8,64}$/);
    expect(topicsOf(body)).toEqual([
      { id: 'agentpass', wedge: 'agentpass', chosen: true },
      { id: 'edge-seo', wedge: 'edge-seo', chosen: true },
      { id: 'markdown-origin', wedge: 'markdown-origin', chosen: true },
      { id: 'consulting', wedge: null, chosen: true },
      { id: 'tollbooth', wedge: 'tollbooth', chosen: false },
      { id: 'compliancefix', wedge: 'compliancefix', chosen: false },
    ]);
    // Signals for the ticked topics that have one, and for no other.
    const signals = body.lensSignals as Record<string, unknown>;
    expect(Object.keys(signals)).toEqual(['agentpass', 'edge-seo', 'markdown-origin']);
    expect(signals['markdown-origin']).toEqual({
      verdict: 'attention',
      negotiated: false,
      httpStatus: 200,
      contentType: 'text/html; charset=utf-8',
      varyAccept: false,
    });
    expect(new TextEncoder().encode(JSON.stringify(body)).length).toBeLessThan(8192);

    const events = await trackedEvents(page);
    expect(events.filter((e) => e.name === 'lead_submit').map((e) => e.data)).toEqual([
      {
        channel: 'scan',
        topic: 'agentpass',
        wedge: 'agentpass',
        source_path: '/ai-readability/',
        topics_n: 4,
      },
    ]);
    // One event per ticked topic whose check fired, none for the unticked ones.
    const next = events.filter((e) => e.name.startsWith('airead-next-')).map((e) => e.name);
    expect(next).toEqual([
      'airead-next-agentpass',
      'airead-next-edge-seo',
      'airead-next-markdown-origin',
    ]);
  });

  test('a pre-ticked speed-help chip left ticked counts as chosen', async ({ page }) => {
    const { posted } = await mock(page, { report: 'clean', psi: 0.3 });
    await runScan(page);
    expect(await checkedIds(page)).toEqual(['consulting']);
    await page.fill('input[name="email"]', 'ops@example.com');
    await page.getByRole('button', { name: 'Send request' }).click();
    await expect(page.getByText(/^Topics sent: /)).toHaveText(
      'Topics sent: Help applying the speed fixes.',
    );
    expect(posted).toHaveLength(1);
    expect(posted[0]).toMatchObject({ topic: 'consulting', wedge: '' });
    expect(topicsOf(posted[0])).toEqual([{ id: 'consulting', wedge: null, chosen: true }]);
    const events = await trackedEvents(page);
    expect(events.filter((e) => e.name === 'lead_submit').map((e) => e.data)).toEqual([
      {
        channel: 'scan',
        topic: 'consulting',
        wedge: '',
        source_path: '/ai-readability/',
        topics_n: 1,
      },
    ]);
  });

  test('a double click sends one request', async ({ page }) => {
    const { posted } = await mock(page);
    await runScan(page);
    let release!: () => void;
    const gate = new Promise<void>((r) => (release = r));
    await page.route('**/ai-readability/api/contact', async (route) => {
      await gate;
      await route.fallback();
    });
    await chip(page, 'tollbooth').check();
    await page.fill('input[name="email"]', 'ops@example.com');
    const button = page.getByRole('button', { name: 'Send request' });
    await button.dblclick();
    // The button says it is busy but stays focusable.
    await expect(button).toHaveAttribute('aria-disabled', 'true');
    await expect(button).toHaveAttribute('aria-busy', 'true');
    await expect(button).not.toHaveAttribute('disabled');
    await button.click({ force: true });
    await button.press('Enter');
    // A submit that does not come from the button is ignored as well.
    await form(page).evaluate((f) => (f as HTMLFormElement).requestSubmit());
    release();
    await expect(sentLine(page)).toBeVisible();
    expect(posted).toHaveLength(1);
    const events = await trackedEvents(page);
    expect(events.filter((e) => e.name === 'lead_submit')).toHaveLength(1);
    expect(events.filter((e) => e.name === 'airead-next-tollbooth')).toHaveLength(1);
  });

  test('a failed send from the keyboard leaves focus on the button', async ({ page }) => {
    const mocked = await mock(page, { failTopics: ['general'] });
    await runScan(page);
    await page.fill('form[data-scan-lead] input[name="email"]', 'ops@example.com');
    const button = page.getByRole('button', { name: 'Send request' });
    await button.focus();
    await page.keyboard.press('Enter');
    await expect(page.locator('[data-scan-lead-msg]')).toHaveText(
      'We could not send your request. Try again.',
    );
    await expect(button).toBeFocused();
    await expect(button).not.toHaveAttribute('aria-disabled');
    await expect(button).not.toHaveAttribute('aria-busy');
    // The retry needs no tabbing back.
    mocked.failTopics.length = 0;
    await page.keyboard.press('Enter');
    await expect(page.getByText('Your question was sent.', { exact: true })).toBeVisible();
    expect(mocked.posted).toHaveLength(2);
  });

  test('Enter in the email field announces the email error', async ({ page }) => {
    const { posted } = await mock(page);
    await runScan(page);
    const email = page.locator('form[data-scan-lead] input[name="email"]');
    const live = page.locator('[data-scan-live]');
    await email.fill('not-an-email');
    await email.press('Enter');
    // Focus was already in the field, so the error goes through the live region.
    await expect(page.locator('[data-scan-lead-email-error]')).toHaveText(
      'Enter a valid email address, like name@example.com.',
    );
    await expect(live).toHaveText('Enter a valid email address, like name@example.com.');
    await expect(email).toBeFocused();
    await email.fill('');
    await email.press('Enter');
    await expect(live).toHaveText('Enter your email address.');
    expect(posted).toHaveLength(0);
  });

  test('an address the service would refuse is caught before sending', async ({ page }) => {
    const { posted } = await mock(page);
    await runScan(page);
    const email = page.locator('form[data-scan-lead] input[name="email"]');
    const error = page.locator('[data-scan-lead-email-error]');
    // The browser's own check accepts an address without a dot after the @.
    await email.fill('name@gmail');
    await page.getByRole('button', { name: 'Send request' }).click();
    await expect(error).toHaveText('Enter a valid email address, like name@example.com.');
    await expect(email).toHaveAttribute('aria-invalid', 'true');
    await expect(email).toBeFocused();
    expect(posted).toHaveLength(0);
    await email.fill('name@gmail.com');
    await expect(error).toBeHidden();
    await page.getByRole('button', { name: 'Send request' }).click();
    await expect(sentLine(page, 'name@gmail.com')).toBeVisible();
    expect(posted).toHaveLength(1);
  });

  for (const [status, line] of [
    [400, 'We could not send your request. Check the email address and try again.'],
    [429, 'Too many requests from this connection. Try again in a minute.'],
    [503, 'We could not send your request. Try again.'],
  ] as const) {
    test(`an answer of ${status} gets its own line and keeps the form`, async ({ page }) => {
      await mock(page);
      await runScan(page);
      await page.route('**/ai-readability/api/contact', (route) =>
        route.fulfill({ status, contentType: 'application/json', body: '{"error":"x"}' }),
      );
      await chip(page, 'edge-seo').check();
      await page.fill('form[data-scan-lead] input[name="email"]', 'ops@example.com');
      await page.getByRole('button', { name: 'Send request' }).click();
      await expect(page.locator('[data-scan-lead-msg]')).toHaveText(line);
      await expect(page.locator('[data-scan-live]')).toHaveText(line);
      await expect(form(page)).toBeVisible();
      expect(await checkedIds(page)).toEqual(['edge-seo']);
      const events = await trackedEvents(page);
      expect(events.filter((e) => e.name === 'lead_submit')).toHaveLength(0);
    });
  }

  test('carries the platform the scan named in the facts', async ({ page }) => {
    const report = JSON.parse(fixture('full'));
    report.report.platform = {
      status: 'ok',
      id: 'wix',
      name: 'Wix',
      kind: 'hosted-builder',
      originControl: 'none',
      confidence: 'high',
      evidence: [{ signal: 'header', id: 'server-pepyaka' }],
    };
    const { posted } = await mock(page, { report });
    await runScan(page);
    await page.fill('input[name="email"]', 'ops@example.com');
    await page.getByRole('button', { name: 'Send request' }).click();
    await expect(sentLine(page)).toBeVisible();
    expect(posted[0].facts).toMatchObject({
      platform: 'wix',
      platformName: 'Wix',
      originControl: 'none',
      server: 'pepyaka',
    });
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
    await page.goto('/ai-readability/');
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
    await page.getByRole('button', { name: 'Send request' }).click();
    await expect(page.getByText('Your question was sent.', { exact: true })).toBeVisible();
    expect(posted).toHaveLength(1);
    expect(posted.map((p) => p.topic)).toEqual(['general']);
    expect(topicsOf(posted[0])).toEqual([{ id: 'general', wedge: null, chosen: true }]);
  });
});

test.describe('next steps and share', () => {
  test('carry the surface’s events, the permalink and the extended report', async ({ page }) => {
    await mock(page, { report: JSON.parse(fixture('full')) });
    await runScan(page);
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
    expect(body).toContain('- Trackers before consent: Attention');
    const events = await trackedEvents(page);
    expect(events.filter((e) => e.name === 'analyze_report_download')).toHaveLength(1);
  });

  test('Copy link to this result copies the permalink, says so and keeps focus', async ({
    page,
    context,
  }) => {
    await context.grantPermissions(['clipboard-read', 'clipboard-write']);
    await mock(page);
    await runScan(page);
    const link = page.locator('[data-scan-permalink]');
    await expect(link).toHaveText('https://modpagespeed.com/ai-readability/api/r/a1b2c3d4e5f6');
    const copy = page.getByRole('button', { name: 'Copy link to this result' });
    await copy.click();
    await expect(page.locator('[data-scan-live]')).toHaveText('Link copied');
    expect(await page.evaluate(() => navigator.clipboard.readText())).toBe(
      'https://modpagespeed.com/ai-readability/api/r/a1b2c3d4e5f6',
    );
    await expect(copy).toBeFocused();
    const events = await trackedEvents(page);
    expect(events.filter((e) => e.name === 'airead-copy-link')).toHaveLength(1);
  });

  test('when the clipboard refuses, the permalink text is selected and no event is sent', async ({
    page,
  }) => {
    await mock(page);
    await runScan(page);
    await page.evaluate(() => {
      Object.defineProperty(navigator, 'clipboard', {
        configurable: true,
        value: { writeText: () => Promise.reject(new Error('refused')) },
      });
    });
    const link = page.locator('[data-scan-permalink]');
    await expect(link).toHaveText('https://modpagespeed.com/ai-readability/api/r/a1b2c3d4e5f6');
    await page.getByRole('button', { name: 'Copy link to this result' }).click();
    await expect(page.locator('[data-scan-live]')).toHaveText(
      'Link selected — press Ctrl/Cmd+C to copy it',
    );
    expect(await page.evaluate(() => String(window.getSelection()))).toBe(
      'https://modpagespeed.com/ai-readability/api/r/a1b2c3d4e5f6',
    );
    const events = await trackedEvents(page);
    expect(events.filter((e) => e.name === 'airead-copy-link')).toHaveLength(0);
  });

  test('use the analyzer’s events on the analyzer page', async ({ page }) => {
    await mock(page);
    await runScan(page, '/analyze/');
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

test.describe('one ticked topic sends its topic and signal', () => {
  // Every topic a scan result can offer, on the full report, with only that
  // topic ticked. The body names the topic, the address, the scanned page and
  // the signal of the lens behind the topic.
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
  const report = JSON.parse(fixture('full')).report;

  for (const c of cases) {
    const name = `${c.wedge}${c.sites ? ' with sites' : ''}${c.note ? ' with a note' : ' and an empty note'}`;
    test(`${name}: the topic and its signal`, async ({ page }) => {
      const sent = await mock(page);
      await runScan(page);
      const email = 'operator@example.com';
      expect(await checkedIds(page)).toEqual([]);
      await chip(page, c.wedge).check();
      await page.fill('input[name="email"]', email);
      if (c.sites) await page.getByLabel('Sites you run (optional)').fill(c.sites);
      if (c.note) await page.getByLabel('Anything we should know? (optional)').fill(c.note);
      await page.getByRole('button', { name: 'Send request' }).click();
      await expect(sentLine(page, email)).toBeVisible();
      expect(sent.posted).toHaveLength(1);
      const [body] = sent.posted;
      expect(typeof body.t).toBe('number');
      expect(body.topic).toBe(c.wedge);
      expect(body.wedge).toBe(c.wedge);
      expect(body.email).toBe(email);
      expect(body.name).toBe('');
      expect(body.url).toBe(report.url);
      const signal = lensSignalFor(c.wedge, report);
      expect(signal).toBeTruthy();
      expect(body.lensSignals).toEqual({ [c.wedge]: signal });
      expect(topicsOf(body)[0]).toEqual({ id: c.wedge, wedge: c.wedge, chosen: true });
      // The form sends the note as typed and the site count as a fact.
      expect(body.message).toBe(c.note ?? '');
      expect((body.facts as Record<string, unknown>).sites).toBe(
        c.sites ? Number(c.sites) : undefined,
      );
    });
  }
});
