// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

// The download page leads with the install command: a platform picker and one
// copyable command directly under the heading, then the integration cards,
// each with an anchor other pages can link to.

const REPOSITORY_COMMAND = 'curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh';

test.describe('/download/ install command', () => {
  test('the first command sits under the heading, inside the first screen', async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await page.goto('/download/');
    const command = page.locator('[data-install-panel="apache"] .install-command');
    await expect(command).toBeVisible();
    await expect(command).toHaveText(REPOSITORY_COMMAND);
    const heading = await page.getByRole('heading', { level: 1 }).boundingBox();
    const box = await command.boundingBox();
    expect(box!.y).toBeGreaterThan(heading!.y + heading!.height);
    expect(box!.y + box!.height).toBeLessThanOrEqual(844);
  });

  test('the platform picker switches the command', async ({ page }) => {
    await page.goto('/download/');
    const picker = page.getByRole('radiogroup', { name: 'Select platform' });
    await expect(picker.getByRole('radio')).toHaveCount(3);
    await expect(picker.getByRole('radio', { name: 'Apache', exact: true })).toBeChecked();

    const panel = (id: string) => page.locator(`[data-install-panel="${id}"]`);
    await expect(panel('apache')).toBeVisible();
    await expect(panel('nginx')).toBeHidden();
    await expect(panel('docker')).toBeHidden();

    await page.locator('label[for="install-tab-docker"]').click();
    await expect(panel('docker')).toBeVisible();
    await expect(panel('apache')).toBeHidden();
    await expect(panel('docker').locator('.install-command')).toContainText(
      'ghcr.io/we-amp/pagespeed-combined:latest',
    );

    // Arrow keys move the choice, as in any radio group.
    await picker.getByRole('radio', { name: 'Docker', exact: true }).press('ArrowLeft');
    await expect(picker.getByRole('radio', { name: 'nginx', exact: true })).toBeChecked();
    await expect(panel('nginx')).toBeVisible();
    await expect(panel('nginx').locator('.install-command')).toHaveText(REPOSITORY_COMMAND);
  });

  test('the copy button copies the shown command and names its event', async ({
    page,
    context,
  }) => {
    await context.grantPermissions(['clipboard-read', 'clipboard-write']);
    await page.goto('/download/');
    for (const [id, event] of [
      ['apache', 'cta_download_cmd_apache'],
      ['nginx', 'cta_download_cmd_nginx'],
      ['docker', 'cta_download_cmd_docker'],
    ]) {
      await page.locator(`label[for="install-tab-${id}"]`).click();
      const panel = page.locator(`[data-install-panel="${id}"]`);
      const button = panel.getByRole('button', { name: 'Copy install command' });
      await expect(button).toBeVisible();
      await expect(button).toHaveAttribute('data-umami-event', event);
      await button.click();
      // The button shows its done state; its name stays the same.
      await expect(button).toHaveAttribute('data-copied', 'true');
      await expect(button).toHaveAccessibleName('Copy install command');
      const copied = await page.evaluate(() => navigator.clipboard.readText());
      expect(copied).toBe(await panel.locator('.install-command').textContent());
      // The prompt is decoration: it is never part of what is copied.
      expect(copied.startsWith('$')).toBe(false);
    }
    // The generic code-block copy button does not attach a second control.
    await expect(page.locator('[data-install-tabs] .code-copy-btn')).toHaveCount(0);
  });

  test('the command works without JavaScript', async ({ browser }) => {
    const context = await browser.newContext({ javaScriptEnabled: false });
    const page = await context.newPage();
    await page.goto('/download/');
    await expect(page.locator('[data-install-panel="apache"]')).toBeVisible();
    await page.locator('label[for="install-tab-nginx"]').click();
    await expect(page.locator('[data-install-panel="nginx"]')).toBeVisible();
    await expect(page.locator('[data-install-panel="apache"]')).toBeHidden();
    await context.close();
  });
});

test.describe('/download/ signposts', () => {
  test('the install commands have an anchor for deep links', async ({ page }) => {
    await page.goto('/download/');
    await expect(page.locator('#install [data-install-tabs]')).toHaveCount(1);
  });

  test('upgraders get a link to the upgrade guide', async ({ page }) => {
    await page.goto('/download/');
    const link = page.locator('a[data-umami-event="cta_download_upgrade"]');
    await expect(link).toHaveAttribute('href', '/docs/upgrade/');
    await expect(link).toContainText('Upgrade guide');
  });

  test('every integration card has its own anchor', async ({ page }) => {
    await page.goto('/download/');
    const anchors: Array<[string, string]> = [
      ['apache', 'Apache'],
      ['nginx', 'nginx'],
      ['docker', 'nginx (Docker)'],
      ['helm', 'Helm'],
      ['iis', 'IIS'],
      ['aspnet-core', 'ASP.NET Core'],
      ['sidecar', 'ASP.NET Core Sidecar'],
    ];
    for (const [id, name] of anchors) {
      const card = page.locator(`.card#${id}`);
      await expect(card).toHaveCount(1);
      await expect(card.getByRole('heading', { level: 3 })).toHaveText(name);
    }
    await expect(page.locator('.card#apache')).toContainText('Install on Apache');
    await expect(page.locator('.card#nginx')).toContainText('Install on nginx');
  });

  test('the nginx walkthrough link stays on this site', async ({ page }) => {
    await page.goto('/download/');
    await expect(page.locator('main a[href*="ngxpagespeed.com"]')).toHaveCount(0);
    await expect(
      page.locator('main').getByRole('link', { name: 'nginx install guide', exact: true }),
    ).toHaveAttribute('href', '/docs/installation-module/#native-nginx-module');
  });
});
