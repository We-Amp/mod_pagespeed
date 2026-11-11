// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The phone shell and card tables, at the audit's device: 390x844, and the
// drawer as a keyboard user meets it at 390, 320 and 640 px.
// Self-contained (support/mock-admin.ts); run after build.sh.

import { expect, test, type Page } from "@playwright/test";
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

// The drawer layout applies to a phone and to a desktop window at 200% zoom
// and more (640 px is a 1280 px window at 200%).
for (const width of [390, 320, 640]) {
  test.describe(`the drawer at ${width} px`, () => {
    const nav = (page: Page) => page.locator("#console-nav");
    const main = (page: Page) => page.locator("main");
    const toggleOf = (page: Page) => page.getByRole("button", { name: "Toggle menu" });
    /** Where the focus is: "nav", "main", "topbar" or "none". */
    const focusPlace = (page: Page) =>
      page.evaluate(() => {
        const el = document.activeElement;
        if (el === null || el === document.body) return "none";
        if (el.closest("#console-nav")) return "nav";
        if (el.closest("main")) return "main";
        return el.closest(".topbar") ? "topbar" : "none";
      });
    async function tabStops(page: Page, presses: number): Promise<string[]> {
      const places: string[] = [];
      for (let i = 0; i < presses; i++) {
        await page.keyboard.press("Tab");
        places.push(await focusPlace(page));
      }
      return places;
    }

    async function open(page: Page, hash = "#/statistics") {
      await page.setViewportSize({ width, height: 844 });
      await mockAdmin(page, "pagespeed_admin", F.allReplies());
      await openConsole(page, "pagespeed_admin", hash);
      await expect(page.getByRole("heading", { level: 1 })).toBeVisible();
      await expect(page.locator("main .loading")).toHaveCount(0);
    }

    test("closed, the drawer is out of the tab order", async ({ page }) => {
      await open(page);
      await expect(nav(page)).toHaveAttribute("inert", "");
      await expect(main(page)).not.toHaveAttribute("inert");
      await toggleOf(page).focus();
      const places = await tabStops(page, 12);
      expect(places).not.toContain("nav");
      expect(places).toContain("main");
    });

    test("it opens with the focus on the current page's link, and Tab stays out of the page behind it", async ({ page }) => {
      await open(page);
      await toggleOf(page).focus();
      await page.keyboard.press("Enter");
      await expect(toggleOf(page)).toHaveAttribute("aria-expanded", "true");
      await expect(nav(page)).not.toHaveAttribute("inert");
      await expect(main(page)).toHaveAttribute("inert", "");
      const current = nav(page).getByRole("link", { name: "Statistics", exact: true });
      await expect(current).toHaveAttribute("aria-current", "page");
      await expect(current).toBeFocused();
      // More presses than the drawer and the top bar have stops: the focus goes round, never behind.
      const places = await tabStops(page, 40);
      expect(places).not.toContain("main");
      expect(places).toContain("nav");
      expect(places).toContain("topbar");
      // The page did not scroll sideways or away under the drawer.
      expect(await page.evaluate(() => document.scrollingElement!.scrollLeft)).toBe(0);
    });

    test("Escape closes it and returns the focus to the menu button", async ({ page }) => {
      await open(page);
      await toggleOf(page).click();
      await expect(nav(page).getByRole("link", { name: "Statistics", exact: true })).toBeFocused();
      await page.keyboard.press("Tab");
      await page.keyboard.press("Escape");
      await expect(toggleOf(page)).toHaveAttribute("aria-expanded", "false");
      await expect(toggleOf(page)).toBeFocused();
      await expect(nav(page)).toHaveAttribute("inert", "");
      await expect(main(page)).not.toHaveAttribute("inert");
      // Escape with the drawer closed is nobody's key.
      await page.keyboard.press("Escape");
      await expect(toggleOf(page)).toBeFocused();
      await expect(toggleOf(page)).toHaveAttribute("aria-expanded", "false");
    });

    test("the backdrop and the current page's own link close it without losing the focus", async ({ page }) => {
      await open(page);
      await toggleOf(page).click();
      await expect(nav(page).getByRole("link", { name: "Statistics", exact: true })).toBeFocused();
      await page.locator(".backdrop").click({ position: { x: width - 10, y: 400 } });
      await expect(toggleOf(page)).toHaveAttribute("aria-expanded", "false");
      await expect(toggleOf(page)).toBeFocused();
      await toggleOf(page).click();
      await page.keyboard.press("Enter"); // the current page's link: no navigation follows
      await expect(toggleOf(page)).toHaveAttribute("aria-expanded", "false");
      await expect(toggleOf(page)).toBeFocused();
      await expect(page.getByRole("heading", { level: 1, name: "Statistics" })).toBeVisible();
    });

    test("another page's link closes it and the focus lands on the new page's heading", async ({ page }) => {
      await open(page);
      await toggleOf(page).click();
      await nav(page).getByRole("link", { name: "Histograms", exact: true }).click();
      await expect(page.getByRole("heading", { level: 1, name: "Histograms" })).toBeFocused();
      await expect(toggleOf(page)).toHaveAttribute("aria-expanded", "false");
      await expect(nav(page)).toHaveAttribute("inert", "");
      await expect(main(page)).not.toHaveAttribute("inert");
    });

    test("the skip link closes an open drawer and reaches the page heading", async ({ page }) => {
      await open(page);
      await toggleOf(page).click();
      const skip = page.getByRole("button", { name: "Skip to content" });
      await skip.focus();
      await page.keyboard.press("Enter");
      await expect(page.getByRole("heading", { level: 1, name: "Statistics" })).toBeFocused();
      await expect(toggleOf(page)).toHaveAttribute("aria-expanded", "false");
    });

    test("the drawer slides only when motion is welcome", async ({ page }) => {
      await page.emulateMedia({ reducedMotion: "no-preference" });
      await open(page);
      const duration = () => nav(page).evaluate((el) => getComputedStyle(el).transitionDuration);
      expect(await duration()).toBe("0.2s");
      await page.emulateMedia({ reducedMotion: "reduce" });
      expect(await duration()).toBe("0s");
      // Without the slide it is in place at once.
      await toggleOf(page).click();
      expect(await nav(page).evaluate((el) => el.getBoundingClientRect().left)).toBe(0);
    });
  });
}

test.describe("the sidebar on a desktop", () => {
  test("is never inert, and growing the window out of the drawer layout closes the drawer", async ({ page }) => {
    await page.setViewportSize({ width: 1280, height: 800 });
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/statistics");
    await expect(page.getByRole("heading", { level: 1, name: "Statistics" })).toBeVisible();
    const nav = page.locator("#console-nav");
    const main = page.locator("main");
    await expect(nav).not.toHaveAttribute("inert");
    await expect(main).not.toHaveAttribute("inert");
    await expect(nav.getByRole("link", { name: "Statistics", exact: true })).toBeVisible();

    await page.setViewportSize(PHONE);
    await expect(nav).toHaveAttribute("inert", "");
    const toggle = page.getByRole("button", { name: "Toggle menu" });
    await toggle.click();
    await expect(main).toHaveAttribute("inert", "");
    await page.setViewportSize({ width: 1280, height: 800 });
    await expect(nav).not.toHaveAttribute("inert");
    await expect(main).not.toHaveAttribute("inert");
    await page.setViewportSize(PHONE);
    await expect(toggle).toHaveAttribute("aria-expanded", "false");
    await expect(nav).toHaveAttribute("inert", "");
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
