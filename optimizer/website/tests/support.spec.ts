// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

// /support/ is the commercial product page: what a subscription buys, the
// three tiers, signed packages and images (what ships today, how to
// verify), partners, consulting, existing customers, and the FAQ.
test.describe('Support page', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/support/');
  });

  test('title, description and H1', async ({ page }) => {
    const title = await page.title();
    expect(title.length).toBeLessThanOrEqual(60);
    expect(title).toContain('mod_pagespeed');
    expect(title).not.toMatch(/mod_pagespeed 2\.1(?!\.)/);
    const description = await page.locator('meta[name="description"]').getAttribute('content');
    expect(description!.length).toBeLessThanOrEqual(155);
    await expect(page.getByRole('heading', { level: 1 })).toHaveCount(1);
  });

  test('renders every section', async ({ page }) => {
    for (const id of [
      'what-you-get',
      'tiers',
      'hardened-builds',
      'verify',
      'hosting-partners',
      'consulting',
      'existing-customers',
      'reach-us',
      'faq',
    ]) {
      await expect(page.locator(`#${id}`), `#${id}`).toBeVisible();
    }
    await expect(page.locator('#tiers [data-tier]')).toHaveCount(3);
    await expect(page.locator('#tiers [data-pricing-status]')).toContainText(
      'Prices are per year, billed annually, excl. VAT.',
    );
  });

  test('the hero price button jumps to the tiers on the same page', async ({ page }) => {
    const button = page.locator('a[data-umami-event-position="hero_pricing"]');
    await expect(button).toHaveAttribute('href', '#tiers');
    await button.click();
    await expect(page).toHaveURL(/\/support\/#tiers$/);
    await expect(page.locator('#tiers h2')).toBeInViewport();
  });

  test('what a subscription buys names channel, response, updates and scope', async ({ page }) => {
    const readout = page.locator('#what-you-get dl');
    for (const key of ['channel:', 'response:', 'security updates:', 'scope:']) {
      await expect(readout).toContainText(key);
    }
    await expect(readout).toContainText('CET business days');
  });

  test('tiers carry their price, first-response target and own button', async ({ page }) => {
    const tiers = page.locator('#tiers');
    await expect(tiers.locator('[data-tier="standard"]')).toContainText('€490');
    await expect(tiers.locator('[data-tier="priority"]')).toContainText('€1,900');
    await expect(tiers.locator('[data-tier="enterprise"]')).toContainText('From');
    await expect(tiers.locator('[data-tier="enterprise"]')).toContainText('€7,500');
    await expect(tiers.locator('[data-tier="standard"]')).toContainText(
      'First response within 2 business days',
    );
    await expect(tiers.locator('[data-tier="enterprise"]')).toContainText('Custom build targets');
    await expect(tiers.locator('[data-tier="standard"] a')).toHaveAttribute(
      'href',
      'https://mpspro.onfastspring.com/support-standard-annual',
    );
    await expect(tiers.locator('[data-tier="enterprise"] a')).toHaveAttribute(
      'href',
      '/contact/?topic=enterprise',
    );
  });

  test('signed artifacts say only what ships today', async ({ page }) => {
    const section = page.locator('#hardened-builds');
    await expect(section).toContainText('GPG-signed');
    await expect(section).toContainText('SHA256SUMS');
    await expect(section).toContainText('SPDX');
    await expect(section).toContainText('cosign');
    await expect(section).toContainText('SBOM and VEX');
    await expect(section).toContainText(
      'Every release from 2.6.0 on carries its SPDX SBOM and VEX',
    );
    await expect(page.locator('main')).not.toContainText('can ask us to send the copy');
    await expect(section).not.toContainText('custom build targets');
    // Security fixes reach everyone at once; no private repository is offered.
    await expect(section).toContainText('reach everyone at the same time');
    await expect(section).not.toContainText('private package repository');
    await expect(section).not.toContainText('before the public release');
    await expect(section).not.toContainText('ahead of the public release');
    await expect(section).not.toContainText('hardened build pipeline');
    // The packages carry no per-build signed SBOM or provenance claim.
    await expect(section).not.toContainText('signed SBOM');
    await expect(section).not.toContainText('with every build');
    const recipe = await page.locator('#verify pre code').innerText();
    expect(recipe.trim().split('\n')).toHaveLength(8);
    expect(recipe).toContain('gpg --show-keys --with-fingerprint');
    expect(recipe).toContain('signed-by=');
    // Each key checked by its full fingerprint, for its own purpose.
    expect(recipe).toContain('DF00 E296 BE91 41CE A541  1346 F50D 6054 F107 12A0');
    expect(recipe).toContain('411D EFDB 5713 51D2 B25A  EFCC 9C02 B5BD CCE7 5313');
    // The release signature is checked against a keyring holding only the release key.
    expect(recipe).toContain('gpgv --keyring ./release.gpg SHA256SUMS.asc SHA256SUMS');
    expect(recipe).not.toContain('gpg --verify');
    expect(recipe).toContain('sha256sum --check');
    // The 1.x key (being revoked) is never linked as a current key.
    await expect(page.locator('a[href*="weamp-pkg-public.asc"]')).toHaveCount(0);
  });

  test('no USD price and no per-month rate', async ({ page }) => {
    const text = await page.locator('main').innerText();
    expect(text).not.toMatch(/\$\s?\d/);
    expect(text).not.toMatch(/\d[\d.,]*\s*\/\s*(?:mo|month)\b/i);
  });

  test('commercial and security addresses are separate', async ({ page }) => {
    const reach = page.locator('#reach-us');
    await expect(reach.locator('a[href="mailto:info@we-amp.com"]')).toBeVisible();
    await expect(reach.locator('a[href="mailto:security@modpagespeed.com"]')).toBeVisible();
    await expect(page.locator(`a[href*="${['sales', 'we-amp.com'].join('@')}"]`)).toHaveCount(0);
  });

  test('existing customers are routed to the transfer topic', async ({ page }) => {
    await expect(
      page.locator('#existing-customers a[href="/contact/?topic=iispeed-transfer"]'),
    ).toBeVisible();
  });

  test('FAQ has eight questions and the FAQPage JSON-LD mirrors them', async ({ page }) => {
    const visible = (await page.locator('#faq details > summary').allInnerTexts()).map((t) =>
      t.trim(),
    );
    expect(visible).toHaveLength(8);
    const blocks = await page.locator('script[type="application/ld+json"]').allTextContents();
    const faq = blocks.map((b) => JSON.parse(b)).find((d) => d['@type'] === 'FAQPage');
    expect(faq).toBeTruthy();
    expect(faq.mainEntity.map((q: { name: string }) => q.name)).toEqual(visible);
    for (const q of faq.mainEntity) {
      expect(q.acceptedAnswer.text).not.toMatch(/<[^>]+>/);
    }
  });

  test('every paid CTA carries cta_commercial with offer, surface and position', async ({
    page,
  }) => {
    const ctas = page.locator('main [data-umami-event="cta_commercial"]');
    const count = await ctas.count();
    expect(count).toBeGreaterThanOrEqual(9);
    const offers = new Set<string>();
    for (let i = 0; i < count; i++) {
      const cta = ctas.nth(i);
      const offer = await cta.getAttribute('data-umami-event-offer');
      expect([
        'support',
        'buy-standard',
        'buy-priority',
        'enterprise',
        'hardened',
        'hosting',
        'consulting',
        'agentic',
      ]).toContain(offer);
      offers.add(offer!);
      await expect(cta).toHaveAttribute('data-umami-event-surface', 'support');
      await expect(cta).toHaveAttribute('data-umami-event-position', /\S/);
    }
    expect([...offers].sort()).toEqual([
      'buy-priority',
      'buy-standard',
      'consulting',
      'enterprise',
      'hosting',
      'support',
    ]);
  });

  test('consulting hands off to we-amp.com', async ({ page }) => {
    await expect(
      page.locator('#consulting a[href="https://we-amp.com/consulting/"]'),
    ).toBeVisible();
    await expect(page.locator('#consulting')).toContainText('fixed-price');
  });
});
