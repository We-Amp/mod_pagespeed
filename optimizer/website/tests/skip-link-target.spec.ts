// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The skip link moves keyboard focus past the navigation. On docs pages the
// <main> element opens with the docs sidebar, so the link there goes to the
// content column instead: the next Tab after it is inside the article, not
// the first sidebar entry. Focus really moves to the target (it can take
// focus), which older screen reader and browser pairs need. The link keeps
// its name in Chromium's accessibility tree, and search sub-results no longer
// read their drawn marker glyph.

import { test, expect, type Page } from '@playwright/test';
import { axQuery, gotoSettled } from './helpers/ax-tree';

async function skip(page: Page, path: string) {
  await gotoSettled(page, path);
  await page.keyboard.press('Tab');
  await expect(page.locator(':focus')).toHaveAttribute('data-skip-link', '');
  const links = await axQuery(page, 'a[data-skip-link]', 'link');
  expect(links).toEqual([{ role: 'link', name: 'Skip to main content' }]);
  await page.keyboard.press('Enter');
}

const activeId = (page: Page) => page.evaluate(() => document.activeElement?.id ?? '');

test.describe('skip link target', () => {
  test('on the home page it focuses main', async ({ page }) => {
    await skip(page, '/');
    await expect.poll(() => activeId(page)).toBe('main-content');
  });

  for (const path of [
    '/docs/getting-started/',
    '/docs/configuration/',
    '/docs/filters/',
    '/docs/filters/inline_css/',
  ]) {
    test(`on ${path} it lands past the docs sidebar`, async ({ page }) => {
      await page.setViewportSize({ width: 1440, height: 900 });
      await skip(page, path);
      await expect.poll(() => activeId(page)).toBe('doc-content');
      // The focused column draws no ring of its own.
      const outline = await page
        .locator('#doc-content')
        .evaluate((el) => getComputedStyle(el).outlineStyle);
      expect(outline).toBe('none');
      await page.keyboard.press('Tab');
      const where = await page.evaluate(() => {
        const a = document.activeElement;
        return {
          inContent: !!a?.closest('#doc-content'),
          inSidebar: !!a?.closest('nav[aria-label="Documentation"]'),
        };
      });
      expect(where).toEqual({ inContent: true, inSidebar: false });
    });
  }
});

// The dev server has no search index, so Pagefind's UI is stood in for: one
// result with one sub-result, styled by Pagefind's own marker rule.
async function stubPagefind(page: Page): Promise<void> {
  await page.route('**/pagefind/*/pagefind-ui.css', (route) =>
    route.fulfill({
      contentType: 'text/css',
      body: '.pagefind-ui__result-nested .pagefind-ui__result-link:before{content:"\\2937  "}',
    }),
  );
  await page.route('**/pagefind/*/pagefind-ui.js', (route) =>
    route.fulfill({
      contentType: 'text/javascript',
      body: `
        window.PagefindUI = function (options) {
          var root = document.querySelector(options.element);
          var input = document.createElement('input');
          input.className = 'pagefind-ui__search-input';
          input.setAttribute('aria-label', 'Search');
          var drawer = document.createElement('div');
          root.appendChild(input);
          root.appendChild(drawer);
          input.addEventListener('input', function () {
            drawer.innerHTML =
              '<div class="pagefind-ui__result"><a class="pagefind-ui__result-link" href="#a">Cache extender</a>' +
              '<div class="pagefind-ui__result-nested"><a class="pagefind-ui__result-link" href="#b">How it works</a></div></div>';
          });
        };`,
    }),
  );
}

test('search sub-results are named without the marker glyph', async ({ page }) => {
  await stubPagefind(page);
  await page.goto('/pricing/');
  await page.keyboard.press('/');
  const input = page.locator('#site-search input.pagefind-ui__search-input');
  await expect(input).toBeFocused();
  await input.fill('cache');
  const sub = page.locator('#site-search .pagefind-ui__result-nested a');
  await expect(sub).toBeVisible();
  // The glyph is still drawn.
  const drawn = await sub.evaluate((el) => getComputedStyle(el, '::before').content);
  expect(drawn).toContain('⤷');
  const links = await axQuery(page, '#site-search', 'link');
  expect(links.map((l) => l.name)).toEqual(['Cache extender', 'How it works']);
});
