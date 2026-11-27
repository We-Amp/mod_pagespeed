// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The per-filter topic pages (/docs/filters/<name>/): three representative
// pages render (one with a live example, one compound name, one noindex), the
// group pages keep their anchors and link each section to its topic page, the
// /docs/filters/ table links each row to one, and a related-filters list sends
// a thin filter to its group-page entry.

import { test, expect } from '@playwright/test';
import { loadFilterTopics } from '../src/lib/filter-topics.mjs';

test.describe('Filter topic pages', () => {
  test('a filter with a live example renders its guide', async ({ page }) => {
    await page.goto('/docs/filters/lazyload_images/');
    await expect(page.locator('main h1')).toHaveText('Lazy-load images (lazyload_images)');
    await expect(page.locator('main h1')).toHaveCount(1);
    await expect(page.locator('[data-topic-lede]')).toContainText('below the fold');
    await expect(page.locator('#when-to-use')).toBeVisible();
    await expect(page.locator('#risks')).toBeVisible();
    await expect(page.locator('#configuration')).toBeVisible();
    await expect(page.locator('main')).toContainText('ModPagespeedEnableFilters lazyload_images');
    await expect(page.locator('main')).toContainText('pagespeed EnableFilters lazyload_images;');
    await expect(page.locator('[data-topic-worker] a').first()).toHaveAttribute(
      'href',
      '/docs/worker-configuration/#lazy-load-images',
    );
    await expect(
      page.locator('[data-topic-example] a[href="/examples/lazyload_images/"]'),
    ).toBeVisible();
    await expect(page.locator('[data-topic-faq] dt')).toHaveCount(3);
    await expect(page.locator('meta[name="robots"]')).toHaveCount(0);
  });

  test('a compound name lists the filters it switches on', async ({ page }) => {
    await page.goto('/docs/filters/rewrite_images/');
    await expect(page.locator('main h1')).toHaveText('Optimize images (rewrite_images)');
    const members = page.locator('[data-alias-members] a');
    await expect(members).toHaveCount(13);
    await expect(
      page.locator('[data-alias-members] a[href="/docs/filters/convert_jpeg_to_webp/"]'),
    ).toBeVisible();
    await expect(
      page.locator('[data-topic-example] a[href="/examples/rewrite_images/"]'),
    ).toBeVisible();
  });

  test('a thin page renders and is noindex', async ({ page }) => {
    await page.goto('/docs/filters/split_html/');
    await expect(page.locator('main h1')).toHaveText('Split HTML (split_html)');
    await expect(page.locator('meta[name="robots"]')).toHaveAttribute('content', 'noindex,follow');
    await expect(page.locator('[data-topic-faq] dt').first()).toBeVisible();
  });

  test('group pages keep their anchors and link each section to its topic page', async ({
    page,
  }) => {
    await page.goto('/docs/javascript-filters/#inline_javascript');
    await expect(page.locator('[id="inline_javascript"]')).toHaveCount(1);
    await expect(page.locator('a[href="/docs/filters/inline_javascript/"]')).toHaveText(
      'Full guide →',
    );

    await page.goto('/docs/image-filters/#convert_jpeg_to_webp');
    await expect(page.locator('[id="convert_jpeg_to_webp"]')).toHaveCount(1);
    await expect(page.locator('a[href="/docs/filters/convert_jpeg_to_webp/"]')).toBeVisible();
  });

  test('the related filters list links a thin filter to its group-page entry', async ({ page }) => {
    const topics = loadFilterTopics();
    const byPath = new Map(topics.map((t) => [t.path, t]));
    const byGroup = new Map(topics.map((t) => [t.groupPath, t]));
    // add_base_tag's related list includes thin HTML filters such as add_head.
    await page.goto('/docs/filters/add_base_tag/');
    const hrefs = await page
      .locator('#related ~ ul a')
      .evaluateAll((links) => links.map((a) => a.getAttribute('href') ?? ''));
    expect(hrefs.length).toBeGreaterThan(0);
    let toGroup = 0;
    for (const href of hrefs) {
      const target = byPath.get(href) ?? byGroup.get(href);
      expect(target, href).toBeDefined();
      if (target!.indexable) expect(href).toBe(target!.path);
      else {
        expect(href).toBe(target!.groupPath);
        toGroup++;
      }
    }
    expect(toGroup).toBeGreaterThan(0);
  });

  test('the filters table links rows to topic pages', async ({ page }) => {
    await page.goto('/docs/filters/');
    const link = page.locator('tr#inline_javascript a[data-topic-link]');
    await expect(link).toHaveAttribute('href', '/docs/filters/inline_javascript/');
    await link.click();
    await expect(page).toHaveURL(/\/docs\/filters\/inline_javascript\/$/);
    await expect(page.locator('main h1')).toHaveText('Inline JavaScript (inline_javascript)');
  });

  test('a page lists the other examples that apply its filter', async ({ page }) => {
    await page.goto('/docs/filters/rewrite_css/');
    await expect(
      page.locator('[data-topic-also] a[href="/examples/rewrite_css_extend_cache/"]'),
    ).toBeVisible();
  });

  test('a page lists a demo whose capture shows the filter acting', async ({ page }) => {
    await page.goto('/docs/filters/extend_cache/');
    await expect(page.locator('[data-not-applied]')).toHaveCount(0);
    await expect(
      page.locator('[data-topic-also] a[href="/examples/rewrite_css_extend_cache/"]'),
    ).toBeVisible();
  });

  test('a filter no example isolates links to the examples that apply it', async ({ page }) => {
    await page.goto('/docs/filters/convert_jpeg_to_webp/');
    const example = page.locator('[data-topic-example]');
    await expect(example.locator('h2')).toHaveText('Live examples');
    await expect(example.locator('a[href="/examples/rewrite_images/"]')).toBeVisible();
  });

  test('an example page links to its full guide', async ({ page }) => {
    await page.goto('/examples/combine_css/');
    await expect(page.locator('a[data-full-guide]')).toHaveAttribute(
      'href',
      '/docs/filters/combine_css/',
    );
  });

  // The Markdown twins (src/pages/docs/filters/[name].md.ts): an indexable
  // page advertises its twin and the server serves it; a thin page does
  // neither.
  test('an indexable page advertises its Markdown twin and the server serves it', async ({
    page,
  }) => {
    await page.goto('/docs/filters/lazyload_images/');
    const alternate = page.locator('head link[rel="alternate"][type="text/markdown"]');
    await expect(alternate).toHaveCount(1);
    await expect(alternate).toHaveAttribute('href', '/docs/filters/lazyload_images.md');
    const res = await page.request.get((await alternate.getAttribute('href'))!);
    expect(res.status()).toBe(200);
    expect(res.headers()['content-type']).toMatch(/^text\/markdown/);
    const body = await res.text();
    expect(body.startsWith('# Lazy-load images (lazyload_images)\n')).toBe(true);
    expect(body).toContain('Canonical URL: https://modpagespeed.com/docs/filters/lazyload_images/');
    expect(body).toContain('## Frequently asked questions');
    expect(body).not.toContain('](/');
    expect((await page.request.get('/docs/filters/lazyload_images.md/')).status()).toBe(404);
  });

  // The demos a page links in its live example section are the ones its
  // Markdown twin links, in the same order, on every indexable page.
  test('every Markdown twin lists the same example demos as its page', async ({
    page,
    request,
  }, testInfo) => {
    test.skip(testInfo.project.name !== 'desktop', 'server output; one project is enough');
    await page.route('**/*', (route) => route.abort());
    const names = loadFilterTopics()
      .filter((t) => t.indexable)
      .map((t) => t.name);
    expect(names.length).toBeGreaterThan(30);
    let withDemos = 0;
    for (const name of names) {
      const html = await (await request.get(`/docs/filters/${name}/`)).text();
      await page.setContent(html, { waitUntil: 'domcontentloaded' });
      const fromPage = await page
        .locator('[data-topic-example] a[href^="/examples/"]')
        .evaluateAll((as) => as.map((a) => a.getAttribute('href')!.split('/')[2]));
      const md = await (await request.get(`/docs/filters/${name}.md`)).text();
      const section = md.match(/\n## Live examples?\n[\s\S]*?(?=\n## )/)?.[0] ?? '';
      const fromTwin = [
        ...section.matchAll(/\]\(https:\/\/modpagespeed\.com\/examples\/([^/)]+)\/\)/g),
      ].map((m) => m[1]);
      expect(fromTwin, name).toEqual(fromPage);
      if (fromPage.length) withDemos++;
    }
    expect(withDemos).toBeGreaterThan(0);
  });

  test('a thin page has no Markdown twin', async ({ page, request }) => {
    await page.goto('/docs/filters/split_html/');
    await expect(page.locator('head link[rel="alternate"][type="text/markdown"]')).toHaveCount(0);
    expect((await request.get('/docs/filters/split_html.md')).status()).toBe(404);
  });

  test('the filters table has a Markdown twin', async ({ page, request }) => {
    await page.goto('/docs/filters/');
    const alternate = page.locator('head link[rel="alternate"][type="text/markdown"]');
    await expect(alternate).toHaveAttribute('href', '/docs/filters.md');
    const res = await request.get('/docs/filters.md');
    expect(res.status()).toBe(200);
    expect(res.headers()['content-type']).toMatch(/^text\/markdown/);
    const body = await res.text();
    expect(body.startsWith('# PageSpeed filters\n')).toBe(true);
    expect(body).toContain('(https://modpagespeed.com/docs/filters/lazyload_images/)');
  });
});
