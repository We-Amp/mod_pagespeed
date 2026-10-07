// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The optimizer daemon's pages: serve savings as the module records them,
// readable health checks, explained back-pressure counters. Self-contained
// (support/mock-admin.ts); run after build.sh.

import { expect, test } from "@playwright/test";
import * as F from "./support/fixtures";
import { mockAdmin, openConsole } from "./support/mock-admin";

test.use({ locale: "en-US", timezoneId: "UTC" });

test.describe("Daemon panels", () => {
  test("serve savings: recorded by the module, per class, with unserved classes named instead of zero rows", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { config: F.CONFIG_VHOST, "v1/daemon/stats": F.DAEMON_STATS });
    await openConsole(page, "pagespeed_admin", "#/daemon/cache");
    await expect(page.getByTestId("serve-savings-source")).toContainText("Recorded by this web server's module");
    const table = page.getByTestId("serve-savings-table");
    await expect(table.getByRole("rowheader")).toHaveText(["CSS", "Images", "Total"]);
    await expect(table).toContainText("648 KB (72%)");
    await expect(page.getByTestId("serve-savings-not-served")).toHaveText(
      "Not served from the optimizer's cache on this server so far: HTML, JavaScript.",
    );
    // The raw counters sit behind a collapsed disclosure, out of the default view.
    const raw = page.getByTestId("serve-savings-raw");
    await expect(raw).toBeVisible();
    await expect(raw).not.toHaveAttribute("open", "");
    await raw.getByText("Raw counters").click();
    await expect(raw.getByText(/css\.hits/)).toBeVisible();
  });

  test("serve savings: nothing served yet", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { config: F.CONFIG_VHOST, "v1/daemon/stats": F.DAEMON_STATS_EMPTY });
    await openConsole(page, "pagespeed_admin", "#/daemon/cache");
    await expect(page.getByTestId("serve-savings-none")).toBeVisible();
    await expect(page.getByTestId("serve-savings-table")).toHaveCount(0);
    await expect(page.getByTestId("serve-savings-not-served")).toContainText("CSS, Images");
  });

  test("serve savings: an optimizer that reports none", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", {
      config: F.CONFIG_VHOST,
      "v1/daemon/stats": { status: 200, body: { cache: { entries: 3, size_bytes: 1024 } } },
    });
    await openConsole(page, "pagespeed_admin", "#/daemon/cache");
    await expect(page.getByTestId("serve-savings-absent")).toBeVisible();
  });

  test("serve savings: the optimizer unreachable keeps the existing empty state", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { config: F.CONFIG_VHOST, "v1/daemon/stats": F.UNREACHABLE });
    await openConsole(page, "pagespeed_admin", "#/daemon/cache");
    await expect(page.getByTestId("daemon-unreachable")).toBeVisible();
  });

  test("serve savings end in a total row", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", {
      config: F.CONFIG_VHOST,
      "v1/daemon/health": F.HEALTH_OK,
      "v1/daemon/stats": F.DAEMON_STATS,
    });
    await openConsole(page, "pagespeed_admin", "#/daemon/cache");
    const total = page.getByTestId("serve-savings-total");
    await expect(total).toContainText("242");
    await expect(total).toContainText("21.4 MB");
    await expect(total).toContainText("20.7 MB");
    await expect(total).toContainText("673 KB (3%)");
  });

  test("a stats endpoint the optimizer is too old for reads as unsupported", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", {
      config: F.CONFIG_VHOST,
      "v1/daemon/health": F.HEALTH_BELOW_FLOOR,
      "v1/daemon/stats": F.UNSUPPORTED,
    });
    await openConsole(page, "pagespeed_admin", "#/daemon/cache");
    await expect(page.getByTestId("daemon-unreachable")).toBeVisible();
    await expect(page.getByTestId("serve-savings-raw")).toHaveCount(0);
  });

  test("the global console shows the same daemon savings", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", {
      config: F.CONFIG_GLOBAL,
      "v1/daemon/health": F.HEALTH_OK,
      "v1/daemon/stats": F.DAEMON_STATS,
    });
    await openConsole(page, "pagespeed_global_admin", "#/daemon/cache");
    await expect(page.getByTestId("serve-savings-total")).toContainText("673 KB (3%)");
  });

  test("health checks read as pass or fail, not as JSON", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { config: F.CONFIG_VHOST, "v1/daemon/health": F.HEALTH_CHECK_FAILING });
    await openConsole(page, "pagespeed_admin", "#/daemon/status");
    await expect(page.getByRole("row", { name: /cache_configured/ })).toContainText("Pass");
    await expect(page.getByRole("row", { name: /cache_open/ })).toContainText("Fail");
    await expect(page.getByText('{"pass"')).toHaveCount(0);
  });

  test("key-value panels lead with the value, and the status pill reads in sentence case", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", {
      config: F.CONFIG_VHOST,
      "v1/daemon/health": F.HEALTH_OK,
      "v1/daemon/stats": F.DAEMON_STATS,
    });
    await openConsole(page, "pagespeed_admin", "#/daemon/status");
    const uptime = page.locator(".info-item", { hasText: "Uptime" });
    await expect(uptime.locator(".info-value")).toHaveCSS("font-size", "24px");
    await expect(uptime.locator(".info-label")).toHaveCSS("text-transform", "none");
    await expect(page.locator(".status-badge")).toHaveText("ok");
    await expect(page.locator(".status-badge")).toHaveCSS("text-transform", "capitalize");
    await expect(page.locator(".ready-flag")).toHaveCSS("text-transform", "capitalize");
  });

  test("back-pressure explains the skipped notifications", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", {
      config: F.CONFIG_VHOST,
      "v1/daemon/stats": F.DAEMON_STATS,
      "v1/daemon/cooldowns": { status: 200, body: { cooldowns: [] } },
    });
    await openConsole(page, "pagespeed_admin", "#/daemon/back-pressure");
    await expect(page.getByText("URLs the optimizer had already handled")).toBeVisible();
    await expect(page.getByText("URLs the optimizer was already working on")).toBeVisible();
  });
});
