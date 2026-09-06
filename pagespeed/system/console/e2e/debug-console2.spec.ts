// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test } from "@playwright/test";

const BASE = "http://localhost:8080/pagespeed_admin/";

test("debug console page v2", async ({ page }) => {
  // Clear cache
  const context = page.context();
  await context.clearCookies();

  await page.goto(`${BASE}#/console`);
  await page.waitForTimeout(6000);

  // Get ALL text from the main content area
  const allText = await page.evaluate(() => document.body.innerText);
  console.log("=== Full body text (first 1000 chars) ===");
  console.log(allText.substring(0, 1000));

  // Check for specific elements from the NEW console
  const hasSearch = await page.locator('input[placeholder*="Search"]').count();
  const hasTable = await page.locator("table").count();
  const hasSparkline = await page.locator("svg").count();
  console.log(`Search inputs: ${hasSearch}, Tables: ${hasTable}, SVGs: ${hasSparkline}`);

  // Check for OLD console elements
  const hasSelect = await page.locator("select").count();
  const hasPre = await page.locator("pre").count();
  console.log(`Select dropdowns: ${hasSelect}, Pre blocks: ${hasPre}`);

  await page.screenshot({ path: "e2e/screenshots/console-debug.png", fullPage: true });
});
