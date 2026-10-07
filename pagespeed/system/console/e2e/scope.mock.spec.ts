// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Which scope the console says it shows. Self-contained
// (support/mock-admin.ts); run after build.sh.

import { expect, test } from "@playwright/test";
import * as F from "./support/fixtures";
import { mockAdmin, openConsole } from "./support/mock-admin";

test.use({ locale: "en-US", timezoneId: "UTC" });

const daemon = { "v1/daemon/health": F.HEALTH_OK, "v1/daemon/stats": F.DAEMON_STATS };

test.describe("Console scope", () => {
  test("a nameless configured host falls back to the request host", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { config: F.CONFIG_VHOST_NAMELESS, stats_json: F.STATS_VHOST, ...daemon });
    await openConsole(page, "pagespeed_admin");
    await expect(page.getByTestId("overview-scope")).toContainText("This virtual host (console.test)");
    await expect(page.getByTestId("overview-scope")).not.toContainText(":0");
    await openConsole(page, "pagespeed_admin", "#/statistics");
    await expect(page.getByText("Statistics of this virtual host (console.test)")).toBeVisible();
    await openConsole(page, "pagespeed_admin", "#/configuration");
    await expect(page.getByText("Configuration of this virtual host (console.test)")).toBeVisible();
  });

  test("a renamed global admin path is labelled by the configuration", async ({ page }) => {
    await mockAdmin(page, "renamed_admin", { config: F.CONFIG_GLOBAL, stats_json: F.STATS_GLOBAL, ...daemon });
    await openConsole(page, "renamed_admin", "#/statistics");
    await expect(page.getByText("Aggregate statistics across all virtual hosts (process-wide)")).toBeVisible();
    await expect(page.locator(".topbar-badge")).toHaveText("Global Admin");
  });

  test("the configuration is read once for the whole console", async ({ page }) => {
    const mock = await mockAdmin(page, "pagespeed_admin", {
      config: F.CONFIG_VHOST,
      stats_json: F.STATS_VHOST,
      message_history: { status: 200, body: { scope: "process", next: 0, messages: [] } },
      ...daemon,
    });
    await openConsole(page, "pagespeed_admin");
    await expect(page.getByTestId("module-card")).toBeVisible();
    await page.locator(".nav-item", { hasText: "Statistics" }).click();
    await page.locator(".nav-item", { hasText: "Logs" }).click();
    await expect(page.getByRole("heading", { level: 1, name: "Logs" })).toBeVisible();
    expect(mock.calls("config")).toBe(1);
  });
});

test.describe("Scope chips and the console label", () => {
  const wholeServer = () => ({ ...F.allReplies(), config: F.CONFIG_GLOBAL, stats_json: F.STATS_GLOBAL, "v1/daemon/stats": F.OPT_STATS_PAGE });
  const perHost = () => ({ ...F.allReplies(), "v1/daemon/stats": F.OPT_STATS_PAGE });

  test("the whole-server console without a lens shows no scope chips; the cards still name their scope", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", wholeServer());
    await openConsole(page, "pagespeed_global_admin", "#/overview");
    await expect(page.getByTestId("module-card")).toBeVisible();
    await expect(page.locator("main .scope-chip")).toHaveCount(0);
    await expect(page.getByTestId("module-card")).toHaveAccessibleName(/whole server/);
    await expect(page.getByTestId("optimizer-card")).toHaveAccessibleName(/whole server/);
    await openConsole(page, "pagespeed_global_admin", "#/savings");
    await expect(page.getByTestId("savings-module-card")).toBeVisible();
    await expect(page.locator("main .scope-chip")).toHaveCount(0);
    await expect(page.getByTestId("savings-module-card")).toHaveAccessibleName(/whole server/);
    await expect(page.getByTestId("savings-optimizer-card")).toHaveAccessibleName(/whole server/);
  });

  test("under a host lens, the cards that do not follow it say whole server", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", wholeServer());
    await openConsole(page, "pagespeed_global_admin", "#/overview?lens=www.example.com");
    await expect(page.getByTestId("module-card").locator(".scope-chip")).toHaveText("whole server");
    await expect(page.getByTestId("optimizer-card").locator(".scope-chip")).toHaveText("whole server");
    await openConsole(page, "pagespeed_global_admin", "#/savings?lens=www.example.com");
    await expect(page.getByTestId("savings-lens-host")).toBeVisible();
    await expect(page.getByTestId("savings-module-card").locator(".scope-chip")).toHaveText("whole server");
    await expect(page.locator("#savings-optimizer-heading .scope-chip")).toHaveText("whole server");
    await expect(page.getByTestId("savings-lens-host").locator(".scope-chip")).toHaveText("this host");
  });

  test("a per-host console marks only the whole-server cards", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", perHost());
    await openConsole(page, "pagespeed_admin", "#/overview");
    await expect(page.getByTestId("module-card")).toBeVisible();
    await expect(page.getByTestId("module-card").locator(".scope-chip")).toHaveCount(0);
    await expect(page.getByTestId("module-card")).toHaveAccessibleName(/this host/);
    await expect(page.getByTestId("optimizer-card").locator(".scope-chip")).toHaveText("whole server");
    await openConsole(page, "pagespeed_admin", "#/savings");
    await expect(page.getByTestId("savings-module-card")).toBeVisible();
    await expect(page.getByTestId("savings-module-card").locator(".scope-chip")).toHaveCount(0);
    await expect(page.getByTestId("savings-module-card")).toHaveAccessibleName(/this host/);
    await expect(page.getByTestId("savings-optimizer-card").locator(".scope-chip")).toHaveText("whole server");
  });

  test("only the whole-server console carries a label in the top bar", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", perHost());
    await openConsole(page, "pagespeed_admin", "#/overview");
    await expect(page.getByTestId("module-card")).toBeVisible();
    await expect(page.locator(".topbar-badge")).toHaveCount(0);
    await expect(page.locator(".topbar")).not.toContainText("Admin");
  });

  test("the whole-server console is labelled as such", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", wholeServer());
    await openConsole(page, "pagespeed_global_admin", "#/overview");
    await expect(page.locator(".topbar-badge")).toHaveText("Global Admin");
  });
});
