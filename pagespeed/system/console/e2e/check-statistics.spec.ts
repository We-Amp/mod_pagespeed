import { test, expect } from "@playwright/test";

const BASE = "http://localhost:8080/pagespeed_admin/";

async function navigateToStats(page) {
  await page.goto(`${BASE}#/statistics`);
  // Wait for data to load - table rows should appear.
  await expect(page.locator("table tbody tr").first()).toBeVisible({
    timeout: 10000,
  });
}

test.describe("Statistics page - detailed checks", () => {
  test("screenshot and table structure", async ({ page }) => {
    await navigateToStats(page);

    // Take a screenshot of the full page.
    await page.screenshot({
      path: "e2e/screenshots/statistics-full.png",
      fullPage: true,
    });

    // Check that the table has Name and Value column headers.
    const headers = page.locator("thead th");
    await expect(headers).toHaveCount(2);
    const headerTexts = await headers.allTextContents();
    expect(headerTexts[0]).toContain("Name");
    expect(headerTexts[1]).toContain("Value");

    // Check that rows have exactly 2 cells each.
    const firstRow = page.locator("table tbody tr").first();
    const cells = firstRow.locator("td");
    await expect(cells).toHaveCount(2);

    // Verify the name cell contains a non-empty string.
    const nameText = await cells.nth(0).textContent();
    expect(nameText!.trim().length).toBeGreaterThan(0);

    // Verify the value cell contains a number (not [object Object] or raw JSON).
    const valueText = await cells.nth(1).textContent();
    expect(valueText).not.toContain("[object");
    expect(valueText).not.toContain("{");
    expect(valueText).not.toContain("undefined");
    expect(valueText).not.toContain("null");
    // It should be a formatted number (digits, commas, possibly a minus sign).
    expect(valueText!.trim()).toMatch(/^-?[\d,]+$/);
  });

  test("all values display as numbers, not objects", async ({ page }) => {
    await navigateToStats(page);

    // Grab all value cells and check none display as objects.
    const valueCells = page.locator("td.value-cell");
    const count = await valueCells.count();
    expect(count).toBeGreaterThan(10);

    for (let i = 0; i < Math.min(count, 50); i++) {
      const text = await valueCells.nth(i).textContent();
      expect(text).not.toContain("[object");
      expect(text).not.toContain("{");
      expect(text!.trim()).toMatch(/^-?[\d,]+$/);
    }
  });

  test("search filtering works correctly", async ({ page }) => {
    await navigateToStats(page);

    // Count initial rows.
    const totalBefore = await page.locator("table tbody tr").count();
    expect(totalBefore).toBeGreaterThan(10);

    // Type a specific search term.
    await page.fill('input[type="text"]', "cache_hits");
    await page.waitForTimeout(300);

    const totalAfter = await page.locator("table tbody tr").count();
    expect(totalAfter).toBeLessThan(totalBefore);
    expect(totalAfter).toBeGreaterThan(0);

    // Every visible row name should contain "cache_hits".
    const nameCells = page.locator("td.name-cell");
    const filteredCount = await nameCells.count();
    for (let i = 0; i < filteredCount; i++) {
      const text = await nameCells.nth(i).textContent();
      expect(text!.toLowerCase()).toContain("cache_hits");
    }

    // Check the count indicator updates.
    const countText = await page.locator(".count").textContent();
    expect(countText).toContain(`${filteredCount} of ${totalBefore}`);

    // Clear search and verify rows restore.
    await page.fill('input[type="text"]', "");
    await page.waitForTimeout(300);
    const totalRestored = await page.locator("table tbody tr").count();
    expect(totalRestored).toBe(totalBefore);
  });

  test("search with no results shows empty message", async ({ page }) => {
    await navigateToStats(page);

    await page.fill('input[type="text"]', "zzzznonexistent_stat_name_xyzzy");
    await page.waitForTimeout(300);

    await expect(page.locator("td.empty")).toBeVisible();
    const emptyText = await page.locator("td.empty").textContent();
    expect(emptyText).toContain("No matching variables");
  });

  test("no visual glitches - overlapping text or broken layout", async ({
    page,
  }) => {
    await navigateToStats(page);

    // Take a screenshot for visual inspection.
    await page.screenshot({
      path: "e2e/screenshots/statistics-layout.png",
      fullPage: true,
    });

    // Check table wrapper has reasonable dimensions.
    const tableWrapper = page.locator(".table-wrapper");
    const box = await tableWrapper.boundingBox();
    expect(box).not.toBeNull();
    expect(box!.width).toBeGreaterThan(200);
    expect(box!.height).toBeGreaterThan(100);

    // Check that the header row is above the first data row (no overlap).
    const headerBox = await page.locator("thead").boundingBox();
    const firstRowBox = await page
      .locator("table tbody tr")
      .first()
      .boundingBox();
    expect(headerBox).not.toBeNull();
    expect(firstRowBox).not.toBeNull();
    expect(headerBox!.y + headerBox!.height).toBeLessThanOrEqual(
      firstRowBox!.y + 2,
    ); // Allow 2px tolerance.

    // Ensure the search input doesn't overlap the table.
    const searchBox = await page.locator(".search-input").boundingBox();
    const tableBox = await page.locator(".table-wrapper").boundingBox();
    expect(searchBox).not.toBeNull();
    expect(tableBox).not.toBeNull();
    expect(searchBox!.y + searchBox!.height).toBeLessThanOrEqual(
      tableBox!.y + 2,
    );

    // No horizontal scrollbar on the main page (check body scroll width).
    const bodyScrollWidth = await page.evaluate(
      () => document.body.scrollWidth,
    );
    const viewportWidth = await page.evaluate(() => window.innerWidth);
    expect(bodyScrollWidth).toBeLessThanOrEqual(viewportWidth + 20); // Small tolerance.
  });

  test("sorting by value works", async ({ page }) => {
    await navigateToStats(page);

    // Click "Value" header to sort by value ascending.
    await page.click("th:has-text('Value')");
    await page.waitForTimeout(300);

    // Get first few values.
    const valueCells = page.locator("td.value-cell");
    const count = await valueCells.count();
    if (count >= 2) {
      const firstVal = parseInt(
        (await valueCells.nth(0).textContent())!.replace(/,/g, ""),
      );
      const lastVal = parseInt(
        (await valueCells.nth(count - 1).textContent())!.replace(/,/g, ""),
      );
      expect(firstVal).toBeLessThanOrEqual(lastVal);
    }

    // Click again to sort descending.
    await page.click("th:has-text('Value')");
    await page.waitForTimeout(300);

    if (count >= 2) {
      const firstVal = parseInt(
        (await valueCells.nth(0).textContent())!.replace(/,/g, ""),
      );
      const secondVal = parseInt(
        (await valueCells.nth(1).textContent())!.replace(/,/g, ""),
      );
      expect(firstVal).toBeGreaterThanOrEqual(secondVal);
    }
  });

  test("no JavaScript errors on statistics page", async ({ page }) => {
    const errors: string[] = [];
    page.on("pageerror", (err) => errors.push(err.message));

    await navigateToStats(page);
    await page.waitForTimeout(2000);

    expect(errors).toEqual([]);
  });
});
