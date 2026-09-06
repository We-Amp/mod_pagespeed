// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from "@playwright/test";

const BASE =
  process.env.CACHES_TEST_URL || "http://localhost:8080/pagespeed_admin/";

async function navigateToCaches(page) {
  await page.goto(`${BASE}#/caches`);
  // Wait for the Caches page to load — the heading should appear.
  await expect(page.locator("h1")).toContainText("Caches", { timeout: 10000 });
}

function clickTab(page, label: string) {
  return page.getByRole("button", { name: label, exact: true }).click();
}

test.describe("Caches page", () => {
  test("Cache Structure tab displays cache hierarchy properly", async ({
    page,
  }) => {
    await navigateToCaches(page);

    // The Cache Structure tab should be active by default.
    await expect(
      page.locator("button.tab.active", { hasText: "Cache Structure" }),
    ).toBeVisible();

    // Wait for data to load (not showing the loading message).
    await expect(page.locator(".loading")).not.toBeVisible({ timeout: 10000 });

    // Take a screenshot.
    await page.screenshot({
      path: "e2e/screenshots/caches-structure.png",
      fullPage: true,
    });

    // The page should NOT display raw JSON or [object Object].
    const content = await page.locator(".tab-content").textContent();
    expect(content).not.toContain("[object Object]");
    expect(content).not.toContain('"caches"');
    expect(content).not.toContain("undefined");

    // The page should display known cache names from the API.
    expect(content).toContain("HTTP Cache");
    expect(content).toContain("Metadata Cache");
    expect(content).toContain("Property Cache");

    // Cache summaries should include implementation details.
    expect(content).toContain("CycloneCache");
  });

  test("Cache Structure shows no empty state when data is present", async ({
    page,
  }) => {
    await navigateToCaches(page);
    await expect(page.locator(".loading")).not.toBeVisible({ timeout: 10000 });

    // Should NOT show the empty message.
    await expect(
      page.locator("text=No cache structure data available"),
    ).not.toBeVisible();
  });

  test("Cache Lookup tab has form controls", async ({ page }) => {
    await navigateToCaches(page);
    await clickTab(page, "Cache Lookup");

    // Take a screenshot.
    await page.screenshot({
      path: "e2e/screenshots/caches-lookup.png",
      fullPage: true,
    });

    // The form should have a text input and a button.
    const input = page.locator("#lookup-url");
    await expect(input).toBeVisible();
    await expect(input).toHaveAttribute("type", "text");

    const button = page.locator('button[type="submit"]');
    await expect(button).toBeVisible();
    await expect(button).toContainText("Lookup");

    // The button should be disabled when the input is empty.
    await expect(button).toBeDisabled();

    // Typing a URL should enable the button.
    await input.fill("http://example.com/test.jpg");
    await expect(button).toBeEnabled();
  });

  test("Cache Lookup performs a lookup and displays results", async ({
    page,
  }) => {
    await navigateToCaches(page);
    await clickTab(page, "Cache Lookup");

    const input = page.locator("#lookup-url");
    await input.fill("http://example.com/test.jpg");

    // Submit the form.
    await page.locator('button[type="submit"]').click();

    // Wait for result to appear.
    await expect(page.locator(".result-box")).toBeVisible({ timeout: 10000 });

    // Take a screenshot of the result.
    await page.screenshot({
      path: "e2e/screenshots/caches-lookup-result.png",
      fullPage: true,
    });

    // The result should contain actual cache lookup data, not raw JSON artifacts.
    const resultText = await page.locator(".result-box").textContent();
    expect(resultText).not.toContain("[object Object]");
    // Should show the URL we looked up (either from API or our input).
    expect(resultText).toContain("example.com/test.jpg");
    // Should contain cache metadata like "cache_ok" from the backend response.
    expect(resultText).toContain("cache_ok");
  });

  test("Purge tab has proper form controls", async ({ page }) => {
    await navigateToCaches(page);
    await clickTab(page, "Purge");

    // Take a screenshot.
    await page.screenshot({
      path: "e2e/screenshots/caches-purge.png",
      fullPage: true,
    });

    // Should have a URL input.
    const input = page.locator("#purge-url");
    await expect(input).toBeVisible();
    await expect(input).toHaveAttribute("type", "text");

    // Should have a Purge URL button (disabled when empty).
    const purgeBtn = page.locator('button[type="submit"]');
    await expect(purgeBtn).toBeVisible();
    await expect(purgeBtn).toContainText("Purge URL");
    await expect(purgeBtn).toBeDisabled();

    // Should have a Purge All button.
    const purgeAllBtn = page.locator("button.btn-danger");
    await expect(purgeAllBtn).toBeVisible();
    await expect(purgeAllBtn).toContainText("Purge All");

    // Should have a hint about what Purge All does.
    await expect(page.locator(".hint")).toContainText("entire cache");
  });

  test("Purge Set tab loads", async ({ page }) => {
    await navigateToCaches(page);
    await clickTab(page, "Purge Set");

    // Take a screenshot.
    await page.screenshot({
      path: "e2e/screenshots/caches-purgeset.png",
      fullPage: true,
    });

    // Should have a Refresh button.
    await expect(
      page.locator("button.btn-secondary", { hasText: "Refresh" }),
    ).toBeVisible({ timeout: 10000 });

    // Wait for loading to finish.
    await expect(page.locator("text=Loading...")).not.toBeVisible({
      timeout: 10000,
    });

    // The tab should show either a purge list, an empty message, or a
    // "not enabled" message — but NOT raw JSON or [object Object].
    const tabContent = await page.locator(".tab-content").textContent();
    expect(tabContent).not.toContain("[object Object]");
    expect(tabContent).not.toContain('"caches"');

    // One of these messages should be present.
    const hasContent =
      tabContent!.includes("Purge set is empty") ||
      tabContent!.includes("purging is not enabled") ||
      tabContent!.includes("Purged URL") ||
      tabContent!.includes("Global invalidation");
    expect(hasContent).toBe(true);
  });

  test("no JavaScript errors on Caches page", async ({ page }) => {
    const errors: string[] = [];
    page.on("pageerror", (err) => errors.push(err.message));

    await navigateToCaches(page);

    // Visit each tab to trigger all code paths.
    await clickTab(page, "Cache Lookup");
    await page.waitForTimeout(500);
    await clickTab(page, "Purge");
    await page.waitForTimeout(500);
    await clickTab(page, "Purge Set");
    await page.waitForTimeout(1000);
    await clickTab(page, "Cache Structure");
    await page.waitForTimeout(500);

    expect(errors).toEqual([]);
  });

  test("tab switching works correctly", async ({ page }) => {
    await navigateToCaches(page);

    // Cache Structure is active by default.
    await expect(
      page.locator("button.tab.active"),
    ).toContainText("Cache Structure");

    // Switch to Cache Lookup.
    await clickTab(page, "Cache Lookup");
    await expect(
      page.locator("button.tab.active"),
    ).toContainText("Cache Lookup");
    // The lookup form should be visible.
    await expect(page.locator("#lookup-url")).toBeVisible();

    // Switch to Purge.
    await clickTab(page, "Purge");
    await expect(
      page.locator("button.tab.active"),
    ).toHaveText(/^Purge$/);
    await expect(page.locator("#purge-url")).toBeVisible();

    // Switch to Purge Set.
    await clickTab(page, "Purge Set");
    await expect(
      page.locator("button.tab.active"),
    ).toHaveText(/^Purge Set$/);

    // Switch back to Cache Structure.
    await clickTab(page, "Cache Structure");
    await expect(
      page.locator("button.tab.active"),
    ).toContainText("Cache Structure");
  });
});
