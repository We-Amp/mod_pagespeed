// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';
import AxeBuilder from '@axe-core/playwright';
import { auditLinks } from './helpers/link-names';

const PAGES_FOR_AXE_SCAN = [
  '/',
  '/features/',
  '/pagespeed-optimization/',
  '/pricing/',
  '/support/',
  '/hosting-partners/',
  '/platform/',
  '/demo/',
  '/docs/',
  '/docs/getting-started/',
  '/docs/filters/',
  '/docs/filters/lazyload_images/',
  '/license/',
  '/contact/',
  '/calculator/',
  '/blog/',
  '/security/',
  '/terms/',
  '/ai-readability/',
  '/ai-readability/hosted-builders/',
  // /privacy/ points at the notice on we-amp.com; it is covered by its own
  // suite in terms-privacy.spec.ts
];

// Scanned at phone width as well: narrow layouts stack and wrap links, which
// is where small or crowded targets (WCAG 2.5.8) and overflowing regions show.
const PAGES_FOR_PHONE_AXE_SCAN = [
  '/pagespeed-optimization/',
  '/docs/filters/',
  '/docs/filters/lazyload_images/',
  '/download/',
  '/docs/getting-started/',
  '/examples/',
  '/alternatives/',
  '/blog/avif-vs-webp-2026/',
  '/error/404/',
];

// The alt-text and link-name audit covers every page the axe scans cover.
const LINK_AUDIT_PAGES = [...new Set([...PAGES_FOR_AXE_SCAN, ...PAGES_FOR_PHONE_AXE_SCAN])];

// Link names that already lead to two pages, each pinned to the pages it
// leads to today, so a new shared name or a further destination fails. Each
// is a wording decision; remove the entry once the names differ.
const KNOWN_SHARED_LINK_NAMES: Record<string, Record<string, string[]>> = {};

test.describe('Accessibility', () => {
  test('skip-to-content link is first focusable element', async ({ page }) => {
    await page.goto('/');
    // A cold dev server reloads the page once, right after the first load, when
    // it has optimized its dependencies. A Tab pressed into the document that
    // is then replaced leaves nothing focused in the new one. So mark the
    // loaded document, press Tab, and read the focus together with the mark: a
    // missing mark means the key press did not land in a fully loaded document
    // that is still there, and the check is repeated on a fresh load.
    type Probe = { __tabProbe?: boolean };
    let href: string | null | undefined;
    for (let attempt = 0; attempt < 3 && href === undefined; attempt++) {
      if (attempt > 0) await page.reload();
      await page
        .evaluate(() => {
          if (document.readyState === 'complete') (window as unknown as Probe).__tabProbe = true;
        })
        .catch(() => undefined);
      // Tab to first focusable element
      await page.keyboard.press('Tab');
      href = await page
        .evaluate(() =>
          (window as unknown as Probe).__tabProbe
            ? (document.activeElement?.getAttribute('href') ?? null)
            : undefined,
        )
        .catch(() => undefined);
    }
    expect(href).toBe('#main-content');
  });

  test('calculator inputs have associated labels', async ({ page }) => {
    await page.goto('/calculator/');
    // per-site licensing renamed the "servers" input to "sites".
    const inputIds = ['pageviews', 'pageweight', 'bwcost', 'sites'];
    for (const id of inputIds) {
      const label = page.locator(`label[for="${id}"]`);
      await expect(label, `Label for ${id} should exist`).toBeAttached();
    }
  });

  test('heading hierarchy is correct (no skips)', async ({ page }) => {
    await page.goto('/');
    const headings = page.locator('main h1, main h2, main h3, main h4, main h5, main h6');
    const count = await headings.count();
    let lastLevel = 0;

    for (let i = 0; i < count; i++) {
      const tag = await headings.nth(i).evaluate((el) => el.tagName.toLowerCase());
      const level = parseInt(tag.charAt(1));
      // Should not skip more than 1 level (e.g., h1 -> h3 is bad)
      if (lastLevel > 0) {
        expect(level <= lastLevel + 1, `Heading skip: h${lastLevel} -> h${level}`).toBeTruthy();
      }
      lastLevel = level;
    }
  });

  test('pricing toggle has appropriate ARIA', async ({ page }) => {
    await page.goto('/pricing/');
    const toggle = page.locator('#billing-toggle');
    // Billing toggle only exists when live pricing is enabled (prices are
    // currently TBD). Skip gracefully when the element is absent.
    if ((await toggle.count()) === 0) {
      test.skip();
      return;
    }
    await expect(toggle).toHaveAttribute('role', 'switch');
    await expect(toggle).toHaveAttribute('aria-checked', /true|false/);
    await expect(toggle).toHaveAttribute('aria-label', /billing/i);
  });

  test('mobile menu button has aria-label', async ({ page }) => {
    await page.goto('/');
    const btn = page.locator('#mobile-menu-btn');
    await expect(btn).toHaveAttribute('aria-label', /menu/i);
  });

  test('page has lang attribute on <html>', async ({ page }) => {
    await page.goto('/');
    const html = page.locator('html');
    await expect(html).toHaveAttribute('lang', 'en');
  });

  // WCAG 1.1.1 and 2.4.4: every image carries an alt attribute, every link a
  // screen reader is offered has a name (an image or icon inside it counts
  // only through its alt, label or <title>), and one name does not lead to
  // two different pages.
  for (const pagePath of LINK_AUDIT_PAGES) {
    test(`images have alt text and links have distinct names: ${pagePath}`, async ({ page }) => {
      // A cold dev server can reload the page once after its first visit,
      // which destroys the context the audit runs in; audit again from a
      // fresh load then.
      const run = async () => {
        await page.goto(pagePath, { waitUntil: 'load' });
        return page.evaluate(auditLinks);
      };
      let audit: Awaited<ReturnType<typeof run>>;
      try {
        audit = await run();
      } catch (err) {
        if (!String(err).includes('Execution context was destroyed')) throw err;
        audit = await run();
      }
      expect.soft(audit.imagesWithoutAlt, 'images without an alt attribute').toEqual([]);
      expect.soft(audit.unnamedLinks, 'links without an accessible name').toEqual([]);
      expect
        .soft(audit.genericLinks, 'links named only "read more", "here" and the like')
        .toEqual([]);
      const known = KNOWN_SHARED_LINK_NAMES[pagePath] ?? {};
      const sameTargets = (a: string[], b: string[] = []) =>
        a.length === b.length && [...a].sort().every((t, i) => t === [...b].sort()[i]);
      const ambiguous = audit.sameNameDifferentTargets.filter(
        (d) => !sameTargets(d.targets, known[d.name]),
      );
      expect
        .soft(
          ambiguous,
          `one link name, several pages:\n${ambiguous
            .map((d) => `  "${d.name}": ${d.targets.join(', ')}`)
            .join('\n')}`,
        )
        .toEqual([]);
    });
  }

  test('the link audit catches an unnamed icon link and a shared name', async ({ page }) => {
    await page.setContent(`
      <img src="data:," />
      <a href="/a/"><svg aria-hidden="true"></svg></a>
      <a href="/b/"><img src="data:," alt="" /></a>
      <a href="/c/"><svg><title>Close</title></svg></a>
      <a href="/d/" aria-label="Docs"><img src="data:," alt="" /></a>
      <a href="/e/">Docs</a>
      <a href="/f/">Read more</a>
      <a href="/g/#one">Guide</a>
      <a href="/g/#two">Guide</a>
    `);
    const audit = await page.evaluate(auditLinks);
    expect(audit.imagesWithoutAlt).toHaveLength(1);
    expect(audit.unnamedLinks).toHaveLength(2);
    expect(audit.genericLinks).toEqual(['"Read more"']);
    expect(audit.sameNameDifferentTargets.map((d) => d.name)).toEqual(['docs']);
  });

  test('main navigation has aria-label', async ({ page }) => {
    await page.goto('/');
    const nav = page.locator('header nav');
    await expect(nav).toHaveAttribute('aria-label', /navigation/i);
  });

  // WCAG 2.4.11 Focus Not Obscured: walking back up a page with Shift+Tab
  // scrolls each stop into view at the top edge, where the sticky header is
  // and, on docs pages that have one, the platform selector pinned under it.
  // Every stop must come to rest below both, not behind them.
  // /docs/cache-modes/ is a page with the platform selector.
  for (const pagePath of [
    '/',
    '/pricing/',
    '/docs/getting-started/',
    '/docs/cache-modes/',
    '/docs/filters/lazyload_images/',
  ]) {
    test(`Shift+Tab never puts focus behind the sticky header: ${pagePath}`, async ({ page }) => {
      test.setTimeout(90_000);
      // Reduced motion makes focus scrolling instant, so each stop can be
      // measured right after the key press.
      await page.emulateMedia({ reducedMotion: 'reduce' });
      await page.goto(pagePath, { waitUntil: 'networkidle' });
      await page.evaluate(() => {
        window.scrollTo(0, document.documentElement.scrollHeight);
        const links = document.querySelectorAll<HTMLElement>('footer a[href]');
        links[links.length - 1].focus();
      });

      const obscured: string[] = [];
      let stops = 0;
      for (; stops < 600; stops++) {
        await page.keyboard.press('Shift+Tab');
        const stop = await page.evaluate(() => {
          const el = document.activeElement as HTMLElement | null;
          const header = document.querySelector('body > header');
          if (!el || el === document.body || !header) return { done: true };
          if (header.contains(el)) return { done: true };
          const rect = el.getBoundingClientRect();
          const headerBottom = header.getBoundingClientRect().bottom;
          // The header only covers anything while it is pinned to the top.
          let edge = headerBottom > 0 ? headerBottom : 0;
          let what = 'header';
          // The platform selector covers its own column while it is pinned
          // under the header.
          const selector = document.getElementById('platform-selector');
          if (selector && !selector.contains(el)) {
            const bar = selector.getBoundingClientRect();
            const pinned =
              getComputedStyle(selector).position === 'sticky' && bar.top <= headerBottom + 1;
            if (pinned && rect.right > bar.left && rect.left < bar.right && bar.bottom > edge) {
              edge = bar.bottom;
              what = 'platform selector';
            }
          }
          const name = (el.getAttribute('aria-label') ?? el.textContent ?? '')
            .trim()
            .replace(/\s+/g, ' ')
            .slice(0, 50);
          return {
            done: false,
            covered: rect.top < edge - 1,
            label: `<${el.tagName.toLowerCase()}> "${name}" top ${Math.round(rect.top)} < ${what} bottom ${Math.round(edge)}`,
          };
        });
        if (stop.done) break;
        if (stop.covered) obscured.push(stop.label!);
      }

      expect(stops, 'the walk reaches the header').toBeLessThan(600);
      expect(stops, 'the walk passes through the page content').toBeGreaterThan(5);
      expect(obscured, `focus stops behind a pinned bar:\n${obscured.join('\n')}`).toHaveLength(0);
    });
  }

  // Forced colors (Windows contrast themes) drop author backgrounds and
  // shadows. States that are a fill in the normal theme must still be visible
  // (WCAG 1.4.1 Use of Color, 1.4.11 Non-text Contrast).
  test.describe('forced colors', () => {
    test.beforeEach(async ({ page }) => {
      await page.emulateMedia({ forcedColors: 'active', reducedMotion: 'reduce' });
    });

    test('the primary button has a visible border', async ({ page }) => {
      await page.goto('/');
      const border = await page
        .locator('main .btn-primary')
        .first()
        .evaluate((el) => {
          const cs = getComputedStyle(el);
          return {
            width: parseFloat(cs.borderTopWidth),
            style: cs.borderTopStyle,
            color: cs.borderTopColor,
            ground: getComputedStyle(document.body).backgroundColor,
          };
        });
      expect(border.width).toBeGreaterThanOrEqual(1);
      expect(border.style).toBe('solid');
      expect(border.color).not.toBe(border.ground);
    });

    test('the selected tab is filled, and its focus ring stands out from the fill', async ({
      page,
    }) => {
      await page.goto('/demo/');
      const selected = page.locator('.product-tab[aria-pressed="true"]').first();
      const other = page.locator('.product-tab[aria-pressed="false"]').first();
      await selected.scrollIntoViewIfNeeded();
      const read = (el: Element) => {
        const cs = getComputedStyle(el);
        return {
          // 'none' keeps the element's own fill; 'auto' lets the mode drop it.
          adjust: cs.getPropertyValue('forced-color-adjust'),
          background: cs.backgroundColor,
          outlineStyle: cs.outlineStyle,
          outlineColor: cs.outlineColor,
        };
      };
      const unfocused = await selected.evaluate(read);
      const unselected = await other.evaluate(read);
      expect(unfocused.adjust, 'the selected tab keeps its fill').toBe('none');
      expect(unselected.adjust, 'an unselected tab has no fill').toBe('auto');
      expect(unfocused.background, 'selected and unselected tabs differ').not.toBe(
        unselected.background,
      );

      // Reach the tab with the keyboard so the focus ring (:focus-visible) shows.
      await other.focus();
      await page.keyboard.press('Shift+Tab');
      await expect(selected).toBeFocused();
      const focused = await selected.evaluate(read);
      expect(unfocused.outlineStyle).toBe('none');
      expect(focused.outlineStyle).toBe('solid');
      expect(focused.outlineColor, 'the ring is not the colour of the fill').not.toBe(
        focused.background,
      );
    });

    test('the chosen install platform is filled, and its focus ring stands out', async ({
      page,
    }) => {
      await page.goto('/');
      const picker = page.locator('[data-install-tabs]').first();
      const selected = picker.locator('.install-radio:checked + .install-tab');
      const other = picker.locator('.install-radio:not(:checked) + .install-tab').first();
      await selected.scrollIntoViewIfNeeded();
      const read = (el: Element) => {
        const cs = getComputedStyle(el);
        return {
          adjust: cs.getPropertyValue('forced-color-adjust'),
          background: cs.backgroundColor,
          outlineStyle: cs.outlineStyle,
          outlineColor: cs.outlineColor,
        };
      };
      const chosen = await selected.evaluate(read);
      const unchosen = await other.evaluate(read);
      expect(chosen.adjust, 'the chosen tab keeps its fill').toBe('none');
      expect(unchosen.adjust, 'an unchosen tab has no fill').toBe('auto');
      expect(chosen.background, 'the chosen tab is filled').not.toBe('rgba(0, 0, 0, 0)');
      expect(chosen.background, 'chosen and unchosen tabs differ').not.toBe(unchosen.background);

      // Focus the checked radio from the keyboard so :focus-visible applies.
      await picker.locator('.install-radio:checked').focus();
      await page.keyboard.press('Shift+Tab');
      await page.keyboard.press('Tab');
      const focused = await selected.evaluate(read);
      expect(focused.outlineStyle).toBe('solid');
      expect(focused.outlineColor, 'the ring is not the colour of the fill').not.toBe(
        focused.background,
      );
    });

    test('the current page in the docs sidebar is marked by a border', async ({ page }) => {
      await page.goto('/docs/getting-started/');
      const nav = page.locator('nav[aria-label="Documentation"]');
      const width = (el: Element) => parseFloat(getComputedStyle(el).borderInlineStartWidth);
      const current = await nav.locator('a[aria-current="page"]').evaluate(width);
      const sibling = await nav.locator('a:not([aria-current])').first().evaluate(width);
      expect(current).toBeGreaterThanOrEqual(3);
      expect(sibling).toBe(0);
    });
  });

  // WCAG 2.2 AA axe-core scan on all buyer-facing pages.
  for (const pagePath of PAGES_FOR_AXE_SCAN) {
    test(`axe-core WCAG 2.2 AA scan: ${pagePath}`, async ({ page }) => {
      // Scan the resting colours. The telemetry strip fades a value in once
      // when the live reading arrives; axe would otherwise measure contrast
      // mid-fade, depending on timing. Reduced motion removes that fade, so
      // the scan measures the resting state.
      await page.emulateMedia({ reducedMotion: 'reduce' });

      // The dev server can reload a page once after its first visit
      // (dependency optimisation). A reload that lands while axe is running
      // destroys the execution context it works in (or leaves the scan
      // waiting on a document that is gone). So load the page, wait until no
      // further load has happened for a moment, and scan; if the context is
      // still destroyed, scan again from a fresh load.
      test.setTimeout(90_000);
      let loads = 0;
      page.on('load', () => {
        loads += 1;
      });
      const scan = async () => {
        await page.goto(pagePath, { waitUntil: 'networkidle' });
        for (let quiet = 0; quiet < 3; quiet++) {
          const seen = loads;
          await page.waitForTimeout(500);
          if (loads !== seen) quiet = -1;
        }

        const builder = new AxeBuilder({ page }).withTags([
          'wcag2a',
          'wcag2aa',
          'wcag21a',
          'wcag21aa',
          'wcag22aa',
        ]);

        // Demo page embeds third-party demo sites in iframes with intentionally
        // non-optimized assets. Exclude iframe content and scrollable demo panels.
        if (pagePath === '/demo/') {
          builder.exclude('iframe').exclude('[role="tabpanel"]').exclude('pre');
        }

        return builder.analyze();
      };
      let results: Awaited<ReturnType<typeof scan>>;
      try {
        results = await scan();
      } catch (err) {
        if (!String(err).includes('Execution context was destroyed')) throw err;
        results = await scan();
      }

      const violations = results.violations.filter(
        (v) => v.impact === 'critical' || v.impact === 'serious',
      );

      expect(
        violations,
        `${pagePath} has ${violations.length} serious/critical a11y violations:\n` +
          violations
            .map((v) => `  [${v.impact}] ${v.id}: ${v.description} (${v.nodes.length} occurrences)`)
            .join('\n'),
      ).toHaveLength(0);
    });
  }

  test.describe('phone width', () => {
    test.use({ viewport: { width: 390, height: 844 } });

    for (const pagePath of PAGES_FOR_PHONE_AXE_SCAN) {
      test(`axe-core WCAG 2.2 AA scan at 390px: ${pagePath}`, async ({ page }) => {
        // Same settling as the scans above: resting colours, and a scan that
        // is repeated when a dev-server reload destroys its context.
        await page.emulateMedia({ reducedMotion: 'reduce' });
        test.setTimeout(90_000);
        let loads = 0;
        page.on('load', () => {
          loads += 1;
        });
        const scan = async () => {
          await page.goto(pagePath, { waitUntil: 'networkidle' });
          for (let quiet = 0; quiet < 3; quiet++) {
            const seen = loads;
            await page.waitForTimeout(500);
            if (loads !== seen) quiet = -1;
          }
          return new AxeBuilder({ page })
            .withTags(['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa', 'wcag22aa'])
            .analyze();
        };
        let results: Awaited<ReturnType<typeof scan>>;
        try {
          results = await scan();
        } catch (err) {
          if (!String(err).includes('Execution context was destroyed')) throw err;
          results = await scan();
        }

        const violations = results.violations.filter(
          (v) => v.impact === 'critical' || v.impact === 'serious',
        );

        expect(
          violations,
          `${pagePath} has ${violations.length} serious/critical a11y violations at 390px:\n` +
            violations
              .map(
                (v) => `  [${v.impact}] ${v.id}: ${v.description} (${v.nodes.length} occurrences)`,
              )
              .join('\n'),
        ).toHaveLength(0);
      });
    }
  });
});
