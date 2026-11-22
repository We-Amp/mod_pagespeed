// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect, type Page } from '@playwright/test';
import AxeBuilder from '@axe-core/playwright';
import { mockScan, startScan } from './helpers/scan-mock';

// axe-core (WCAG 2.2 AA) over the rendered scan result with a mocked scanner:
// each panel open in turn, the contact form filled, the email error showing
// and the confirmation, on a phone, a tablet and a desktop. Every other result
// kind is checked at the tablet width with each panel open. Every finding
// fails, whatever its impact, and no rule or element is left out.

async function findings(page: Page): Promise<string[]> {
  // A colour that is still in a transition (a button leaving its disabled
  // state) is not the colour the visitor reads: let transitions finish first.
  await page.evaluate(() =>
    Promise.all(document.getAnimations().map((a) => a.finished.catch(() => null))),
  );
  const results = await new AxeBuilder({ page })
    .include('[data-scan-results]')
    .withTags(['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa', 'wcag22aa'])
    .analyze();
  return results.violations.flatMap((v) => v.nodes.map((n) => `${v.id}: ${n.target.join(' ')}`));
}

for (const fixture of ['full', 'hosted'] as const) {
  for (const [label, width, height] of [
    ['390 px', 390, 844],
    ['768 px', 768, 1024],
    ['desktop', 1280, 800],
  ] as const) {
    test(`axe: the ${fixture} result at ${label}`, async ({ page }) => {
      test.slow();
      await page.setViewportSize({ width, height });
      await mockScan(page, fixture);
      await startScan(page);
      expect(await findings(page), 'all panels closed').toEqual([]);

      for (const id of ['speed', 'airead', 'risk']) {
        await page.click(`[data-scan-tile="${id}"]`);
        await expect(page.locator(`[data-scan-panel="${id}"]`)).toBeVisible();
        expect(await findings(page), `${id} panel open`).toEqual([]);
      }
      // A step moves focus into a panel.
      await page.locator('[data-scan-step] button').first().click();
      expect(await findings(page), 'after a step').toEqual([]);

      // The email error.
      await page.fill('#scan-lead-email', 'not-an-address');
      await page.click('[data-scan-lead] button[type="submit"]');
      await expect(page.locator('[data-scan-lead-email-error]')).toBeVisible();
      expect(await findings(page), 'email error').toEqual([]);

      // The form filled in.
      await page.locator('[data-scan-chip="agents"] input').check();
      await page.fill('#scan-lead-email', 'owner@example.org');
      await page.fill('#scan-lead-sites', '3');
      await page.fill('#scan-lead-note', 'Where do I start?');
      expect(await findings(page), 'form filled').toEqual([]);

      // The confirmation.
      await page.click('[data-scan-lead] button[type="submit"]');
      await expect(page.locator('[data-scan-results]')).toContainText('AI agents and crawlers.');
      await expect(page.locator('[data-scan-lead]')).toBeHidden();
      expect(await findings(page), 'confirmation').toEqual([]);
      expect(
        await page.evaluate(
          () => document.documentElement.scrollWidth <= document.documentElement.clientWidth,
        ),
      ).toBe(true);
    });
  }
}

for (const fixture of [
  'clean',
  'blocked',
  'unknown',
  'sample-builder',
  'sample-static',
  'sample-product',
] as const) {
  test(`axe: the ${fixture} result at 768 px`, async ({ page }) => {
    test.slow();
    await page.setViewportSize({ width: 768, height: 1024 });
    await mockScan(page, fixture);
    await startScan(page);
    expect(await findings(page), 'all panels closed').toEqual([]);
    for (const id of ['speed', 'airead', 'risk']) {
      await page.click(`[data-scan-tile="${id}"]`);
      await expect(page.locator(`[data-scan-panel="${id}"]`)).toBeVisible();
      expect(await findings(page), `${id} panel open`).toEqual([]);
    }
    expect(
      await page.evaluate(
        () => document.documentElement.scrollWidth <= document.documentElement.clientWidth,
      ),
    ).toBe(true);
  });
}
