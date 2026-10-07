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
    // The dialog has exactly one focusable control (Close): Tab must not
    // escape it to reach anything behind the dialog, such as the nav.
    await page.keyboard.press("Tab");
    await expect(close).toBeFocused();
    await page.keyboard.press("Tab");
    await expect(close).toBeFocused();
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
