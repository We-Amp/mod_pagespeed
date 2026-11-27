// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';
import { stubUmami, trackedEvents } from './helpers/umami';

// The QuickMessage widget is mounted in BaseLayout, so it renders on every page.
test.describe('QuickMessage widget', () => {
  test('launcher is present site-wide', async ({ page }) => {
    for (const path of ['/', '/pricing/', '/blog/']) {
      await page.goto(path);
      await expect(page.locator('[data-qm-launcher]')).toBeVisible();
    }
  });

  test('opens on click, focuses the textarea, closes on Escape', async ({ page }) => {
    await page.goto('/');
    const launcher = page.locator('[data-qm-launcher]');
    const panel = page.locator('[data-qm-panel]');

    await expect(panel).toBeHidden();
    await launcher.click();
    await expect(panel).toBeVisible();
    await expect(page.locator('[data-qm-message]')).toBeFocused();
    await expect(launcher).toHaveAttribute('aria-expanded', 'true');

    await page.keyboard.press('Escape');
    await expect(panel).toBeHidden();
    await expect(launcher).toHaveAttribute('aria-expanded', 'false');
    await expect(launcher).toBeFocused();
  });

  test('optional email field is visible from the start and not required', async ({ page }) => {
    await page.goto('/');
    await page.locator('[data-qm-launcher]').click();
    await expect(page.locator('[data-qm-email-wrap]')).toBeVisible();
    await expect(page.locator('[data-qm-email]')).toBeVisible();
    await expect(page.locator('[data-qm-email]')).not.toHaveAttribute('required', /.*/);
    await expect(page.locator('label[for="qm-email"]')).toContainText('optional');
  });

  test('a sent message reports lead_submit next to quick_message_sent', async ({ page }) => {
    await page.route('**/ai-readability/api/contact', async (route) => {
      await route.fulfill({ status: 200, contentType: 'application/json', body: '{"ok":true}' });
    });
    await stubUmami(page);
    await page.goto('/pricing/');
    await page.locator('[data-qm-launcher]').click();
    await page.locator('[data-qm-message]').fill('Hello');
    await page.locator('[data-qm-email]').fill('reply@example.com');
    await page.locator('[data-qm-submit]').click();
    await expect(page.locator('[data-qm-success]')).toBeVisible();
    const events = await trackedEvents(page);
    expect(events).toContainEqual({ name: 'quick_message_sent', data: { with_email: true } });
    expect(events).toContainEqual({
      name: 'lead_submit',
      data: { channel: 'quick', topic: 'quick-message', wedge: '', source_path: '/pricing/' },
    });
  });

  test('sends a one-way message (no email) and shows success', async ({ page }) => {
    let posted: any = null;
    await page.route('**/ai-readability/api/contact', async (route) => {
      posted = route.request().postDataJSON();
      await route.fulfill({ status: 200, contentType: 'application/json', body: '{"ok":true}' });
    });

    await page.goto('/pricing/');
    await page.locator('[data-qm-launcher]').click();
    await page.locator('[data-qm-message]').fill('Does this work behind Cloudflare?');
    await page.locator('[data-qm-submit]').click();

    await expect(page.locator('[data-qm-success]')).toBeVisible();
    expect(posted).toMatchObject({
      message: 'Does this work behind Cloudflare?',
      email: '',
      topic: 'quick-message',
      // PATH ONLY — never the full href / query string.
      url: '/pricing/',
    });
  });

  test('requires a non-empty message before sending', async ({ page }) => {
    let hits = 0;
    await page.route('**/ai-readability/api/contact', async (route) => {
      hits += 1;
      await route.fulfill({ status: 200, contentType: 'application/json', body: '{"ok":true}' });
    });

    await page.goto('/');
    await page.locator('[data-qm-launcher]').click();
    await page.locator('[data-qm-submit]').click();
    await expect(page.locator('[data-qm-status]')).toContainText('Type a message');
    expect(hits).toBe(0);
  });

  test('rejects a malformed email but keeps the message', async ({ page }) => {
    let hits = 0;
    await page.route('**/ai-readability/api/contact', async (route) => {
      hits += 1;
      await route.fulfill({ status: 200, contentType: 'application/json', body: '{"ok":true}' });
    });

    await page.goto('/');
    await page.locator('[data-qm-launcher]').click();
    await page.locator('[data-qm-message]').fill('hello there');
    await page.locator('[data-qm-email]').fill('not-an-email');
    await page.locator('[data-qm-submit]').click();
    await expect(page.locator('[data-qm-status]')).toContainText("doesn't look right");
    expect(hits).toBe(0);
  });

  test('shows a privacy notice naming email delivery + linking the policy', async ({ page }) => {
    await page.goto('/');
    await page.locator('[data-qm-launcher]').click();
    const privacy = page.locator('.qm-privacy');
    await expect(privacy).toContainText('go to We-Amp by email');
    await expect(privacy).not.toContainText('Telegram');
    await expect(privacy.locator('a[href="https://we-amp.com/privacy/"]')).toBeVisible();
  });

  test('the launcher hides while the search dialog is open', async ({ page }) => {
    await page.goto('/');
    const qm = page.locator('[data-qm-launcher]');
    await expect(qm).toBeVisible();
    await page.keyboard.press('/');
    await expect(page.locator('#site-search')).toBeVisible();
    await expect(qm).toBeHidden();
    await page.keyboard.press('Escape');
    await expect(qm).toBeVisible();
  });

  test('at handset width the launcher waits for the first viewport to pass', async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await page.goto('/');
    const qm = page.locator('[data-qm-launcher]');
    await expect(qm).toBeHidden();
    await page.evaluate(() => window.scrollTo(0, window.innerHeight));
    await expect(qm).toBeVisible();
  });

  test('at handset width the launcher steps aside for a copy button in its corner', async ({
    page,
  }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await page.goto('/docs/configuration/');
    const qm = page.locator('[data-qm-launcher]');
    // A code block's copy button below the first viewport.
    const target = await page.evaluate(() => {
      const btns = Array.from(document.querySelectorAll<HTMLElement>('.code-copy-btn'));
      const b = btns.find((el) => el.getBoundingClientRect().top > window.innerHeight * 2);
      if (!b) return null;
      b.setAttribute('data-test-copy', '');
      return true;
    });
    expect(target, 'a copy button below the fold').toBe(true);
    const btn = page.locator('[data-test-copy]');
    const scrollBtnTo = (fromBottom: number) =>
      btn.evaluate((el, d) => {
        const r = el.getBoundingClientRect();
        window.scrollTo({
          top: window.scrollY + r.bottom - (window.innerHeight - d),
          behavior: 'instant',
        });
      }, fromBottom);
    // Past the first viewport with the copy button clear of the corner: the
    // launcher shows.
    await scrollBtnTo(300);
    await expect(qm).toBeVisible();
    // Copy button in the launcher's corner: the launcher is out of the way.
    await scrollBtnTo(24);
    await expect(qm).toBeHidden();
    // Copy button well above the corner: the launcher is back.
    await scrollBtnTo(300);
    await expect(qm).toBeVisible();
  });

  test('the launcher is the only floating control: 40 px, 16 px from the corner', async ({
    page,
  }) => {
    for (const path of ['/', '/docs/configuration/', '/blog/']) {
      await page.goto(path);
      await expect(page.locator('[data-share-fab], #sticky-cta')).toHaveCount(0);
      // Past the first viewport: at handset width the launcher waits for that.
      await page.evaluate(() => window.scrollTo(0, window.innerHeight));
      await expect(page.locator('[data-qm-launcher]')).toBeVisible();
      const box = await page.locator('[data-qm-launcher]').boundingBox();
      const vp = page.viewportSize()!;
      expect(box, path).not.toBeNull();
      expect(Math.round(box!.width)).toBe(40);
      expect(Math.round(box!.height)).toBe(40);
      expect(Math.round(vp.width - (box!.x + box!.width))).toBe(16);
      expect(Math.round(vp.height - (box!.y + box!.height))).toBe(16);
    }
  });

  test('on a very short viewport the launcher is not pinned', async ({ page }) => {
    await page.setViewportSize({ width: 844, height: 390 });
    await page.goto('/pricing/');
    const root = page.locator('[data-qm]');
    expect(await root.evaluate((e) => getComputedStyle(e).position)).not.toBe('fixed');
    // It sits in the page flow after the footer and still opens.
    await page.evaluate(() => window.scrollTo(0, document.body.scrollHeight));
    const launcher = page.locator('[data-qm-launcher]');
    await expect(launcher).toBeVisible();
    const order = await page.evaluate(() => {
      const footer = document.querySelector('[data-site-footer]')!.getBoundingClientRect();
      const qm = document.querySelector('[data-qm-launcher]')!.getBoundingClientRect();
      return qm.top >= footer.bottom - 1;
    });
    expect(order).toBe(true);
    await launcher.click();
    await expect(page.locator('[data-qm-panel]')).toBeVisible();

    // The boundary itself, at handset width: in the flow from the start, so
    // the page does not grow when the first viewport has been scrolled past.
    await page.setViewportSize({ width: 390, height: 480 });
    await page.goto('/');
    expect(await root.evaluate((e) => getComputedStyle(e).position)).not.toBe('fixed');
    expect(await root.evaluate((e) => getComputedStyle(e).display)).not.toBe('none');
  });

  test('the launcher is inert while the mobile menu is open', async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await page.goto('/');
    await page.locator('#mobile-menu-btn').click();
    await expect(page.locator('[data-qm]')).toHaveAttribute('inert', '');
    await page.keyboard.press('Escape');
    await expect(page.locator('[data-qm]')).not.toHaveAttribute('inert', '');
  });
});
