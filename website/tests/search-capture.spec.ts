// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';
import { stubUmami, trackedEvents } from './helpers/umami';

// The search-capture pass: the pages that rank for the head terms carry the
// searcher's words in their titles, and readers arriving from the archived
// PageSpeed Module docs on developers.google.com get a one-paragraph
// orientation on the three pages those docs redirect to.

const titles: Array<[string, string]> = [
  ['/', 'PageSpeed module for nginx and Apache: mod_pagespeed'],
  ['/docs/getting-started/', 'Install mod_pagespeed: nginx, Apache, Docker, IIS'],
  ['/docs/installation-module/', 'Install mod_pagespeed on Apache and nginx: signed packages'],
  ['/docs/filters/', 'PageSpeed filters: the complete reference'],
  ['/analyze/', 'Free PageSpeed Insights test, with the fix attached'],
  [
    '/alternatives/google-pagespeed-module/',
    'What is the Google PageSpeed module, and what replaced it',
  ],
  ['/core-web-vitals/cls/', 'How to fix Cumulative Layout Shift (CLS) in 2026'],
  ['/core-web-vitals/lcp/', 'How to fix Largest Contentful Paint (LCP) in 2026'],
  ['/ai-readability/', 'AI readability checker: can ChatGPT read your page? (free)'],
  [
    '/blog/economics-of-image-optimization/',
    'Image CDN cost comparison: Cloudflare, Cloudinary, imgix',
  ],
  ['/blog/fix-lcp-wordpress-2026/', 'Improve LCP, FCP and TTFB on WordPress at the server (2026)'],
  [
    '/blog/ssimulacra2-image-quality-verification/',
    'SSIMULACRA2: how to verify image quality after compression',
  ],
  ['/blog/content-hash-urls/', 'Content-hash URLs: why .pagespeed. URLs carry a hash'],
  ['/security/', 'Security policy and vulnerability reporting'],
  // A docs title without a seoTitle: the title plus the product-name suffix.
  ['/docs/security/', 'Security hardening guide — mod_pagespeed'],
];

test.describe('search capture: titles', () => {
  for (const [path, title] of titles) {
    test(`${path} has the search title`, async ({ page }) => {
      await page.goto(path);
      await expect(page).toHaveTitle(title);
      expect(title.length).toBeLessThanOrEqual(60);
    });
  }

  test('getting started leads with the install heading', async ({ page }) => {
    await page.goto('/docs/getting-started/');
    await expect(page.locator('main h1').first()).toHaveText('Install mod_pagespeed');
  });

  test('the PageSpeed module explainer answers availability and the browser extension', async ({
    page,
  }) => {
    await page.goto('/alternatives/google-pagespeed-module/');
    await expect(page.locator('main h1').first()).toHaveText(
      'What is the Google PageSpeed module, and what replaced it',
    );
    await expect(page.getByRole('heading', { name: 'Is it still available?' })).toBeVisible();
    const extension = page.getByRole('heading', {
      name: 'Where did the PageSpeed browser extension go?',
    });
    await expect(extension).toBeVisible();
    await expect(
      extension.locator('xpath=following-sibling::p[1]').locator('a[href="/analyze/"]'),
    ).toBeVisible();
  });

  test('the CLS and LCP pillars carry the server-fix box', async ({ page }) => {
    for (const path of ['/core-web-vitals/cls/', '/core-web-vitals/lcp/']) {
      await page.goto(path);
      const box = page.locator('section[aria-labelledby="fix-at-the-server"]');
      await expect(box).toBeVisible();
      expect(await box.locator('a[href^="/docs/"]').count()).toBeGreaterThanOrEqual(3);
    }
  });

  test('the AI readability page answers whether AI crawlers execute JavaScript', async ({
    page,
  }) => {
    await page.goto('/ai-readability/');
    await expect(
      page.getByRole('heading', { name: 'Can AI crawlers execute JavaScript?' }),
    ).toBeVisible();
  });
});

const orientationPages = [
  '/docs/getting-started/',
  '/docs/installation-module/',
  '/docs/configuration/',
];

test.describe('search capture: Google-referral orientation', () => {
  for (const path of orientationPages) {
    test(`${path} shows the orientation for a developers.google.com referrer`, async ({ page }) => {
      await stubUmami(page);
      await page.goto(path, {
        referer: 'https://developers.google.com/speed/pagespeed/module/',
      });
      const aside = page.locator('#referral-orientation');
      await expect(aside).toBeVisible();
      await expect(aside).toContainText('Arriving from Google’s PageSpeed Module docs?');
      await expect(aside.locator('a[href="/docs/getting-started/"]')).toBeVisible();
      await expect(aside.locator('a[href="/alternatives/google-pagespeed-module/"]')).toBeVisible();
      await expect
        .poll(async () =>
          (await trackedEvents(page)).filter((e) => e.name === 'referral_orientation_shown'),
        )
        .toHaveLength(1);
    });

    test(`${path} keeps the orientation hidden without that referrer`, async ({ page }) => {
      await stubUmami(page);
      await page.goto(path);
      await expect(page.locator('#referral-orientation')).toBeHidden();
      await page.goto(path, { referer: 'https://www.google.com/' });
      await expect(page.locator('#referral-orientation')).toBeHidden();
      const events = await trackedEvents(page);
      expect(events.filter((e) => e.name === 'referral_orientation_shown')).toHaveLength(0);
    });
  }

  test('other docs pages do not carry the orientation', async ({ page }) => {
    await page.goto('/docs/filters/', {
      referer: 'https://developers.google.com/speed/pagespeed/module/',
    });
    await expect(page.locator('#referral-orientation')).toHaveCount(0);
  });
});
