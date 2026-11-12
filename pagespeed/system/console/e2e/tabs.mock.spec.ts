// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { expect, test } from "@playwright/test";
import * as F from "./support/fixtures";
import { mockAdmin, openConsole } from "./support/mock-admin";

test.use({ locale: "en-US", timezoneId: "UTC" });

test.describe("Caches tabs", () => {
  test("are a tablist with arrow-key navigation", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", "#/caches");
    const tabs = page.getByRole("tablist", { name: "Cache views" });
    await expect(tabs).toBeVisible();
    const structure = page.getByRole("tab", { name: "Cache Structure" });
    const lookup = page.getByRole("tab", { name: "Cache Lookup" });
    await expect(structure).toHaveAttribute("aria-selected", "true");
    await expect(lookup).toHaveAttribute("aria-selected", "false");
    await expect(lookup).toHaveAttribute("tabindex", "-1");
    await structure.focus();
    await page.keyboard.press("ArrowRight");
    await expect(lookup).toHaveAttribute("aria-selected", "true");
    await expect(lookup).toBeFocused();
  });
});

test.describe("Configuration tabs", () => {
  test("the view switch is a tablist, not segmented buttons (per-host console)", async ({ page }) => {
    // The stock config fixture is empty; the tabs need real config text.
    await mockAdmin(page, "pagespeed_admin", { ...F.allReplies(), config: F.CONFIG_VHOST_WITH_TEXT });
    await openConsole(page, "pagespeed_admin", "#/configuration");
    const tabs = page.getByRole("tablist", { name: "Configuration view" });
    await expect(tabs).toBeVisible();
    const site = page.getByRole("tab", { name: "This site" });
    const effective = page.getByRole("tab", { name: "Effective" });
    await expect(site).toHaveAttribute("aria-selected", "true");
    await expect(page.getByRole("tabpanel", { name: "This site" })).toBeVisible();
    await site.focus();
    await page.keyboard.press("ArrowRight");
    await expect(effective).toHaveAttribute("aria-selected", "true");
    await expect(page.getByRole("tabpanel", { name: "Effective" })).toBeVisible();
  });

  test("the whole-server console names its first tab Whole server", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", {
      ...F.allReplies(),
      config: F.ok({ ...(F.CONFIG_VHOST_WITH_TEXT.body as Record<string, unknown>), scope: "global", host: "" }),
    });
    await openConsole(page, "pagespeed_global_admin", "#/configuration");
    await expect(page.getByRole("tablist", { name: "Configuration view" }).getByRole("tab")).toHaveText([
      "Whole server",
      "Effective",
    ]);
    await expect(page.getByRole("tab", { name: "Whole server" })).toHaveAttribute("aria-selected", "true");
    await expect(page.getByRole("tabpanel", { name: "Whole server" })).toBeVisible();
  });
});
