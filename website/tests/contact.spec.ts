// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';
import { stubUmami, trackedEvents } from './helpers/umami';

const TOPICS = [
  'setup',
  'support',
  'enterprise',
  'hosting',
  'consulting',
  'audit',
  'iispeed-transfer',
  'agents',
  'other',
];

test.describe('Contact page', () => {
  test('offers one topic per offer, in order', async ({ page }) => {
    await page.goto('/contact/');
    const values = await page
      .locator('#contact-topic option')
      .evaluateAll((opts) => opts.map((o) => (o as HTMLOptionElement).value));
    expect(values).toEqual(TOPICS);
  });

  test('title and description are sentence case and in range', async ({ page }) => {
    await page.goto('/contact/');
    const title = await page.title();
    expect(title.length).toBeLessThanOrEqual(60);
    expect(title).not.toMatch(/Enterprise|Security\b/);
    const description = await page.locator('meta[name="description"]').getAttribute('content');
    expect(description!.length).toBeLessThanOrEqual(155);
  });

  // ?topic= keeps working for every link already in the wild.
  const prefill: Array<[string, string]> = [
    ['support', 'support'],
    ['enterprise', 'enterprise'],
    ['audit', 'audit'],
    ['hosting', 'hosting'],
    ['hardened', 'support'],
    ['consulting', 'consulting'],
    ['iispeed-transfer', 'iispeed-transfer'],
    ['airead', 'agents'],
    ['agent-optimize', 'agents'],
    ['other', 'other'],
    ['no-such-topic', 'setup'],
  ];
  for (const [param, expected] of prefill) {
    test(`?topic=${param} selects ${expected}`, async ({ page }) => {
      await page.goto(`/contact/?topic=${param}`);
      await expect(page.locator('#contact-topic')).toHaveValue(expected);
    });
  }

  test('company, role and deployment size show only for commercial topics', async ({ page }) => {
    await page.goto('/contact/');
    const qualifiers = page.locator('#contact-qualifiers');
    await expect(qualifiers).toBeHidden();
    for (const topic of [
      'support',
      'enterprise',
      'hosting',
      'consulting',
      'audit',
      'iispeed-transfer',
    ]) {
      await page.selectOption('#contact-topic', topic);
      await expect(qualifiers, topic).toBeVisible();
      await expect(page.locator('#contact-org')).toBeVisible();
      await expect(page.locator('#contact-role')).toBeVisible();
      await expect(page.locator('#contact-size')).toBeVisible();
    }
    for (const topic of ['setup', 'agents', 'other']) {
      await page.selectOption('#contact-topic', topic);
      await expect(qualifiers, topic).toBeHidden();
    }
  });

  test('the hosting partner qualifiers arrive as message prefill', async ({ page }) => {
    await page.goto(
      '/contact/?topic=hosting&hosts=Up+to+100+hosts&panel=cPanel%2FEA4&distros=AlmaLinux+9',
    );
    await expect(page.locator('#contact-topic')).toHaveValue('hosting');
    const message = await page.locator('#contact-message').inputValue();
    expect(message).toContain('Hosts: Up to 100 hosts');
    expect(message).toContain('Control panel: cPanel/EA4');
    expect(message).toContain('Distributions: AlmaLinux 9');
  });

  test('shows what happens next and one commercial address', async ({ page }) => {
    await page.goto('/contact/');
    const steps = page.locator('ol[aria-label="What happens next"] li');
    await expect(steps).toHaveCount(3);
    await expect(page.locator('a[href="mailto:info@we-amp.com"]').first()).toBeVisible();
    await expect(page.locator('a[href="mailto:security@modpagespeed.com"]')).toBeVisible();
    // Without JavaScript the form posts natively to the capture endpoint.
    await expect(page.locator('#contact-form')).toHaveAttribute(
      'action',
      '/ai-readability/api/contact',
    );
    // The page script fills the hidden page-load timestamp.
    await expect(page.locator('#contact-form input[name="t"]')).toHaveValue(/^\d+$/);
    expect(await page.content()).not.toContain(['sales', 'we-amp.com'].join('@'));
  });

  test('submit folds qualifiers into the message and reports the lead', async ({ page }) => {
    let posted: Record<string, unknown> | null = null;
    await page.route('**/ai-readability/api/contact', async (route) => {
      posted = route.request().postDataJSON();
      await route.fulfill({ status: 200, contentType: 'application/json', body: '{"ok":true}' });
    });
    await stubUmami(page);
    await page.goto('/contact/?topic=support');

    await page.fill('#contact-name', 'Grace');
    await page.fill('#contact-email', 'grace@example.com');
    await page.fill('#contact-org', 'Example Corp');
    await page.fill('#contact-role', 'SRE lead');
    await page.fill('#contact-size', 'Twelve nginx hosts');
    await page.fill('#contact-message', 'We run nginx on twelve hosts.');
    await page.locator('#contact-submit').click();

    await expect(page.locator('#contact-status')).toContainText('Thanks');
    expect(posted).toMatchObject({ topic: 'support', name: 'Grace', email: 'grace@example.com' });
    // The honeypot rides along empty; the visible company field is `org`.
    expect(posted).toMatchObject({ company: '' });
    const message = String((posted as unknown as { message: string }).message);
    expect(message).toContain('Company: Example Corp');
    expect(message).toContain('Role: SRE lead');
    expect(message).toContain('Deployment size: Twelve nginx hosts');
    expect(message).toContain('We run nginx on twelve hosts.');

    const events = await trackedEvents(page);
    expect(events).toContainEqual({ name: 'contact_submit', data: { topic: 'support' } });
    expect(events).toContainEqual({
      name: 'lead_submit',
      data: { channel: 'contact', topic: 'support', wedge: '', source_path: '/contact/' },
    });
  });

  test('a hosting-topic submit reports the hosting lead channel', async ({ page }) => {
    await page.route('**/ai-readability/api/contact', async (route) => {
      await route.fulfill({ status: 200, contentType: 'application/json', body: '{"ok":true}' });
    });
    await stubUmami(page);
    await page.goto('/contact/?topic=hosting');
    await page.fill('#contact-name', 'Margaret');
    await page.fill('#contact-email', 'margaret@example.com');
    await page.fill('#contact-message', 'About 80 hosts on cPanel.');
    await page.locator('#contact-submit').click();
    await expect(page.locator('#contact-status')).toContainText('Thanks');
    const events = await trackedEvents(page);
    expect(events).toContainEqual({
      name: 'lead_submit',
      data: { channel: 'hosting', topic: 'hosting', wedge: '', source_path: '/contact/' },
    });
  });

  test('forwards a known wedge from the query into lead_submit and drops an unknown one', async ({
    page,
  }) => {
    await page.route('**/ai-readability/api/contact', async (route) => {
      await route.fulfill({ status: 200, contentType: 'application/json', body: '{"ok":true}' });
    });
    await stubUmami(page);
    for (const [wedge, expected] of [
      ['crawler-cost', 'crawler-cost'],
      ['unknown-thing', ''],
    ] as const) {
      await page.goto(`/contact/?topic=consulting&wedge=${wedge}`);
      await page.fill('#contact-name', 'Margaret');
      await page.fill('#contact-email', 'margaret@example.com');
      await page.fill('#contact-message', 'Crawler load on a shared origin.');
      await page.locator('#contact-submit').click();
      await expect(page.locator('#contact-status')).toContainText('Thanks');
      expect(await trackedEvents(page)).toContainEqual({
        name: 'lead_submit',
        data: {
          channel: 'contact',
          topic: 'consulting',
          wedge: expected,
          source_path: '/contact/',
        },
      });
    }
  });

  test('a non-commercial topic posts no qualifier lines', async ({ page }) => {
    let posted: Record<string, unknown> | null = null;
    await page.route('**/ai-readability/api/contact', async (route) => {
      posted = route.request().postDataJSON();
      await route.fulfill({ status: 200, contentType: 'application/json', body: '{"ok":true}' });
    });
    await page.goto('/contact/?topic=support');
    await page.fill('#contact-org', 'Typed then abandoned');
    await page.selectOption('#contact-topic', 'setup');
    await page.fill('#contact-name', 'Linus');
    await page.fill('#contact-email', 'linus@example.com');
    await page.fill('#contact-message', 'nginx 1.24 on Ubuntu 24.04.');
    await page.locator('#contact-submit').click();
    await expect(page.locator('#contact-status')).toContainText('Thanks');
    expect(posted).toMatchObject({ topic: 'setup', message: 'nginx 1.24 on Ubuntu 24.04.' });
    expect(typeof (posted as unknown as Record<string, unknown>).t).toBe('number');
  });
});

test.describe('Hosting partners qualifier form', () => {
  test('hands host count, panel and distributions to the contact form', async ({ page }) => {
    await page.goto('/hosting-partners/');
    // The production smoke asserts this plain link exists.
    await expect(page.locator('a[href="/contact/?topic=hosting"]')).toBeVisible();
    await page.selectOption('#hp-hosts', 'Up to 100 hosts');
    await page.selectOption('#hp-panel', 'cPanel/EA4');
    await page.fill('#hp-distros', 'AlmaLinux 9');
    const submit = page.locator('form[action="/contact/"] button[type="submit"]');
    await expect(submit).toHaveAttribute('data-umami-event', 'cta_commercial');
    await expect(submit).toHaveAttribute('data-umami-event-offer', 'hosting');
    await submit.click();
    await expect(page).toHaveURL(/\/contact\/\?topic=hosting&/);
    await expect(page.locator('#contact-topic')).toHaveValue('hosting');
    await expect(page.locator('#contact-message')).toHaveValue(/Hosts: Up to 100 hosts/);
  });
});

test.describe('Contact thank-you page', () => {
  test('/contact/thanks/ renders the confirmation and is noindex', async ({ page }) => {
    const res = await page.goto('/contact/thanks/');
    expect(res?.status()).toBe(200);
    await expect(page.getByRole('heading', { level: 1 })).toHaveText('Thanks, your message is in.');
    await expect(page.locator('meta[name="robots"]')).toHaveAttribute('content', /noindex/);
  });
});
