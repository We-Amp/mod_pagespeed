// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tables and lists from the keyboard; counter names on a phone.
// Self-contained (support/mock-admin.ts); run after build.sh.

import { expect, test } from "@playwright/test";
import * as F from "./support/fixtures";
import { mockAdmin, openConsole } from "./support/mock-admin";

test.use({ locale: "en-US", timezoneId: "UTC" });

test.describe("Tables", () => {
  test("Statistics: sortable headers are buttons that announce the sort", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/statistics");
    const name = page.getByRole("columnheader", { name: "Name" }).first();
    const value = page.getByRole("columnheader", { name: "Value" }).first();
    await expect(name).toHaveAttribute("aria-sort", "ascending");
    await expect(value).toHaveAttribute("aria-sort", "none");
    await value.getByRole("button").focus();
    await page.keyboard.press("Enter");
    await expect(value).toHaveAttribute("aria-sort", "descending");
    await expect(name).toHaveAttribute("aria-sort", "none");
    await page.keyboard.press("Space");
    await expect(value).toHaveAttribute("aria-sort", "ascending");
  });

  test("Statistics: the delta column sorts from the keyboard", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/statistics");
    await page.getByRole("checkbox", { name: "Δ since open" }).check();
    const delta = page.getByRole("columnheader", { name: "Δ since open" }).first();
    await expect(delta).toHaveAttribute("aria-sort", "none");
    await delta.getByRole("button").focus();
    await page.keyboard.press("Enter");
    await expect(delta).not.toHaveAttribute("aria-sort", "none");
  });

  test("Histograms: a histogram is chosen with the keyboard", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), histograms: F.HISTOGRAMS });
    await openConsole(page, "pagespeed_admin", "#/histograms");
    const second = page.getByRole("button", { name: "Rewrite Latency ms" });
    await expect(second).toHaveAttribute("aria-pressed", "false");
    await second.focus();
    await page.keyboard.press("Enter");
    await expect(second).toHaveAttribute("aria-pressed", "true");
    await expect(page.getByRole("heading", { level: 2, name: "Rewrite Latency ms" })).toBeVisible();
  });

  test("Statistics shows descriptions as a second line until the toggle is turned off", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/statistics");
    await expect(page.getByRole("columnheader", { name: "Description" })).toHaveCount(0);
    await expect(page.locator(".stat-desc").first()).toBeVisible();
  });

  test("on a phone, counter names wrap between words and the page does not scroll sideways", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/statistics");
    const cell = page.locator("td.name-cell").first();
    await expect(cell).toBeVisible();
    expect(await cell.locator("wbr").count()).toBeGreaterThan(0);
    await expect(cell).toHaveCSS("word-break", "normal");
    expect(
      await page.evaluate(
        () => document.scrollingElement!.scrollWidth <= document.scrollingElement!.clientWidth + 1,
      ),
    ).toBe(true);
  });

  test("sortable columns show a sort affordance before they are sorted", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/statistics");
    const nameHeader = page.getByRole("columnheader", { name: "Name" }).first();
    const valueHeader = page.getByRole("columnheader", { name: "Value" }).first();
    await expect(nameHeader).toHaveAttribute("aria-sort", "ascending");
    await expect(valueHeader).toHaveAttribute("aria-sort", "none");
    await expect(valueHeader.getByRole("button")).toContainText("↕");
    await valueHeader.getByRole("button").click();
    await expect(valueHeader).toHaveAttribute("aria-sort", "descending");
    await expect(valueHeader.getByRole("button")).toContainText("▼");
    await expect(nameHeader.getByRole("button")).toContainText("↕");
  });
});
