// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from "@playwright/test";

const BASE = "http://localhost:8080/pagespeed_admin/";

// Helper: navigate to a hash route and wait for content to load.
async function navigateTo(page, hash: string) {
  await page.goto(`${BASE}${hash}`);
  // Wait for the "Loading..." text to disappear (or never appear).
  await page.waitForTimeout(500);
}

// ---------------------------------------------------------------------------
// App shell & navigation
// ---------------------------------------------------------------------------
test.describe("App shell", () => {
  test("loads and shows topbar with title", async ({ page }) => {
    await page.goto(BASE);
    // The chrome derives from the product facts: the product name in the
    // logo, the vendor link, and the console-scope badge.
    await expect(page.locator(".topbar-logo")).toContainText("mod_pagespeed");
    await expect(page.locator(".topbar-badge")).toHaveText("Admin");
  });

  test("sidebar has all 12 nav items", async ({ page }) => {
    await page.goto(BASE);
    const items = page.locator(".nav-item");
    await expect(items).toHaveCount(12);
    const labels = await items.allTextContents();
    expect(labels).toEqual([
      "Statistics",
      "Configuration",
      "Histograms",
      "Caches",
      "Console",
      "Messages",
      "Graphs",
      "Daemon Status",
      "Daemon Cache",
      "Daemon Back-pressure",
      "Support",
      "About",
    ]);
  });

  test("clicking nav items changes the hash", async ({ page }) => {
    await page.goto(BASE);
    await page.click('button.nav-item:has-text("Configuration")');
    await expect(page).toHaveURL(/.*#\/configuration/);
    await page.click('button.nav-item:has-text("Messages")');
    await expect(page).toHaveURL(/.*#\/messages/);
  });

  test("default route loads statistics page", async ({ page }) => {
    await page.goto(BASE);
    // The default route should show the statistics page content.
    await expect(page.locator("table tbody tr").first()).toBeVisible({
      timeout: 10000,
    });
  });

  test("no JavaScript errors on initial load", async ({ page }) => {
    const errors: string[] = [];
    page.on("pageerror", (err) => errors.push(err.message));
    await page.goto(BASE);
    await page.waitForTimeout(2000);
    expect(errors).toEqual([]);
  });
});

// ---------------------------------------------------------------------------
// Statistics page
// ---------------------------------------------------------------------------
test.describe("Statistics page", () => {
  test("displays statistics table with data", async ({ page }) => {
    await navigateTo(page, "#/statistics");
    // Wait for the table to have rows.
    await expect(page.locator("table tbody tr").first()).toBeVisible({
      timeout: 10000,
    });
    const rowCount = await page.locator("table tbody tr").count();
    expect(rowCount).toBeGreaterThan(5);
  });

  test("search filters statistics", async ({ page }) => {
    await navigateTo(page, "#/statistics");
    await expect(page.locator("table tbody tr").first()).toBeVisible({
      timeout: 10000,
    });
    const totalBefore = await page.locator("table tbody tr").count();

    await page.fill('input[type="text"]', "cache_hits");
    await page.waitForTimeout(300);
    const totalAfter = await page.locator("table tbody tr").count();
    expect(totalAfter).toBeLessThan(totalBefore);
    expect(totalAfter).toBeGreaterThan(0);
  });

  test("column headers are clickable for sorting", async ({ page }) => {
    await navigateTo(page, "#/statistics");
    await expect(page.locator("table tbody tr").first()).toBeVisible({
      timeout: 10000,
    });
    // Click "Value" header to sort.
    await page.click("th >> text=Value");
    await page.waitForTimeout(300);
    // Should still have rows (no crash).
    const rowCount = await page.locator("table tbody tr").count();
    expect(rowCount).toBeGreaterThan(0);
  });
});

// ---------------------------------------------------------------------------
// Configuration page
// ---------------------------------------------------------------------------
test.describe("Configuration page", () => {
  test("displays config text", async ({ page }) => {
    await navigateTo(page, "#/configuration");
    // Wait for the pre block with config content.
    const pre = page.locator("pre");
    await expect(pre).toBeVisible({ timeout: 10000 });
    const text = await pre.textContent();
    expect(text).toContain("Version");
    expect(text).toContain("Filters");
  });

  test("has a Refresh button", async ({ page }) => {
    await navigateTo(page, "#/configuration");
    await expect(page.locator("button >> text=Refresh")).toBeVisible();
  });
});

// ---------------------------------------------------------------------------
// Histograms page
// ---------------------------------------------------------------------------
test.describe("Histograms page", () => {
  test("loads without errors", async ({ page }) => {
    const errors: string[] = [];
    page.on("pageerror", (err) => errors.push(err.message));
    await navigateTo(page, "#/histograms");
    await page.waitForTimeout(2000);
    expect(errors).toEqual([]);
  });

  test("shows histogram content or empty message", async ({ page }) => {
    await navigateTo(page, "#/histograms");
    await page.waitForTimeout(3000);
    // Either shows histogram data in a pre block, or "No histograms" message.
    const pre = page.locator("pre.histogram-block");
    const empty = page.locator("text=No histogram");
    const hasContent = (await pre.count()) > 0 || (await empty.count()) > 0;
    expect(hasContent).toBe(true);
  });

  test("has auto-refresh controls", async ({ page }) => {
    await navigateTo(page, "#/histograms");
    await expect(page.locator("button >> text=Pause")).toBeVisible({
      timeout: 5000,
    });
    await expect(page.locator("button >> text=Refresh Now")).toBeVisible();
  });
});

// ---------------------------------------------------------------------------
// Caches page
// ---------------------------------------------------------------------------
test.describe("Caches page", () => {
  test("shows tabs for cache operations", async ({ page }) => {
    await navigateTo(page, "#/caches");
    await expect(page.locator("button.tab >> text=Cache Structure")).toBeVisible();
    await expect(page.locator("button.tab >> text=Cache Lookup")).toBeVisible();
    await expect(page.locator('button.tab', { hasText: /^Purge$/ })).toBeVisible();
    await expect(page.locator('button.tab', { hasText: 'Purge Set' })).toBeVisible();
  });

  test("Cache Structure tab shows cache info", async ({ page }) => {
    await navigateTo(page, "#/caches");
    // Default tab is structure.
    const pre = page.locator("pre");
    await expect(pre).toBeVisible({ timeout: 10000 });
    const text = await pre.textContent();
    expect(text).toContain("Cache");
  });

  test("Cache Lookup tab has URL input", async ({ page }) => {
    await navigateTo(page, "#/caches");
    await page.click("button.tab >> text=Cache Lookup");
    await expect(page.locator('input[type="text"]')).toBeVisible();
    await expect(page.locator('button[type="submit"]', { hasText: 'Lookup' })).toBeVisible();
  });

  test("Purge tab has URL input and Purge All button", async ({ page }) => {
    await navigateTo(page, "#/caches");
    await page.click("button.tab >> text=Purge");
    await expect(page.locator("button >> text=Purge URL")).toBeVisible();
    await expect(page.locator("button >> text=Purge All")).toBeVisible();
  });
});

// ---------------------------------------------------------------------------
// Console page
// ---------------------------------------------------------------------------
test.describe("Console page", () => {
  test("loads without errors", async ({ page }) => {
    const errors: string[] = [];
    page.on("pageerror", (err) => errors.push(err.message));
    await navigateTo(page, "#/console");
    await page.waitForTimeout(2000);
    expect(errors).toEqual([]);
  });

  test("has time range controls", async ({ page }) => {
    await navigateTo(page, "#/console");
    await expect(page.locator("select, button >> text=Refresh")).toBeVisible({
      timeout: 5000,
    });
  });
});

// ---------------------------------------------------------------------------
// Messages page
// ---------------------------------------------------------------------------
test.describe("Messages page", () => {
  test("shows messages with severity filters", async ({ page }) => {
    await navigateTo(page, "#/messages");
    // Wait for messages to load.
    await page.waitForTimeout(3000);
    // Should have filter checkboxes.
    await expect(page.locator('input[type="checkbox"]').first()).toBeVisible();
    // Should have at least one message row (startup messages).
    const rows = page.locator(".message-row");
    await expect(rows.first()).toBeVisible({ timeout: 10000 });
    const count = await rows.count();
    expect(count).toBeGreaterThan(0);
  });

  test("severity filter labels show counts", async ({ page }) => {
    await navigateTo(page, "#/messages");
    await page.waitForTimeout(3000);
    // Should show "Info (N)" in one of the filter badges.
    const infoFilter = page.locator("text=/Info \\(\\d+\\)/");
    await expect(infoFilter).toBeVisible({ timeout: 5000 });
  });

  test("auto-refresh toggle works", async ({ page }) => {
    await navigateTo(page, "#/messages");
    const pauseBtn = page.locator("button >> text=Pause Auto-Refresh");
    await expect(pauseBtn).toBeVisible({ timeout: 5000 });
    await pauseBtn.click();
    await expect(
      page.locator("button >> text=Resume Auto-Refresh"),
    ).toBeVisible();
  });
});

// ---------------------------------------------------------------------------
// Graphs page
// ---------------------------------------------------------------------------
test.describe("Graphs page", () => {
  test("loads without errors", async ({ page }) => {
    const errors: string[] = [];
    page.on("pageerror", (err) => errors.push(err.message));
    await navigateTo(page, "#/graphs");
    await page.waitForTimeout(2000);
    expect(errors).toEqual([]);
  });

  test("has time range selector", async ({ page }) => {
    await navigateTo(page, "#/graphs");
    await expect(page.locator("select").first()).toBeVisible({ timeout: 5000 });
  });
});

// ---------------------------------------------------------------------------
// Cross-page: no JS errors on any page
// ---------------------------------------------------------------------------
test("no JavaScript errors navigating through all pages", async ({ page }) => {
  const errors: string[] = [];
  page.on("pageerror", (err) => errors.push(`${err.message}`));

  const hashes = [
    "#/statistics",
    "#/configuration",
    "#/histograms",
    "#/caches",
    "#/console",
    "#/messages",
    "#/graphs",
    "#/daemon/status",
    "#/daemon/cache",
    "#/daemon/back-pressure",
    "#/support",
    "#/about",
  ];

  await page.goto(BASE);
  for (const hash of hashes) {
    await page.goto(`${BASE}${hash}`);
    await page.waitForTimeout(1500);
  }

  if (errors.length > 0) {
    console.log("JS errors found:", errors);
  }
  expect(errors).toEqual([]);
});

// ---------------------------------------------------------------------------
// API smoke tests (via page context)
// ---------------------------------------------------------------------------
test.describe("API endpoints return valid JSON", () => {
  const endpoints = [
    { path: "stats_json", field: "variables" },
    { path: "config", field: "config" },
    { path: "histograms", field: "histograms" },
    { path: "cache", field: "caches" },
    { path: "message_history", field: "messages" },
  ];

  for (const ep of endpoints) {
    test(`${ep.path} returns JSON with "${ep.field}" field`, async ({
      request,
    }) => {
      const resp = await request.get(`${BASE}${ep.path}`);
      expect(resp.ok()).toBeTruthy();
      const contentType = resp.headers()["content-type"] ?? "";
      expect(contentType).toContain("application/json");
      const json = await resp.json();
      expect(json).toHaveProperty(ep.field);
    });
  }
});
