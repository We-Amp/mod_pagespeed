// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from "@playwright/test";

const BASE = "http://localhost:8080/pagespeed_admin/";

async function navigateTo(page, hash: string) {
  await page.goto(`${BASE}${hash}`);
  await page.waitForTimeout(500);
}

// ---------------------------------------------------------------------------
// Messages page
// ---------------------------------------------------------------------------
test.describe("Messages page", () => {
  test("displays messages with severity badges", async ({ page }) => {
    await navigateTo(page, "#/messages");
    await page.waitForTimeout(3000);

    // Take screenshot.
    await page.screenshot({
      path: "e2e/screenshots/messages-page.png",
      fullPage: true,
    });

    // Should have message rows visible.
    const rows = page.locator(".message-row");
    await expect(rows.first()).toBeVisible({ timeout: 10000 });
    const rowCount = await rows.count();
    expect(rowCount).toBeGreaterThan(0);

    // Each visible message row should have a severity tag.
    const tags = page.locator(".severity-tag");
    const tagCount = await tags.count();
    expect(tagCount).toBeGreaterThan(0);

    // Verify severity tags have content and are uppercase labels.
    const firstTagText = await tags.first().textContent();
    expect(firstTagText?.trim().length).toBeGreaterThan(0);
    expect(firstTagText?.trim()).toMatch(/^(INFO|WARNING|ERROR|FATAL)$/);

    // Message text should be visible and not empty.
    const messageTexts = page.locator(".message-text");
    const firstMsg = await messageTexts.first().textContent();
    expect(firstMsg?.trim().length).toBeGreaterThan(0);
  });

  test("severity filter checkboxes work", async ({ page }) => {
    await navigateTo(page, "#/messages");
    await page.waitForTimeout(3000);

    // All four filter checkboxes should be visible.
    const checkboxes = page.locator('.filter-checkbox input[type="checkbox"]');
    await expect(checkboxes).toHaveCount(4);

    // Verify severity counts show in filter labels.
    const infoFilter = page.locator("text=/Info \\(\\d+\\)/");
    await expect(infoFilter).toBeVisible({ timeout: 5000 });

    // Count messages before unchecking Info.
    const rowsBefore = await page.locator(".message-row").count();

    // Uncheck the Info checkbox (last one).
    const infoCheckbox = checkboxes.nth(3);
    await infoCheckbox.uncheck();
    await page.waitForTimeout(300);

    // The "Showing X of Y" count should update.
    const showingText = await page.locator(".count").textContent();
    expect(showingText).toContain("Showing");

    // Re-check Info.
    await infoCheckbox.check();
    await page.waitForTimeout(300);
    const rowsAfter = await page.locator(".message-row").count();
    expect(rowsAfter).toBe(rowsBefore);
  });

  test("no overlapping elements or broken layout", async ({ page }) => {
    await navigateTo(page, "#/messages");
    await page.waitForTimeout(3000);

    // Header and filters should not overlap.
    const header = page.locator(".header");
    const filters = page.locator(".filters");

    const headerBox = await header.boundingBox();
    const filtersBox = await filters.boundingBox();

    expect(headerBox).not.toBeNull();
    expect(filtersBox).not.toBeNull();

    if (headerBox && filtersBox) {
      // Filters should be below the header, not overlapping.
      expect(filtersBox.y).toBeGreaterThanOrEqual(
        headerBox.y + headerBox.height - 2,
      );
    }

    // No raw JSON displayed on the page.
    const bodyText = await page.locator(".page").textContent();
    expect(bodyText).not.toContain('"severity"');
  });

  test("no JavaScript errors", async ({ page }) => {
    const errors: string[] = [];
    page.on("pageerror", (err) => errors.push(err.message));
    await navigateTo(page, "#/messages");
    await page.waitForTimeout(3000);
    expect(errors).toEqual([]);
  });
});

// ---------------------------------------------------------------------------
// Graphs page
// ---------------------------------------------------------------------------
test.describe("Graphs page", () => {
  test("time range selector displays correctly", async ({ page }) => {
    await navigateTo(page, "#/graphs");
    await page.waitForTimeout(3000);

    // Take screenshot.
    await page.screenshot({
      path: "e2e/screenshots/graphs-page.png",
      fullPage: true,
    });

    // Time range selector should be visible.
    const select = page.locator("select.select");
    await expect(select).toBeVisible({ timeout: 5000 });

    // Should have time range options.
    const options = select.locator("option");
    const optCount = await options.count();
    expect(optCount).toBeGreaterThanOrEqual(4);
  });

  test("displays graph cards, empty state, or error without broken layout", async ({
    page,
  }) => {
    await navigateTo(page, "#/graphs");
    await page.waitForTimeout(4000);

    // Should either show graph cards, an empty state, or an error message.
    // It should NOT show raw JSON or HTML markup.
    const graphCards = page.locator(".graph-card");
    const emptyState = page.locator('[data-testid="graphs-empty-state"]');
    const emptyText = page.locator(".empty");
    const errorMsg = page.locator(".error");
    const toolbar = page.locator(".toolbar");

    const hasGraphs = (await graphCards.count()) > 0;
    const hasEmptyState = (await emptyState.count()) > 0;
    const hasEmptyText = (await emptyText.count()) > 0;
    const hasError = (await errorMsg.count()) > 0;
    const hasToolbar = (await toolbar.count()) > 0;

    // At least one meaningful UI state should be showing.
    expect(hasGraphs || hasEmptyState || hasEmptyText || hasError || hasToolbar).toBe(true);

    if (hasGraphs) {
      // Verify graph cards have names and sparklines.
      const firstName = await page.locator(".graph-name").first().textContent();
      expect(firstName?.trim().length).toBeGreaterThan(0);

      const svgs = page.locator(".sparkline svg");
      expect(await svgs.count()).toBeGreaterThan(0);
    }

    // No raw JSON or HTML markup on the page.
    const bodyText = await page.locator(".page").textContent();
    expect(bodyText).not.toContain("<!doctype");
    expect(bodyText).not.toContain("<html");
  });

  test("Pause/Resume toggle works", async ({ page }) => {
    await navigateTo(page, "#/graphs");
    await page.waitForTimeout(2000);

    const pauseBtn = page.locator("button", { hasText: "Pause" });
    await expect(pauseBtn).toBeVisible({ timeout: 5000 });
    await pauseBtn.click();
    await expect(page.locator("button", { hasText: "Resume" })).toBeVisible();
  });

  test("no JavaScript errors", async ({ page }) => {
    const errors: string[] = [];
    page.on("pageerror", (err) => errors.push(err.message));
    await navigateTo(page, "#/graphs");
    await page.waitForTimeout(3000);
    expect(errors).toEqual([]);
  });
});
