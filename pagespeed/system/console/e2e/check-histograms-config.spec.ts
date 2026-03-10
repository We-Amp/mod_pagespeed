import { test, expect } from "@playwright/test";

// Use the Vite dev server (which proxies API calls to the real backend)
// so that we test the latest source. The dev server must be running.
// Override with ADMIN_BASE_URL env var if needed.
const BASE = process.env.ADMIN_BASE_URL ?? "http://localhost:8080/pagespeed_admin/";

// Helper: navigate to a hash route and wait for content to load.
async function navigateTo(page: import("@playwright/test").Page, hash: string) {
  await page.goto(`${BASE}${hash}`);
  await page.waitForTimeout(500);
}

// ---------------------------------------------------------------------------
// Histograms page
// ---------------------------------------------------------------------------
test.describe("Histograms page", () => {
  test("renders a proper data table, not raw HTML or JSON", async ({
    page,
  }) => {
    await navigateTo(page, "#/histograms");
    await page.waitForTimeout(3000);
    await page.screenshot({
      path: "e2e/screenshots/histograms-table.png",
      fullPage: true,
    });

    // Should have a structured table with histogram data.
    const table = page.locator("table.histogram-table");
    await expect(table).toBeVisible({ timeout: 10000 });

    // Table should have header columns.
    const headers = table.locator("thead th");
    await expect(headers.first()).toBeVisible();
    const headerTexts = await headers.allTextContents();
    expect(headerTexts).toContain("Histogram Name");
    expect(headerTexts).toContain("Count");
    expect(headerTexts).toContain("Avg");

    // Table should have at least one data row.
    const rows = table.locator("tbody tr");
    const rowCount = await rows.count();
    expect(rowCount).toBeGreaterThan(0);
  });

  test("does not show raw HTML tags or script code", async ({ page }) => {
    await navigateTo(page, "#/histograms");
    await page.waitForTimeout(3000);

    const pageText = await page.locator(".page").textContent();
    expect(pageText).toBeDefined();

    // No raw HTML tags like <hr/>, <em>, <td>, <script>, etc.
    expect(pageText).not.toMatch(/<hr\s*\/?>/i);
    expect(pageText).not.toMatch(/<em>/i);
    expect(pageText).not.toMatch(/<\/td>/i);
    expect(pageText).not.toMatch(/<script>/i);
    expect(pageText).not.toMatch(/<div\s/i);

    // No embedded JavaScript from the backend.
    expect(pageText).not.toContain("function setHistogram");
    expect(pageText).not.toContain("var currentHistogram");
    expect(pageText).not.toContain("document.getElementById");
  });

  test("histogram values are properly separated in columns", async ({
    page,
  }) => {
    await navigateTo(page, "#/histograms");
    await page.waitForTimeout(3000);

    const table = page.locator("table.histogram-table");
    await expect(table).toBeVisible({ timeout: 10000 });

    // Get the first data row's cells and verify they contain individual values.
    const firstRow = table.locator("tbody tr").first();
    const cells = firstRow.locator("td");
    const cellCount = await cells.count();

    // Should have 10 columns: name, count, avg, stddev, min, median, max, p90, p95, p99.
    expect(cellCount).toBe(10);

    // The name cell should contain a histogram name (not concatenated numbers).
    const nameText = await cells.nth(0).textContent();
    expect(nameText?.trim().length).toBeGreaterThan(3);

    // The count cell should be a clean number.
    const countText = await cells.nth(1).textContent();
    expect(countText?.trim()).toMatch(/^\d+$/);
  });

  test("clicking a histogram row shows its bucket detail", async ({
    page,
  }) => {
    await navigateTo(page, "#/histograms");
    await page.waitForTimeout(3000);

    const table = page.locator("table.histogram-table");
    await expect(table).toBeVisible({ timeout: 10000 });

    // Click the first row.
    await table.locator("tbody tr").first().click();
    await page.waitForTimeout(300);

    // A detail section should appear.
    const detailTable = page.locator("table.detail-table");
    await expect(detailTable).toBeVisible({ timeout: 5000 });

    // Should have bucket rows.
    const bucketRows = detailTable.locator("tbody tr");
    const bucketCount = await bucketRows.count();
    expect(bucketCount).toBeGreaterThan(0);

    // Distribution bars should be present.
    const bars = detailTable.locator(".bar");
    const barCount = await bars.count();
    expect(barCount).toBeGreaterThan(0);
  });

  test("search filter narrows histogram rows", async ({ page }) => {
    await navigateTo(page, "#/histograms");
    await page.waitForTimeout(3000);

    const table = page.locator("table.histogram-table");
    await expect(table).toBeVisible({ timeout: 10000 });

    const totalBefore = await table.locator("tbody tr").count();

    // Type a filter that should match only some histograms.
    await page.fill(".search-input", "Rewrite");
    await page.waitForTimeout(300);

    const totalAfter = await table.locator("tbody tr").count();
    expect(totalAfter).toBeGreaterThan(0);
    expect(totalAfter).toBeLessThanOrEqual(totalBefore);

    // The visible row should contain "Rewrite" in its name.
    const firstName = await table
      .locator("tbody tr")
      .first()
      .locator("td")
      .first()
      .textContent();
    expect(firstName?.toLowerCase()).toContain("rewrite");
  });

  test("auto-refresh controls work", async ({ page }) => {
    await navigateTo(page, "#/histograms");

    // Pause button should be visible (auto-refresh is on by default).
    const pauseBtn = page.locator("button", {
      hasText: "Pause Auto-Refresh",
    });
    await expect(pauseBtn).toBeVisible({ timeout: 5000 });

    // Click Pause.
    await pauseBtn.click();

    // Should now show "Resume Auto-Refresh".
    const resumeBtn = page.locator("button", {
      hasText: "Resume Auto-Refresh",
    });
    await expect(resumeBtn).toBeVisible({ timeout: 3000 });

    // Click Resume.
    await resumeBtn.click();

    // Should switch back to Pause.
    await expect(pauseBtn).toBeVisible({ timeout: 3000 });

    // Refresh Now button should always be visible.
    await expect(
      page.locator("button", { hasText: "Refresh Now" }),
    ).toBeVisible();
  });

  test("no JavaScript errors on histogram page", async ({ page }) => {
    const errors: string[] = [];
    page.on("pageerror", (err) => errors.push(err.message));

    await navigateTo(page, "#/histograms");
    await page.waitForTimeout(3000);

    expect(errors).toEqual([]);
  });
});

// ---------------------------------------------------------------------------
// Configuration page
// ---------------------------------------------------------------------------
test.describe("Configuration page", () => {
  test("displays clean config text in a pre block", async ({ page }) => {
    await navigateTo(page, "#/configuration");
    await page.waitForTimeout(2000);
    await page.screenshot({
      path: "e2e/screenshots/configuration.png",
      fullPage: true,
    });

    const pre = page.locator("pre.config-block");
    await expect(pre).toBeVisible({ timeout: 10000 });

    const text = await pre.textContent();
    expect(text).toBeDefined();
    expect(text!.length).toBeGreaterThan(50);

    // Should contain expected config sections.
    expect(text).toContain("Version");
    expect(text).toContain("Filters");
    expect(text).toContain("Options");
  });

  test("config text has no raw HTML tags", async ({ page }) => {
    await navigateTo(page, "#/configuration");
    await page.waitForTimeout(2000);

    const pre = page.locator("pre.config-block");
    await expect(pre).toBeVisible({ timeout: 10000 });

    const text = await pre.textContent();
    expect(text).not.toMatch(/<hr\s*\/?>/i);
    expect(text).not.toMatch(/<em>/i);
    expect(text).not.toMatch(/<\/em>/i);
    expect(text).not.toMatch(/<br\s*\/?>/i);
    expect(text).not.toMatch(/<[a-z][^>]*>/i);
  });

  test("Refresh button reloads config", async ({ page }) => {
    await navigateTo(page, "#/configuration");

    const refreshBtn = page.locator("button", { hasText: "Refresh" });
    await expect(refreshBtn).toBeVisible({ timeout: 5000 });

    // Click Refresh and verify the config is still displayed.
    await refreshBtn.click();
    await page.waitForTimeout(1000);

    const pre = page.locator("pre.config-block");
    await expect(pre).toBeVisible({ timeout: 10000 });
    const text = await pre.textContent();
    expect(text).toContain("Version");
  });

  test("no JavaScript errors on config page", async ({ page }) => {
    const errors: string[] = [];
    page.on("pageerror", (err) => errors.push(err.message));

    await navigateTo(page, "#/configuration");
    await page.waitForTimeout(2000);

    expect(errors).toEqual([]);
  });
});
