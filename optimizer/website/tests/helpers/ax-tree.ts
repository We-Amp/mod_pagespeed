// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import type { Page } from '@playwright/test';

export interface AxEntry {
  role: string;
  name: string;
}

// Reads Chromium's own accessibility tree (what a screen reader is given), not
// Playwright's re-computation of it: the nodes with `role` (and, when given,
// exactly this accessible name) under the first element matching
// `rootSelector`. Ignored nodes are left out. A cold dev server reloads the
// page once after its first load, which invalidates the node ids of the
// replaced document, so the read is repeated on a fresh document.
export async function axQuery(
  page: Page,
  rootSelector: string,
  role?: string,
  accessibleName?: string,
): Promise<AxEntry[]> {
  for (let attempt = 0; ; attempt++) {
    await page.waitForLoadState('load');
    const cdp = await page.context().newCDPSession(page);
    try {
      const { root } = await cdp.send('DOM.getDocument', { depth: 0 });
      const { nodeId } = await cdp.send('DOM.querySelector', {
        nodeId: root.nodeId,
        selector: rootSelector,
      });
      if (!nodeId) throw new Error(`axQuery: nothing matches ${rootSelector}`);
      const { nodes } = await cdp.send('Accessibility.queryAXTree', {
        nodeId,
        ...(role ? { role } : {}),
        ...(accessibleName !== undefined ? { accessibleName } : {}),
      });
      return nodes
        .filter((n) => !n.ignored)
        .map((n) => ({ role: String(n.role?.value ?? ''), name: String(n.name?.value ?? '') }));
    } catch (e) {
      if (attempt >= 2 || !String(e).includes('Could not find node')) throw e;
      await page.waitForTimeout(500);
    } finally {
      await cdp.detach().catch(() => undefined);
    }
  }
}

// Opens `path` and waits until the network is quiet, so a cold dev server's
// one-time reload has happened before the test reads the page.
export async function gotoSettled(page: Page, path: string): Promise<void> {
  await page.goto(path);
  await page.waitForLoadState('networkidle');
}
