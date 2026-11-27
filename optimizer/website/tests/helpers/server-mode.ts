// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { expect, type APIRequestContext, type Page } from '@playwright/test';

// The suite runs against the dev server by default, or against a built server
// (`node dist/server/entry.mjs`, reused with PW_REUSE_SERVER=1; see
// src/lib/build-id.mjs). The dev server injects its client script
// (/@vite/client) into every page; a built page never carries it.
export async function isBuiltServer(request: APIRequestContext): Promise<boolean> {
  const html = await (await request.get('/')).text();
  return !html.includes('/@vite/client');
}

// The branded error page (src/components/ErrorContent.astro) for `code`, not
// a redirect stub or a bare server response.
export async function expectErrorPage(page: Page, code: '404' | '500'): Promise<void> {
  const heading = page.locator('#error-heading');
  await expect(heading).toBeVisible();
  await expect(page.locator('section:has(> #error-heading) > p').first()).toHaveText(code);
  await expect(page.locator('meta[http-equiv="refresh"]')).toHaveCount(0);
}
