// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

test.describe('Responsive layout', () => {
  test('desktop: header nav links visible, hamburger hidden', async ({ page }) => {
    await page.setViewportSize({ width: 1280, height: 720 });
    await page.goto('/');
    // Desktop nav should be visible
    const desktopNav = page.locator('header .hidden.lg\\:flex');
    await expect(desktopNav).toBeVisible();
    // Hamburger should be hidden
    await expect(page.locator('#mobile-menu-btn')).toBeHidden();
  });

  test('mobile: header nav links hidden, hamburger visible', async ({ page }) => {
    await page.setViewportSize({ width: 375, height: 667 });
    await page.goto('/');
    // Desktop nav should be hidden
    const desktopNav = page.locator('header .hidden.lg\\:flex');
    await expect(desktopNav).toBeHidden();
    // Hamburger should be visible
    await expect(page.locator('#mobile-menu-btn')).toBeVisible();
  });

  test('mobile: hamburger opens nav menu', async ({ page }) => {
    await page.setViewportSize({ width: 375, height: 667 });
    await page.goto('/');
    const menu = page.locator('#mobile-menu');
    await expect(menu).toBeHidden();

    await page.locator('#mobile-menu-btn').click();
    await expect(menu).toBeVisible();

    // Menu should contain nav links: Docs direct, the Support group, Contact.
    await expect(menu.locator('a[href="/docs/"]')).toBeVisible();
    await expect(menu.locator('a[href="/support/"]').first()).toBeVisible();
    await expect(menu.locator('a[href="/contact/"]')).toBeVisible();
  });

  test('pricing cards are visible on mobile', async ({ page }) => {
    await page.setViewportSize({ width: 375, height: 667 });
    await page.goto('/pricing/');
    // Three tier cards; Priority is the featured one.
    await expect(page.locator('[data-tier]').first()).toBeVisible();
    await expect(page.locator('[data-tier="priority"].card-featured')).toBeVisible();
  });

  test('footer is visible on mobile', async ({ page }) => {
    await page.setViewportSize({ width: 375, height: 667 });
    await page.goto('/');
    await expect(page.locator('footer')).toBeVisible();
  });

  test('calculator inputs are visible on mobile', async ({ page }) => {
    await page.setViewportSize({ width: 375, height: 667 });
    await page.goto('/calculator/');
    await expect(page.locator('#pageviews')).toBeVisible();
    // per-site licensing renamed the "servers" input to "sites".
    await expect(page.locator('#sites')).toBeVisible();
  });

  test('demo tabs are visible on mobile', async ({ page }) => {
    await page.setViewportSize({ width: 375, height: 667 });
    await page.goto('/demo/');
    await expect(page.locator('#tab-ecommerce')).toBeVisible();
  });
});

test.describe('Reference-table names stay whole on phones', () => {
  // A two-column reference table (a name and its description) used to let the
  // name column shrink to about five characters at 320px, splitting names
  // such as "standby" and "unplugged" mid-word. Short names must stay on one
  // line; only a name too long for the column may break.
  for (const path of ['/docs/configuration/', '/docs/api-reference/']) {
    test(`${path}: short first-column names are not split at 320px`, async ({ page }) => {
      await page.setViewportSize({ width: 320, height: 740 });
      await page.goto(path);
      const split = await page.evaluate(() =>
        Array.from(document.querySelectorAll('.prose table'))
          .filter((t) => t.querySelector('tr')?.children.length === 2)
          .flatMap((t) => Array.from(t.querySelectorAll('tr > :first-child code')))
          .filter((code) => (code.textContent ?? '').trim().length <= 12)
          .filter((code) => {
            const range = document.createRange();
            range.selectNodeContents(code);
            return new Set(Array.from(range.getClientRects(), (r) => Math.round(r.top))).size > 1;
          })
          .map((code) => code.textContent),
      );
      expect(split).toEqual([]);
    });
  }
});

// Docs pages must not scroll horizontally on narrow screens. Long
// unbreakable tokens (filter names, flags, URLs) and the permalink anchor
// appended to article headings used to push these pages past the viewport;
// the global prose styles now wrap inside the word. The pages below are the
// ones that regressed, checked at the narrow end of the phone range.
const narrowDocPages = [
  '/docs/configuration/',
  '/docs/image-filters/',
  '/docs/worker-configuration/',
  '/docs/filters/rewrite_style_attributes/',
  '/docs/filters/rewrite_style_attributes_with_url/',
  '/docs/filters/resize_rendered_image_dimensions/',
];

// Footer and top-strip links are spaced so a thumb can hit them: rows of at
// least 28 px, the two icon links 44 px square, the newsletter consent row
// 44 px, and the telemetry state link at least 24 px.
test.describe('Phone tap targets in the page chrome', () => {
  test('footer links, icons, consent row and telemetry link at 360px', async ({ page }) => {
    await page.setViewportSize({ width: 360, height: 800 });
    await page.goto('/pricing/');
    const rows = await page
      .locator('[data-site-footer] ul a')
      .evaluateAll((els) => els.map((el) => Math.round(el.getBoundingClientRect().height)));
    expect(rows.length).toBeGreaterThan(10);
    expect(rows.filter((h) => h < 28)).toEqual([]);

    for (const title of ['GitHub', 'RSS feed']) {
      const box = await page.locator(`[data-site-footer] a[title="${title}"]`).boundingBox();
      expect(box, title).not.toBeNull();
      expect(Math.round(box!.width)).toBeGreaterThanOrEqual(44);
      expect(Math.round(box!.height)).toBeGreaterThanOrEqual(44);
    }

    const consent = await page
      .locator('[data-site-footer] label:has(input[type="checkbox"])')
      .boundingBox();
    expect(consent).not.toBeNull();
    expect(Math.round(consent!.height)).toBeGreaterThanOrEqual(44);

    const state = await page.locator('a.t-state-link').boundingBox();
    expect(state).not.toBeNull();
    expect(Math.round(state!.height)).toBeGreaterThanOrEqual(24);
  });
});

// Small text links get a taller tap area on phones without moving the layout:
// a tap 8 px above or below the text still lands on the link.
const smallLinks: Array<{ path: string; selector: string }> = [
  { path: '/alternatives/cloudflare-apo/', selector: 'nav[aria-label="Breadcrumb"] a[href="/"]' },
  { path: '/examples/combine_css/', selector: 'main a[href="/examples/"] >> nth=-1' },
  { path: '/blog/fix-cls-nginx-2026/', selector: 'nav[aria-label="Article navigation"] a' },
  { path: '/contact/', selector: '#contact-copy-email' },
];

test.describe('Small text links on phones', () => {
  for (const { path, selector } of smallLinks) {
    test(`${path}: the tap area reaches past the text`, async ({ page }) => {
      await page.setViewportSize({ width: 360, height: 800 });
      await page.goto(path);
      const link = page.locator(selector);
      await link.scrollIntoViewIfNeeded();
      const hits = await link.evaluate((el) => {
        const r = el.getBoundingClientRect();
        const x = r.left + r.width / 2;
        return [r.top - 8, r.bottom + 8].map((y) => {
          const hit = document.elementFromPoint(x, y);
          return !!hit && (hit === el || el.contains(hit));
        });
      });
      expect(hits).toEqual([true, true]);
    });
  }
});

// The scrollable install snippets on the comparison pages take keyboard focus
// (so they can be scrolled without a pointer) and use 14 px code on phones.
test('alternatives install snippets: focusable, 14px on a phone', async ({ page }) => {
  await page.setViewportSize({ width: 360, height: 800 });
  await page.goto('/alternatives/cloudflare-apo/');
  const pres = page.locator('main pre.overflow-x-auto');
  expect(await pres.count()).toBeGreaterThan(0);
  const info = await pres.evaluateAll((els) =>
    els.map((el) => ({
      tabindex: el.getAttribute('tabindex'),
      size: parseFloat(getComputedStyle(el).fontSize),
    })),
  );
  expect(info.filter((i) => i.tabindex !== '0' || i.size < 14)).toEqual([]);
});

test.describe('Docs pages fit narrow viewports', () => {
  for (const width of [320, 360, 390]) {
    test(`no horizontal scroll at ${width}px`, async ({ page }) => {
      await page.setViewportSize({ width, height: 844 });
      for (const path of narrowDocPages) {
        await page.goto(path);
        // On article pages the client script appends permalink anchors to
        // headings; they are part of the layout that must fit, so wait for
        // them before measuring.
        if ((await page.locator('article').count()) > 0) {
          await expect(page.locator('article .heading-anchor').first()).toBeAttached();
        }
        // Compare with the width we set, not innerWidth: mobile emulation
        // widens innerWidth to fit overflowing content, so it can never fail.
        const scrollWidth = await page.evaluate(() => document.documentElement.scrollWidth);
        expect(scrollWidth, `${path} at ${width}`).toBeLessThanOrEqual(width);
      }
    });
  }

  test('/docs/http-api/ has no horizontal scroll at 360px', async ({ page }) => {
    await page.setViewportSize({ width: 360, height: 844 });
    await page.goto('/docs/http-api/');
    await expect(page.locator('article .heading-anchor').first()).toBeAttached();
    const scrollWidth = await page.evaluate(() => document.documentElement.scrollWidth);
    expect(scrollWidth).toBeLessThanOrEqual(360);
  });

  // The page is not .prose, so the global long-token wrapping rule does not
  // reach it, and the source URL it prints is one long token. The PR sweep of
  // the geometry oracle measures 375, where that token still fits; 320 is where
  // it pushed the page wide.
  test('/ai-crawler-cost/ has no horizontal scroll at 320px', async ({ page }) => {
    await page.setViewportSize({ width: 320, height: 844 });
    await page.goto('/ai-crawler-cost/');
    const scrollWidth = await page.evaluate(() => document.documentElement.scrollWidth);
    expect(scrollWidth).toBeLessThanOrEqual(320);
  });
});

test.describe('No sideways scroll at 320px', () => {
  // A long unbreakable token (a directive or filter name in a title, a command
  // line in a code block) must wrap or scroll inside its own box; it must never
  // make the whole page wider than a small phone.
  const pages = [
    '/',
    '/download/',
    '/pricing/',
    '/support/',
    '/features/',
    '/contact/',
    '/docs/',
    '/docs/getting-started/',
    '/docs/configuration/',
    '/examples/',
    '/examples/combine_css/',
    '/blog/',
    '/blog/fix-cls-nginx-2026/',
    '/alternatives/',
    '/alternatives/cloudflare-apo/',
    '/ai-readability/hosted-builders/',
    // A static file under public/; the dev server only serves it by file name.
    '/data/web-bot-auth-2026-10-19/index.html',
  ];
  for (const path of pages) {
    test(`${path} fits 320px`, async ({ page }) => {
      await page.setViewportSize({ width: 320, height: 740 });
      const res = await page.goto(path);
      expect(res?.status()).toBe(200);
      const scrollWidth = await page.evaluate(() => document.documentElement.scrollWidth);
      expect(scrollWidth).toBeLessThanOrEqual(320);
    });
  }
});

test.describe('Phone form fields are at least 16px', () => {
  // iOS Safari zooms the page into a text field set below 16px when it takes
  // focus and leaves the page zoomed after blur. Every text field, text area
  // and select, including the closed message launcher's, must be 16px or more
  // on a phone.
  const pages = [
    '/',
    '/blog/fix-cls-nginx-2026/',
    '/features/',
    '/contact/',
    '/calculator/',
    '/analyze/',
    '/ai-crawler-cost/',
    '/ai-readability/',
    '/hosting-partners/',
  ];
  const skip = ['checkbox', 'radio', 'hidden', 'range', 'submit', 'button', 'file', 'color'];
  for (const path of pages) {
    test(`${path} fields are 16px or more at 390px`, async ({ page }) => {
      await page.setViewportSize({ width: 390, height: 740 });
      await page.goto(path);
      const small = await page.evaluate(
        (skipTypes) =>
          Array.from(document.querySelectorAll('input, select, textarea'))
            .filter((el) => !skipTypes.includes((el as HTMLInputElement).type))
            .filter((el) => !el.closest('[aria-hidden="true"]'))
            .map((el) => ({
              field: el.id || (el as HTMLInputElement).name,
              size: parseFloat(getComputedStyle(el).fontSize),
            }))
            .filter((f) => f.size < 16),
        skip,
      );
      expect(small).toEqual([]);
    });
  }
});

// Card descriptions on the hub pages are reading text, not captions: on a
// phone they use the 16 px body size, from 640 px up they keep the compact
// 14 px card size. Short card lines (eyebrows, "Read" links) stay small.
const cardCopy: Array<{ path: string; selector: string }> = [
  { path: '/docs/', selector: 'main a p' },
  { path: '/examples/', selector: 'main a p' },
  { path: '/alternatives/', selector: 'main .card-stretch p' },
  { path: '/alternatives/cloudflare-apo/', selector: 'main ul dl dd' },
];

async function cardCopySizes(page: import('@playwright/test').Page, selector: string) {
  return page.locator(selector).evaluateAll((els) =>
    els
      .filter((el) => (el.textContent ?? '').trim().length >= 40)
      .filter((el) => (el as HTMLElement).offsetParent !== null)
      .map((el) => ({
        text: (el.textContent ?? '').trim().slice(0, 40),
        size: parseFloat(getComputedStyle(el).fontSize),
      })),
  );
}

test.describe('Hub card descriptions', () => {
  for (const { path, selector } of cardCopy) {
    test(`${path} card copy is 16px on a phone`, async ({ page }) => {
      await page.setViewportSize({ width: 360, height: 800 });
      await page.goto(path);
      const sizes = await cardCopySizes(page, selector);
      expect(sizes.length, `${path} has card copy`).toBeGreaterThan(0);
      expect(sizes.filter((s) => s.size < 16)).toEqual([]);
    });
  }

  test('desktop card copy keeps the compact size', async ({ page }) => {
    await page.setViewportSize({ width: 1280, height: 800 });
    for (const { path, selector } of cardCopy.slice(0, 3)) {
      await page.goto(path);
      const sizes = await cardCopySizes(page, selector);
      expect(sizes.length, `${path} has card copy`).toBeGreaterThan(0);
      expect(
        sizes.filter((s) => s.size !== 14),
        path,
      ).toEqual([]);
    }
  });
});
