// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect, type Page } from '@playwright/test';
import { mockScan, startScan, type Fixture } from './helpers/scan-mock';
import { trackedEvents } from './helpers/umami';

// The "Your next steps" list at the top of a scan result, the summary line
// above it, the note under the topics and the seconds counter of the wait.

const CONTENT =
  'Put this page’s main text in the HTML the server sends, so it does not depend on JavaScript.';
const TITLE =
  'JavaScript changes this page’s title after it loads. Put the final one in the HTML the server sends.';
const A11Y =
  'The scan found 7 accessibility issues: Images must have alternative text; Elements must meet minimum color contrast ratio thresholds; <html> element must have a lang attribute; and 4 more. These are automated checks, not an accessibility audit.';
const A11Y_BUILDER =
  'The scan found 2 accessibility issues: Elements must meet minimum color contrast ratio thresholds; Heading levels should only increase by one. These are automated checks, not an accessibility audit.';
const READABLE =
  'The text of this page is in the HTML, and robots.txt blocks none of the AI crawlers this scan checks.';

const steps = (page: Page) =>
  page.locator('[data-scan-step]').evaluateAll((items) =>
    items.map((li) => ({
      text: li.querySelector('span')!.textContent,
      tag: li.querySelector('[data-scan-step-tag]')!.textContent,
      show: li.querySelector('button')!.textContent,
    })),
  );

test.describe('next steps on a scan result', () => {
  test('a result with gaps lists three steps under the tiles, each with who can do it', async ({
    page,
  }) => {
    await mockScan(page, 'full');
    await startScan(page);
    const block = page.locator('[data-scan-steps]');
    await expect(block.getByRole('heading', { level: 3, name: 'Your next steps' })).toBeVisible();
    expect(await steps(page)).toEqual([
      { text: CONTENT, tag: 'needs a developer', show: 'Show in AI readability' },
      { text: TITLE, tag: 'needs a developer', show: 'Show in Risk & SEO' },
      { text: A11Y, tag: 'in your CMS or page template', show: 'Show in AI readability' },
    ]);
    // Four qualify (Speed is poor too) and three are listed: the line says so.
    await expect(page.locator('[data-scan-health]')).toHaveText(
      '4 things to improve. The first three are below.',
    );
    await expect(page.locator('[data-scan-readable]')).toBeHidden();
    // The order on the page: summary line, tiles, steps, form.
    const tops = await page.evaluate(() =>
      [
        '[data-scan-health]',
        '[data-scan-tile="risk"]',
        '[data-scan-steps]',
        '[data-scan-lead]',
      ].map((q) => document.querySelector(q)!.getBoundingClientRect().top),
    );
    expect([...tops].sort((a, b) => a - b)).toEqual(tops);
  });

  test('a step opens its panel, focuses its row visibly and counts as one event', async ({
    page,
  }) => {
    await mockScan(page, 'full');
    await startScan(page);
    await page.locator('[data-scan-step="content"] button').click();
    await expect(page.locator('[data-scan-tile="airead"]')).toHaveAttribute(
      'aria-expanded',
      'true',
    );
    const row = page.locator('[data-scan-panel="airead"] [data-scan-cat="Content fidelity"]');
    await expect(row).toBeVisible();
    await expect(row).toBeFocused();
    expect(await row.evaluate((el) => getComputedStyle(el).outlineStyle)).toBe('solid');
    // The accessibility step lands on the row that lists the rules it names.
    await page.locator('[data-scan-step="accessibility"] button').click();
    const a11y = page.locator('[data-scan-panel="airead"] [data-scan-a11y]');
    await expect(a11y).toBeFocused();
    await expect(a11y.getByRole('heading', { level: 3 })).toHaveText(
      'Accessibility: 7 issues found by automated checks',
    );
    await expect(a11y.locator('li')).toHaveText([
      'Images must have alternative text (12 elements)',
      'Elements must meet minimum color contrast ratio thresholds (9 elements)',
      '<html> element must have a lang attribute (1 element)',
      'and 4 more',
    ]);
    await page.locator('[data-scan-step="seo"] button').click();
    await expect(page.locator('[data-scan-panel="airead"]')).toBeHidden();
    await expect(
      page.locator('[data-scan-panel="risk"] [data-risk-row="seoDefects"]'),
    ).toBeFocused();
    // Three clicks: three step events and no tile event, whether the panel
    // was closed (first and third) or already open (second).
    const events = await trackedEvents(page);
    expect(events.filter((e) => e.name === 'scan-step-open').map((e) => e.data)).toEqual([
      { step: 'content' },
      { step: 'accessibility' },
      { step: 'seo' },
    ]);
    expect(events.filter((e) => e.name === 'scan-tile-open')).toEqual([]);
    // A tile opened by hand still counts as a tile.
    await page.click('[data-scan-tile="speed"]');
    expect(
      (await trackedEvents(page)).filter((e) => e.name === 'scan-tile-open').map((e) => e.data),
    ).toEqual([{ pillar: 'speed' }]);
  });

  test('a panel with no row for the step takes the focus itself, visibly', async ({ page }) => {
    await mockScan(page, 'clean');
    await startScan(page);
    await page.locator('[data-scan-step="speed"] button').click();
    const panel = page.locator('[data-scan-panel="speed"]');
    await expect(panel).toBeFocused();
    // A whole panel is shown from its top, not from its middle.
    await expect
      .poll(async () => {
        const box = await panel.boundingBox();
        return box!.y >= 0 && box!.y < page.viewportSize()!.height / 2;
      })
      .toBe(true);
    expect(await panel.evaluate((el) => getComputedStyle(el).outlineStyle)).toBe('solid');
  });

  test('a late speed result changes the count, not the steps being read', async ({ page }) => {
    await mockScan(page, 'full', { psiDelay: 1500 });
    await startScan(page, false);
    await expect(page.locator('[data-scan-tile="airead"]')).not.toHaveAttribute(
      'data-state',
      'checking',
      { timeout: 15000 },
    );
    const before = await steps(page);
    expect(before.map((s) => s.text)).toEqual([CONTENT, TITLE, A11Y]);
    await expect(page.locator('[data-scan-health]')).toHaveText('Checking 1 of 3 areas…');
    await page.locator('#scan-lead-email').focus();
    await expect(page.locator('[data-scan-health]')).toHaveText(
      '4 things to improve. The first three are below.',
      { timeout: 15000 },
    );
    expect(await steps(page)).toEqual(before);
    await expect(page.locator('#scan-lead-email')).toBeFocused();
    await expect(page.locator('[data-scan-live]')).not.toContainText('next steps');
  });

  test('a late speed result is added after the steps shown when there is room', async ({
    page,
  }) => {
    await mockScan(page, 'sample-builder', { psiDelay: 1500 });
    await startScan(page, false);
    await expect(page.locator('[data-scan-step]')).toHaveCount(1, { timeout: 15000 });
    await expect(page.locator('[data-scan-step]')).toHaveCount(2, { timeout: 15000 });
    expect(await steps(page)).toEqual([
      { text: A11Y_BUILDER, tag: 'in your site editor', show: 'Show in AI readability' },
      {
        text: 'Speed is poor on mobile. On Wix the parts you control are images, fonts and added apps.',
        tag: 'in your site editor',
        show: 'Show in Speed',
      },
    ]);
    await expect(page.locator('[data-scan-health]')).toHaveText(
      '2 things to improve. Start with the steps below.',
    );
  });

  test('a hosted result asks for nothing the owner cannot do there', async ({ page }) => {
    await mockScan(page, 'hosted');
    await startScan(page);
    const list = await steps(page);
    expect(list).toEqual([
      {
        text: 'Most of this page’s text only appears after JavaScript runs. On Wix you cannot change how pages are served; check that the main text is in standard text blocks, not in an embedded app or widget.',
        tag: 'in your site editor',
        show: 'Show in AI readability',
      },
      {
        text: 'An app or script changes this page’s title after it loads. Set the title in the page’s SEO settings and remove the script that overwrites it.',
        tag: 'in your site settings',
        show: 'Show in Risk & SEO',
      },
      { text: A11Y, tag: 'in your site editor', show: 'Show in AI readability' },
    ]);
    await expect(page.locator('[data-scan-steps]')).not.toContainText(
      /needs a developer|needs server access|the server sends/,
    );
    await expect(page.locator('[data-scan-fit-text]')).toHaveText(
      'This site runs on Wix, a hosted platform. mod_pagespeed installs on a web server you run yourself, so it cannot be used for this site. The findings below still apply.',
    );
    await expect(page.locator('[data-scan-fit-links]')).toHaveText(
      'Wix help pages: Wix help: page SEO settings, Wix help: robots.txt.',
    );
    // Without the statement the guide is linked from the notice.
    await expect(page.locator('[data-scan-fit-guide]')).toBeVisible();
    await expect(page.locator('[data-scan-readable-guide]')).toHaveCount(0);
    // Only the agents topic is offered, so no status note is shown.
    await expect(page.locator('[data-scan-topics-note]')).toBeHidden();
  });

  test('a readable page on a hosted builder says so, above its one step', async ({ page }) => {
    await mockScan(page, 'sample-builder', { speed: 'good' });
    await startScan(page);
    const readable = page.locator('[data-scan-readable]');
    await expect(readable.locator('[data-scan-readable-text]')).toHaveText(READABLE);
    expect(await steps(page)).toEqual([
      { text: A11Y_BUILDER, tag: 'in your site editor', show: 'Show in AI readability' },
    ]);
    // The statement is not a step and is not counted.
    await expect(page.locator('[data-scan-health]')).toHaveText(
      '1 thing to improve. Start with the step below.',
    );
    await expect(page.locator('[data-scan-steps]')).not.toContainText(/cite|rank|mention/i);
    // The guide is linked once: beside the statement, not in the notice as well.
    const guide = readable.locator('[data-scan-readable-guide]');
    await expect(guide).toBeVisible();
    await expect(guide).toHaveText('What you can change on a hosted builder');
    await expect(guide).toHaveAttribute('href', '/ai-readability/hosted-builders/');
    await expect(page.locator('[data-scan-fit-guide]')).toBeHidden();
    await expect(
      page.locator('[data-scan-results] a[href="/ai-readability/hosted-builders/"]:visible'),
    ).toHaveCount(1);
    // The step leads to a row that lists both rules.
    await page.locator('[data-scan-step="accessibility"] button').click();
    await expect(page.locator('[data-scan-a11y] li')).toHaveText([
      'Elements must meet minimum color contrast ratio thresholds (2 elements)',
      'Heading levels should only increase by one (3 elements)',
    ]);
  });

  test('a hand-built site gets one step for its missing head tags, tagged for a CMS', async ({
    page,
  }) => {
    await mockScan(page, 'sample-static', { speed: 'good' });
    await startScan(page);
    const list = await steps(page);
    expect(list[0]).toEqual({
      text: 'This page has no meta description and no canonical link. Add them.',
      tag: 'in your CMS or page template',
      show: 'Show in Risk & SEO',
    });
    expect(list.map((s) => s.tag)).toEqual(Array(3).fill('in your CMS or page template'));
    await expect(page.locator('[data-scan-readable-text]')).toHaveText(READABLE);
    await expect(page.locator('[data-scan-readable-guide]')).toBeHidden();
    await expect(page.locator('[data-scan-health]')).toHaveText(
      '3 things to improve. Start with the steps below.',
    );
  });

  test('a clean result says nothing is urgent and keeps the areas sentence', async ({ page }) => {
    await mockScan(page, 'sample-product', { speed: 'good' });
    await startScan(page);
    await expect(page.locator('[data-scan-step]')).toHaveCount(0);
    await expect(page.locator('[data-scan-readable-text]')).toHaveText(READABLE);
    await expect(page.locator('[data-scan-steps-none]')).toHaveText(
      'Nothing urgent on this page. Open a panel for the details.',
    );
    await expect(page.locator('[data-scan-health]')).toHaveText(
      'None of the 3 areas checked needs attention.',
    );
  });

  test('a page the scan could not read gets a plain summary, not a count', async ({ page }) => {
    await mockScan(page, 'blocked', { speed: 'good' });
    await startScan(page);
    await expect(page.locator('[data-scan-health]')).toHaveText(
      'We could read almost nothing on this page.',
    );
    expect((await steps(page)).map((s) => s.text)).toEqual([
      'The scan found almost no text on this page, even after running JavaScript. Check that the page is public and does not block automated visitors, then scan it again.',
      'Your robots.txt blocks 9 AI crawlers. Remove those rules if you want AI crawlers to read the site.',
    ]);
  });

  test('the topics carry plain labels and a note about what the marks mean', async ({ page }) => {
    await mockScan(page, 'full');
    await startScan(page);
    await expect(page.locator('[data-scan-chip="tollbooth"]')).toHaveText(
      'Requiring a license token from AI crawlers (experimental) (flagged by your scan)',
    );
    await expect(page.locator('[data-scan-chip="agents"]')).toHaveText('AI agents and crawlers');
    await expect(page.locator('[data-scan-topics-note]')).toHaveText(
      'Experimental features are off by default and may change. Planned and not offered yet mean not available today. We will tell you where each one stands.',
    );
  });
});

test.describe('the wait', () => {
  test('counts the seconds in text, also with reduced motion, outside the live region', async ({
    page,
  }) => {
    await page.emulateMedia({ reducedMotion: 'reduce' });
    await mockScan(page, 'full', { scanDelay: 2600, psiDelay: 2600 });
    await startScan(page, false);
    const elapsed = page.locator('[data-scan-elapsed]');
    await expect(elapsed).toHaveText('0 s');
    await expect(elapsed).toHaveText('2 s', { timeout: 5000 });
    expect(await elapsed.evaluate((el) => !!el.closest('[aria-live], [role="status"]'))).toBe(
      false,
    );
    await expect(page.locator('[data-scan-live]')).not.toContainText(/\d s$/);
    // The count ends with the wait.
    await expect(elapsed).toHaveText('', { timeout: 15000 });
  });
});

test.describe('the result on a phone', () => {
  test('the quick-message button never covers a tile’s details control at 390x844', async ({
    page,
  }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await mockScan(page, 'full');
    await startScan(page);
    const launcher = page.locator('[data-qm-launcher]');
    await expect(launcher).toBeVisible();
    for (const id of ['speed', 'airead', 'risk']) {
      const toggle = page.locator(`[data-scan-tile="${id}"] .scan-toggle-closed`);
      // Bring the control level with the fixed button, where it would be covered.
      // The page may still be scrolling to the result, so try until it holds.
      const handle = await launcher.elementHandle();
      const measure = async () => {
        await toggle.evaluate((el, target) => {
          const box = el.getBoundingClientRect();
          const to = (target as Element).getBoundingClientRect();
          window.scrollBy({
            top: box.top + box.height / 2 - (to.top + to.height / 2),
            behavior: 'instant',
          });
        }, handle);
        const a = (await toggle.boundingBox())!;
        const b = (await launcher.boundingBox())!;
        return { a, b, level: a.y < b.y + b.height && b.y < a.y + a.height };
      };
      // The first tiles may sit too high on the page to scroll level with it.
      if (id === 'risk') {
        await expect
          .poll(async () => (await measure()).level, {
            message: 'the control is level with the button',
          })
          .toBe(true);
      }
      const { a, b } = await measure();
      const apart = a.x + a.width <= b.x || b.x + b.width <= a.x;
      expect(apart, `${id}: the two boxes do not intersect`).toBe(true);
    }
  });

  // The grade must not be pushed off the first screen by what sits above the tiles.
  for (const fixture of [
    'full',
    'hosted',
    'clean',
    'blocked',
    'sample-builder',
    'sample-static',
    'sample-product',
  ] as Fixture[]) {
    test(`the first tile is on the first screen at 390x844 (${fixture})`, async ({ page }) => {
      await page.setViewportSize({ width: 390, height: 844 });
      await mockScan(page, fixture);
      await startScan(page);
      // Measured from the top of the result, where the page scrolls to when a scan starts.
      const { tile, steps, tiles } = await page.evaluate(() => {
        const top = document.querySelector('[data-scan-results]')!.getBoundingClientRect().top;
        const box = (q: string) => document.querySelector(q)!.getBoundingClientRect();
        return {
          tile: box('[data-scan-tile="speed"]').bottom - top,
          tiles: box('[data-scan-tile="risk"]').bottom - top,
          steps: box('[data-scan-steps]').top - top,
        };
      });
      expect(tile).toBeLessThanOrEqual(844 * 0.75);
      // The steps come right after the last tile.
      expect(steps).toBeGreaterThanOrEqual(tiles);
      expect(steps - tiles).toBeLessThanOrEqual(16);
    });
  }
});
