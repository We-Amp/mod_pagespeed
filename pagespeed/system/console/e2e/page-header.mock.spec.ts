// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Every page carries the shared header. Self-contained
// (support/mock-admin.ts); run after build.sh.

import { expect, test } from "@playwright/test";
import * as F from "./support/fixtures";
import { mockAdmin, openConsole } from "./support/mock-admin";

test.use({ locale: "en-US", timezoneId: "UTC" });

interface Route {
  hash: string;
  admin: "pagespeed_admin" | "pagespeed_global_admin";
  h1: string;
  polls: boolean;
}

const ROUTES: Route[] = [
  { hash: "#/statistics", admin: "pagespeed_admin", h1: "Statistics", polls: true },
  { hash: "#/configuration", admin: "pagespeed_admin", h1: "Configuration", polls: false },
  { hash: "#/histograms", admin: "pagespeed_admin", h1: "Histograms", polls: true },
  { hash: "#/caches", admin: "pagespeed_admin", h1: "Caches", polls: true },
  { hash: "#/logs", admin: "pagespeed_admin", h1: "Logs", polls: true },
  { hash: "#/graphs", admin: "pagespeed_admin", h1: "Graphs", polls: true },
  { hash: "#/optimizer", admin: "pagespeed_admin", h1: "Optimizer status", polls: true },
  { hash: "#/urls", admin: "pagespeed_global_admin", h1: "URLs", polls: true },
  { hash: "#/logs?source=optimizer", admin: "pagespeed_global_admin", h1: "Logs", polls: true },
  { hash: "#/support", admin: "pagespeed_admin", h1: "Support", polls: false },
  { hash: "#/about", admin: "pagespeed_admin", h1: "About", polls: false },
  { hash: "#/overview", admin: "pagespeed_admin", h1: "Overview", polls: true },
  { hash: "#/savings", admin: "pagespeed_admin", h1: "Savings", polls: true },
];

for (const route of ROUTES) {
  test(`#${route.hash.slice(2)} carries the shared page header`, async ({ page }) => {
    // The stock urls leaf answers 403 whole_server_console_only, which the
    // URLs page correctly treats as terminal (it stops polling, and the
    // stamp then reads "paused · updated …"); give it a real page so the
    // route is asserted in its polling state.
    const replies = route.admin === "pagespeed_global_admin"
      ? { ...F.allReplies(), config: F.CONFIG_GLOBAL, "v1/daemon/cache/urls": F.URLS_TWO_PAGES }
      : F.allReplies();
    await mockAdmin(page, route.admin, replies);
    await openConsole(page, route.admin, route.hash);
    const header = page.locator("main .page-header");
    await expect(header).toHaveCount(1);
    await expect(header.getByRole("heading", { level: 1, name: route.h1 })).toBeVisible();
    const toggle = page.getByRole("button", { name: /Auto-refresh · \d+ s/ });
    if (route.polls) {
      await expect(toggle).toBeVisible();
      await expect(page.getByRole("button", { name: "Refresh", exact: true })).toBeVisible();
      await expect(page.getByText(/^updated /)).toBeVisible();
    } else {
      await expect(toggle).toHaveCount(0);
    }
  });
}

test("Configuration keeps its manual refresh, and it still refetches", async ({ page }) => {
  const mock = await mockAdmin(page, "pagespeed_admin", F.allReplies());
  await openConsole(page, "pagespeed_admin", "#/configuration");
  await expect(page.getByRole("heading", { level: 1, name: "Configuration" })).toBeVisible();
  const refresh = page.getByRole("button", { name: "Refresh", exact: true });
  await expect(refresh).toBeVisible();
  const before = mock.calls("config");
  await refresh.click();
  await expect.poll(() => mock.calls("config")).toBeGreaterThan(before);
  await expect(page.locator("main .page-header")).toContainText("this virtual host");
});
