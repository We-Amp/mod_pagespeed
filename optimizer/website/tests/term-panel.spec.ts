// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

// A term panel's accessible name is its command line (or nothing), never the
// docked copy button's label; a <figcaption> head row would make it so. Hidden
// panels (inactive quick-start tabs) have no accessible name and are skipped.
for (const path of ['/', '/download/apt-yum/']) {
  test(`term panels on ${path} are not named after the copy button`, async ({ page }) => {
    await page.goto(path);
    const panels = page.locator('figure.term-panel');
    const n = await panels.count();
    expect(n).toBeGreaterThan(0);
    for (let i = 0; i < n; i++) {
      const panel = panels.nth(i);
      if (!(await panel.isVisible())) continue;
      await expect(panel).not.toHaveAccessibleName(/^Copy/);
      await expect(panel.locator('figcaption.term-head')).toHaveCount(0);
      const cmd = panel.locator('.term-head span.font-mono');
      if ((await cmd.count()) > 0) {
        const command = ((await cmd.first().textContent()) ?? '').trim();
        if (command) await expect(panel).toHaveAccessibleName(command);
      }
    }
  });
}
