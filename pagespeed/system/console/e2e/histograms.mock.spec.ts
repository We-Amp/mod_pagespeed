// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The Histograms page's readability: units, significant digits, names that
// break between words. Self-contained (support/mock-admin.ts); run after build.sh.

import { expect, test } from "@playwright/test";
import * as F from "./support/fixtures";
import { mockAdmin, openConsole } from "./support/mock-admin";

test.use({ locale: "en-US", timezoneId: "UTC" });

test("each histogram carries its unit and three significant digits", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), histograms: F.HISTOGRAMS });
  await openConsole(page, "pagespeed_admin", "#/histograms");
  await expect(page.getByRole("columnheader", { name: "Unit" })).toBeVisible();
  const us = page.getByRole("row", { name: /Html Time us/ });
  await expect(us.locator("td").nth(1)).toHaveText("µs");
  await expect(us.getByRole("cell", { name: "1,500", exact: true }).first()).toBeVisible(); // avg
  const ms = page.getByRole("row", { name: /Rewrite Latency ms/ });
  await expect(ms.locator("td").nth(1)).toHaveText("ms");
  await expect(ms.getByRole("cell", { name: "12.0", exact: true }).first()).toBeVisible(); // avg
  await expect(ms.getByRole("cell", { name: "—", exact: true }).first()).toBeVisible(); // stddev null
});

test("names wrap between words, not through them", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), histograms: F.HISTOGRAMS });
  await openConsole(page, "pagespeed_admin", "#/histograms");
  await expect(page.locator(".name-cell").first()).toHaveCSS("word-break", "keep-all");
});

test("the shared header carries an auto-refresh toggle at this page's interval", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), histograms: F.HISTOGRAMS });
  await openConsole(page, "pagespeed_admin", "#/histograms");
  const toggle = page.getByRole("button", { name: "Auto-refresh · 10 s" });
  await expect(toggle).toHaveAttribute("aria-pressed", "true");
  await expect(page.getByRole("button", { name: "Refresh", exact: true })).toBeVisible();
  await expect(page.getByText(/^updated /)).toBeVisible();
});

test("the bucket detail keeps one-percent granularity in the console's percent format", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), histograms: F.HISTOGRAMS });
  await openConsole(page, "pagespeed_admin", "#/histograms");
  await page.getByRole("button", { name: "Rewrite Latency ms" }).click();
  const detail = page.locator(".detail-table");
  await expect(detail.getByRole("cell", { name: "100%", exact: true }).first()).toBeVisible();
});
