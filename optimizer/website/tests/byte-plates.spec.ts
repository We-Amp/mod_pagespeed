// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect, type Page } from '@playwright/test';

// ByteDelta plates must fit their frame at every supported width: a squeezed
// bar column collapses the bars to a hairline and can push the value text
// past the plate border. Checked on the plate itself, on each of its value
// texts (row labels, row numbers, the total), and on the bar column width —
// a 1fr bar track can collapse to 0 while the overflow still hides inside
// the plate's own padding, which scrollWidth alone cannot see.
const WIDTHS = [360, 640, 768, 1024, 1280, 1440];
const MIN_BAR_PX = 24;

async function expectPlatesFit(page: Page, scope: string, where: string) {
  const plates = page.locator(`${scope} [data-ui="byte-delta"]`);
  const count = await plates.count();
  expect(count, `${where}: expected byte-delta plates on the page`).toBeGreaterThan(0);
  for (let i = 0; i < count; i++) {
    const plate = plates.nth(i);
    await expect(plate).toBeVisible();
    // 1px tolerance for subpixel rounding.
    const problems = await plate.evaluate((el, minBarPx) => {
      const bad: string[] = [];
      const check = (node: Element, name: string) => {
        if (node.scrollWidth - node.clientWidth > 1) {
          bad.push(`${name}: scrollWidth ${node.scrollWidth} > clientWidth ${node.clientWidth}`);
        }
      };
      check(el, 'plate');
      el.querySelectorAll<HTMLElement>('.bd-num, .bd-row dt, .bd-total-nums').forEach((node, j) => {
        check(node, `text ${j}`);
      });
      el.querySelectorAll<HTMLElement>('.bd-bar').forEach((node, j) => {
        const w = node.getBoundingClientRect().width;
        if (w < minBarPx) bad.push(`bar ${j}: ${w.toFixed(1)}px < ${minBarPx}px`);
      });
      return bad;
    }, MIN_BAR_PX);
    expect(problems, `${where}, plate ${i}`).toEqual([]);
  }
}

test.describe('byte-delta plates fit their frame', () => {
  for (const width of WIDTHS) {
    test(`/demo/ at ${width}px, every site panel`, async ({ page }) => {
      await page.setViewportSize({ width, height: 900 });
      await page.goto('/demo/');
      await page.evaluate(() => document.fonts.ready);
      for (const site of ['ecommerce', 'blog', 'news', 'portfolio']) {
        await page.locator(`#site-picker button[data-site="${site}"]`).click();
        await expectPlatesFit(page, `#panel-${site}`, `/demo/ ${site} panel @${width}px`);
      }
    });

    test(`/examples/combine_css/ at ${width}px`, async ({ page }) => {
      await page.setViewportSize({ width, height: 900 });
      await page.goto('/examples/combine_css/');
      await page.evaluate(() => document.fonts.ready);
      await expectPlatesFit(page, 'body', `/examples/combine_css/ @${width}px`);
    });
  }
});
