// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The Caches page. Self-contained (support/mock-admin.ts); run after build.sh.

import { expect, test } from "@playwright/test";
import * as F from "./support/fixtures";
import { mockAdmin, openConsole } from "./support/mock-admin";

test.use({ locale: "en-US", timezoneId: "UTC" });

test.describe("Caches", () => {
  test("the views are a tablist operated with the arrow keys", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), cache: F.CACHE_STRUCTURE_PURGE_ON });
    await openConsole(page, "pagespeed_admin", "#/caches");
    const tabs = page.getByRole("tablist", { name: "Cache views" }).getByRole("tab");
    await expect(tabs).toHaveText(["Cache Structure", "Cache Lookup", "Purge", "Purge Set"]);
    await expect(tabs.nth(0)).toHaveAttribute("aria-selected", "true");
    await tabs.nth(0).focus();
    await page.keyboard.press("ArrowRight");
    await expect(tabs.nth(1)).toBeFocused();
    await expect(tabs.nth(1)).toHaveAttribute("aria-selected", "true");
    await expect(page.getByRole("tabpanel", { name: "Cache Lookup" })).toBeVisible();
    await page.keyboard.press("End");
    await expect(tabs.nth(3)).toBeFocused();
    await page.keyboard.press("Home");
    await expect(tabs.nth(0)).toBeFocused();
  });

  test("with purging off, the purge views are hidden and the page says how to turn it on", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), cache: F.CACHE_STRUCTURE });
    await openConsole(page, "pagespeed_admin", "#/caches");
    await expect(page.getByRole("tablist", { name: "Cache views" }).getByRole("tab")).toHaveText([
      "Cache Structure",
      "Cache Lookup",
    ]);
    await expect(page.getByTestId("purge-disabled-note")).toContainText("EnableCachePurge");
  });

  test("layers are described in words; their icons are hidden from screen readers", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), cache: F.CACHE_STRUCTURE });
    await openConsole(page, "pagespeed_admin", "#/caches");
    const metadata = page.locator(".cache-card", { has: page.getByRole("heading", { name: "Metadata Cache", exact: true }) });
    await expect(metadata).toContainText("Write-Through (L1, then L2)");
    await expect(metadata).toContainText("Shared Memory (64-byte blocks)");
    await expect(metadata).toContainText("Cyclone Disk Cache (Small Tier)");
    await expect(page.locator(".layer-kind").first()).toHaveText(/Memory|Disk|HTTP|Compressed/);
    await expect(page.locator(".layer-icon")).toHaveCount(0);
  });

  test("a cohort column is wide enough that its layer labels do not stack four lines deep", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), cache: F.CACHE_STRUCTURE });
    await openConsole(page, "pagespeed_admin", "#/caches");
    const property = page.locator(".cache-card", { hasText: "Property Cache" });
    // Two cohorts each carry this layer; any one of them proves the width.
    const label = property.locator(".layer", { hasText: "Shared Memory (64-byte blocks)" }).locator(".layer-label").first();
    await expect(label).toBeVisible();
    const lineHeight = await label.evaluate((el) => Number.parseFloat(getComputedStyle(el).lineHeight));
    const height = await label.evaluate((el) => el.getBoundingClientRect().height);
    expect(height).toBeLessThanOrEqual(lineHeight * 2 + 1); // at most two lines
  });

  test("a malformed cache summary does not stop the page", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", {
      ...F.allReplies(),
      cache: { status: 200, body: { caches: [{ name: "Odd Cache", summary: "A((" }], purge_enabled: false } },
    });
    await openConsole(page, "pagespeed_admin", "#/caches");
    await expect(page.getByRole("heading", { name: "Odd Cache" })).toBeVisible();
  });

  test("a lookup result with an error is announced", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await page.route(/\/cache\?url=/, (route) =>
      route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ url: "https://example.test/x.jpg", error: "not found in cache" }),
      }),
    );
    await openConsole(page, "pagespeed_admin", "#/caches");
    await page.getByRole("tab", { name: "Cache Lookup" }).click();
    await page.getByLabel("URL to look up:").fill("https://example.test/x.jpg");
    await page.getByRole("button", { name: "Lookup" }).click();
    const alert = page.getByRole("alert").filter({ hasText: "not found in cache" });
    await expect(alert).toBeVisible();
  });

  test("a failed purge-set read is announced", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), cache: F.CACHE_STRUCTURE_PURGE_ON });
    await page.route(/\/cache\?new_set=/, (route) =>
      route.fulfill({ status: 500, contentType: "application/json", body: JSON.stringify({ error: "boom" }) }),
    );
    await openConsole(page, "pagespeed_admin", "#/caches");
    await page.getByRole("tab", { name: "Purge Set" }).click();
    const alert = page.getByRole("alert").filter({ hasText: "boom" });
    await expect(alert).toBeVisible();
  });
});
