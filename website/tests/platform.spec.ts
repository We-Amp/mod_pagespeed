// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';
import { stubUmami, trackedEvents } from './helpers/umami';

// The planned-pack pages: /platform/ carries one interest form per direction
// (the `wedge` field), the two sub-pages one form each. Every form posts to
// the shared capture endpoint with topic "platform" and the wedge as a
// structured field, and fires umami <wedge> plus lead_submit on a successful
// POST only. The word "wedge" is a form field, never visible copy.

const HUB_WEDGES = [
  'page-integrity',
  'third-party-freeze',
  'consent-enforcement',
  'edge-seo',
  'response-firewall',
  'rewriter-sdk',
  'request-a-pack',
];

test.describe('Transform packs interest forms', () => {
  test('/platform/ carries one form per direction and the status line', async ({ page }) => {
    await page.goto('/platform/');
    await expect(page.getByRole('heading', { level: 1 })).toHaveText('One interceptor, many packs');
    await expect(page.locator('main')).toContainText(
      'The edge-SEO pack is free and available now.',
    );
    await expect(page.locator('main')).toContainText('The other packs are planned.');
    await expect(page.locator('main a[href="/packs/"]')).toBeVisible();
    const wedges = await page
      .locator('form[data-platform-form]')
      .evaluateAll((forms) => forms.map((f) => (f as HTMLElement).dataset.wedge));
    expect(wedges).toEqual(HUB_WEDGES);
    for (const wedge of HUB_WEDGES) {
      const form = page.locator(`form[data-wedge="${wedge}"]`);
      await expect(form.locator('input[name="topic"]')).toHaveValue('platform');
      await expect(form.locator('input[name="wedge"]')).toHaveValue(wedge);
      await expect(form.locator('input[name="email"]')).toHaveAttribute('required', '');
    }
    // The field name is not copy.
    const visible = await page.locator('main').innerText();
    expect(visible.toLowerCase()).not.toContain('wedge');
    await expect(page.locator('main a[href="/platform/consent/"]')).toBeVisible();
    await expect(page.locator('main a[href="/platform/edge-seo/"]')).toBeVisible();
  });

  test('a card submit posts the exact body and fires the wedge and lead events', async ({
    page,
  }) => {
    let posted: Record<string, unknown> | null = null;
    await page.route('**/ai-readability/api/contact', async (route) => {
      posted = route.request().postDataJSON();
      await route.fulfill({ status: 200, contentType: 'application/json', body: '{"ok":true}' });
    });
    await stubUmami(page);
    await page.goto('/platform/');
    const form = page.locator('form[data-wedge="page-integrity"]');
    await form.locator('input[name="email"]').fill('grace@example.com');
    await form.locator('input[name="site"]').fill('https://shop.example.com/');
    await form.locator('textarea[name="message"]').fill('Checkout is on a separate host.');
    await form.locator('button[type="submit"]').click();
    await expect(form.locator('[data-status]')).toContainText('Thanks');
    expect(posted).toEqual({
      email: 'grace@example.com',
      name: '',
      topic: 'platform',
      wedge: 'page-integrity',
      url: '/platform/',
      message:
        'Pack: Payment-page script integrity\nSite: https://shop.example.com/\n\nCheckout is on a separate host.',
      t: expect.any(Number),
    });
    const events = await trackedEvents(page);
    expect(events).toContainEqual({ name: 'page-integrity', data: undefined });
    expect(events).toContainEqual({
      name: 'lead_submit',
      data: {
        channel: 'platform',
        topic: 'platform',
        wedge: 'page-integrity',
        source_path: '/platform/',
      },
    });
  });

  test('a failed POST reports the error and fires no event', async ({ page }) => {
    await page.route('**/ai-readability/api/contact', async (route) => {
      await route.fulfill({ status: 500, contentType: 'application/json', body: '{}' });
    });
    await stubUmami(page);
    await page.goto('/platform/');
    const form = page.locator('form[data-wedge="rewriter-sdk"]');
    await form.locator('input[name="email"]').fill('grace@example.com');
    await form.locator('button[type="submit"]').click();
    await expect(form.locator('[data-status]')).toContainText('Could not send');
    expect(await trackedEvents(page)).toEqual([]);
  });

  test('/platform/consent/ has one consent-enforcement form and no URL checker', async ({
    page,
  }) => {
    await page.goto('/platform/consent/');
    await expect(page.getByRole('heading', { level: 1 })).toHaveText(
      'Does your site leak before consent?',
    );
    const forms = page.locator('form[data-platform-form]');
    await expect(forms).toHaveCount(1);
    await expect(forms.first()).toHaveAttribute('data-wedge', 'consent-enforcement');
    await expect(page.locator('main input[type="url"]:not([name="site"])')).toHaveCount(0);
  });

  test('/platform/edge-seo/ folds the site count into the message', async ({ page }) => {
    let posted: Record<string, unknown> | null = null;
    await page.route('**/ai-readability/api/contact', async (route) => {
      posted = route.request().postDataJSON();
      await route.fulfill({ status: 200, contentType: 'application/json', body: '{"ok":true}' });
    });
    await stubUmami(page);
    await page.goto('/platform/edge-seo/');
    const form = page.locator('form[data-platform-form]');
    await expect(form).toHaveCount(1);
    await expect(form).toHaveAttribute('data-wedge', 'edge-seo');
    await form.locator('input[name="email"]').fill('grace@example.com');
    await form.locator('input[name="sites"]').fill('40');
    await form.locator('button[type="submit"]').click();
    await expect(form.locator('[data-status]')).toContainText('Thanks');
    expect(posted).toMatchObject({
      topic: 'platform',
      wedge: 'edge-seo',
      url: '/platform/edge-seo/',
      message: 'Pack: Edge SEO at the origin\nSites managed: 40',
    });
    const events = await trackedEvents(page);
    expect(events.map((e) => e.name)).toEqual(['edge-seo', 'lead_submit']);
  });

  test('the footer links to the hub', async ({ page }) => {
    await page.goto('/');
    const link = page.locator('footer a[href="/platform/"]');
    await expect(link).toHaveText('Transform packs');
    await expect(link).toHaveAttribute('data-umami-event', 'nav-platform-footer');
  });
});
