// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { readFileSync } from 'node:fs';
import { test, expect } from '@playwright/test';
import { parse as parseYaml } from 'yaml';

// The current release, read from the manifest every page derives it from.
const MANIFEST_SEMVER: string = parseYaml(
  readFileSync(new URL('../src/content/releases-2.1/release.yaml', import.meta.url), 'utf8'),
).release.semver;

const allPages = [
  '/',
  '/features/',
  '/pricing/',
  '/docs/',
  '/blog/',
  '/demo/',
  '/calculator/',
  '/contact/',
  '/security/',
  '/license/',
  '/terms/',
  '/pagespeed-markers/',
  // Hand-written HTML in public/, outside the layout's head tags. The dev
  // server serves a public/ file only by its full name (the directory URL is
  // its 404 page there), so name the file.
  '/cyclone-benchmark/index.html',
  // /privacy/ points at the notice on we-amp.com; it is covered by its own
  // suite in terms-privacy.spec.ts
];

test.describe('SEO', () => {
  for (const path of allPages) {
    test(`${path} has a <title> tag`, async ({ page }) => {
      await page.goto(path);
      const title = await page.title();
      expect(title.length).toBeGreaterThan(0);
    });
  }

  for (const path of allPages) {
    test(`${path} has a meta description`, async ({ page }) => {
      await page.goto(path);
      const desc = page.locator('meta[name="description"]');
      const content = await desc.getAttribute('content');
      expect(content).toBeTruthy();
      expect(content!.length).toBeGreaterThan(10);
    });
  }

  test('every page has og:title and og:description', async ({ page }) => {
    for (const path of allPages) {
      await page.goto(path);
      const ogTitle = page.locator('meta[property="og:title"]');
      await expect(ogTitle).toHaveAttribute('content', /.+/);
      const ogDesc = page.locator('meta[property="og:description"]');
      await expect(ogDesc).toHaveAttribute('content', /.+/);
    }
  });

  test('every page has canonical URL', async ({ page }) => {
    for (const path of allPages) {
      const response = await page.goto(path);
      expect(response?.status(), path).toBe(200);
      const canonical = page.locator('link[rel="canonical"]');
      const href = await canonical.getAttribute('href');
      expect(href).toContain('modpagespeed.com');
    }
  });

  test('landing page has JSON-LD structured data', async ({ page }) => {
    await page.goto('/');
    // Home emits multiple top-level nodes (Organization + WebSite +
    // SoftwareApplication); assert the first parses as schema.org JSON-LD.
    const jsonLd = page.locator('script[type="application/ld+json"]').first();
    await expect(jsonLd).toBeAttached();
    const content = await jsonLd.textContent();
    const data = JSON.parse(content!);
    expect(data['@context']).toBe('https://schema.org');
  });

  test('pricing page has JSON-LD structured data', async ({ page }) => {
    await page.goto('/pricing/');
    const jsonLd = page.locator('script[type="application/ld+json"]').first();
    await expect(jsonLd).toBeAttached();
  });

  // Product / Merchant Listing JSON-LD. /1.1/ retired (its product page is
  // superseded by /); /download/ is now the sole surface for this check —
  // the Offer lives there.
  const productPages = [
    {
      path: '/download/',
      expectType: (t: unknown) =>
        Array.isArray(t) ? (t as string[]).includes('Product') : t === 'Product',
    },
  ];

  test('Product JSON-LD pages have Merchant Listing fields', async ({ page }) => {
    for (const { path, expectType } of productPages) {
      await test.step(path, async () => {
        await page.goto(path);
        const jsonLd = page.locator('script[type="application/ld+json"]').first();
        await expect(jsonLd).toBeAttached();
        const content = await jsonLd.textContent();
        const data = JSON.parse(content!);
        expect(expectType(data['@type'])).toBe(true);
        expect(typeof data.image).toBe('string');
        expect(data.image).toMatch(/^https?:\/\//);
        expect(data.brand?.name).toBeTruthy();
        // The converged product is open source, so the node is a single $0
        // Offer (accurate and rich-result-valid; a Product node must carry
        // offers). The MerchantReturnPolicy is intentionally omitted from the
        // structured data (see src/data/offer-jsonld.ts) because /terms/ grants
        // no unconditional money-back guarantee.
        expect(data.offers['@type']).toBe('Offer');
        expect(data.offers.price).toBe(0);
        expect(data.offers.priceCurrency).toBe('EUR');
        expect(data.offers.availability).toMatch(/InStock/);
        expect(typeof data.offers.url).toBe('string');
        expect(data.offers.url).toMatch(/^https?:\/\//);
      });
    }
  });

  // Post-convergence: one site name everywhere — the /1.1/*-specific
  // og:site_name flip is retired. The /1.1/ product page itself is retired
  // (superseded by /); /1.1/docs/ (once narrowed to the archived
  // release-notes page) is now retired too, its content folded into
  // /docs/release-notes/. There is no page left to compare og:site_name
  // against, so this asserts the retirement itself rather than dropping
  // the test.
  test('retired /1.1/docs/ page no longer carries its own og:site_name', async ({ page }) => {
    const response = await page.goto('/1.1/docs/release-notes/');
    expect(response?.status()).toBe(404);
  });

  // Post-convergence: one breadcrumb model — the segmentNames override that
  // rendered the '1.1' path segment as "mod_pagespeed 1.15" is retired along
  // with the page it rendered on. Same reasoning as the og:site_name check
  // above: assert the route is gone rather than test breadcrumb JSON-LD on
  // a page that no longer builds.
  test('retired /1.1/docs/ page carries no breadcrumb JSON-LD (route is gone)', async ({
    page,
  }) => {
    const response = await page.goto('/1.1/docs/release-notes/');
    expect(response?.status()).toBe(404);
  });

  // /download/ must actually offer the current release: a bare integration
  // grid with no current row was a past defect. The exact release from the
  // manifest is named once, on the chip above the grid (the download page is
  // where the version is the point), and there is one card per integration.
  // Every card is the current release, so no card carries its own version.
  test('/download/ offers the current release above one card per integration', async ({ page }) => {
    await page.goto('/download/');
    const grid = page.locator('#integrations');
    await expect(grid.locator('p', { hasText: 'see release notes' })).toContainText(
      `v${MANIFEST_SEMVER}`,
    );
    for (const name of ['Apache', 'nginx', 'nginx (Docker)', 'Helm', 'ASP.NET Core Sidecar']) {
      await test.step(name, async () => {
        const card = grid.locator('.card', {
          has: page.getByRole('heading', { level: 3, name, exact: true }),
        });
        await expect(card).toHaveCount(1);
        await expect(card).not.toContainText(`mod_pagespeed ${MANIFEST_SEMVER}`);
        await expect(card).not.toContainText(/mod_pagespeed \d+\.\d+/);
      });
    }
  });

  // One product node everywhere (src/data/software-jsonld.ts): the plain
  // product name with no version (owner ruling, 2026-10-20), the old names
  // as alternateName, and the exact release from the manifest as
  // softwareVersion.
  for (const path of ['/', '/download/', '/pricing/']) {
    test(`${path} product JSON-LD: the product name, the manifest version`, async ({ page }) => {
      await page.goto(path);
      const nodes = (await page.locator('script[type="application/ld+json"]').allTextContents())
        .map((text) => JSON.parse(text))
        .flatMap((data) => (Array.isArray(data['@graph']) ? data['@graph'] : [data]));
      const products = nodes.filter((node) =>
        [node['@type']].flat().includes('SoftwareApplication'),
      );
      expect(products.length).toBeGreaterThan(0);
      for (const node of products) {
        expect(node['@id']).toBe('https://modpagespeed.com/#software');
        expect(node.name).toBe('mod_pagespeed');
        expect(node.softwareVersion).toBe(MANIFEST_SEMVER);
        expect(node.alternateName).toEqual(
          expect.arrayContaining(['ngx_pagespeed', 'ModPageSpeed', 'WeAmp.PageSpeed.AspNetCore']),
        );
        // The 2.0 line is a separate line with its own releases (it stays on
        // the Brand), so it is not an alternate name of this node.
        expect(node.alternateName).not.toContain('ModPageSpeed 2.0');
        expect(node.applicationCategory).toBe('DeveloperApplication');
        expect(node.operatingSystem).toEqual(['Linux', 'Windows', 'macOS']);
        expect(node.publisher['@id']).toBe('https://we-amp.com/#organization');
      }
    });
  }

  test('home Organization brand is the plain product name', async ({ page }) => {
    await page.goto('/');
    const nodes = (await page.locator('script[type="application/ld+json"]').allTextContents()).map(
      (text) => JSON.parse(text),
    );
    const org = nodes.find((node) => node['@type'] === 'Organization');
    expect(org['@id']).toBe('https://we-amp.com/#organization');
    expect(org.brand.name).toBe('mod_pagespeed');
  });

  test('/analyze/ structured data names the tool by the current product name only', async ({
    page,
  }) => {
    await page.goto('/analyze/');
    const blocks = await page.locator('script[type="application/ld+json"]').allTextContents();
    const app = blocks.map((b) => JSON.parse(b)).find((node) => node['@type'] === 'WebApplication');
    expect(app.name).toBe('mod_pagespeed Analyze');
    expect(app).not.toHaveProperty('alternateName');
    expect(blocks.join('\n')).not.toMatch(/ModPageSpeed/);
  });

  test('og:image is set on all pages', async ({ page }) => {
    for (const path of allPages) {
      await page.goto(path);
      const ogImage = page.locator('meta[property="og:image"]');
      const content = await ogImage.getAttribute('content');
      expect(content).toBeTruthy();
    }
  });

  test('twitter:card meta tag is present', async ({ page }) => {
    await page.goto('/');
    const twitterCard = page.locator('meta[name="twitter:card"]');
    await expect(twitterCard).toHaveAttribute('content', 'summary_large_image');
  });

  test('no duplicate title tags', async ({ page }) => {
    await page.goto('/');
    // Scope to the document head: the home page embeds inline architecture SVGs
    // that each carry their own accessible <svg><title>, so a bare `title`
    // locator now also matches those. The SEO invariant is exactly one
    // document <title> in <head>.
    const titles = page.locator('head > title');
    expect(await titles.count()).toBe(1);
  });

  test('blog posts declare og:type article with publish time', async ({ page }) => {
    await page.goto('/blog/memory-mapped-cache-zero-copy-serving/');
    await expect(page.locator('meta[property="og:type"]')).toHaveAttribute('content', 'article');
    const published = await page
      .locator('meta[property="article:published_time"]')
      .getAttribute('content');
    expect(Number.isNaN(Date.parse(published!))).toBe(false);
  });

  test('non-article pages keep og:type website', async ({ page }) => {
    await page.goto('/');
    await expect(page.locator('meta[property="og:type"]')).toHaveAttribute('content', 'website');
    expect(await page.locator('meta[property="article:published_time"]').count()).toBe(0);
  });

  test('favicon link is present', async ({ page }) => {
    await page.goto('/');
    const favicon = page.locator('link[rel="icon"]');
    expect(await favicon.count()).toBeGreaterThan(0);
  });
});
