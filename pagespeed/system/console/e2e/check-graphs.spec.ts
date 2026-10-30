// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from "@playwright/test";
import { LIVE_BASE } from "./support/live";

const BASE = LIVE_BASE;

async function navigateTo(page, hash: string) {
  await page.goto(`${BASE}${hash}`);
  await page.waitForTimeout(500);
}

test.describe("Graphs page", () => {
  test("time range selector displays correctly", async ({ page }) => {
    await navigateTo(page, "#/graphs");

    // The selector is the labelled "Time range" control in the page toolbar.
    const select = page.getByLabel("Time range");
    await expect(select).toBeVisible({ timeout: 10000 });
    await expect(select.locator("option")).toHaveCount(6);

    await select.selectOption("60");
    await expect(select).toHaveValue("60");
  });

  test("shows the default charts, or says why there are none", async ({
    page,
  }) => {
    await navigateTo(page, "#/graphs");

    // Wait for the first statistics poll to settle the page: either the
    // "At a glance" section or an error is drawn once the poll answers.
    const glance = page.getByRole("heading", { name: "At a glance" });
    const failure = page.getByRole("alert");
    await expect(glance.or(failure).first()).toBeVisible({ timeout: 15000 });

    if ((await glance.count()) > 0) {
      // Every drawn chart is a labelled image with a title and a latest
      // value; charts without data yet are listed in the hidden notice.
      const cards = page.locator('[data-testid^="curated-"]:not([data-testid="curated-latest"]):not([data-testid="curated-hidden"])');
      const drawn = await cards.count();
      const hidden = await page.getByTestId("curated-hidden").count();
      expect(drawn + hidden).toBeGreaterThan(0);
      if (drawn > 0) {
        await expect(cards.first().locator("h3")).not.toBeEmpty();
        await expect(cards.first().getByRole("img")).toBeVisible();
        await expect(cards.first().getByTestId("curated-latest")).toBeVisible();
      }
    }

    // No raw JSON or HTML markup on the page.
    const bodyText = await page.locator(".page").textContent();
    expect(bodyText).not.toContain("<!doctype");
    expect(bodyText).not.toContain("<html");
  });

  test("every counter view draws one card per counter", async ({ page }) => {
    await navigateTo(page, "#/graphs?counters=all");
    const cards = page.locator('[data-testid^="graph-card-"]');
    await expect(cards.first()).toBeVisible({ timeout: 15000 });
    await expect(cards.first().locator("h2")).not.toBeEmpty();
    await expect(cards.first().getByRole("img")).toBeVisible();
  });

  test("auto-refresh toggle works", async ({ page }) => {
    await navigateTo(page, "#/graphs");

    // Auto-refresh is on by default and is a pressed-state toggle.
    const toggle = page.getByRole("button", { name: /^Auto-refresh/ });
    await expect(toggle).toBeVisible({ timeout: 10000 });
    await expect(toggle).toHaveAttribute("aria-pressed", "true");
    await toggle.click();
    await expect(toggle).toHaveAttribute("aria-pressed", "false");
    await toggle.click();
    await expect(toggle).toHaveAttribute("aria-pressed", "true");
  });

  test("no JavaScript errors", async ({ page }) => {
    const errors: string[] = [];
    page.on("pageerror", (err) => errors.push(err.message));
    await navigateTo(page, "#/graphs");
    // Two statistics polls (5 s apart) cover the first draw and a refresh.
    await expect(page.getByRole("heading", { name: "Graphs" })).toBeVisible();
    await page.waitForTimeout(6000);
    expect(errors).toEqual([]);
  });
});
