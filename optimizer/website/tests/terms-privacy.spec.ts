// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { readFileSync } from 'node:fs';
import { test, expect } from '@playwright/test';

// The current terms version, read the way test/sync/terms-version-sync.test.ts
// reads it: TERMS_VERSION_WEB in the pricing page, kept in lockstep with the
// "Version" line on /terms/. No hand edit here when the terms change.
const TERMS_VERSION_WEB = (() => {
  const src = readFileSync(new URL('../src/pages/pricing.astro', import.meta.url), 'utf8');
  const m = src.match(/TERMS_VERSION_WEB\s*=\s*'([^']+)'/);
  if (!m) throw new Error('terms-privacy.spec: TERMS_VERSION_WEB not found in pricing.astro');
  return m[1];
})();

test.describe('/terms/ page', () => {
  test('has correct page title', async ({ page }) => {
    await page.goto('/terms/');
    await expect(page).toHaveTitle(/Terms of Service/i);
  });

  test('has H1 heading', async ({ page }) => {
    await page.goto('/terms/');
    // Scope to the in-page h1 — the Astro dev toolbar injects its own h1 nodes
    // (outside <main>), so a bare `h1` locator is not strict-mode safe in dev.
    await expect(page.locator('main h1')).toHaveText(/Terms of Service/i);
  });

  test('displays the current terms version identifier', async ({ page }) => {
    await page.goto('/terms/');
    // Scoped to main: the telemetry strip shows a capture date (2026-10-..)
    // until its live value replaces it, which also matches a bare '2026-10'.
    await expect(page.locator('main').getByText(`Version ${TERMS_VERSION_WEB}`)).toBeVisible();
  });

  test('displays effective date November 2026', async ({ page }) => {
    await page.goto('/terms/');
    await expect(page.getByText('November 2026')).toBeVisible();
  });

  test('links to privacy policy', async ({ page }) => {
    await page.goto('/terms/');
    // The one We-Amp privacy notice lives on we-amp.com (apex, never www).
    const privacyLink = page.locator('main a[href="https://we-amp.com/privacy/"]');
    await expect(privacyLink.first()).toBeVisible();
  });

  test('links to /license/', async ({ page }) => {
    await page.goto('/terms/');
    const licenseLink = page.locator('main a[href="/license/"]');
    await expect(licenseLink.first()).toBeVisible();
  });

  test('has info@we-amp.com contact link', async ({ page }) => {
    await page.goto('/terms/');
    const emailLink = page.locator('a[href="mailto:info@we-amp.com"]');
    await expect(emailLink.first()).toBeVisible();
  });

  test('uses BaseLayout (header nav + footer)', async ({ page }) => {
    await page.goto('/terms/');
    await expect(page.locator('header nav')).toBeVisible();
    await expect(page.locator('footer')).toBeVisible();
  });

  test('displays company name We-Amp B.V. and KvK 57898138', async ({ page }) => {
    await page.goto('/terms/');
    await expect(page.getByText('We-Amp B.V.').first()).toBeVisible();
    await expect(page.getByText('57898138').first()).toBeVisible();
  });

  test('mentions the 14-day consumer withdrawal right', async ({ page }) => {
    await page.goto('/terms/');
    await expect(page.getByText('14-day right of withdrawal').first()).toBeVisible();
  });

  test('states the software license and the subscription model, no retired model', async ({
    page,
  }) => {
    await page.goto('/terms/');
    const main = page.locator('main');
    await expect(main).toContainText('Apache License 2.0');
    await expect(main).not.toContainText('private package repository');
    await expect(main).not.toContainText('SLA-backed');
    await expect(main).not.toContainText('hardened');
    await expect(main).not.toContainText('when available');
    await expect(main).not.toContainText('X-PageSpeed-Warn');
    await expect(main).not.toContainText('unlicensed');
    await expect(main).not.toContainText('license key');
  });

  test('mentions Netherlands (governing law)', async ({ page }) => {
    await page.goto('/terms/');
    await expect(page.getByText('Netherlands').first()).toBeVisible();
  });
});

test.describe('/privacy/ page', () => {
  // Production answers /privacy/ with a 301 to https://we-amp.com/privacy/ from
  // the server's redirect map. The dev server serves the pointer page, whose
  // canonical is the notice on we-amp.com.
  test('points at the one privacy notice on we-amp.com', async ({ page }) => {
    await page.goto('/privacy/');
    await expect(page.locator('link[rel="canonical"]')).toHaveAttribute(
      'href',
      'https://we-amp.com/privacy/',
    );
    await expect(page.locator('main a[href="https://we-amp.com/privacy/"]')).toBeVisible();
  });

  test('the footer links the notice on we-amp.com', async ({ page }) => {
    await page.goto('/');
    await expect(
      page.locator('footer').getByRole('link', { name: 'Privacy', exact: true }),
    ).toHaveAttribute('href', 'https://we-amp.com/privacy/');
    await expect(page.locator('a[href="/privacy/"]')).toHaveCount(0);
  });
});
