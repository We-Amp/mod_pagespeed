// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test } from "@playwright/test";
import { LIVE_BASE } from "./support/live";

const BASE = LIVE_BASE;

test("screenshot console large", async ({ page }) => {
  await page.setViewportSize({ width: 1280, height: 900 });
  await page.goto(`${BASE}#/console`);
  await page.waitForTimeout(5000);
  await page.screenshot({
    path: "e2e/screenshots/console-large.png",
    fullPage: true,
  });
});
