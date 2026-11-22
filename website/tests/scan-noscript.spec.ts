// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

// With JavaScript off, a visitor can still send an inquiry from the scan pages
// and the contact page: a native form post to the contact endpoint, answered
// with a redirect to the thanks page. The endpoint is mocked; it is never called.

for (const [route, topic] of [
  ['/analyze/', 'consulting'],
  ['/ai-readability/', 'agents'],
  ['/contact/', 'other'],
] as const) {
  test(`${route} posts an inquiry without JavaScript and lands on the thanks page`, async ({
    browser,
  }) => {
    const context = await browser.newContext({ javaScriptEnabled: false });
    const page = await context.newPage();
    const posts: Array<{ body: string; type: string }> = [];
    await page.route('**/ai-readability/api/contact', (r) => {
      const req = r.request();
      posts.push({ body: req.postData() ?? '', type: req.headers()['content-type'] ?? '' });
      return r.fulfill({ status: 303, headers: { location: '/contact/thanks/' } });
    });
    await page.goto(route);
    const form =
      route === '/contact/'
        ? page.locator('#contact-form')
        : page.locator('form[data-ui="noscript-lead"]');
    await expect(form).toBeVisible();
    await form.getByLabel('Email', { exact: true }).fill('visitor@example.com');
    await form.getByLabel('Name').fill('A visitor');
    await form.getByLabel('Message').fill('Hello from a browser without scripts.');
    if (route === '/contact/') {
      await form.locator('select[name="topic"]').selectOption(topic);
    } else {
      await form.getByLabel('Site address').fill('https://example.com/');
    }
    // Submit with Enter in a field: with scripts disabled Playwright's click
    // never sees the button settle, but Enter is the same native form submit.
    await expect(form.getByRole('button', { name: 'Send message' })).toBeVisible();
    await form.getByLabel('Email', { exact: true }).press('Enter');
    await expect(page).toHaveURL(/\/contact\/thanks\/$/);
    expect(posts).toHaveLength(1);
    expect(posts[0].type).toContain('application/x-www-form-urlencoded');
    const body = new URLSearchParams(posts[0].body);
    expect(body.get('email')).toBe('visitor@example.com');
    expect(body.get('name')).toBe('A visitor');
    expect(body.get('message')).toBe('Hello from a browser without scripts.');
    expect(body.get('topic')).toBe(topic);
    expect(body.get('redirect')).toBe('/contact/thanks/');
    expect(body.get('company')).toBe('');
    if (route !== '/contact/') expect(body.get('url')).toBe('https://example.com/');
    await context.close();
  });
}

for (const route of ['/analyze/', '/ai-readability/']) {
  test(`with JavaScript on, the noscript form is not rendered on ${route}`, async ({ page }) => {
    await page.goto(route);
    await expect(page.locator('form[data-ui="noscript-lead"]')).toHaveCount(0);
  });
}

// The scan box and its results need JavaScript. Without it they are hidden, so
// the only form a visitor sees is the one that works: the noscript inquiry form.
for (const route of ['/analyze/', '/ai-readability/']) {
  test(`${route} shows no scan form without JavaScript, only the inquiry form`, async ({
    browser,
  }) => {
    const context = await browser.newContext({ javaScriptEnabled: false });
    const page = await context.newPage();
    await page.goto(route);
    await expect(page.locator('form[data-scan-box]')).toHaveCount(1);
    await expect(page.locator('form[data-scan-box]')).toBeHidden();
    await expect(page.locator('form[data-scan-lead]')).toBeHidden();
    await expect(page.locator('main form:visible')).toHaveCount(1);
    await expect(page.locator('form[data-ui="noscript-lead"]')).toBeVisible();
    await expect(page.getByRole('heading', { name: 'The scan needs JavaScript' })).toBeVisible();
    await expect(page.getByRole('heading', { level: 1 })).toBeVisible();
    await context.close();
  });
}

// Without JavaScript, /analyze/ keeps no control that needs a script: the only
// visible controls in <main> belong to the inquiry form, which posts natively.
// The highlight cards all show (the default view), and the copy that used to
// point at the hidden analyzer reads right on its own.
test('/analyze/ keeps no script-only control without JavaScript', async ({ browser }) => {
  const context = await browser.newContext({ javaScriptEnabled: false });
  const page = await context.newPage();
  await page.goto('/analyze/');
  const controls = page.locator(
    'main :is(button, input:not([type="hidden"]), select, textarea, [role="button"], [role="tab"]):visible',
  );
  const outside = await controls.evaluateAll((els) =>
    els
      .filter((el) => !el.closest('form[data-ui="noscript-lead"]'))
      .map((el) => el.outerHTML.slice(0, 120)),
  );
  expect(outside).toEqual([]);
  await expect(page.locator('#psi-highlights-toggle')).toBeHidden();
  const cards = page.locator('#psi-highlights-list > li');
  expect(await cards.count()).toBeGreaterThan(0);
  for (const card of await cards.all()) await expect(card).toBeVisible();
  await expect(page.locator('main')).toContainText(
    'No account, no upload. Test a page, get the fixes.',
  );
  await expect(page.locator('main')).toContainText(
    'The report from a test labels every one of these.',
  );
  await context.close();
});

// The scan box is hidden without JavaScript, so no visible text may point at
// it: no "analyzer", and no "above" in the sense of the box above
// ("above-the-fold" is a different word).
for (const route of ['/analyze/', '/ai-readability/']) {
  test(`${route} has no visible text pointing at the hidden scan box without JavaScript`, async ({
    browser,
  }) => {
    const context = await browser.newContext({ javaScriptEnabled: false });
    const page = await context.newPage();
    await page.goto(route);
    const text = await page.locator('main').innerText();
    expect(text).not.toMatch(/analy[sz]er/i);
    expect(text).not.toMatch(/\babove\b(?!-the-fold)/i);
    await context.close();
  });
}
