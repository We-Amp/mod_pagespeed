// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Copy buttons say what happened through one polite live region, because a
// changed button label alone is not reliably spoken. Covers the code-block
// button, the button docked in a term panel, the install-command button, the
// share "Copy link" button and the contact address button, for a copy that
// works and one the browser refuses. Checked against
// Chromium's accessibility tree.

import { test, expect, type Page, type Locator } from '@playwright/test';
import { axQuery, gotoSettled } from './helpers/ax-tree';

async function spoken(page: Page): Promise<string> {
  const status = await axQuery(page, '#copy-status', 'status');
  expect(status).toHaveLength(1);
  const text = await axQuery(page, '#copy-status', 'StaticText');
  return text.map((t) => t.name).join('');
}

// The accessible name of the focused element, from Chromium's tree.
async function focusedName(page: Page): Promise<string> {
  const cdp = await page.context().newCDPSession(page);
  try {
    const { nodes } = await cdp.send('Accessibility.getFullAXTree');
    const focused = nodes.find((n) =>
      n.properties?.some((p) => p.name === 'focused' && p.value?.value === true),
    );
    return String(focused?.name?.value ?? '');
  } finally {
    await cdp.detach();
  }
}

async function expectSpoken(page: Page, message: string) {
  await expect.poll(() => spoken(page)).toBe(message);
}

const CASES: Array<{ name: string; path: string; button: (page: Page) => Locator }> = [
  {
    name: 'code block',
    path: '/docs/getting-started/',
    button: (page) => page.locator('article pre:not(.term-body) .code-copy-btn').first(),
  },
  {
    name: 'term panel',
    path: '/download/apt-yum/',
    button: (page) => page.locator('.term-panel .term-head .code-copy-btn').first(),
  },
  {
    name: 'install command',
    path: '/download/',
    button: (page) => page.locator('[data-install-copy]:visible').first(),
  },
  {
    name: 'share link',
    path: '/blog/agentic-web-at-the-origin/',
    button: (page) => page.locator('[data-share-copy]').first(),
  },
  {
    name: 'contact address',
    path: '/contact/',
    button: (page) => page.locator('#contact-copy-email'),
  },
];

test.describe('copy buttons announce the result', () => {
  test.beforeEach(async ({ context }) => {
    await context.grantPermissions(['clipboard-read', 'clipboard-write']);
  });

  for (const c of CASES) {
    test(`${c.name}: a copy says "Copied to clipboard"`, async ({ page }) => {
      await gotoSettled(page, c.path);
      const live = page.locator('#copy-status');
      await expect(live).toHaveAttribute('role', 'status');
      await expect(live).toHaveAttribute('aria-live', 'polite');
      expect(await spoken(page)).toBe('');

      const button = c.button(page);
      await button.focus();
      const before = await focusedName(page);
      const tooltip = await button.getAttribute('title');
      expect(before).not.toBe('');
      await page.keyboard.press('Enter');
      await expectSpoken(page, 'Copied to clipboard');
      // The focused button keeps its name (only its icon changes), so the copy
      // is spoken once, by the live region, not again by the button.
      expect(await focusedName(page)).toBe(before);
      if (tooltip !== null) await expect(button).toHaveAttribute('title', tooltip);
      else await expect(button).not.toHaveAttribute('title', /.*/);
      // A second copy in a row is spoken again: the region is cleared and
      // then written anew, rather than left holding the same text.
      await page.evaluate(() => {
        const live = document.getElementById('copy-status')!;
        const seen: string[] = [];
        (window as unknown as { __copySeen: string[] }).__copySeen = seen;
        new MutationObserver(() => seen.push(live.textContent ?? '')).observe(live, {
          childList: true,
          characterData: true,
          subtree: true,
        });
      });
      await page.keyboard.press('Enter');
      await expect
        .poll(() => page.evaluate(() => (window as unknown as { __copySeen: string[] }).__copySeen))
        .toEqual(expect.arrayContaining(['', 'Copied to clipboard']));
      // The message does not stay behind in the page.
      await expect.poll(() => spoken(page), { timeout: 8000 }).toBe('');
    });

    test(`${c.name}: a refused copy says how to copy instead`, async ({ page }) => {
      await page.addInitScript(() => {
        Object.defineProperty(navigator.clipboard, 'writeText', {
          value: () => Promise.reject(new Error('denied')),
        });
        // The share button falls back to the older copy command; refuse that too.
        Object.defineProperty(document, 'execCommand', { value: () => false });
      });
      await gotoSettled(page, c.path);
      const button = c.button(page);
      await button.focus();
      const before = await focusedName(page);
      await page.keyboard.press('Enter');
      await expectSpoken(page, 'Copy failed, select the text to copy');
      expect(await focusedName(page)).toBe(before);
    });
  }
});

test('every copy button speaks through the one shared announcer', async ({ page, context }) => {
  await context.grantPermissions(['clipboard-read', 'clipboard-write']);
  for (const c of CASES) {
    await gotoSettled(page, c.path);
    await page.evaluate(() => {
      const w = window as unknown as { siteAnnounce: (_m: string) => void; __calls: string[] };
      const original = w.siteAnnounce;
      w.__calls = [];
      w.siteAnnounce = (m: string) => {
        w.__calls.push(m);
        original(m);
      };
    });
    await c.button(page).click();
    await expect
      .poll(() => page.evaluate(() => (window as unknown as { __calls: string[] }).__calls))
      .toEqual(['Copied to clipboard']);
  }
});

test('a newer message is not cut short by an older one', async ({ page }) => {
  await gotoSettled(page, '/download/');
  const say = (m: string) =>
    page.evaluate(
      (x) => (window as unknown as { siteAnnounce: (_m: string) => void }).siteAnnounce(x),
      m,
    );
  await say('first');
  await expectSpoken(page, 'first');
  await page.waitForTimeout(3000);
  await say('second');
  await expectSpoken(page, 'second');
  // The first message's clear would have fired by now; the second stays.
  await page.waitForTimeout(2000);
  expect(await spoken(page)).toBe('second');
  await expect.poll(() => spoken(page), { timeout: 8000 }).toBe('');
});

// Buttons whose visible text says "Copied" for a moment: the text is visible
// feedback only, the accessible name stays the same, and the result is spoken
// once, by the shared announcer.
async function spyAnnouncer(page: Page) {
  await page.evaluate(() => {
    const w = window as unknown as { siteAnnounce: (_m: string) => void; __calls: string[] };
    const original = w.siteAnnounce;
    w.__calls = [];
    w.siteAnnounce = (m: string) => {
      w.__calls.push(m);
      original(m);
    };
  });
}
const calls = (page: Page) =>
  page.evaluate(() => (window as unknown as { __calls: string[] }).__calls);

test.describe('visible copy feedback keeps the accessible name', () => {
  test.beforeEach(async ({ context }) => {
    await context.grantPermissions(['clipboard-read', 'clipboard-write']);
  });

  test('contact address: "(copied)" shows, the name stays', async ({ page }) => {
    await gotoSettled(page, '/contact/');
    await spyAnnouncer(page);
    const button = page.locator('#contact-copy-email');
    await button.focus();
    const name = async () =>
      (await axQuery(page, '#contact-copy-email', 'button')).map((b) => b.name);
    expect(await name()).toEqual(['Copy email address']);
    await page.keyboard.press('Enter');
    await expect(button).toHaveText('(copied)');
    expect(await name()).toEqual(['Copy email address']);
    await expect(button).toBeFocused();
    await expect.poll(() => calls(page)).toEqual(['Copied to clipboard']);
    await expect(button).toHaveText('(copy)', { timeout: 4000 });
  });
});
