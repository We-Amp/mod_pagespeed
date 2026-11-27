// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Link names, landmarks and form errors on the product, guide and blog pages,
// as a screen reader is given them (Chromium's accessibility tree):
// - whole-card links are named by the card title, and the whole card still
//   follows the link on a click and rings on keyboard focus;
// - call-to-action links do not read their arrow glyph;
// - citation links open in the same tab;
// - scrollable configuration samples are named, focusable regions;
// - the contact form is named by the page heading, and an empty send flags
//   every empty required field and moves focus to the first;
// - the Core Web Vitals fix list is part of the page, not a complementary.

import { test, expect, type Page } from '@playwright/test';
import { axQuery, gotoSettled } from './helpers/ax-tree';

const squash = (s: string) => s.replace(/\s+/g, ' ').trim();
const ARROW = /[←-⇿⤀-⥿]/;

const CARD_GRIDS: Array<{ path: string; root: string; count: number }> = [
  { path: '/alternatives/', root: 'main', count: 4 },
  { path: '/how-it-works/', root: 'main', count: 3 },
  { path: '/features/', root: 'main section.section', count: 7 },
  { path: '/blog/agentic-web-at-the-origin/', root: 'main', count: 1 },
];

async function cardLinkNames(page: Page, root: string) {
  const titles = await page
    .locator(`${root} .card-stretch .card-stretch-link`)
    .evaluateAll((els) => els.map((el) => (el.textContent ?? '').replace(/\s+/g, ' ').trim()));
  const names = (await axQuery(page, root, 'link')).map((l) => squash(l.name));
  return { titles, names };
}

test.describe('whole-card links', () => {
  for (const g of CARD_GRIDS) {
    test(`${g.path}: each card link is named by its title`, async ({ page }) => {
      await gotoSettled(page, g.path);
      const { titles, names } = await cardLinkNames(page, g.root);
      expect(titles.length).toBeGreaterThanOrEqual(g.count);
      for (const t of titles) expect(names).toContain(t);
      // No card link reads the card body.
      for (const t of titles) expect(t.split(' ').length).toBeLessThan(16);
    });
  }

  test('a click on a card body follows the card link', async ({ page }) => {
    await page.setViewportSize({ width: 1440, height: 900 });
    await gotoSettled(page, '/alternatives/');
    const card = page.locator('main ul .card-stretch').first();
    const href = await card.locator('.card-stretch-link').getAttribute('href');
    const cta = card.getByText('Read the comparison');
    await cta.scrollIntoViewIfNeeded();
    const box = (await cta.boundingBox())!;
    await page.mouse.click(box.x + box.width / 2, box.y + box.height / 2);
    await expect(page).toHaveURL(new RegExp(`${href!.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}$`));
  });

  test('keyboard focus on a card link rings the whole card', async ({ page }) => {
    await page.setViewportSize({ width: 1440, height: 900 });
    await gotoSettled(page, '/how-it-works/');
    const links = page.locator('main .card-stretch-link');
    const cards = page.locator('main .card-stretch');
    await links.first().focus();
    await page.keyboard.press('Tab');
    await expect(links.nth(1)).toBeFocused();
    await expect(cards.nth(1)).toHaveCSS('outline-style', 'solid');
    await expect(links.nth(1)).toHaveCSS('outline-style', 'none');
  });
});

test.describe('call-to-action names', () => {
  for (const path of ['/pricing/', '/support/', '/blog/agentic-web-at-the-origin/']) {
    test(`${path}: no link reads an arrow glyph`, async ({ page }) => {
      await gotoSettled(page, path);
      const names = (await axQuery(page, 'main', 'link')).map((l) => squash(l.name));
      expect(names.length).toBeGreaterThan(3);
      expect(names.filter((n) => ARROW.test(n))).toEqual([]);
    });
  }

  test('the blog back link and plan buttons keep their words', async ({ page }) => {
    await gotoSettled(page, '/blog/agentic-web-at-the-origin/');
    expect(await axQuery(page, 'main', 'link', 'Back to blog')).toHaveLength(1);
    await gotoSettled(page, '/pricing/');
    const names = (await axQuery(page, 'main', 'link')).map((l) => squash(l.name));
    expect(names).toContain('Buy Standard');
  });
});

test.describe('citations open in the same tab', () => {
  for (const [path, href] of [
    ['/about/', 'developers.googleblog.com'],
    ['/contact/', 'https://we-amp.com/'],
    ['/alternatives/', 'builtwith.com'],
    ['/blog/agentic-web-at-the-origin/', 'linkedin.com'],
  ]) {
    test(`${path} ${href}`, async ({ page }) => {
      await gotoSettled(page, path);
      const link = page.locator(`main a[href*="${href}"]`).first();
      await expect(link).toBeAttached();
      await expect(link).not.toHaveAttribute('target', /.*/);
    });
  }
});

test('alternatives configuration samples are named, focusable regions', async ({ page }) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await gotoSettled(page, '/alternatives/nitropack/');
  const regions = (await axQuery(page, 'main', 'region')).map((r) => squash(r.name));
  expect(regions).toEqual(
    expect.arrayContaining([
      expect.stringMatching(/^Installing mod_pagespeed for nginx on Debian/),
      'Then enable the filters (nginx config, one-time):',
    ]),
  );
  const pre = page.locator('main pre[aria-labelledby="config-2"]');
  await pre.focus();
  await expect(pre).toBeFocused();
});

test.describe('contact form', () => {
  test('is named by the page heading', async ({ page }) => {
    await gotoSettled(page, '/contact/');
    const forms = await axQuery(page, 'main', 'form');
    expect(forms.map((f) => squash(f.name))).toEqual([
      'One form for setup, support and partnerships.',
    ]);
  });

  test('an empty send flags each empty field and focuses the first', async ({ page }) => {
    await gotoSettled(page, '/contact/');
    await page.locator('#contact-email').fill('someone@example.com');
    await page.locator('#contact-submit').click();
    await expect(page.locator('#contact-name')).toBeFocused();
    await expect(page.locator('#contact-status')).toHaveText(
      'Please fill in name, email and message before sending.',
    );
    for (const [id, invalid] of [
      ['contact-name', 'true'],
      ['contact-email', 'false'],
      ['contact-message', 'true'],
    ]) {
      await expect(page.locator(`#${id}`)).toHaveAttribute('aria-invalid', invalid);
    }
    await expect(page.locator('#contact-name')).toHaveAttribute(
      'aria-errormessage',
      'contact-status',
    );
    const invalidNodes = await page.evaluate(async () => {
      const ids = ['contact-name', 'contact-message'];
      return ids.map((id) => document.getElementById(id)!.matches('[aria-invalid="true"]'));
    });
    expect(invalidNodes).toEqual([true, true]);
    // Filling the fields clears the flags on the next send attempt.
    await page.locator('#contact-name').fill('A');
    await page.locator('#contact-message').fill('B');
    await page.route('**/ai-readability/api/contact', (r) => r.abort());
    await page.locator('#contact-submit').click();
    await expect(page.locator('#contact-name')).toHaveAttribute('aria-invalid', 'false');
  });

  test('without JavaScript the browser still validates', async ({ page }) => {
    await gotoSettled(page, '/contact/');
    // The attribute is set by the script only; the served markup leaves the
    // browser's own validation on.
    const html = await (await page.request.get('/contact/')).text();
    const openTag = html.match(/<form\b[^>]*id="contact-form"[^>]*>/)?.[0] ?? '';
    expect(openTag).not.toBe('');
    expect(openTag).not.toMatch(/novalidate/i);
    expect(
      await page.evaluate(
        () => (document.getElementById('contact-form') as HTMLFormElement).noValidate,
      ),
    ).toBe(true);
  });
});

test('the Core Web Vitals fix list is a region, not a complementary', async ({ page }) => {
  await gotoSettled(page, '/core-web-vitals/lcp/');
  expect(await axQuery(page, 'main', 'complementary', 'Fix it at the server')).toHaveLength(0);
  expect(await axQuery(page, 'main', 'region', 'Fix it at the server')).toHaveLength(1);
});
