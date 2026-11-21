// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { readFileSync } from 'node:fs';
import AxeBuilder from '@axe-core/playwright';
import { test, expect, type Page } from '@playwright/test';
import { stubUmami, trackedEvents } from './helpers/umami';

// What a scan result offers depends on where the site runs. A site on a hosted
// platform gets a note and nothing that needs a server the visitor runs; any
// other result, and a result without a platform reading, gets one line and
// the offers it always had. The scanner, PageSpeed Insights and the contact
// endpoint are mocked; the real services are never called.

const read = (name: string) =>
  readFileSync(new URL(`./fixtures/scan/${name}.json`, import.meta.url), 'utf8');

const GATED = [
  'tollbooth',
  'agentpass',
  'compliancefix',
  'consent-enforcement',
  'page-integrity',
  'third-party-freeze',
  'edge-seo',
  'response-firewall',
  'markdown-origin',
];

const NOTICE =
  'This site runs on Wix, a hosted platform. mod_pagespeed installs on a web server you run yourself, so it cannot be used for this site. The findings below still apply. Three things you can do in Wix:';
const SPOKEN =
  'This site runs on Wix, a hosted platform; mod_pagespeed cannot be used here. A note under the heading says what you can do.';
const LINE =
  'The fixes and offers below that use mod_pagespeed apply only if you run the web server yourself.';

type Posted = Array<Record<string, unknown>>;

async function mock(
  page: Page,
  report: 'hosted' | 'unknown' | 'full' | object,
  delay: { psi?: number; scan?: number } = {},
): Promise<Posted> {
  const posted: Posted = [];
  const wait = (ms?: number) => (ms ? new Promise((done) => setTimeout(done, ms)) : null);
  await page.route('**/psi/v5/runPagespeed**', async (route) => {
    await wait(delay.psi);
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: read(route.request().url().includes('strategy=desktop') ? 'psi-desktop' : 'psi-mobile'),
    });
  });
  await page.route('**/ai-readability/api/scan**', async (route) => {
    await wait(delay.scan);
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: typeof report === 'string' ? read(`report-${report}`) : JSON.stringify(report),
    });
  });
  await page.route('**/ai-readability/api/contact', async (route) => {
    posted.push(route.request().postDataJSON() as Record<string, unknown>);
    await route.fulfill({ status: 200, contentType: 'application/json', body: '{"ok":true}' });
  });
  return posted;
}

async function runScan(page: Page) {
  await stubUmami(page);
  await page.goto('/ai-readability/');
  await page.waitForLoadState('networkidle');
  await page.fill('#scan-url', 'studio.example.org');
  await page.click('#scan-submit');
  for (const tile of ['speed', 'airead']) {
    await expect(page.locator(`[data-scan-tile="${tile}"] [data-scan-status]`)).not.toHaveText(
      'Checking…',
      { timeout: 15000 },
    );
  }
  await expect(page.locator('[data-scan-chip="agents"]')).toBeVisible();
}

const chipIds = (page: Page) =>
  page
    .locator('[data-scan-chip]')
    .evaluateAll((els) => els.map((e) => (e as HTMLElement).dataset.scanChip));

async function open(page: Page, pillar: 'speed' | 'airead' | 'risk') {
  const panel = page.locator(`[data-scan-panel="${pillar}"]`);
  if (!(await panel.isVisible())) await page.click(`[data-scan-tile="${pillar}"]`);
  await expect(panel).toBeVisible();
  return panel;
}

test.describe('scan result on a hosted platform', () => {
  test('shows the note under the heading and reads it out once', async ({ page }) => {
    await mock(page, 'hosted');
    await runScan(page);
    const notice = page.locator('[data-scan-fit-notice]');
    await expect(notice).toBeVisible();
    await expect(notice.locator('[data-scan-fit-text]')).toHaveText(NOTICE);
    const actions = notice.locator('[data-scan-fit-actions] li');
    await expect(actions).toHaveCount(3);
    await expect(actions.first()).toHaveText(
      'Put your services, location and contact details in page text, not in images.',
    );
    await expect(actions.nth(2)).toContainText(
      'Open the robots.txt editor in the SEO section of your Wix dashboard and check that the file does not block AI crawlers.',
    );
    // The vendor help opens in a new tab and says so.
    const links = notice.locator('a');
    await expect(links).toHaveCount(2);
    for (const link of await links.all()) {
      expect(await link.getAttribute('href')).toMatch(/^https:\/\/support\.wix\.com\//);
      await expect(link).toHaveAttribute('target', '_blank');
      await expect(link).toHaveAttribute('rel', 'noopener noreferrer');
      await expect(link).toHaveAccessibleName(/^Wix help: .+ \(opens in new tab\)$/);
    }
    // It is a note, not an alert, and it sits before the tiles.
    await expect(notice).not.toHaveAttribute('role', /.+/);
    expect(
      await notice.evaluate(
        (el) =>
          !!(
            el.compareDocumentPosition(document.querySelector('[data-scan-tiles]')!) &
            Node.DOCUMENT_POSITION_FOLLOWING
          ),
      ),
    ).toBe(true);
    await expect(page.locator('[data-scan-fit-line]')).toBeHidden();
  });

  for (const [label, delay] of [
    ['the scan lands last', { scan: 600 }],
    ['Speed lands last', { psi: 600 }],
  ] as const) {
    test(`reads one short sentence out, once, when ${label}`, async ({ page }) => {
      await mock(page, 'hosted', delay);
      await page.addInitScript(() => {
        const w = window as unknown as { __said: string[] };
        w.__said = [];
        document.addEventListener('DOMContentLoaded', () => {
          const live = document.querySelector('[data-scan-live]');
          if (!live) return;
          new MutationObserver(() => {
            const text = live.textContent ?? '';
            if (text && w.__said.at(-1) !== text) w.__said.push(text);
          }).observe(live, { childList: true, characterData: true, subtree: true });
        });
      });
      await runScan(page);
      // It is the last thing said, so no later result replaced it unread.
      await expect(page.locator('[data-scan-live]')).toContainText(SPOKEN);
      const said = await page.evaluate(() => (window as unknown as { __said: string[] }).__said);
      expect(said.filter((t) => t.includes(SPOKEN))).toHaveLength(1);
      expect(said.some((t) => t.includes('Three things'))).toBe(false);
    });
  }

  test('shows no server offer while the scan is on its way', async ({ page }) => {
    await mock(page, 'hosted', { scan: 1500 });
    await stubUmami(page);
    await page.goto('/ai-readability/');
    await page.waitForLoadState('networkidle');
    await page.fill('#scan-url', 'studio.example.org');
    await page.click('#scan-submit');
    await expect(page.locator('[data-scan-tile="speed"] [data-scan-status]')).not.toHaveText(
      'Checking…',
      { timeout: 15000 },
    );
    // Speed is in, the scan is not: nothing that may have to vanish is shown.
    await expect(page.locator('[data-scan-results]')).toHaveAttribute(
      'data-server-offers',
      'pending',
    );
    await expect(page.locator('[data-scan-cta="download"]')).toBeHidden();
    await expect(page.locator('[data-scan-cta="support"]')).toBeHidden();
    await expect(page.locator('[data-scan-fit-notice]')).toBeHidden();
    const speed = await open(page, 'speed');
    await expect(speed.getByRole('heading', { name: 'Flagged audits' })).toBeVisible();
    await expect(speed.getByRole('heading', { name: 'Fixes on your server' })).toBeHidden();
    await expect(speed.getByRole('button', { name: 'Show the configuration' })).toBeHidden();
    await expect(page.locator('[data-scan-results]')).toHaveAttribute('data-server-offers', 'off', {
      timeout: 15000,
    });
    await expect(page.locator('[data-scan-cta="download"]')).toBeHidden();
  });

  test('offers nothing that needs a server the visitor runs', async ({ page }) => {
    await mock(page, 'hosted');
    await runScan(page);
    await expect(page.locator('[data-scan-audit]')).toBeHidden();
    await expect(page.locator('[data-scan-cta="audit"]')).toBeHidden();
    await expect(page.locator('[data-scan-next]')).toBeHidden();
    await expect(page.locator('[data-scan-cta="download"]')).toBeHidden();
    await expect(page.locator('[data-scan-cta="support"]')).toBeHidden();
    // Not the speed-help topic either: the speed fixes are server fixes.
    expect(await chipIds(page)).toEqual(['agents']);
    // The share row is not an offer: it stays.
    await expect(page.locator('[data-scan-download]')).toBeVisible();
  });

  test('keeps the findings in the panels without the server sentences', async ({ page }) => {
    await mock(page, 'hosted');
    await runScan(page);

    const risk = await open(page, 'risk');
    await expect(risk.locator('[data-risk-row]')).toHaveCount(4);
    await expect(risk.locator('[data-risk-building]')).toHaveCount(0);
    await expect(risk.locator('a')).toHaveCount(0);
    await expect(risk.locator('[data-risk-row="scriptInventory"] [data-risk-sentence]')).toHaveText(
      'Some scripts on this page need attention. 29 external scripts: 27 from Wix’s own hosts, 2 from third parties. Heuristic host list; not an assessment against any standard.',
    );

    const airead = await open(page, 'airead');
    const access = airead.locator('.scan-airead-cat', { hasText: 'Crawler access' });
    await expect(access.locator('[data-scan-crawler-rules]')).toHaveText(
      'robots.txt lets all 21 AI crawlers we check fetch the home page (/). Rules for other paths are not read.',
    );
    // The row says one thing about robots.txt.
    await expect(access).not.toContainText('robots.txt allows AI crawlers');
    const found = airead.locator('[data-scan-also-found] li');
    await expect(found.first()).toHaveText(
      'About 38% of this page’s content is in the raw HTML, and robots.txt lets AI crawlers fetch it.',
    );
    await expect(airead.locator('[data-scan-also-found]')).not.toContainText(
      /your own server|self-hosted|impostor|mod_pagespeed/,
    );
    await expect(airead.locator('[data-scan-also-found] a')).toHaveCount(0);

    const speed = await open(page, 'speed');
    // Speed shows what PageSpeed Insights measured and nothing about the product.
    await expect(speed.getByRole('heading', { name: 'Flagged audits' })).toBeVisible();
    await expect(speed.getByRole('heading', { name: 'Fixes on your server' })).toBeHidden();
    await expect(speed.getByRole('button', { name: 'Show the configuration' })).toBeHidden();
    const flagged = Number(
      /flagged (\d+) performance audits? on this page\./.exec(await speed.innerText())?.[1],
    );
    expect(flagged).toBeGreaterThan(0);
    await expect(speed.locator('[data-speed-flagged] li')).toHaveCount(flagged);
    const shown = await speed.innerText();
    for (const word of ['mod_pagespeed', 'addressed', 'address', 'Partial fix', 'Test first']) {
      expect(shown).not.toContain(word);
    }
    // No filter-name button is reachable.
    expect(
      await speed
        .locator('button')
        .evaluateAll((els) => els.filter((e) => (e as HTMLElement).offsetParent !== null).length),
    ).toBe(0);
  });

  test('a general question can still be sent, with no flagged topics', async ({ page }) => {
    const posted = await mock(page, 'hosted');
    await runScan(page);
    await page.fill('form[data-scan-lead] input[name="email"]', 'owner@example.org');
    await page.click('form[data-scan-lead] button[type="submit"]');
    await expect(page.getByText('Your question was sent.', { exact: true })).toBeVisible();
    expect(posted).toHaveLength(1);
    expect(posted[0].topic).toBe('general');
    expect(posted[0].topics).toEqual([{ id: 'general', wedge: null, chosen: true }]);
    expect(posted[0].lensSignals).toEqual({});
    expect(posted[0].facts).toMatchObject({
      platform: 'wix',
      platformName: 'Wix',
      originControl: 'none',
    });
    const scan = (await trackedEvents(page)).find((e) => e.name === 'ai-scan');
    expect(scan?.data).toMatchObject({ platform: 'wix', origin_control: 'none' });
  });

  test('a new scan of another site brings the offers back', async ({ page }) => {
    await mock(page, 'hosted');
    await runScan(page);
    await expect(page.locator('[data-scan-fit-notice]')).toBeVisible();
    await page.unroute('**/ai-readability/api/scan**');
    await page.route('**/ai-readability/api/scan**', (route) =>
      route.fulfill({
        status: 200,
        contentType: 'application/json',
        body: read('report-unknown'),
      }),
    );
    await page.fill('#scan-url', 'shop.example.com');
    await page.click('#scan-submit');
    await expect(page.locator('[data-scan-fit-line]')).toHaveText(LINE, { timeout: 15000 });
    await expect(page.locator('[data-scan-fit-notice]')).toBeHidden();
    await expect(page.locator('[data-scan-next]')).toBeVisible();
    await expect(page.locator('[data-scan-chip="audit"]')).toBeVisible();
  });

  for (const [label, width, height] of [
    ['390px', 390, 844],
    ['desktop', 1280, 800],
  ] as const) {
    test(`axe-core WCAG 2.2 AA scan of the hosted result at ${label}`, async ({ page }) => {
      await page.setViewportSize({ width, height });
      await mock(page, 'hosted');
      await runScan(page);
      await open(page, 'airead');
      // No horizontal scrolling at this width.
      expect(
        await page.evaluate(
          () => document.documentElement.scrollWidth <= document.documentElement.clientWidth,
        ),
      ).toBe(true);
      const results = await new AxeBuilder({ page })
        .include('[data-scan-results]')
        .withTags(['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa', 'wcag22aa'])
        .analyze();
      // The same bar as the site-wide scan: critical and serious findings fail.
      // The three tile buttons are left out of one rule: their accessible name
      // ("Show Speed details") differs from their visible text by design of the
      // tiles, which this scan is not about.
      const findings = results.violations
        .filter((v) => v.impact === 'critical' || v.impact === 'serious')
        .flatMap((v) => v.nodes.map((n) => `${v.id}: ${n.target.join(' ')}`))
        .filter((f) => !/^label-content-name-mismatch: #scan-tile-(speed|airead|risk)$/.test(f));
      expect(findings).toEqual([]);
    });
  }
});

test.describe('scan result without a hosted platform', () => {
  test('a failed scan shows neither the line nor the note', async ({ page }) => {
    await mock(page, 'full');
    await page.unroute('**/ai-readability/api/scan**');
    await page.route('**/ai-readability/api/scan**', (route) =>
      route.fulfill({
        status: 503,
        contentType: 'application/json',
        body: JSON.stringify({ error: 'scanner unavailable' }),
      }),
    );
    await runScan(page);
    await expect(page.locator('[data-scan-fit-line]')).toHaveText('');
    await expect(page.locator('[data-scan-fit-notice]')).toBeHidden();
    await expect(page.locator('[data-scan-audit]')).toBeHidden();
    expect(await chipIds(page)).toEqual(['consulting', 'agents']);
  });

  for (const [label, report] of [
    ['an unknown platform', 'unknown'],
    ['no platform reading', 'full'],
  ] as const) {
    test(`${label}: the muted line and the usual offers`, async ({ page }) => {
      await mock(page, report);
      await runScan(page);
      await expect(page.locator('[data-scan-fit-line]')).toHaveText(LINE);
      await expect(page.locator('[data-scan-fit-notice]')).toBeHidden();
      await expect(page.locator('[data-scan-audit]')).toBeVisible();
      await expect(page.locator('[data-scan-cta="download"]')).toBeVisible();
      await expect(page.locator('[data-scan-cta="support"]')).toBeVisible();
      expect(await chipIds(page)).toEqual([...GATED, 'consulting', 'agents', 'audit']);

      const risk = await open(page, 'risk');
      await expect(
        risk.locator('[data-risk-row="scriptInventory"] [data-risk-sentence]'),
      ).toHaveText(
        'Some scripts on this page need attention. Heuristic host list; not an assessment against any standard.',
      );
      expect(await risk.locator('a').count()).toBeGreaterThan(0);

      const airead = await open(page, 'airead');
      const access = airead.locator('.scan-airead-cat', { hasText: 'Crawler access' });
      const first = airead.locator('[data-scan-also-found] li').first();
      if (report === 'unknown') {
        // robots.txt blocks some crawlers: every line on the page agrees.
        await expect(access.locator('[data-scan-crawler-rules]')).toHaveText(
          'robots.txt blocks 2 of 8 AI crawlers we check for training data and 1 of 13 that fetch pages to answer questions from the home page (/): GPTBot, ClaudeBot, OAI-SearchBot.',
        );
        await expect(access).not.toContainText('robots.txt allows AI crawlers');
        await expect(first).toHaveText(
          'About 38% of this page’s content is in the raw HTML, but your origin can’t tell a real signed agent from an impostor.',
        );
        await expect(airead).not.toContainText(/crawlers? (is|are) allowed/);
      } else {
        // No robots.txt reading in the result: the row is the scanner's own.
        await expect(airead.locator('[data-scan-crawler-rules]')).toHaveCount(0);
        await expect(first).toHaveText(
          'About 38% of this page’s content is in the raw HTML, and AI crawlers are allowed to read it, but your origin can’t tell a real signed agent from an impostor.',
        );
      }

      const speed = await open(page, 'speed');
      await expect(speed.getByRole('heading', { name: 'Fixes on your server' })).toBeVisible();
      await expect(speed.getByRole('heading', { name: 'Flagged audits' })).toBeHidden();
      await expect(speed.getByRole('button', { name: 'Show the configuration' })).toBeVisible();
      await expect(speed).toContainText(/mod_pagespeed addresses \d+ of \d+ flagged audits/);
      await expect(speed.locator('[data-speed-flagged]')).toBeHidden();
    });
  }

  for (const width of [320, 390, 640, 768, 1280]) {
    test(`the tiles do not move when the result lands at ${width} px`, async ({ page }) => {
      await page.setViewportSize({ width, height: 900 });
      await mock(page, 'unknown', { scan: 1200 });
      await stubUmami(page);
      await page.goto('/ai-readability/');
      await page.waitForLoadState('networkidle');
      await page.fill('#scan-url', 'shop.example.com');
      await page.click('#scan-submit');
      await expect(page.locator('[data-scan-tile="speed"] [data-scan-status]')).not.toHaveText(
        'Checking…',
        { timeout: 15000 },
      );
      const top = () =>
        page.evaluate(
          () => document.querySelector('[data-scan-tiles]')!.getBoundingClientRect().top + scrollY,
        );
      const before = await top();
      // The offers are out of sight until the result says they apply.
      await expect(page.locator('[data-scan-cta="download"]')).toBeHidden();
      await expect(page.locator('[data-scan-fit-line]')).toHaveText(LINE, { timeout: 15000 });
      await expect(page.locator('[data-scan-cta="download"]')).toBeVisible();
      // Within two pixels: the line has its space before it has its text.
      expect(Math.abs((await top()) - before)).toBeLessThanOrEqual(2);
    });
  }

  test('a result that says the platform could not be read behaves like no reading', async ({
    page,
  }) => {
    const data = JSON.parse(read('report-full'));
    data.report.platform = { status: 'error' };
    await mock(page, data);
    await runScan(page);
    await expect(page.locator('[data-scan-fit-line]')).toHaveText(LINE);
    expect(await chipIds(page)).toEqual([...GATED, 'consulting', 'agents', 'audit']);
  });
});
