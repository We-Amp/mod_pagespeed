// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import type { Page } from '@playwright/test';

// Replace the umami tracker with a recorder before any page script runs, and
// keep the real tracker from loading over it. Call before page.goto().
export async function stubUmami(page: Page): Promise<void> {
  await page.route('**/umami.we-amp.com/**', (route) => route.abort());
  await page.addInitScript(() => {
    const events: Array<{ name: string; data: unknown }> = [];
    Object.defineProperty(window, '__events', { value: events });
    Object.defineProperty(window, 'umami', {
      value: { track: (name: string, data: unknown) => events.push({ name, data }) },
      writable: false,
    });
  });
}

export async function trackedEvents(
  page: Page,
): Promise<Array<{ name: string; data: Record<string, unknown> }>> {
  return page.evaluate(() => (window as unknown as { __events: never[] }).__events);
}
