// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

// The static part of /ai-readability/: what a visitor sees before a scan.
// No scan is started, so nothing here calls the scanner.

test.describe('/ai-readability/ landing', () => {
  test('on a phone the URL field and its submit button are in the first viewport', async ({
    page,
  }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await page.goto('/ai-readability/');
    for (const selector of ['#scan-url', '#scan-submit']) {
      const control = page.locator(selector);
      await expect(control).toBeVisible();
      const box = await control.boundingBox();
      expect(box, selector).not.toBeNull();
      expect(box!.y, `${selector} top`).toBeGreaterThanOrEqual(0);
      expect(box!.y + box!.height, `${selector} bottom`).toBeLessThanOrEqual(844);
      expect(box!.x, `${selector} left`).toBeGreaterThanOrEqual(0);
      expect(box!.x + box!.width, `${selector} right`).toBeLessThanOrEqual(390);
    }
    expect(await page.evaluate(() => window.scrollY)).toBe(0);
  });

  test('the heading, the subhead and the scan box come before the explanatory text', async ({
    page,
  }) => {
    await page.goto('/ai-readability/');
    const h1 = page.locator('h1#airead-title');
    await expect(h1).toHaveText('Can AI crawlers read your page?');
    const top = async (selector: string) => (await page.locator(selector).boundingBox())!.y;
    expect(await top('h1#airead-title')).toBeLessThan(await top('#scan-form'));
    expect(await top('#scan-submit')).toBeLessThan(await top('[data-airead-audience]'));
  });

  test('every question in the FAQPage structured data is on the page with its answer', async ({
    page,
  }) => {
    await page.goto('/ai-readability/');
    const blocks = await page.locator('script[type="application/ld+json"]').allTextContents();
    const nodes = blocks.flatMap((b) => [JSON.parse(b)].flat());
    const faq = nodes.find((n) => n['@type'] === 'FAQPage');
    expect(faq).toBeTruthy();
    expect(faq.mainEntity.length).toBe(10);
    const visible = (await page.locator('main details').allTextContents()).map((t) =>
      t.replace(/\s+/g, ' ').trim(),
    );
    for (const item of faq.mainEntity) {
      const match = visible.find((t) => t.startsWith(item.name));
      expect(match, item.name).toBeTruthy();
      expect(match).toContain(item.acceptedAnswer.text);
    }
  });

  test('the tool links to the crawler cost meter and the speed test in its body', async ({
    page,
  }) => {
    await page.goto('/ai-readability/');
    for (const href of ['/ai-crawler-cost/', '/analyze/']) {
      await expect(page.locator(`main p a[href="${href}"]`)).toHaveCount(1);
    }
  });
});
