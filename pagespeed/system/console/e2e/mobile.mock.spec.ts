// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The phone shell and card tables, at the audit's device: 390x844.
// Self-contained (support/mock-admin.ts); run after build.sh.

import { expect, test } from "@playwright/test";
import * as F from "./support/fixtures";
import { mockAdmin, openConsole } from "./support/mock-admin";

test.use({ locale: "en-US", timezoneId: "UTC" });

const PHONE = { width: 390, height: 844 } as const;

test.describe("the phone shell", () => {
  test("the drawer names which console it is", async ({ page }) => {
    await page.setViewportSize(PHONE);
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/overview");
    await page.getByRole("button", { name: "Toggle menu" }).click();
    await expect(page.locator(".drawer-chip")).toHaveText("www.example.test:80");
  });

  test("the whole-server drawer says so", async ({ page }) => {
    await page.setViewportSize(PHONE);
    await mockAdmin(page, "pagespeed_global_admin", { ...F.allReplies(), config: F.CONFIG_GLOBAL });
    await openConsole(page, "pagespeed_global_admin", "#/overview");
    await page.getByRole("button", { name: "Toggle menu" }).click();
    await expect(page.locator(".drawer-chip")).toHaveText("Whole server");
  });

  test("the drawer closes on a keyboard navigation, not just a tap", async ({ page }) => {
    await page.setViewportSize(PHONE);
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/overview");
    const toggle = page.getByRole("button", { name: "Toggle menu" });
    await toggle.click();
    await expect(toggle).toHaveAttribute("aria-expanded", "true");
    await page.keyboard.press("g");
    await page.keyboard.press("s"); // Statistics
    await expect(page.getByRole("heading", { level: 1, name: "Statistics" })).toBeVisible();
    await expect(toggle).toHaveAttribute("aria-expanded", "false");
  });

  test("the document scrolls and the top bar stays put", async ({ page }) => {
    await page.setViewportSize(PHONE);
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/statistics");
    await expect(page.getByRole("heading", { level: 1, name: "Statistics" })).toBeVisible();
    const scrolled = await page.evaluate(() => {
      document.scrollingElement!.scrollTop = 600;
      return document.scrollingElement!.scrollTop;
    });
    expect(scrolled).toBeGreaterThan(0);
    const top = await page.evaluate(() => document.querySelector(".topbar")!.getBoundingClientRect().top);
    expect(top).toBe(0);
    expect(
      await page.evaluate(
        () => document.scrollingElement!.scrollWidth <= document.scrollingElement!.clientWidth + 1,
      ),
    ).toBe(true);
  });
});

test.describe("card tables", () => {
  test("the URL index stacks into labelled cards with every value visible", async ({ page }) => {
    await page.setViewportSize(PHONE);
    await mockAdmin(page, "pagespeed_global_admin", {
      ...F.allReplies(),
      config: F.CONFIG_GLOBAL,
      "v1/daemon/cache/urls": F.URLS_TWO_PAGES,
    });
    await openConsole(page, "pagespeed_global_admin", "#/urls");
    const table = page.getByTestId("urls-table");
    await expect(table.locator("thead").first()).toHaveCSS("position", "absolute");
    const hero = table.locator("tbody tr", { hasText: "hero.png" });
    await expect(hero).toBeVisible();
    await expect(hero.locator("[data-label='Host']")).toHaveText("www.example.test");
    await expect(hero.locator("[data-label='Variants']")).toHaveText("2");
    await expect(hero.locator("[data-label='Cooldown']")).toHaveText("—");
    expect(
      await page.evaluate(
        () => document.scrollingElement!.scrollWidth <= document.scrollingElement!.clientWidth + 1,
      ),
    ).toBe(true);
    // Every card sits on one surface colour: the table's zebra striping and
    // hover tint belong to the desktop table, not to the stacked cards.
    const cards = table.locator("tbody tr");
    const surfaces = await cards.evaluateAll((rows) =>
      rows.map((row) => getComputedStyle(row).backgroundColor),
    );
    expect(surfaces.length).toBeGreaterThan(1);
    expect(new Set(surfaces).size).toBe(1);
    await cards.nth(1).hover();
    expect(await cards.nth(1).evaluate((el) => getComputedStyle(el).backgroundColor)).toBe(surfaces[0]);
  });

  test("the URL detail variants stack the same way", async ({ page }) => {
    await page.setViewportSize(PHONE);
    await mockAdmin(page, "pagespeed_global_admin", {
      ...F.allReplies(),
      config: F.CONFIG_GLOBAL,
      "v1/daemon/cache/alternates": F.ALTERNATES_HERO,
    });
    await openConsole(
      page, "pagespeed_global_admin",
      "#/urls/detail?url=%2Fhero.png&host=www.example.test&scheme=https",
    );
    const table = page.getByTestId("variants-table");
    await expect(table.locator("thead").first()).toHaveCSS("position", "absolute");
    const webp = table.locator("tbody tr", { hasText: "webp" });
    await expect(webp.locator("[data-label='Size']")).toContainText("KB");
    await expect(webp.locator("[data-label='Hits']")).toHaveText("1,042");
  });
});
