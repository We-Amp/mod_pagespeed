// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The console shell: landmarks, grouped navigation, keyboard, focus,
// headings, announced errors. Self-contained (support/mock-admin.ts); run
// after build.sh.

import { expect, test, type Locator, type Page } from "@playwright/test";
import * as F from "./support/fixtures";
import { NETWORK_ERROR, mockAdmin, openConsole } from "./support/mock-admin";

test.use({ locale: "en-US", timezoneId: "UTC" });

const ROUTES = [
  "#/overview", "#/statistics", "#/configuration", "#/histograms", "#/caches", "#/console", "#/messages",
  "#/graphs", "#/optimizer", "#/support", "#/about",
];

async function ready(page: Page) {
  await expect(page.getByRole("heading", { level: 1 })).toBeVisible();
  await expect(page.locator("main .loading")).toHaveCount(0);
}

/** Tabs forward until `target` is the focused element, or gives up. */
async function tabTo(page: Page, target: Locator, max = 40): Promise<void> {
  for (let i = 0; i < max; i++) {
    if (await target.evaluate((el) => el === document.activeElement)) return;
    await page.keyboard.press("Tab");
  }
  await expect(target).toBeFocused();
}

/**
 * Some controls keep the default outline (a solid ring); the five
 * `.search-input`/`.form-input` rules trade it for a border-colour and
 * box-shadow change instead. Either counts as a visible focus indicator.
 */
async function expectVisibleFocus(target: Locator): Promise<void> {
  await expect(target).toBeFocused();
  const [outlineStyle, boxShadow] = await target.evaluate((el) => {
    const cs = getComputedStyle(el);
    return [cs.outlineStyle, cs.boxShadow];
  });
  expect(outlineStyle !== "none" || boxShadow !== "none", `outline-style: ${outlineStyle}; box-shadow: ${boxShadow}`).toBe(
    true,
  );
}

test.describe("Console shell", () => {
  test("has a banner, a labelled navigation and a main landmark", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/statistics");
    await ready(page);
    await expect(page.getByRole("banner")).toBeVisible();
    await expect(page.getByRole("navigation", { name: "Console pages" })).toBeVisible();
    await expect(page.getByRole("main")).toBeVisible();
  });

  test("groups the navigation into module, optimizer and help pages", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin");
    const nav = page.getByRole("navigation", { name: "Console pages" });
    await expect(nav.getByRole("list").first().getByRole("link")).toHaveText(["Overview", "Savings"]);
    await expect(nav.getByRole("list", { name: "Module" }).getByRole("link")).toHaveText([
      "Statistics", "Histograms", "Caches", "Configuration",
    ]);
    await expect(nav.getByRole("list", { name: "Optimizer" }).getByRole("link")).toHaveText(["Status", "Logs"]);
    await expect(nav.getByRole("list", { name: "Help" }).getByRole("link")).toHaveText(["Support", "About"]);
    await expect(page.locator(".nav-item")).toHaveCount(10);
  });

  test("the whole-server console's navigation gains the URLs page", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", {
      ...F.allReplies(),
      config: F.CONFIG_GLOBAL,
    });
    await openConsole(page, "pagespeed_global_admin");
    const nav = page.getByRole("navigation", { name: "Console pages" });
    await expect(nav.getByRole("list").first().getByRole("link")).toHaveText(["Overview", "Savings", "URLs"]);
    await expect(nav.getByRole("list", { name: "Optimizer" }).getByRole("link")).toHaveText(["Status", "Logs"]);
    await expect(page.locator(".nav-item")).toHaveCount(11);
  });

  test("marks the current page for assistive technology", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/daemon/cache");
    await ready(page);
    await expect(page.getByRole("link", { name: "Status", exact: true })).toHaveAttribute("aria-current", "page");
    await expect(page.locator('.nav-item[aria-current="page"]')).toHaveCount(1);
  });

  test("each entry names its shortcut beside it; the link's name is the page's", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/overview");
    const statistics = page.getByRole("link", { name: "Statistics", exact: true });
    await expect(statistics).toHaveAttribute("aria-keyshortcuts", "g s");
    await expect(page.locator(".nav-row", { has: statistics }).locator(".nav-key")).toHaveText("g s");
    await expect(page.getByRole("link", { name: "Status", exact: true })).toHaveAttribute("aria-keyshortcuts", "g d");
    await expect(page.getByRole("link", { name: "Support", exact: true })).not.toHaveAttribute("aria-keyshortcuts", /.+/);
  });

  test("Graphs is a view of Statistics: the sidebar marks Statistics, and the views switch", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/graphs");
    await expect(page.getByRole("heading", { level: 1, name: "Graphs" })).toBeVisible();
    await expect(page.getByRole("link", { name: "Statistics", exact: true })).toHaveAttribute("aria-current", "page");
    await expect(page.locator('.nav-item[aria-current="page"]')).toHaveCount(1);
    const views = page.getByRole("navigation", { name: "Statistics views" });
    await views.getByRole("link", { name: "Table" }).click();
    await expect(page).toHaveURL(/#\/statistics$/);
    await expect(page.getByRole("heading", { level: 1, name: "Statistics" })).toBeVisible();
    await views.getByRole("link", { name: "Graphs" }).click();
    await expect(page).toHaveURL(/#\/graphs$/);
  });

  test("phone: the drawer lists the same pages, without the shortcut hints", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await mockAdmin(page, "pagespeed_global_admin", { ...F.allReplies(), config: F.CONFIG_GLOBAL });
    await openConsole(page, "pagespeed_global_admin", "#/overview");
    await page.getByRole("button", { name: "Toggle menu" }).click();
    await expect(page.locator(".nav-item")).toHaveCount(11);
    await expect(page.locator(".nav-key").first()).toBeHidden();
  });

  test("the keyboard reaches the navigation; the new page's heading gets focus and names the tab", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/statistics");
    await ready(page);
    await page.getByRole("link", { name: "Logs", exact: true }).focus();
    await page.keyboard.press("Enter");
    await expect(page).toHaveURL(/#\/logs$/);
    await expect(page.getByRole("heading", { level: 1, name: "Logs" })).toBeFocused();
    await expect(page).toHaveTitle("Logs — mod_pagespeed Admin Console");
  });

  test("focus is visible when moving through the page with the keyboard", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/statistics");
    await ready(page);
    const overview = page.getByRole("link", { name: "Overview", exact: true });
    for (let i = 0; i < 10; i++) {
      await page.keyboard.press("Tab");
      if (await overview.evaluate((el) => el === document.activeElement)) break;
    }
    await expect(overview).toBeFocused();
    await expect(overview).toHaveCSS("outline-style", "solid");
  });

  test("the heading the shell focuses after a navigation draws no focus box; Tab still shows the ring", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/statistics");
    await ready(page);
    const outlineOf = (target: Locator) => target.evaluate((el) => getComputedStyle(el).outlineStyle);

    // A navigation made with the keyboard: the case Chromium counts as focus-visible.
    await page.getByRole("link", { name: "Histograms", exact: true }).focus();
    await page.keyboard.press("Enter");
    await expect(page).toHaveURL(/#\/histograms$/);
    const h1 = page.getByRole("heading", { level: 1, name: "Histograms" });
    await expect(h1).toBeFocused();
    expect(await outlineOf(h1)).toBe("none");

    // A navigation made by script (no user input at all).
    await page.evaluate(() => {
      window.location.hash = "#/optimizer?section=cache";
    });
    const section = page.locator("#optimizer-cache-heading");
    await expect(section).toBeFocused();
    expect(await outlineOf(section)).toBe("none");

    // The next Tab moves on and the ring is back for keyboard focus.
    await page.keyboard.press("Tab");
    await expectVisibleFocus(page.locator(":focus"));
    await expect(section).not.toHaveClass(/programmatic-focus/);
  });

  test("focus is visible on a search input and a sortable header button", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/statistics");
    await ready(page);
    const search = page.getByPlaceholder("Search name or description...");
    await tabTo(page, search);
    await expectVisibleFocus(search);
    // Statistics renders one table per family, so the "Name" sort button
    // exists per table; any one of them proves the focus point.
    const nameHeader = page.getByRole("button", { name: "Name" }).first();
    await tabTo(page, nameHeader);
    await expectVisibleFocus(nameHeader);
  });

  test("focus is visible on a tab", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), cache: F.CACHE_STRUCTURE_PURGE_ON });
    await openConsole(page, "pagespeed_admin", "#/caches");
    await ready(page);
    const structureTab = page.getByRole("tab", { name: "Cache Structure" });
    await tabTo(page, structureTab);
    await expectVisibleFocus(structureTab);
  });

  test("focus is visible on the connection banner's Retry now button", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), config: NETWORK_ERROR, stats_json: NETWORK_ERROR });
    await openConsole(page, "pagespeed_admin", "#/statistics");
    const retry = page.getByRole("button", { name: "Retry now" });
    await expect(retry).toBeVisible();
    await tabTo(page, retry);
    await expectVisibleFocus(retry);
  });

  test("skip to content moves focus to the page heading", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/statistics");
    await ready(page);
    await page.keyboard.press("Tab");
    await expect(page.getByRole("button", { name: "Skip to content" })).toBeFocused();
    await page.keyboard.press("Enter");
    await expect(page.getByRole("heading", { level: 1, name: "Statistics" })).toBeFocused();
  });

  test("the menu button says whether the menu is open", async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/statistics");
    const toggle = page.getByRole("button", { name: "Toggle menu" });
    await expect(toggle).toHaveAttribute("aria-expanded", "false");
    await toggle.click();
    await expect(toggle).toHaveAttribute("aria-expanded", "true");
  });

  test("a first-load error is announced", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), stats_json: F.MODULE_FAILURE });
    await openConsole(page, "pagespeed_admin", "#/statistics");
    await expect(page.getByRole("alert")).toContainText("statistics unavailable");
  });

  test("a failed first read of the configuration is retried once the connection recovers", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", {
      ...F.allReplies(),
      config: (call) => (call === 0 ? NETWORK_ERROR : F.CONFIG_VHOST),
    });
    await openConsole(page, "pagespeed_admin", "#/statistics");
    await ready(page);
    // The first read of /config failed, so the scope still guesses from the
    // path; a later request succeeding (the refresh button) restores the
    // connection and the shell tries /config again.
    await page.getByRole("button", { name: "Refresh", exact: true }).click();
    await expect(page.locator(".page-subtitle")).toContainText("www.example.test:80");
  });

  for (const hash of ROUTES) {
    test(`${hash} has one h1 and no skipped heading levels`, async ({ page }) => {
      await mockAdmin(page, "pagespeed_admin", F.allReplies());
      await openConsole(page, "pagespeed_admin", hash);
      await ready(page);
      const levels = await page
        .locator("main")
        .evaluate((m) => [...m.querySelectorAll("h1, h2, h3, h4, h5, h6")].map((h) => Number(h.tagName[1])));
      expect(levels[0]).toBe(1);
      expect(levels.filter((l) => l === 1)).toHaveLength(1);
      for (let i = 1; i < levels.length; i++) expect(levels[i], `${hash}: ${levels.join(",")}`).toBeLessThanOrEqual(levels[i - 1] + 1);
    });
  }
});

test.describe("top bar", () => {
  for (const scheme of ["light", "dark"] as const) {
    test.describe(scheme, () => {
      test.use({ colorScheme: scheme });
      test("is a dark band with white text", async ({ page }) => {
        // The whole-server console: only it carries the console label.
        await mockAdmin(page, "pagespeed_global_admin", { ...F.allReplies(), config: F.CONFIG_GLOBAL });
        await openConsole(page, "pagespeed_global_admin", "#/overview");
        const topbar = page.locator(".topbar");
        await expect(topbar).toBeVisible();
        await expect(topbar).toHaveCSS(
          "background-color",
          scheme === "dark" ? "rgb(23, 38, 63)" : "rgb(29, 78, 216)",
        );
        await expect(page.locator(".topbar-badge")).toHaveCSS("color", "rgb(255, 255, 255)");
        // The logo accent keeps its light fill in dark too: a fill tuned for
        // a light band disappears on the dark one.
        await expect(page.locator(".logo-accent").first()).toHaveCSS("fill", "rgb(96, 165, 250)");
      });
    });
  }
});

test.describe("shortcut dialog", () => {
  for (const viewport of [
    { width: 1440, height: 900 },
    { width: 390, height: 844 },
  ]) {
    test(`is centred at ${viewport.width}x${viewport.height}`, async ({ page }) => {
      await page.setViewportSize(viewport);
      await mockAdmin(page, "pagespeed_admin", F.allReplies());
      await openConsole(page, "pagespeed_admin", "#/overview");
      await page.getByRole("button", { name: "Keyboard shortcuts" }).click();
      const dialog = page.getByRole("dialog");
      await expect(dialog).toBeVisible();
      const box = await dialog.boundingBox();
      expect(box).not.toBeNull();
      expect(Math.abs(box!.x + box!.width / 2 - viewport.width / 2)).toBeLessThanOrEqual(2);
      expect(Math.abs(box!.y + box!.height / 2 - viewport.height / 2)).toBeLessThanOrEqual(2);
      // The trap still works: Tab from the close control wraps inside the dialog.
      await page.keyboard.press("Tab");
      expect(await page.evaluate(() => document.activeElement?.closest("dialog") !== null)).toBe(true);
    });
  }
});

test("the page heading takes focus without a ring for mouse users", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", F.allReplies());
  await openConsole(page, "pagespeed_admin", "#/overview");
  await page.locator(".nav-item", { hasText: "Statistics" }).click();
  const h1 = page.getByRole("heading", { level: 1, name: "Statistics" });
  await expect(h1).toBeFocused();
  await expect(h1).toHaveCSS("outline-style", "none");
  // A keyboard-made navigation (g o) moves focus the same way and draws no
  // ring either: that focus is the console's, not the user's. Keyboard Tab
  // focus keeps its ring (the "draws no focus box" case above). (The old
  // page's heading is gone from the DOM by now; the new heading holding
  // focus is what proves focus moved.)
  await page.keyboard.press("g");
  await page.keyboard.press("o");
  const home = page.getByRole("heading", { level: 1, name: "Overview" });
  await expect(home).toBeFocused();
  await expect(home).toHaveCSS("outline-style", "none");
});
