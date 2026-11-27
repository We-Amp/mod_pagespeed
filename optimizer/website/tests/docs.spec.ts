// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

const docSlugs = [
  'getting-started',
  'installation-docker',
  'installation-module',
  'migrating-to-2-1',
  'configuration',
  'deployment',
  'api-reference',
  'troubleshooting',
  'aspnet-getting-started',
  'aspnet-configuration',
  'browser-analysis',
  'cache-control',
  'helm-deployment',
  'http-api',
  'license',
  'workbench',
  // Converged from the legacy docs-1.1 collection into the main docs tree.
  'css-filters',
  'html-filters',
  'image-filters',
  'javascript-filters',
  'filter-selection',
  'iis-configuration',
  'https-configuration',
  'domain-configuration',
  'admin-console',
  'security',
  'worker-configuration',
  // The structure tranche: Start here, Install, Upgrade and migrate, Operate.
  'is-it-working',
  'install-iis',
  'install-envoy',
  'upgrade',
  'migrate-from-1-15',
  'migrate-from-ngx-pagespeed',
  'migrate-from-iispeed',
  'migrate-from-google-mod-pagespeed',
  'monitoring',
  'uninstall',
  'production-deployment',
  'cpanel',
  'release-notes',
];

// The sidebar and hub groups, in the order the audiences read them.
const GROUP_ORDER = [
  'Start here',
  'Install',
  'Upgrade and migrate',
  'Configure',
  'Filters',
  'Operate',
  'Agents',
  'Reference',
  'Release notes by line',
];

const NEW_PAGES = [
  'is-it-working',
  'install-iis',
  'install-envoy',
  'upgrade',
  'migrate-from-1-15',
  'migrate-from-ngx-pagespeed',
  'migrate-from-iispeed',
  'migrate-from-google-mod-pagespeed',
  'monitoring',
  'uninstall',
];

// Platform-tabbed pages among the converged set (see docSlugs above) — pages
// whose content has nginx/Apache/IIS blocks and so render the platform
// selector.
const platformTabbedSlugs = [
  'filter-selection',
  'https-configuration',
  'domain-configuration',
  'admin-console',
];

test.describe('Docs', () => {
  test('docs index renders', async ({ page }) => {
    await page.goto('/docs/');
    await expect(page).toHaveTitle(/Doc/i);
    await expect(page.locator('main h1').first()).toBeVisible();
  });

  test('docs index links to all doc pages', async ({ page }) => {
    await page.goto('/docs/');
    for (const slug of docSlugs) {
      await expect(page.locator(`a[href="/docs/${slug}/"]`).first()).toBeAttached();
    }
  });

  for (const slug of docSlugs) {
    test(`doc page "${slug}" renders`, async ({ page }) => {
      await page.goto(`/docs/${slug}/`);
      const h1 = page.locator('h1').first();
      await expect(h1).toBeVisible();
      const title = await h1.textContent();
      expect(title!.trim().length).toBeGreaterThan(0);
    });
  }

  test('doc page has sidebar navigation', async ({ page }) => {
    await page.goto('/docs/getting-started/');
    // Desktop sidebar or the mobile "Docs menu" disclosure should exist
    const sidebar = page.locator('nav[aria-label="Documentation"]');
    const mobileNav = page.locator('#docs-menu');
    const hasSidebar = (await sidebar.count()) > 0;
    const hasMobileNav = (await mobileNav.count()) > 0;
    expect(hasSidebar || hasMobileNav).toBeTruthy();
  });

  // Post-convergence: the docs tree is single-line, so neither the desktop
  // sidebar nor the mobile selector renders a "2.0 | 1.1" version toggle any
  // more, and there is exactly one doc-tree sidebar on the page (not one per
  // version).
  test('docs page renders no version toggle, desktop or mobile', async ({ page }) => {
    await page.goto('/docs/getting-started/');
    await expect(page.locator('[role="group"][aria-label="Documentation version"]')).toHaveCount(0);
    await expect(page.locator('nav[aria-label="Documentation"]')).toHaveCount(1);
  });

  // The /1.1/docs/ archive route (formerly the sole surviving docs-1.1 page)
  // is retired now that its release notes are folded into the converged
  // /docs/release-notes/ page. Assert the old route is actually gone rather
  // than dropping this test outright — there is no page left to check a
  // version toggle on.
  test('retired /1.1/docs/release-notes/ route is gone', async ({ page }) => {
    const response = await page.goto('/1.1/docs/release-notes/');
    expect(response?.status()).toBe(404);
  });

  test('doc pages have correct titles', async ({ page }) => {
    await page.goto('/docs/getting-started/');
    await expect(page).toHaveTitle(/Install mod_pagespeed:/i);
  });

  // v2.0.14 trial-experience fix (docs/fix-trial-quickstart-and-faq):
  // the ASP.NET getting-started page must keep documenting the worker
  // SocketPath default and the content-negotiation (Vary) model, so the
  // "Dashboard shows zeros" misconfiguration stays explained.
  test('aspnet getting-started covers SocketPath and content negotiation', async ({ page }) => {
    await page.goto('/docs/aspnet-getting-started/');
    const body = page.locator('main');
    await expect(body).toContainText('SocketPath');
    await expect(body).toContainText('Vary');
    // The "zeros" troubleshooting heading must be present and linkable.
    await expect(page.locator('#my-dashboard-shows-zeros-and-nothing-is-moving')).toBeAttached();
  });

  // The pages converged from the legacy docs-1.1 collection: each one must
  // show up in the main docs sidebar navigation, not just the hub index.
  test('converged pages appear in the docs sidebar navigation', async ({ page }) => {
    await page.goto('/docs/getting-started/');
    const sidebar = page.locator('nav[aria-label="Documentation"]');
    for (const slug of ['css-filters', 'filter-selection', 'admin-console', 'iis-configuration']) {
      await expect(sidebar.locator(`a[href="/docs/${slug}/"]`)).toBeAttached();
    }
  });

  // Platform-tabbed pages converged into the main docs tree must get the same
  // working nginx/Apache/IIS switcher the legacy /1.1/docs/ route has, with
  // the choice persisted across page loads via the shared docs-platform key.
  for (const slug of platformTabbedSlugs) {
    test(`platform tabs on /docs/${slug}/ switch platform and persist the choice`, async ({
      page,
    }) => {
      await page.goto(`/docs/${slug}/`);
      const apacheTab = page.locator('[data-platform-tab="apache"]');
      await expect(apacheTab).toBeAttached();
      await expect(apacheTab).toHaveAttribute('aria-pressed', 'false');

      await apacheTab.click();
      await expect(apacheTab).toHaveAttribute('aria-pressed', 'true');
      await expect(page.locator('html')).toHaveAttribute('data-active-platform', 'apache');

      // Reload: the choice must persist via the docs-platform localStorage key.
      await page.reload();
      await expect(page.locator('html')).toHaveAttribute('data-active-platform', 'apache');
      await expect(page.locator('[data-platform-tab="apache"]')).toHaveAttribute(
        'aria-pressed',
        'true',
      );
    });
  }

  // Non-platform-tabbed converged pages must render exactly as before: no
  // platform selector at all.
  test('non-platform-tabbed converged pages render no platform selector', async ({ page }) => {
    await page.goto('/docs/css-filters/');
    await expect(page.locator('#platform-selector')).toHaveCount(0);
  });

  // Storage-blocked visitors (Safari private mode, strict cookie/storage
  // settings): every localStorage access for the docs-platform key must be
  // wrapped, in both the sitewide pre-paint script and the docs page script,
  // so a throwing storage backend never leaves data-active-platform unset
  // (which would hide every platform-specific content block, per
  // global.css's default-to-hidden rule) and never leaves the tabs inert.
  test('platform selector still works when storage access throws', async ({ page }) => {
    const pageErrors: Error[] = [];
    page.on('pageerror', (err) => pageErrors.push(err));

    // The dev-only Astro toolbar (astro/dist/runtime/client/dev-toolbar) has
    // its own unwrapped localStorage read for its settings, unrelated to this
    // site's code and absent from a production build — block it so this test
    // isolates our own scripts' behavior instead of a dev-tooling artifact.
    await page.route('**/dev-toolbar/**', (route) => route.abort());

    // Scoped to `localStorage` specifically (not Storage.prototype, which
    // sessionStorage also inherits from and an unrelated component on this
    // page uses) — this mirrors real Safari ITP / strict-settings behavior,
    // where merely reading the `localStorage` property throws.
    await page.addInitScript(() => {
      Object.defineProperty(window, 'localStorage', {
        configurable: true,
        get() {
          throw new DOMException('Storage access is blocked.', 'SecurityError');
        },
      });
    });

    await page.goto('/docs/filter-selection/');

    // Default platform content is visible even though storage never
    // resolved — data-active-platform must fall back to a deterministic
    // default rather than staying unset.
    await expect(page.locator('html')).toHaveAttribute('data-active-platform', 'nginx');
    await expect(page.locator('[data-platform-tab="nginx"]')).toHaveAttribute(
      'aria-pressed',
      'true',
    );
    await expect(page.locator('[data-platform="nginx"]').first()).toBeVisible();

    // Clicking another tab still switches the visible content — the click
    // listeners must have attached despite storage throwing during init.
    const apacheTab = page.locator('[data-platform-tab="apache"]');
    await apacheTab.click();
    await expect(page.locator('html')).toHaveAttribute('data-active-platform', 'apache');
    await expect(apacheTab).toHaveAttribute('aria-pressed', 'true');
    await expect(page.locator('[data-platform="apache"]').first()).toBeVisible();

    expect(pageErrors).toEqual([]);
  });
});

// Docs page mechanics: search entry point, previous/next, "Last updated",
// the edit link, copy buttons on code blocks, the mobile disclosure menu and
// the hub's section headings.
test.describe('Docs mechanics', () => {
  const editUrlPattern =
    /^https:\/\/github\.com\/We-Amp\/pagespeed-optimizer\/edit\/main\/website\/src\/content\/docs\/[a-z0-9-]+\.mdx?$/;

  test('docs hub has a search box that opens the search dialog', async ({ page }) => {
    await page.goto('/docs/');
    const box = page.locator('main button[data-search-open]');
    await expect(box).toBeVisible();
    await expect(box).toContainText('Search the docs');
    await box.click();
    const dialog = page.locator('dialog#site-search');
    await expect(dialog).toBeVisible();
    await expect(dialog).toHaveAttribute('open', '');
    await page.keyboard.press('Escape');
    await expect(dialog).toBeHidden();
  });

  test('docs hub has exactly one "Reference" heading and a popular strip', async ({ page }) => {
    await page.goto('/docs/');
    const reference = page.locator('main h2', { hasText: /^\s*Reference\s*$/ });
    await expect(reference).toHaveCount(1);
    // The filters reference page sits inside that one Reference section.
    await expect(page.locator('main a[href="/docs/filters/"]')).toHaveCount(2); // popular + card
    const popular = page.locator('nav[aria-label="Popular pages"] a');
    await expect(popular).toHaveCount(5);
    await expect(popular.first()).toHaveAttribute('href', '/docs/getting-started/');
  });

  test('release notes have one destination from the docs sidebar', async ({ page }) => {
    await page.goto('/docs/getting-started/');
    const sidebar = page.locator('nav[aria-label="Documentation"]');
    await expect(sidebar.locator('a[href="/docs/release-notes/"]')).toHaveCount(1);
    await expect(sidebar.locator('a[href="/blog/"]')).toHaveCount(0);
  });

  test('a middle doc has previous and next links that follow the sidebar order', async ({
    page,
  }) => {
    await page.goto('/docs/installation-module/');
    const order = await page
      .locator('nav[aria-label="Documentation"] a[href^="/docs/"]')
      .evaluateAll((links) => links.map((a) => a.getAttribute('href')));
    const here = order.indexOf('/docs/installation-module/');
    expect(here).toBeGreaterThan(0);
    expect(here).toBeLessThan(order.length - 1);

    const pager = page.locator('nav[aria-label="Previous and next pages"]');
    await expect(pager).toBeVisible();
    await expect(pager.locator('a[rel="prev"]')).toHaveAttribute('href', order[here - 1]!);
    await expect(pager.locator('a[rel="next"]')).toHaveAttribute('href', order[here + 1]!);
  });

  test('first doc has no previous link; last doc has no next link', async ({ page }) => {
    await page.goto('/docs/getting-started/');
    await expect(page.locator('a[rel="prev"]')).toHaveCount(0);
    await expect(page.locator('a[rel="next"]')).toHaveCount(1);
  });

  test('"Last updated" is visible and dateModified in the JSON-LD matches it', async ({ page }) => {
    await page.goto('/docs/configuration/');
    const updated = page.locator('main time[datetime]').first();
    await expect(updated).toBeVisible();
    await expect(page.getByText(/Last updated \d{1,2} [A-Z][a-z]+ \d{4}/)).toBeVisible();
    const iso = await updated.getAttribute('datetime');
    expect(iso).toMatch(/^\d{4}-\d{2}-\d{2}$/);
    const jsonLd = await page
      .locator('script[type="application/ld+json"]')
      .evaluateAll((scripts) => scripts.map((s) => JSON.parse(s.textContent || '{}')));
    const article = jsonLd.find((j) => j['@type'] === 'TechArticle');
    expect(article?.dateModified).toBe(iso);
  });

  test('"Edit this page" links to the content file on GitHub', async ({ page }) => {
    await page.goto('/docs/configuration/');
    const edit = page.locator('main a', { hasText: 'Edit this page' });
    await expect(edit).toBeVisible();
    expect(await edit.getAttribute('href')).toMatch(editUrlPattern);
    expect(await edit.getAttribute('href')).toContain('/configuration.md');
  });

  // Every docs page is also served as Markdown at its URL with a .md suffix
  // (src/pages/docs/[slug].md.ts), the body the agent files carry: the title
  // as the one H1, the lede, the canonical URL, absolute links.
  test('/docs/getting-started.md serves the page as Markdown', async ({ request }) => {
    const res = await request.get('/docs/getting-started.md');
    expect(res.status()).toBe(200);
    expect(res.headers()['content-type']).toMatch(/^text\/markdown/);
    const body = await res.text();
    expect(body.startsWith('# Install mod_pagespeed\n')).toBe(true);
    expect(body).toContain('Canonical URL: https://modpagespeed.com/docs/getting-started/');
    expect(body).toContain('](https://modpagespeed.com/docs/is-it-working/)');
    expect(body).not.toContain('](/docs/');
    expect(body).not.toMatch(/^import |<[A-Z][A-Za-z]*\b[^>]*\/>/m);
  });

  test('the HTML page advertises its Markdown twin in <head> and beside the edit link', async ({
    page,
  }) => {
    await page.goto('/docs/getting-started/');
    const alternate = page.locator('head link[rel="alternate"][type="text/markdown"]');
    await expect(alternate).toHaveCount(1);
    await expect(alternate).toHaveAttribute('href', '/docs/getting-started.md');
    const view = page.locator('main a', { hasText: 'View as Markdown' });
    await expect(view).toBeVisible();
    await expect(view).toHaveAttribute('href', '/docs/getting-started.md');
    // The twin a page points at is the one the server serves.
    const res = await page.request.get((await view.getAttribute('href'))!);
    expect(res.status()).toBe(200);
    expect(res.headers()['content-type']).toMatch(/^text\/markdown/);
  });

  test('an unknown slug has no Markdown twin', async ({ request }) => {
    const res = await request.get('/docs/no-such-page.md');
    expect(res.status()).toBe(404);
  });

  // The trailing-slash scheme applies to pages; a file-like URL is served
  // without one and has no slash variant, the same answer /llms.txt/ and
  // /api/product.json/ give. The Markdown route follows that scheme.
  test('the Markdown route is a file-like URL under the trailing-slash scheme', async ({
    request,
  }) => {
    expect((await request.get('/docs/getting-started.md')).status()).toBe(200);
    expect((await request.get('/docs/getting-started.md/')).status()).toBe(404);
    expect((await request.get('/llms.txt/')).status()).toBe(404);
  });

  test('code blocks have a copy button that copies the code', async ({ page, context }) => {
    await context.grantPermissions(['clipboard-read', 'clipboard-write']);
    await page.goto('/docs/installation-docker/');
    const pre = page.locator('main pre:has(> code)').first();
    const button = pre.locator('button.code-copy-btn');
    await expect(button).toBeAttached();
    await expect(button).toHaveAttribute('aria-label', /copy/i);

    await button.click();
    await expect(button).toHaveAttribute('data-copied', 'true');
    // The name stays put; the page's live region reports the copy.
    await expect(button).toHaveAttribute('aria-label', 'Copy code to clipboard');
    await expect(page.locator('#copy-status')).toHaveText('Copied to clipboard');
    const expected = (await pre.locator('code').textContent()) ?? '';
    const clipboard = await page.evaluate(() => navigator.clipboard.readText());
    expect(clipboard).toBe(expected);
  });

  test('copy button is keyboard operable', async ({ page, context }) => {
    await context.grantPermissions(['clipboard-read', 'clipboard-write']);
    await page.goto('/docs/installation-docker/');
    const button = page.locator('main pre:has(> code) button.code-copy-btn').first();
    await button.focus();
    await page.keyboard.press('Enter');
    await expect(button).toHaveAttribute('data-copied', 'true');
  });

  test('mobile docs navigation is a disclosure, not a navigate-on-change select', async ({
    page,
  }) => {
    await page.setViewportSize({ width: 375, height: 740 });
    await page.goto('/docs/configuration/');
    await expect(page.locator('select#docs-nav')).toHaveCount(0);
    const menu = page.locator('details#docs-menu');
    await expect(menu).toBeVisible();
    await expect(menu).not.toHaveAttribute('open', '');
    const current = menu.locator('nav a[aria-current="page"]');
    await expect(current).toBeHidden();

    await menu.locator('summary').click();
    await expect(menu).toHaveAttribute('open', '');
    await expect(current).toBeVisible();
    await expect(current).toHaveAttribute('href', '/docs/configuration/');
    // Opening the menu changes nothing but the disclosure: same page.
    await expect(page).toHaveURL('/docs/configuration/');
  });

  // Pagefind runs WebAssembly; a Content-Security-Policy without
  // 'wasm-unsafe-eval' makes compilation throw. The dialog must say so
  // visibly instead of leaving the input "Searching…" forever. Simulated by
  // making the probe compile throw, the way a blocked CSP does.
  test('search dialog reports when WebAssembly is blocked', async ({ page }) => {
    await page.addInitScript(() => {
      Object.defineProperty(WebAssembly, 'Module', {
        configurable: true,
        value: function () {
          throw new Error('CompileError: WebAssembly is blocked');
        },
      });
    });
    await page.goto('/docs/');
    await page.locator('main button[data-search-open]').click();
    const status = page.locator('#site-search-status');
    await expect(status).toBeVisible();
    await expect(status).toContainText(/WebAssembly is blocked/);
    await expect(page.locator('#site-search input.pagefind-ui__search-input')).toHaveCount(0);
  });
});

// Docs structure: the groups and their order, the retitled pages at their old
// URLs, the preserved anchors, the quickstart chooser and the retired page.
test.describe('Docs structure', () => {
  test('sidebar groups appear in the audience order', async ({ page }) => {
    await page.goto('/docs/getting-started/');
    const labels = await page
      .locator('nav[aria-label="Documentation"] button[aria-expanded]')
      .allTextContents();
    // The per-line release notes have one entry, under Reference; they are a
    // group on the hub only.
    expect(labels.map((l) => l.trim())).toEqual(
      GROUP_ORDER.filter((g) => g !== 'Release notes by line'),
    );
  });

  test('the hub lists every group once, in order, and links the new pages', async ({ page }) => {
    await page.goto('/docs/');
    const headings = await page.locator('main h2.eyebrow').allTextContents();
    expect(headings.map((h) => h.trim())).toEqual([...GROUP_ORDER, 'Concepts']);
    for (const slug of NEW_PAGES) {
      await expect(page.locator(`main a[href="/docs/${slug}/"]`).first()).toBeAttached();
    }
    // The hero card is Getting started; "Is it working?" sits in its own
    // Start here section rather than vanishing behind the hero.
    await expect(page.locator('main a[href="/docs/is-it-working/"]')).toHaveCount(1);
  });

  test('retitled pages keep their URLs', async ({ page }) => {
    const titles: Array<[string, string]> = [
      ['/docs/installation-module/', 'Install the module on Apache and nginx'],
      ['/docs/deployment/', 'Run in production'],
      ['/docs/production-deployment/', 'ASP.NET Core in production'],
      ['/docs/migrating-to-2-1/', 'Migrate from ModPageSpeed 2.0'],
      ['/docs/aspnet-configuration/', 'ASP.NET Core settings'],
    ];
    for (const [path, title] of titles) {
      await page.goto(path);
      await expect(page.locator('main h1').first()).toHaveText(title);
    }
  });

  test('anchors referenced elsewhere survive the heading changes', async ({ page }) => {
    const anchors: Array<[string, string]> = [
      ['/docs/installation-module/', 'using-the-optimizer-daemon-with-nginx'],
      ['/docs/installation-module/', 'nginx-compatibility'],
      ['/docs/admin-console/', 'optimizer-daemon-panels'],
      ['/docs/migrating-to-2-1/', 'upgrading-from-1-15'],
      ['/docs/migrating-to-2-1/', 'coming-from-open-source'],
      ['/docs/iis-configuration/', 'install-the-module'],
      ['/docs/iis-configuration/', 'iis-tuning'],
      ['/docs/deployment/', 'monitoring'],
      ['/docs/deployment/', 'cache-sizing-recommendations'],
      ['/docs/uninstall/', 'roll-back-to-the-previous-release'],
    ];
    for (const [path, id] of anchors) {
      await page.goto(path);
      await expect(page.locator(`[id="${id}"]`), `${path}#${id}`).toHaveCount(1);
    }
  });

  test('getting started is a chooser with four quickstarts and one verify step', async ({
    page,
  }) => {
    await page.goto('/docs/getting-started/');
    const main = page.locator('main');
    for (const id of [
      'pick-an-integration',
      'quickstart-apache-or-nginx-module',
      'quickstart-docker-reverse-proxy',
      'quickstart-iis',
      'quickstart-aspnet-core',
      'verify-it-works',
    ]) {
      await expect(main.locator(`[id="${id}"]`), id).toHaveCount(1);
    }
    // Every quickstart ends in the same verify step, which uses GET: the
    // Docker reverse proxy does not cache HEAD, so `curl -I` never shows a HIT.
    expect(await main.locator('a[href="#verify-it-works"]').count()).toBeGreaterThanOrEqual(4);
    await expect(main).toContainText('curl -s -o /dev/null -D - http://localhost/');
    await expect(main).not.toContainText('curl -I http://localhost/');
    await expect(main.locator('a[href="/docs/is-it-working/"]').first()).toBeAttached();
  });

  test('is-it-working carries the header table, the bypass parameter and the health probe', async ({
    page,
  }) => {
    await page.goto('/docs/is-it-working/');
    const main = page.locator('main');
    for (const text of [
      'X-Mod-Pagespeed',
      'X-Page-Speed',
      'X-PageSpeed',
      '?PageSpeed=off',
      '/v1/health',
    ]) {
      await expect(main).toContainText(text);
    }
    await expect(main.locator('a[href="/pagespeed-markers/"]')).toBeAttached();
  });

  test('/pagespeed-markers/ links to is-it-working', async ({ page }) => {
    await page.goto('/pagespeed-markers/');
    await expect(page.locator('main a[href="/docs/is-it-working/"]')).toBeAttached();
  });

  test('the retired documentation-plan page is a 301 to the hub', async ({ request, baseURL }) => {
    const res = await request.get('/docs/documentation-plan/', { maxRedirects: 0 });
    expect(res.status()).toBe(301);
    expect(new URL(res.headers()['location'], baseURL).pathname).toBe('/docs/');
  });

  test('prose says worker, not daemon, on the pages this tranche owns', async ({ page }) => {
    for (const path of ['/docs/getting-started/', '/docs/installation-module/']) {
      await page.goto(path);
      const text = (await page.locator('main .prose').innerText()).replace(
        /\b(?:Daemon[A-Za-z]+|ipro_daemon_\w+|pagespeed_daemon\.conf|daemon\.env|daemon-reload|\/v1\/daemon\/[^\s]*)\b/g,
        '',
      );
      expect(text, path).not.toMatch(/\bdaemon\b/i);
    }
  });
});

test.describe('Admin console docs', () => {
  test('opens with a tip pointing at the live console', async ({ page }) => {
    await page.goto('/docs/admin-console/');
    const tip = page.locator('aside.callout-tip').first();
    await expect(tip).toContainText('See it running');
    await expect(tip.locator('a[href="https://we-amp.com/pagespeed_global_admin/"]')).toBeVisible();
    await expect(tip).toContainText('public on purpose');
  });
});

// The docs sidebar: collapsible groups with the current one open, one entry
// for the release notes, the filter reference under Filters, and a rail that
// stays in view and scrolls on its own.
test.describe('Docs sidebar', () => {
  const rail = 'nav[aria-label="Documentation"]';

  test('groups are disclosure buttons and only the current group is open', async ({ page }) => {
    await page.goto('/docs/configuration/');
    const toggles = page.locator(`${rail} button[aria-expanded]`);
    expect(await toggles.count()).toBeGreaterThanOrEqual(8);
    const open = page.locator(`${rail} button[aria-expanded="true"]`);
    await expect(open).toHaveCount(1);
    await expect(open).toHaveText('Configure');

    // Every toggle names the list it controls; the list is shown exactly when
    // the toggle says expanded.
    const states = await toggles.evaluateAll((buttons) =>
      buttons.map((b) => {
        const list = document.getElementById(b.getAttribute('aria-controls') ?? '');
        return {
          expanded: b.getAttribute('aria-expanded'),
          list: list?.tagName ?? null,
          hidden: list?.hidden ?? null,
        };
      }),
    );
    for (const s of states) {
      expect(s.list).toBe('UL');
      expect(s.hidden).toBe(s.expanded !== 'true');
    }

    const current = page.locator(`${rail} a[aria-current="page"]`);
    await expect(current).toHaveCount(1);
    await expect(current).toBeVisible();
    await expect(current).toHaveAttribute('href', '/docs/configuration/');
    await expect(page.locator(`${rail} a[href="/docs/monitoring/"]`)).toBeHidden();
  });

  test('a group opens and closes from the keyboard and focus stays on its button', async ({
    page,
  }) => {
    await page.goto('/docs/getting-started/');
    const toggle = page.locator(`${rail} button[aria-expanded]`, { hasText: 'Operate' });
    const link = page.locator(`${rail} a[href="/docs/deployment/"]`);
    await expect(toggle).toHaveAttribute('aria-expanded', 'false');
    await expect(link).toBeHidden();

    await toggle.focus();
    await page.keyboard.press('Enter');
    await expect(toggle).toHaveAttribute('aria-expanded', 'true');
    await expect(toggle).toBeFocused();
    await expect(link).toBeVisible();
    // The current group stays open: opening one group closes no other.
    await expect(page.locator(`${rail} a[aria-current="page"]`)).toBeVisible();

    // The next Tab stop is the first link of the group just opened.
    await page.keyboard.press('Tab');
    await expect(link).toBeFocused();

    await toggle.focus();
    await page.keyboard.press('Space');
    await expect(toggle).toHaveAttribute('aria-expanded', 'false');
    await expect(toggle).toBeFocused();
    await expect(link).toBeHidden();
  });

  test('the release notes are one entry, and a per-line page opens its group', async ({ page }) => {
    await page.goto('/docs/release-notes-2-5/');
    const nav = page.locator(rail);
    await expect(nav.locator('a[href^="/docs/release-notes"]')).toHaveCount(1);
    const entry = nav.locator('a[href="/docs/release-notes/"]');
    await expect(entry).toHaveText('Release notes');
    await expect(entry).toBeVisible();
    await expect(nav.locator('button[aria-expanded="true"]')).toHaveText('Reference');
    // The entry leads to this page but is not this page.
    await expect(nav.locator('a[aria-current]')).toHaveCount(0);
    // Reading order still walks the per-line pages.
    await expect(page.locator('nav[aria-label="Previous and next pages"] a')).not.toHaveCount(0);
  });

  test('the filter reference is an entry under Filters', async ({ page }) => {
    await page.goto('/docs/image-filters/');
    const nav = page.locator(rail);
    await expect(nav.locator('button[aria-expanded="true"]')).toHaveText('Filters');
    const entry = nav.locator('a[href="/docs/filters/"]');
    await expect(entry).toHaveText('Filter reference');
    await expect(entry).toBeVisible();
    await expect(entry).not.toHaveAttribute('aria-current', 'page');
  });

  test('the filter reference and a filter page sit in the docs frame', async ({ page }) => {
    await page.goto('/docs/filters/');
    const nav = page.locator(rail);
    await expect(nav).toHaveCount(1);
    await expect(nav).toBeVisible();
    await expect(nav.locator('button[aria-expanded="true"]')).toHaveText('Filters');
    await expect(nav.locator('a[aria-current="page"]')).toHaveAttribute('href', '/docs/filters/');
    await expect(page.locator('main h1')).toHaveCount(1);
    // The table keeps a row per filter, with the filter name as its id.
    await expect(page.locator('tr[id="lazyload_images"]')).toHaveCount(1);

    await page.goto('/docs/filters/lazyload_images/');
    await expect(nav).toBeVisible();
    await expect(nav.locator('button[aria-expanded="true"]')).toHaveText('Filters');
    await expect(nav.locator('a[href="/docs/filters/"]')).toBeVisible();
    await expect(nav.locator('a[aria-current]')).toHaveCount(0);
    await expect(page.locator('main h1')).toHaveCount(1);
  });

  test('the rail stays in view and scrolls on its own', async ({ page }) => {
    await page.setViewportSize({ width: 1280, height: 720 });
    await page.emulateMedia({ reducedMotion: 'reduce' });
    await page.goto('/docs/configuration/');
    const nav = page.locator(rail);
    const headerBottom = () =>
      page.evaluate(() => document.querySelector('body > header')!.getBoundingClientRect().bottom);

    await page.evaluate(() => window.scrollTo(0, 4000));
    const pinned = await nav.boundingBox();
    expect(pinned!.y).toBeGreaterThanOrEqual((await headerBottom()) - 1);
    expect(pinned!.y + pinned!.height).toBeLessThanOrEqual(720);

    // With every group open the rail is taller than the window: it scrolls,
    // the page does not move, and a focused entry is fully inside its box.
    const closed = nav.locator('button[aria-expanded="false"]');
    while ((await closed.count()) > 0) await closed.first().click();
    const overflow = await nav.evaluate((el) => el.scrollHeight - el.clientHeight);
    expect(overflow).toBeGreaterThan(0);
    const pageY = await page.evaluate(() => window.scrollY);
    const last = nav.locator('ul a').last();
    await last.focus();
    expect(await page.evaluate(() => window.scrollY)).toBe(pageY);
    const box = await nav.boundingBox();
    const link = await last.boundingBox();
    expect(link!.y).toBeGreaterThanOrEqual(box!.y);
    expect(link!.y + link!.height).toBeLessThanOrEqual(box!.y + box!.height);

    // Walking back up with Shift+Tab never leaves a stop clipped by the rail
    // or behind the header (WCAG 2.4.11 Focus Not Obscured), and never moves
    // the page: this page has the platform selector, so the page scroller
    // keeps a taller band clear at its top, and the rail must start below it.
    const top = await headerBottom();
    for (let i = 0; i < 80; i++) {
      await page.keyboard.press('Shift+Tab');
      const stop = await page.evaluate(() => {
        const el = document.activeElement as HTMLElement;
        const scroller = el.closest('nav[aria-label="Documentation"]');
        if (!scroller) return null;
        const r = el.getBoundingClientRect();
        const b = scroller.getBoundingClientRect();
        return {
          top: r.top,
          bottom: r.bottom,
          boxTop: b.top,
          boxBottom: b.bottom,
          pageY: window.scrollY,
        };
      });
      if (!stop) break;
      expect(stop.pageY).toBe(pageY);
      expect(stop.top).toBeGreaterThanOrEqual(Math.max(stop.boxTop, top) - 1);
      expect(stop.bottom).toBeLessThanOrEqual(stop.boxBottom + 1);
    }
  });

  // Before the page scrolls the rail sits below its sticky top. It must still
  // end 5rem above the bottom of the window, the band a page scroller may keep
  // clear for a pinned control: an entry focused inside that band scrolls the
  // page along with the rail. The band is set here so the test holds on its own.
  test('at the top of the page the open rail fits the window and Tab never moves the page', async ({
    page,
  }) => {
    await page.setViewportSize({ width: 1280, height: 720 });
    await page.emulateMedia({ reducedMotion: 'reduce' });
    // getting-started has no platform selector, cache-modes has one. At 40px
    // the rail is still on its way to its sticky top: its height follows it.
    for (const [url, startY] of [
      ['/docs/getting-started/', 0],
      ['/docs/cache-modes/', 0],
      ['/docs/cache-modes/', 40],
    ] as const) {
      await page.goto(url);
      await page.addStyleTag({ content: 'html { scroll-padding-bottom: 5rem !important; }' });
      const nav = page.locator(rail);
      const closed = nav.locator('button[aria-expanded="false"]');
      while ((await closed.count()) > 0) await closed.first().click();
      await page.evaluate((y) => window.scrollTo(0, y), startY);
      await expect
        .poll(async () => {
          const b = await nav.boundingBox();
          return b!.y + b!.height;
        })
        .toBeLessThanOrEqual(720 - 80 + 1);
      const box = await nav.boundingBox();
      expect(box!.y).toBeGreaterThan(100);
      expect(await nav.evaluate((el) => el.scrollHeight - el.clientHeight)).toBeGreaterThan(0);

      await nav.locator('button[aria-expanded]').first().focus();
      const stops = await nav
        .locator('a[href], button')
        .evaluateAll((els) => els.filter((e) => e.getClientRects().length > 0).length);
      const walk = async (key: string) => {
        for (let i = 0; i < stops - 1; i++) {
          await page.keyboard.press(key);
          const stop = await page.evaluate(() => {
            const el = document.activeElement as HTMLElement;
            const scroller = el.closest('nav[aria-label="Documentation"]');
            if (!scroller) return null;
            const r = el.getBoundingClientRect();
            const b = scroller.getBoundingClientRect();
            return { top: r.top, bottom: r.bottom, boxTop: b.top, boxBottom: b.bottom };
          });
          expect(stop).not.toBeNull();
          expect(await page.evaluate(() => window.scrollY)).toBe(startY);
          expect(stop!.top).toBeGreaterThanOrEqual(stop!.boxTop - 0.5);
          expect(stop!.bottom).toBeLessThanOrEqual(stop!.boxBottom + 0.5);
        }
      };
      await walk('Tab');
      await walk('Shift+Tab');
    }
  });

  // Keeping a focused entry clear of the rail's edges is for keyboard focus
  // only: a mouse press on an entry half under the edge must not scroll it
  // away from the pointer, or the click lands elsewhere and is lost.
  test('a mouse click on an entry at the rail edge acts on it and leaves the rail still', async ({
    page,
  }) => {
    await page.setViewportSize({ width: 1280, height: 720 });
    await page.emulateMedia({ reducedMotion: 'reduce' });
    await page.goto('/docs/cache-modes/');
    const nav = page.locator(rail);
    const closed = nav.locator('button[aria-expanded="false"]');
    while ((await closed.count()) > 0) await closed.first().click();
    await page.evaluate(() => window.scrollTo(0, 1200));
    // Scroll the rail so a toggle shows only its top 8px above the bottom edge.
    const target = await nav.evaluate((el) => {
      el.scrollTop = 0;
      const box = el.getBoundingClientRect();
      const toggles = [...el.querySelectorAll<HTMLElement>('button[aria-expanded]')];
      const toggle = toggles.find((t) => t.getBoundingClientRect().top > box.top + 300)!;
      el.scrollTop += toggle.getBoundingClientRect().top - (box.bottom - 8);
      (document.activeElement as HTMLElement | null)?.blur();
      const r = toggle.getBoundingClientRect();
      return {
        index: toggles.indexOf(toggle),
        x: r.left + 20,
        y: box.bottom - 4,
        expanded: toggle.getAttribute('aria-expanded'),
        scrollTop: el.scrollTop,
      };
    });
    expect(target.scrollTop).toBeGreaterThan(0);
    const toggle = nav.locator('button[aria-expanded]').nth(target.index);
    await page.mouse.click(target.x, target.y);
    await expect(toggle).toHaveAttribute(
      'aria-expanded',
      target.expanded === 'true' ? 'false' : 'true',
    );
    // Two frames: the keyboard correction would have run by then.
    await page.evaluate(
      () => new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r))),
    );
    expect(await nav.evaluate((el) => el.scrollTop)).toBe(target.scrollTop);
  });

  // Without JavaScript nothing can toggle a group, so every group is open and
  // every link can be followed, in the rail and in the phone menu.
  test('without JavaScript every sidebar link is shown', async ({ browser }) => {
    const context = await browser.newContext({ javaScriptEnabled: false });
    const page = await context.newPage();
    const shown = (scope: string) =>
      page.locator(`${scope} ul a`).evaluateAll((links) => ({
        total: links.length,
        visible: links.filter((a) => (a as HTMLElement).getClientRects().length > 0).length,
      }));

    await page.setViewportSize({ width: 1280, height: 720 });
    await page.goto('/docs/getting-started/');
    const inRail = await shown(rail);
    expect(inRail.total).toBeGreaterThan(40);
    expect(inRail.visible).toBe(inRail.total);
    await expect(page.locator(`${rail} a[aria-current="page"]`)).toBeVisible();

    await page.setViewportSize({ width: 375, height: 740 });
    await page.goto('/docs/getting-started/');
    await page.locator('details#docs-menu > summary').click();
    const inMenu = await shown('details#docs-menu');
    expect(inMenu.total).toBe(inRail.total);
    expect(inMenu.visible).toBe(inMenu.total);
    await context.close();
  });

  // The browser can paint between the sidebar markup and the script that
  // closes its groups. Remove that script from the served page: what is left
  // is what such a paint shows, and it must already be the closed sidebar.
  test('before its script runs the sidebar already shows only the current group', async ({
    page,
  }) => {
    // Fetched once, up front: the handler below then answers every load of
    // the page (the dev server can reload it once) without a request of its own.
    const html = await (await page.request.get('/docs/configuration/')).text();
    // Drop every script element that carries the marker (rail and menu).
    const kept: string[] = [];
    let pos = 0;
    let dropped = 0;
    for (;;) {
      const open = html.indexOf('<script', pos);
      if (open < 0) break;
      const close = html.indexOf('</script>', open);
      if (close < 0) break;
      const end = close + '</script>'.length;
      const element = html.slice(open, end);
      kept.push(html.slice(pos, open));
      if (element.includes('data-docs-nav-ready')) dropped += 1;
      else kept.push(element);
      pos = end;
    }
    kept.push(html.slice(pos));
    const stripped = kept.join('');
    expect(dropped).toBe(2);
    expect(stripped.length).toBeLessThan(html.length);
    await page.route('**/docs/configuration/', (route) =>
      route.fulfill({ status: 200, contentType: 'text/html', body: stripped }),
    );
    await page.setViewportSize({ width: 1280, height: 720 });
    await page.goto('/docs/configuration/');
    const nav = page.locator(rail);
    await expect(nav.locator('[data-docs-nav-ready]')).toHaveCount(0);
    await expect(nav.locator('a[aria-current="page"]')).toBeVisible();
    const state = await nav.locator('ul a').evaluateAll((links) => {
      const shown = links.filter((a) => (a as HTMLElement).getClientRects().length > 0);
      return {
        shown: shown.length,
        outsideCurrent: shown.filter((a) => !a.closest('[data-docs-nav-current]')).length,
      };
    });
    expect(state.shown).toBeGreaterThan(0);
    expect(state.outsideCurrent).toBe(0);
    // Only the current group's chevron reads as open.
    const turned = await nav
      .locator('button[aria-expanded] svg')
      .evaluateAll((icons) => icons.filter((i) => getComputedStyle(i).transform !== 'none').length);
    expect(turned).toBe(1);
  });

  // Loading a docs page on a slow processor must not shift the layout in the
  // sidebar: the groups never collapse after they were painted.
  test('the sidebar does not shift while the page loads', async ({ page }) => {
    await page.setViewportSize({ width: 1280, height: 720 });
    const cdp = await page.context().newCDPSession(page);
    await cdp.send('Emulation.setCPUThrottlingRate', { rate: 6 });
    await page.addInitScript(() => {
      const w = window as unknown as { __sidebarShift: number };
      w.__sidebarShift = 0;
      new PerformanceObserver((list) => {
        for (const entry of list.getEntries()) {
          const shift = entry as unknown as {
            value: number;
            hadRecentInput: boolean;
            sources?: { node?: Node | null }[];
          };
          if (shift.hadRecentInput) continue;
          const inSidebar = (shift.sources ?? []).some(
            (s) => s.node instanceof Element && s.node.closest('nav[aria-label="Documentation"]'),
          );
          if (inSidebar) w.__sidebarShift += shift.value;
        }
      }).observe({ type: 'layout-shift', buffered: true });
    });
    for (const path of ['/docs/configuration/', '/docs/filters/']) {
      await page.goto(path, { waitUntil: 'load' });
      await expect(page.locator(`${rail} button[aria-expanded="true"]`)).toHaveCount(1);
      await page.waitForTimeout(500);
      const shift = await page.evaluate(
        () => (window as unknown as { __sidebarShift: number }).__sidebarShift,
      );
      expect(shift, `layout shift in the sidebar on ${path}`).toBe(0);
    }
  });

  test('on a phone the same groups sit in the Docs menu, current group open', async ({ page }) => {
    await page.setViewportSize({ width: 375, height: 740 });
    await page.goto('/docs/filters/lazyload_images/');
    await expect(page.locator(rail)).toBeHidden();
    const menu = page.locator('details#docs-menu');
    await expect(menu).toBeVisible();
    await menu.locator('summary').click();
    await expect(menu.locator('button[aria-expanded="true"]')).toHaveText('Filters');
    await expect(menu.locator('a[href="/docs/filters/"]')).toBeVisible();
    const closed = menu.locator('button[aria-expanded]', { hasText: 'Install' });
    await closed.click();
    await expect(closed).toHaveAttribute('aria-expanded', 'true');
    await expect(menu.locator('a[href="/docs/installation-docker/"]')).toBeVisible();
  });
});
