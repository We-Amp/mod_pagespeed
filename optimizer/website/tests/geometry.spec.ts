// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Geometry oracle. One test per page template (every route file under
// src/pages that renders HTML; dynamic routes use the first page their index
// links to, so a new template joins automatically), each loaded once and then
// measured at every width in WIDTHS:
//
//   1. no horizontal scroll;
//   2. the header logo has a real box (minimum width, height above 0);
//   3. visible header items do not overlap each other;
//   4. scrolled to the bottom, no fixed or sticky element covers footer text;
//   5. (1280 and 375 only) every disclosure toggle flips aria-expanded (or the
//      details open state) and its icon's computed transform changes.
//
// These are the defects review rounds kept finding by eye: a header that
// collapsed the logo at 768, floating buttons on top of the legal line, a
// chevron that never rotated.

import { readdirSync } from 'node:fs';
import path from 'node:path';
import { test, expect, type Page } from '@playwright/test';
import { parseWidths } from './helpers/widths';

// GEOMETRY_WIDTHS narrows the sweep (see tests/helpers/widths.ts): pull requests
// use 375, the nightly run uses "all".
const WIDTHS = parseWidths(process.env.GEOMETRY_WIDTHS);
const HEIGHT = 800;
const MIN_LOGO_WIDTH = 100;

const pagesDir = path.resolve(process.cwd(), 'src/pages');

// Route files that are not an HTML page, or are not part of the shipped site.
// The top-level 404 and 500 routes only render on an error; their prerendered
// copies under error/ are plain pages and are swept.
const SKIP = [/\.ts$/, /^go\//, /^kit\//, /^404\.astro$/, /^500\.astro$/];

function walk(dir: string, rel = ''): string[] {
  return readdirSync(path.join(dir, rel), { withFileTypes: true }).flatMap((e) => {
    const r = rel ? `${rel}/${e.name}` : e.name;
    return e.isDirectory() ? walk(dir, r) : [r];
  });
}

type Route = { file: string; url: string; dynamic: boolean };

function routes(): Route[] {
  return walk(pagesDir)
    .filter((f) => f.endsWith('.astro') && !SKIP.some((re) => re.test(f)))
    .sort()
    .map((file) => {
      const dynamic = file.includes('[');
      let url = '/' + file.replace(/\.astro$/, '').replace(/(^|\/)index$/, '');
      if (!url.endsWith('/')) url += '/';
      return { file, url, dynamic };
    });
}

// A dynamic route has no fixed URL: take the first link its parent page carries
// that points one level below it.
async function resolveUrl(page: Page, route: Route): Promise<string> {
  if (!route.dynamic) return route.url;
  const parent = route.url.slice(0, route.url.indexOf('[')).replace(/\/?$/, '/');
  await page.goto(parent);
  const href = await page.evaluate((prefix) => {
    const a = Array.from(document.querySelectorAll<HTMLAnchorElement>('main a[href]')).find((x) => {
      const h = x.getAttribute('href') ?? '';
      return h.startsWith(prefix) && h.length > prefix.length && !h.includes('#');
    });
    return a?.getAttribute('href') ?? '';
  }, parent);
  return href;
}

// Runs in the page. Returns human-readable violations for the current width.
function measure(minLogoWidth: number): string[] {
  const out: string[] = [];
  const vw = document.documentElement.clientWidth;
  const rect = (el: Element) => el.getBoundingClientRect();
  const shown = (el: Element) => {
    const cs = getComputedStyle(el);
    const r = rect(el);
    return (
      cs.display !== 'none' &&
      cs.visibility !== 'hidden' &&
      Number(cs.opacity) > 0.05 &&
      r.width > 2 &&
      r.height > 2
    );
  };

  if (document.documentElement.scrollWidth > vw + 1) {
    out.push(`horizontal scroll: scrollWidth ${document.documentElement.scrollWidth} > ${vw}`);
  }

  const logo = document.querySelector('body > header a[href="/"]');
  if (!logo) out.push('header logo link missing');
  else {
    const r = rect(logo);
    if (r.width < minLogoWidth || r.height <= 0) {
      out.push(`header logo box ${Math.round(r.width)}x${Math.round(r.height)}`);
    }
  }

  const items = Array.from(
    document.querySelectorAll('body > header a, body > header button'),
  ).filter(shown);
  for (let i = 0; i < items.length; i++) {
    for (let j = i + 1; j < items.length; j++) {
      const a = items[i];
      const b = items[j];
      if (a.contains(b) || b.contains(a)) continue;
      const ra = rect(a);
      const rb = rect(b);
      const w = Math.min(ra.right, rb.right) - Math.max(ra.left, rb.left);
      const h = Math.min(ra.bottom, rb.bottom) - Math.max(ra.top, rb.top);
      if (w > 1 && h > 1) {
        const name = (e: Element) =>
          (e.getAttribute('aria-label') || e.textContent || e.tagName).trim().slice(0, 24);
        out.push(`header items overlap: "${name(a)}" and "${name(b)}"`);
      }
    }
  }
  return out;
}

// Runs in the page after scrolling to the bottom: fixed/sticky elements that sit
// on top of footer text.
function footerCover(): string[] {
  const out: string[] = [];
  const footer = document.querySelector('[data-site-footer]');
  if (!footer) return out;
  const covers = Array.from(document.querySelectorAll('body *')).filter((el) => {
    const cs = getComputedStyle(el);
    if (cs.position !== 'fixed' && cs.position !== 'sticky') return false;
    if (footer.contains(el)) return false;
    const r = el.getBoundingClientRect();
    // A top-anchored sticky header can always be scrolled away from the text
    // under it; only elements that stay on top of reachable text count.
    if (r.top <= 1) return false;
    return (
      cs.display !== 'none' &&
      cs.visibility !== 'hidden' &&
      Number(cs.opacity) > 0.05 &&
      r.width > 0 &&
      r.height > 0
    );
  });
  const walker = document.createTreeWalker(footer, NodeFilter.SHOW_TEXT);
  for (let n = walker.nextNode(); n; n = walker.nextNode()) {
    if (!n.textContent?.trim()) continue;
    const range = document.createRange();
    range.selectNodeContents(n);
    for (const t of Array.from(range.getClientRects())) {
      if (t.width < 1 || t.height < 1) continue;
      for (const c of covers) {
        const r = c.getBoundingClientRect();
        const w = Math.min(t.right, r.right) - Math.max(t.left, r.left);
        const h = Math.min(t.bottom, r.bottom) - Math.max(t.top, r.top);
        if (w > 1 && h > 1) {
          const hit = document.elementFromPoint(
            Math.max(t.left, r.left) + w / 2,
            Math.max(t.top, r.top) + h / 2,
          );
          if (hit && (c === hit || c.contains(hit))) {
            const id = (c as HTMLElement).id || (c as HTMLElement).className || c.tagName;
            out.push(
              `fixed/sticky element "${String(id).slice(0, 40)}" covers footer text "${n.textContent.trim().slice(0, 30)}"`,
            );
          }
        }
      }
    }
  }
  return Array.from(new Set(out));
}

async function settle(page: Page, ms = 180) {
  await page.evaluate(
    () => new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r))),
  );
  await page.waitForTimeout(ms);
}

// Click each disclosure toggle: aria-expanded (or details[open]) must flip and
// the icon inside it must change its computed transform.
async function checkDisclosures(page: Page, label: string): Promise<string[]> {
  const out: string[] = [];
  const toggles = page.locator(
    'header button[aria-expanded]:not(#mobile-menu-btn):not([aria-haspopup="dialog"]), main button[aria-expanded]:not([aria-haspopup="dialog"])',
  );
  const n = await toggles.count();
  for (let i = 0; i < n; i++) {
    const t = toggles.nth(i);
    if (!(await t.isVisible())) continue;
    const name = ((await t.getAttribute('aria-label')) || (await t.textContent()) || `#${i}`)
      .trim()
      .slice(0, 30);
    const before = await t.getAttribute('aria-expanded');
    const icon = t.locator('svg').first();
    const hasIcon = (await icon.count()) > 0;
    const tf = () =>
      icon.evaluate((e) => {
        const cs = getComputedStyle(e);
        return `${cs.transform}|${cs.rotate}|${cs.scale}`;
      });
    const tf0 = hasIcon ? await tf() : '';
    await t.click();
    await settle(page);
    const after = await t.getAttribute('aria-expanded');
    if (after === before) out.push(`${label}: toggle "${name}" did not flip aria-expanded`);
    if (hasIcon && (await tf()) === tf0) {
      out.push(`${label}: toggle "${name}" icon transform did not change`);
    }
    await t.click();
    await settle(page, 0);
  }
  return out;
}

async function checkMobileMenuDisclosures(page: Page, label: string): Promise<string[]> {
  const out: string[] = [];
  await page.locator('#mobile-menu-btn').click();
  await settle(page);
  const sums = page.locator('#mobile-menu details > summary');
  const n = await sums.count();
  for (let i = 0; i < n; i++) {
    const s = sums.nth(i);
    if (!(await s.isVisible())) continue;
    const name = ((await s.textContent()) || `#${i}`).trim().slice(0, 30);
    const open = () => s.evaluate((e) => (e.parentElement as HTMLDetailsElement).open);
    const icon = s.locator('svg').first();
    const hasIcon = (await icon.count()) > 0;
    const tf = () =>
      icon.evaluate((e) => {
        const cs = getComputedStyle(e);
        return `${cs.transform}|${cs.rotate}|${cs.scale}`;
      });
    const o0 = await open();
    const tf0 = hasIcon ? await tf() : '';
    await s.click();
    await settle(page);
    if ((await open()) === o0) out.push(`${label}: details "${name}" did not toggle`);
    if (hasIcon && (await tf()) === tf0) {
      out.push(`${label}: details "${name}" icon transform did not change`);
    }
    await s.click();
    await settle(page, 0);
  }
  await page.locator('#mobile-menu-btn').click();
  await settle(page, 0);
  return out;
}

test.describe('Geometry oracle', () => {
  for (const route of routes()) {
    test(`${route.file}`, async ({ page }) => {
      test.setTimeout(120_000);
      await page.setViewportSize({ width: 1280, height: HEIGHT });
      const url = await resolveUrl(page, route);
      expect(url, `no page found for dynamic route ${route.file}`).not.toBe('');
      // The dev server can reload a page once after its first visit (dependency
      // optimisation); measure again from a fresh load when that happens.
      const run = async (): Promise<string[]> => {
        await page.setViewportSize({ width: 1280, height: HEIGHT });
        await page.goto(url, { waitUntil: 'networkidle' });
        await page.evaluate(() => document.fonts.ready);
        const found: string[] = [];
        for (const width of WIDTHS) {
          await page.setViewportSize({ width, height: HEIGHT });
          await page.evaluate(() => window.scrollTo(0, 0));
          await settle(page, 0);
          for (const v of await page.evaluate(measure, MIN_LOGO_WIDTH))
            found.push(`${width}px: ${v}`);

          await page.evaluate(() => window.scrollTo(0, document.documentElement.scrollHeight));
          await settle(page, 200);
          for (const v of await page.evaluate(footerCover)) found.push(`${width}px (bottom): ${v}`);

          if (width === 1280) found.push(...(await checkDisclosures(page, '1280px')));
          if (width === 375) {
            await page.evaluate(() => window.scrollTo(0, 0));
            found.push(...(await checkMobileMenuDisclosures(page, '375px')));
          }
        }
        // The desktop dropdown check always runs, even when the sweep is narrowed.
        if (!WIDTHS.includes(1280)) {
          await page.setViewportSize({ width: 1280, height: HEIGHT });
          await page.evaluate(() => window.scrollTo(0, 0));
          found.push(...(await checkDisclosures(page, '1280px')));
        }
        return found;
      };
      let problems: string[];
      try {
        problems = await run();
      } catch (err) {
        if (!String(err).includes('Execution context was destroyed')) throw err;
        problems = await run();
      }
      expect(problems, `${url}\n${problems.join('\n')}`).toEqual([]);
    });
  }
});
