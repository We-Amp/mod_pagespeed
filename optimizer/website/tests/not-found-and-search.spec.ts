// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect, type Page } from '@playwright/test';
import { stubUmami, trackedEvents } from './helpers/umami';

// The dev server has no search index, so these tests serve a stand-in for
// Pagefind's UI bundle: the same input and message classes, "No results for …"
// for a query containing "zz" and three result links for anything else. What
// is under test is the site's own code around the bundle: the hidden status
// region, the link row under an empty search, the events and the pre-filled
// query from the 404 page.
async function stubPagefind(page: Page): Promise<void> {
  await page.route('**/pagefind/*/pagefind-ui.css', (route) =>
    route.fulfill({ contentType: 'text/css', body: '' }),
  );
  await page.route('**/pagefind/*/pagefind-ui.js', (route) =>
    route.fulfill({
      contentType: 'text/javascript',
      body: `
        window.PagefindUI = function (options) {
          window.__pagefindOptions = options;
          var root = document.querySelector(options.element);
          var input = document.createElement('input');
          input.className = 'pagefind-ui__search-input';
          input.setAttribute('aria-label', 'Search');
          var drawer = document.createElement('div');
          root.appendChild(input);
          root.appendChild(drawer);
          input.addEventListener('input', function () {
            var term = input.value;
            drawer.textContent = '';
            if (!term) return;
            var message = document.createElement('p');
            message.className = 'pagefind-ui__message';
            drawer.appendChild(message);
            if (term.indexOf('zz') !== -1) {
              message.textContent = options.translations.zero_results.replace('[SEARCH_TERM]', term);
              return;
            }
            message.textContent = '3 results for ' + term;
            for (var i = 0; i < 3; i++) {
              var result = document.createElement('div');
              result.className = 'pagefind-ui__result';
              var link = document.createElement('a');
              link.className = 'pagefind-ui__result-link';
              link.href = '#result-' + i;
              link.textContent = 'Result ' + i;
              result.appendChild(link);
              drawer.appendChild(result);
            }
          });
        };`,
    }),
  );
}

const input = (page: Page) => page.locator('#site-search input.pagefind-ui__search-input');

test.describe('site search: status, empty state and events', () => {
  test.beforeEach(async ({ page }) => {
    await stubUmami(page);
    await stubPagefind(page);
    await page.goto('/pricing/');
    await page.keyboard.press('/');
    await expect(input(page)).toBeFocused();
  });

  test('the field asks for the search keyboard, results are taller rows on a phone', async ({
    page,
  }) => {
    await expect(input(page)).toHaveAttribute('inputmode', 'search');
    await expect(input(page)).toHaveAttribute('autocomplete', 'off');
    await page.setViewportSize({ width: 360, height: 800 });
    await input(page).pressSequentially('image');
    const heights = await page
      .locator('#site-search .pagefind-ui__result-link')
      .evaluateAll((els) => els.map((el) => Math.round(el.getBoundingClientRect().height)));
    expect(heights.length).toBeGreaterThan(0);
    expect(heights.filter((h) => h < 28)).toEqual([]);
  });

  test('the result count reaches a status region that is always in the tree', async ({ page }) => {
    const count = page.locator('#site-search-count');
    await expect(count).toHaveAttribute('role', 'status');
    await expect(count).not.toHaveAttribute('hidden', /.*/);
    await expect(count).toHaveText('');
    // Visually hidden, not display:none (a hidden live region is not announced).
    expect(await count.evaluate((el) => getComputedStyle(el).display)).not.toBe('none');
    const box = await count.boundingBox();
    expect(box?.width ?? 0).toBeLessThanOrEqual(1);

    await input(page).pressSequentially('image');
    await expect(count).toHaveText('3 results for image');
    // The visible loading / unavailable line stays out of it.
    await expect(page.locator('#site-search-status')).toBeHidden();
  });

  test('the count is announced once typing pauses, not on every keystroke', async ({ page }) => {
    await page.evaluate(() => {
      const seen: string[] = [];
      (window as unknown as { __announced: string[] }).__announced = seen;
      const count = document.getElementById('site-search-count')!;
      new MutationObserver(() => seen.push(count.textContent ?? '')).observe(count, {
        childList: true,
        characterData: true,
        subtree: true,
      });
    });
    await input(page).pressSequentially('filters', { delay: 30 });
    await expect(page.locator('#site-search-count')).toHaveText('3 results for filters');
    const announced = await page.evaluate(
      () => (window as unknown as { __announced: string[] }).__announced,
    );
    expect(announced).toEqual(['3 results for filters']);
  });

  test('an empty search shows the fixed link row and reports it without the query', async ({
    page,
  }) => {
    const row = page.locator('#site-search-empty');
    await expect(row).toBeHidden();

    await input(page).fill('zzqqxx');
    await expect(row).toBeVisible();
    await expect(row.locator('p')).toHaveText('Try a directive or filter name, or start here:');
    const links = await row
      .locator('a')
      .evaluateAll((as) => as.map((a) => [a.textContent?.trim(), a.getAttribute('href')]));
    expect(links).toEqual([
      ['Install', '/docs/getting-started/'],
      ['Configuration reference', '/docs/configuration/'],
      ['Filter reference', '/docs/filters/'],
      ['Download', '/download/'],
      ['Pricing', '/pricing/'],
      ['Contact', '/contact/'],
    ]);
    await expect(page.locator('#site-search-count')).toHaveText('No results for zzqqxx');
    await expect
      .poll(async () => (await trackedEvents(page)).filter((e) => e.name === 'search_no_results'))
      .toHaveLength(1);
    const events = await trackedEvents(page);
    expect(JSON.stringify(events)).not.toContain('zzqqxx');

    // A search that finds something takes the row away again.
    await input(page).fill('image');
    await expect(row).toBeHidden();
    await input(page).fill('');
    await expect(row).toBeHidden();
    await expect(page.locator('#site-search-count')).toHaveText('');
  });

  test('the link row is reachable by keyboard and Tab stays in the dialog', async ({ page }) => {
    await input(page).fill('zzqqxx');
    await expect(page.locator('#site-search-empty')).toBeVisible();
    await page.keyboard.press('Tab');
    await expect(page.locator('#site-search-empty a').first()).toBeFocused();
    for (let i = 0; i < 12; i++) {
      await page.keyboard.press('Tab');
      const inside = await page.evaluate(
        () =>
          !!document.activeElement?.closest('#site-search') ||
          document.activeElement === document.body,
      );
      expect(inside).toBe(true);
    }
  });

  test('following a result is reported', async ({ page }) => {
    await input(page).fill('image');
    await page.locator('#site-search .pagefind-ui__result a').nth(1).click();
    const events = await trackedEvents(page);
    expect(events.filter((e) => e.name === 'search_result_click')).toHaveLength(1);
    expect(events.filter((e) => e.name === 'search_no_results')).toHaveLength(0);
  });

  test('the bundle loads from a versioned directory, never bare /pagefind/', async ({ page }) => {
    // The bundle's file names are the same in every Pagefind release and the
    // web server lets browsers keep script and style files for a long time, so
    // the directory must name the version for a returning visitor to get the
    // new bundle.
    const bundlePath = await page.evaluate(
      () =>
        (window as unknown as { __pagefindOptions: { bundlePath: string } }).__pagefindOptions
          .bundlePath,
    );
    expect(bundlePath).toMatch(/^\/pagefind\/v\d+\.\d+\.\d+[^/]*\/$/);
    const loaded = await page.evaluate(() => ({
      scripts: [...document.querySelectorAll('script[src*="/pagefind/"]')].map((s) =>
        s.getAttribute('src'),
      ),
      styles: [...document.querySelectorAll('link[href*="/pagefind/"]')].map((l) =>
        l.getAttribute('href'),
      ),
    }));
    expect(loaded.scripts).toEqual([`${bundlePath}pagefind-ui.js`]);
    expect(loaded.styles).toEqual([`${bundlePath}pagefind-ui.css`]);
  });

  test('ranking options are passed to Pagefind at search time', async ({ page }) => {
    const ranking = await page.evaluate(
      () =>
        (window as unknown as { __pagefindOptions: { ranking: Record<string, number> } })
          .__pagefindOptions.ranking,
    );
    expect(ranking.termFrequency).toBeGreaterThanOrEqual(0);
    expect(ranking.termFrequency).toBeLessThan(1);
    expect(ranking.pageLength).toBeGreaterThanOrEqual(0);
    expect(ranking.pageLength).toBeLessThan(0.75);
  });
});

test.describe('search index hints on the page', () => {
  test('a named page carries its weight and short result title', async ({ page }) => {
    await page.goto('/pricing/');
    const main = page.locator('main#main-content');
    await expect(main).toHaveAttribute('data-pagefind-body', '');
    await expect(main).toHaveAttribute('data-pagefind-meta', 'section:Site');
    expect(Number(await main.getAttribute('data-pagefind-weight'))).toBeGreaterThan(1);
    const title = main.locator('[data-pagefind-meta^="title:"]');
    await expect(title).toHaveAttribute('data-pagefind-meta', 'title:Pricing');
    await expect(title).toBeHidden();
  });

  test('a blog page keeps the defaults; a docs page is weighted', async ({ page }) => {
    await page.goto('/blog/');
    const main = page.locator('main#main-content');
    await expect(main).not.toHaveAttribute('data-pagefind-weight', /.*/);
    await expect(main.locator('[data-pagefind-meta^="title:"]')).toHaveCount(0);
    await page.goto('/docs/configuration/');
    expect(Number(await main.getAttribute('data-pagefind-weight'))).toBeGreaterThan(1);
    await expect(main.locator('[data-pagefind-meta^="title:"]')).toHaveCount(0);
  });
});

test.describe('404 page', () => {
  test('says what happened and offers search and the common destinations', async ({ page }) => {
    await stubUmami(page);
    const response = await page.goto('/docs/image-filtres/');
    expect(response?.status()).toBe(404);
    await expect(page.getByRole('heading', { level: 1 })).toHaveText('Page not found');
    await expect(page.locator('main')).toContainText(
      'This page does not exist. Search the site, or start from one of these.',
    );
    await expect(page.locator('main a.btn-primary')).toHaveText('Go home');

    const search = page.locator('main button[data-search-open]');
    await expect(search).toHaveText('Search the site');
    await expect(search).toHaveAttribute('data-umami-event', 'error_404_search');
    await expect(search).toHaveAttribute('aria-haspopup', 'dialog');

    const links = page.locator('main a[data-umami-event="error_404_link"]');
    expect(
      await links.evaluateAll((as) =>
        as.map((a) => [a.textContent?.trim(), a.getAttribute('href')]),
      ),
    ).toEqual([
      ['Install', '/docs/getting-started/'],
      ['Configuration reference', '/docs/configuration/'],
      ['Filter reference', '/docs/filters/'],
      ['Download', '/download/'],
      ['Support', '/support/'],
    ]);
    // Every destination exists.
    for (const href of await links.evaluateAll((as) => as.map((a) => a.getAttribute('href')))) {
      const res = await page.request.get(href!);
      expect(res.status(), href!).toBe(200);
    }
  });

  test('the request readout has no copy button and no empty head row', async ({ page }) => {
    await page.goto('/no-such-page/');
    const readout = page.locator('[data-error-readout]');
    await expect(readout.locator('.term-body')).toContainText('GET /no-such-page/');
    await expect(readout.locator('.term-body')).toContainText('404');
    // The copy-button script has run by now on pages that have code blocks.
    await page.waitForLoadState('load');
    await expect(readout.locator('.code-copy-btn')).toHaveCount(0);
    await expect(readout.locator('.term-head')).toHaveCount(0);
  });

  test('the search button opens the dialog with the missing page as the query', async ({
    page,
  }) => {
    await stubUmami(page);
    await stubPagefind(page);
    await page.goto('/docs/image-filtres.html');
    const search = page.locator('main button[data-search-open]');
    await search.click();
    await expect(page.locator('dialog#site-search')).toBeVisible();
    await expect(input(page)).toHaveValue('image filtres');
    await expect(input(page)).toBeFocused();
    await expect(page.locator('#site-search-count')).toHaveText('3 results for image filtres');

    await page.keyboard.press('Escape');
    await expect(page.locator('dialog#site-search')).toBeHidden();
    await expect(search).toBeFocused();
  });

  test('a path with markup in it is only ever typed into the input', async ({ page }) => {
    await stubPagefind(page);
    await page.goto('/%3Cimg%20src%3Dx%20onerror%3Dwindow.__x%3D1%3E');
    await page.locator('main button[data-search-open]').click();
    await expect(input(page)).toHaveValue('<img src=x onerror=window x=1>');
    expect(await page.evaluate(() => (window as unknown as { __x?: number }).__x)).toBeUndefined();
    await expect(page.locator('#site-search img')).toHaveCount(0);
  });

  // The prerendered copy's own URL is not a page anyone asked for: opening the
  // search from it must not search for "404".
  test('the prerendered 404 copy opens the search with an empty input', async ({ page }) => {
    await stubUmami(page);
    await stubPagefind(page);
    await page.goto('/error/404/');
    await page.locator('main button[data-search-open]').click();
    await expect(page.locator('dialog#site-search')).toBeVisible();
    await expect(input(page)).toBeFocused();
    await expect(input(page)).toHaveValue('');
    // Longer than the announcement delay: nothing was searched or reported.
    await page.waitForTimeout(1000);
    await expect(page.locator('#site-search-count')).toHaveText('');
    await expect(page.locator('#site-search .pagefind-ui__message')).toHaveCount(0);
    expect((await trackedEvents(page)).filter((e) => e.name.startsWith('search_'))).toEqual([]);
  });

  test('the header search opens empty on a 404', async ({ page }) => {
    await stubPagefind(page);
    await page.goto('/no-such-page/');
    await page.locator('header button[data-search-open]:has-text("Search")').click();
    await expect(input(page)).toHaveValue('');
  });

  test('the 500 page keeps its single onward link and its copy button', async ({ page }) => {
    await page.goto('/error/500/');
    await expect(page.locator('main button[data-search-open]')).toHaveCount(0);
    await expect(page.locator('main a[href="/contact/"]')).toContainText('Report an issue');
    await expect(page.locator('[data-error-readout] .term-head')).toHaveCount(1);
  });
});
