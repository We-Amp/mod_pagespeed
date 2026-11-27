// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The footer's link groups form a named navigation landmark, each group title
// is a heading, and the newsletter form is a named form, so a screen reader
// user can jump to the footer links, to a group, or to the signup. Checked
// against Chromium's accessibility tree.

import { test, expect } from '@playwright/test';
import { axQuery, gotoSettled } from './helpers/ax-tree';
import { footerGroups } from '../src/data/nav';

const POST = '/blog/fix-lcp-nginx-2026/';

for (const path of ['/', '/docs/getting-started/', '/blog/', POST]) {
  test(`${path}: footer links are a named navigation with group headings`, async ({ page }) => {
    await gotoSettled(page, path);

    // One navigation in the footer, and its name is used by no other.
    const footerNavs = await axQuery(page, 'footer[data-site-footer]', 'navigation');
    expect(footerNavs.map((n) => n.name)).toEqual(['Footer']);
    const allNavs = (await axQuery(page, 'body', 'navigation')).map((n) => n.name);
    expect(allNavs.filter((n) => n === 'Footer')).toHaveLength(1);
    expect(allNavs.filter((n) => n === '')).toEqual([]);

    // Each group title is a heading inside that navigation, in order. Chromium
    // reports the CSS upper-casing in the name, so compare without case.
    const headings = await axQuery(page, 'footer[data-site-footer] nav', 'heading');
    expect(headings.map((h) => h.name.toLowerCase())).toEqual(
      footerGroups.map((g) => g.title.toLowerCase()),
    );
    // Every footer heading is an h2: the footer follows the page's own h1.
    const levels = await page
      .locator('footer[data-site-footer] :is(h1, h2, h3, h4, h5, h6)')
      .evaluateAll((els) => els.map((el) => el.tagName));
    expect(new Set(levels)).toEqual(new Set(['H2']));

    // Every group's links are still inside the landmark.
    const navLinks = await axQuery(page, 'footer[data-site-footer] nav', 'link');
    // (The landmark also holds the GitHub, RSS and Sponsor links beside the logo.)
    const groupLinks = footerGroups.reduce((n, g) => n + g.links.length, 0);
    expect(navLinks.length).toBe(groupLinks + 3);

    // The newsletter signup is a named form.
    const forms = await axQuery(page, 'footer[data-site-footer]', 'form');
    expect(forms.map((f) => f.name)).toEqual(['Newsletter']);
  });
}

test('the footer groups keep their grid layout', async ({ page }) => {
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto('/');
  const nav = page.locator('footer[data-site-footer] nav[aria-label="Footer"]');
  // The groups sit side by side in one row at this width, as before.
  const tops = await nav
    .locator(':scope > div.grid > div')
    .evaluateAll((els) => els.slice(0, 3).map((el) => Math.round(el.getBoundingClientRect().top)));
  expect(new Set(tops).size).toBe(1);
});

test('a blog post names its two signup forms apart', async ({ page }) => {
  await gotoSettled(page, POST);
  const forms = (await axQuery(page, 'body', 'form')).map((f) => f.name);
  expect(forms).toContain('Newsletter');
  expect(forms).toContain('Like this kind of writeup?');
  expect(new Set(forms).size).toBe(forms.length);
  expect(forms.filter((n) => n === '')).toEqual([]);
});
