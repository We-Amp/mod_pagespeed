// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Keyboard shortcuts. Self-contained (support/mock-admin.ts); run after build.sh.

import { expect, test } from "@playwright/test";
import * as F from "./support/fixtures";
import { mockAdmin, openConsole } from "./support/mock-admin";

test.use({ locale: "en-US", timezoneId: "UTC" });

test.describe("Keyboard shortcuts", () => {
  test("? lists the shortcuts; Escape closes the list", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/statistics");
    await expect(page.getByRole("heading", { level: 1, name: "Statistics" })).toBeVisible();
    await page.keyboard.press("?");
    const dialog = page.getByRole("dialog", { name: "Keyboard shortcuts" });
    await expect(dialog).toBeVisible();
    await expect(dialog).toContainText("Go to Optimizer status");
    await page.keyboard.press("Escape");
    await expect(dialog).toBeHidden();
  });

  test("the top bar's button opens the same list", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/statistics");
    await page.getByRole("button", { name: "Keyboard shortcuts" }).click();
    await expect(page.getByRole("dialog", { name: "Keyboard shortcuts" })).toBeVisible();
  });

  test("the dialog traps Tab inside it, and Escape returns focus to the button that opened it", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/statistics");
    const opener = page.getByRole("button", { name: "Keyboard shortcuts" });
    await opener.click();
    const dialog = page.getByRole("dialog", { name: "Keyboard shortcuts" });
    await expect(dialog).toBeVisible();
    const close = page.getByRole("button", { name: "Close" });
    const toggle = dialog.getByRole("switch", { name: "Use keyboard shortcuts" });
    // The dialog's controls are the switch and Close: Tab must not escape
    // them to reach anything behind the dialog, such as the nav.
    await page.keyboard.press("Tab");
    await expect(toggle).toBeFocused();
    await page.keyboard.press("Tab");
    await expect(close).toBeFocused();
    await page.keyboard.press("Tab");
    await expect(toggle).toBeFocused();
    await page.keyboard.press("Shift+Tab");
    await expect(close).toBeFocused();
    await page.keyboard.press("Escape");
    await expect(dialog).toBeHidden();
    await expect(opener).toBeFocused();
  });

  test("g then s goes to Statistics", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin");
    await expect(page.getByRole("heading", { level: 1, name: "Overview" })).toBeVisible();
    await page.keyboard.press("g");
    await page.keyboard.press("s");
    await expect(page).toHaveURL(/#\/statistics$/);
    await expect(page.getByRole("heading", { level: 1, name: "Statistics" })).toBeFocused();
  });

  test("g then l opens Logs", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", {
      ...F.allReplies(),
      config: F.CONFIG_GLOBAL,
      "v1/daemon/logs": F.LOGS_PAGE,
    });
    await openConsole(page, "pagespeed_global_admin");
    await expect(page.getByRole("heading", { level: 1, name: "Overview" })).toBeVisible();
    await page.keyboard.press("g");
    await page.keyboard.press("l");
    await expect(page).toHaveURL(/#\/logs$/);
    await expect(page.getByRole("heading", { level: 1, name: "Logs" })).toBeFocused();
  });

  test("g then u goes to URLs", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", {
      ...F.allReplies(),
      config: F.CONFIG_GLOBAL,
    });
    await openConsole(page, "pagespeed_global_admin");
    await expect(page.getByRole("heading", { level: 1, name: "Overview" })).toBeVisible();
    await page.keyboard.press("g");
    await page.keyboard.press("u");
    await expect(page).toHaveURL(/#\/urls$/);
    await expect(page.getByRole("heading", { level: 1, name: "URLs" })).toBeFocused();
  });

  test("g then v goes to Savings", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin");
    await expect(page.getByRole("heading", { level: 1, name: "Overview" })).toBeVisible();
    await page.keyboard.press("g");
    await page.keyboard.press("v");
    await expect(page).toHaveURL(/#\/savings$/);
    await expect(page.getByRole("heading", { level: 1, name: "Savings" })).toBeFocused();
  });

  test("typing in a search box is not a shortcut", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/statistics");
    const search = page.getByPlaceholder("Search name or description...");
    await search.click();
    await page.keyboard.type("gs?r");
    await expect(search).toHaveValue("gs?r");
    await expect(page).toHaveURL(/#\/statistics$/);
    await expect(page.getByRole("dialog")).toHaveCount(0);
  });

  test("a checkbox does not take the keys: the shortcuts work from it", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/statistics");
    const box = page.getByRole("checkbox", { name: "Description column" });
    await box.focus();
    await page.keyboard.press("g");
    await page.keyboard.press("h");
    await expect(page).toHaveURL(/#\/histograms$/);
  });

  test("g then p goes to Rule packs", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin");
    await expect(page.getByRole("heading", { level: 1, name: "Overview" })).toBeVisible();
    await page.keyboard.press("g");
    await page.keyboard.press("p");
    await expect(page).toHaveURL(/#\/rule-packs$/);
    await expect(page.getByRole("heading", { level: 1, name: "Rule packs" })).toBeFocused();
  });

  test("r refreshes the page's data; / jumps to its search box", async ({ page }) => {
    const mock = await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/statistics");
    await expect(page.getByText("css_filter_total_bytes_saved").first()).toBeVisible();
    const before = mock.calls("stats_json");
    await page.keyboard.press("r");
    await expect.poll(() => mock.calls("stats_json")).toBeGreaterThanOrEqual(before + 1);
    await page.keyboard.press("/");
    await expect(page.getByPlaceholder("Search name or description...")).toBeFocused();
  });
});

test.describe("Keyboard shortcuts: the list names this console's pages", () => {
  test("whole-server: the list has URLs and Rule packs", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", { ...F.allReplies(), config: F.CONFIG_GLOBAL });
    await openConsole(page, "pagespeed_global_admin", "#/statistics");
    await page.keyboard.press("?");
    const dialog = page.getByRole("dialog", { name: "Keyboard shortcuts" });
    await expect(dialog.locator("kbd", { hasText: /^g u$/ })).toHaveCount(1);
    await expect(dialog).toContainText("Go to URLs");
    await expect(dialog).toContainText("Go to Rule packs");
  });

  test("per-host: no URLs row, and g u stays on the page", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/statistics");
    await expect(page.getByRole("heading", { level: 1, name: "Statistics" })).toBeVisible();
    await page.keyboard.press("?");
    const dialog = page.getByRole("dialog", { name: "Keyboard shortcuts" });
    await expect(dialog).toContainText("Go to Statistics");
    await expect(dialog).toContainText("Go to Rule packs");
    await expect(dialog.locator("kbd", { hasText: /^g u$/ })).toHaveCount(0);
    await expect(dialog).not.toContainText("Go to URLs");
    await page.keyboard.press("Escape");
    await expect(dialog).toBeHidden();
    await page.keyboard.press("g");
    await page.keyboard.press("u");
    await expect(page).toHaveURL(/#\/statistics$/);
  });
});

test.describe("Keyboard shortcuts: the off switch", () => {
  test("on by default; turned off, no key does anything and the sidebar hides its hints; remembered after a reload", async ({ page }) => {
    await page.setViewportSize({ width: 1280, height: 800 });
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/statistics");
    await expect(page.getByRole("heading", { level: 1, name: "Statistics" })).toBeVisible();
    const opener = page.getByRole("button", { name: "Keyboard shortcuts" });
    await opener.click();
    const dialog = page.getByRole("dialog", { name: "Keyboard shortcuts" });
    const toggle = dialog.getByRole("switch", { name: "Use keyboard shortcuts" });
    await expect(toggle).toBeChecked();
    await toggle.click();
    await expect(toggle).not.toBeChecked();
    await expect(dialog.getByTestId("shortcut-switch-note")).toContainText("Off in this browser");
    await dialog.getByRole("button", { name: "Close" }).click();
    await expect(dialog).toBeHidden();

    const hints = page.locator(".nav-key");
    await page.getByRole("link", { name: "Statistics", exact: true }).hover();
    await expect(hints).toHaveCount(0);

    await page.locator("main h1").click();
    await page.keyboard.press("?");
    await expect(dialog).toBeHidden();
    await page.keyboard.press("g");
    await page.keyboard.press("h");
    await expect(page).toHaveURL(/#\/statistics$/);
    await page.keyboard.press("/");
    await expect(page.getByPlaceholder("Search name or description...")).not.toBeFocused();

    await page.reload();
    await expect(page.getByRole("heading", { level: 1, name: "Statistics" })).toBeVisible();
    await page.keyboard.press("?");
    await expect(dialog).toBeHidden();
    await expect(page.locator(".nav-key")).toHaveCount(0);

    // The button still opens the list, where they are turned back on.
    await opener.click();
    await expect(toggle).not.toBeChecked();
    await toggle.click();
    await expect(toggle).toBeChecked();
    await page.keyboard.press("Escape");
    await expect(dialog).toBeHidden();
    await expect(page.locator(".nav-key").first()).toHaveText("g o");
    await page.locator("main h1").click();
    await page.keyboard.press("g");
    await page.keyboard.press("h");
    await expect(page).toHaveURL(/#\/histograms$/);
  });

  test("storage that refuses: the shortcuts are on, and the switch still works for the page view", async ({ page }) => {
    await page.addInitScript(() => {
      Object.defineProperty(window, "localStorage", {
        configurable: true,
        get() {
          throw new DOMException("denied", "SecurityError");
        },
      });
    });
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/statistics");
    await expect(page.getByRole("heading", { level: 1, name: "Statistics" })).toBeVisible();
    await page.keyboard.press("?");
    const dialog = page.getByRole("dialog", { name: "Keyboard shortcuts" });
    const toggle = dialog.getByRole("switch", { name: "Use keyboard shortcuts" });
    await expect(toggle).toBeChecked();
    await toggle.click();
    await expect(toggle).not.toBeChecked();
    await page.keyboard.press("Escape");
    await expect(dialog).toBeHidden();
    await page.locator("main h1").click();
    await page.keyboard.press("?");
    await expect(dialog).toBeHidden();
  });
});
