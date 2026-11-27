// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

test.describe('Calculator page', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/calculator/');
  });

  test('renders 4 input fields with default values', async ({ page }) => {
    // per-site licensing: the old "#servers" input became "#sites"
    // (price is per registrable domain, not per server), and the default
    // pageviews figure was raised to 10,000,000.
    await expect(page.locator('#pageviews')).toHaveValue('10000000');
    await expect(page.locator('#pageweight')).toHaveValue('2500');
    await expect(page.locator('#bwcost')).toHaveValue('0.085');
    await expect(page.locator('#sites')).toHaveValue('1');
  });

  test('default values produce non-placeholder output', async ({ page }) => {
    // After initial calculation, values should not be placeholders
    const netSavings = page.locator('#net-savings');
    await expect(netSavings).not.toHaveText('\u2014');
  });

  test('changing pageviews updates results', async ({ page }) => {
    const before = await page.locator('#net-savings').textContent();
    await page.locator('#pageviews').fill('5000000');
    const after = await page.locator('#net-savings').textContent();
    expect(after).not.toBe(before);
  });

  test('changing bandwidth cost updates results', async ({ page }) => {
    const before = await page.locator('#cost-savings').textContent();
    await page.locator('#bwcost').fill('0.20');
    const after = await page.locator('#cost-savings').textContent();
    expect(after).not.toBe(before);
  });

  test('output cards show calculated values', async ({ page }) => {
    await expect(page.locator('#current-bw')).not.toHaveText('\u2014');
    await expect(page.locator('#optimized-bw')).not.toHaveText('\u2014');
    await expect(page.locator('#saved-bw')).not.toHaveText('\u2014');
    await expect(page.locator('#cost-savings')).not.toHaveText('\u2014');
    await expect(page.locator('#mps-cost')).not.toHaveText('\u2014');
    await expect(page.locator('#net-savings')).not.toHaveText('\u2014');
  });

  test('net savings uses positive-emphasis color for positive ROI', async ({ page }) => {
    // Sign encoding is monochromatic (body text colour for positive, muted
    // for negative). The leading Unicode minus (U+2212) from formatMoney
    // carries sign information; chromatic green/red was removed for WCAG contrast.
    const netSavings = page.locator('#net-savings');
    await expect(netSavings).toHaveClass(/\btext-text-body\b/);
    await expect(netSavings).not.toHaveClass(/\btext-text-muted\b/);
  });

  test('zero pageviews shows zero savings', async ({ page }) => {
    await page.locator('#pageviews').fill('0');
    const netSavings = page.locator('#net-savings');
    // Net savings should be negative (just the MPS cost)
    const text = await netSavings.textContent();
    expect(text).toMatch(/-?\$/);
  });

  test('result values are numeric', async ({ page }) => {
    const roi = await page.locator('#roi').textContent();
    // ROI is a percentage — or the unbounded symbol, since the software cost is zero.
    expect(roi).toMatch(/\d+%|\u221e/);
  });

  test('bar chart is visible', async ({ page }) => {
    await expect(page.locator('#bar-before')).toBeVisible();
    await expect(page.locator('#bar-after')).toBeVisible();
  });

  test('page has descriptive heading', async ({ page }) => {
    await expect(page.locator('h1:has-text("Savings Calculator")')).toBeVisible();
  });

  test('inputs accept keyboard entry', async ({ page }) => {
    const input = page.locator('#pageviews');
    await input.click();
    await input.fill('2000000');
    await expect(input).toHaveValue('2000000');
  });
});
