// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { expect, test } from "@playwright/test";
import { mockAdmin, openConsole } from "./support/mock-admin";
import * as F from "./support/fixtures";

test.use({ locale: "en-US", timezoneId: "UTC" });

test("shows when the counters were last refreshed", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", { config: F.CONFIG_VHOST, stats_json: F.STATS_VHOST });
  await openConsole(page, "pagespeed_admin", "#/statistics");
  await expect(page.getByTestId("page-updated")).toBeVisible();
});

test("the shared page header carries the title, scope, and refresh controls", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", F.allReplies());
  await openConsole(page, "pagespeed_admin", "#/statistics");
  await expect(page.getByRole("heading", { level: 1, name: "Statistics" })).toBeVisible();
  const toggle = page.getByRole("button", { name: "Auto-refresh · 5 s" });
  await expect(toggle).toHaveAttribute("aria-pressed", "true");
  await expect(page.getByRole("button", { name: "Refresh", exact: true })).toBeVisible();
  const stamp = page.getByText(/^updated /);
  await expect(stamp).toBeVisible();
  await expect(stamp).toHaveAttribute("title", /^\d{4}-\d{2}-\d{2}T/);
  await toggle.click();
  await expect(toggle).toHaveAttribute("aria-pressed", "false");
  await expect(page.getByText(/^paused · updated /)).toBeVisible();
});

test("the page uses the shared full-bleed width", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", F.allReplies());
  await openConsole(page, "pagespeed_admin", "#/statistics");
  await expect(page.locator("main .page")).toHaveCSS("max-width", "none");
});

test("counters group into named, collapsible families", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", F.allReplies());
  await openConsole(page, "pagespeed_admin", "#/statistics");
  const groups = page.locator(".stat-group");
  await expect(groups).toHaveCount(5);
  // All five of this fixture's prefixes (css, image, ipro, javascript, num)
  // are curated, and curated groups order by label.localeCompare —
  // "Counts" sorts before "CSS". The array form pins the full order.
  await expect(groups.locator("summary")).toContainText([
    "Counts", "CSS", "Images", "In-place optimization", "JavaScript",
  ]);
  const ipro = groups.filter({ hasText: "In-place optimization" });
  await expect(ipro.locator("tbody tr")).toHaveCount(2);
  await ipro.locator("summary").click();
  await expect(ipro.locator("tbody tr").first()).toBeHidden();
});

test("the delta column is opt-in and shows movement since the page opened", async ({ page }) => {
  const firstVars = (F.STATS_VHOST.body as { variables: Record<string, number> }).variables;
  const secondReply = {
    status: 200,
    body: {
      variables: {
        ...firstVars,
        num_flushes: firstVars.num_flushes + 25,
        ipro_daemon_served: Math.max(0, firstVars.ipro_daemon_served - 3),
      },
      maxlength: 60,
    },
  };
  await mockAdmin(page, "pagespeed_admin", {
    ...F.allReplies(),
    stats_json: (call) => (call === 0 ? F.STATS_VHOST : secondReply),
  });
  await openConsole(page, "pagespeed_admin", "#/statistics");
  await expect(page.getByRole("columnheader", { name: "Δ since open" })).toHaveCount(0);
  await page.getByRole("checkbox", { name: "Δ since open" }).check();
  await expect(page.getByRole("columnheader", { name: "Δ since open" }).first()).toBeVisible();
  await page.getByRole("button", { name: "Refresh", exact: true }).click();
  const counts = page.locator(".stat-group", { hasText: "Counts" });
  await expect(counts.locator("tr", { hasText: "num_flushes" })).toContainText("+25");
  const ipro = page.locator(".stat-group", { hasText: "In-place optimization" });
  await expect(ipro.locator("tr", { hasText: "ipro_daemon_served" })).toContainText("-3");
  await expect(page.locator(".delta-pos").first()).toBeVisible();
  await expect(page.locator(".delta-neg").first()).toBeVisible();
});

test("descriptions render as a second line in the name cell, only while the toggle is on", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", F.allReplies());
  await openConsole(page, "pagespeed_admin", "#/statistics");
  await expect(page.getByRole("columnheader", { name: "Description" })).toHaveCount(0);
  await expect(page.locator("tr", { hasText: "num_flushes" }).locator(".stat-desc")).toBeVisible();
  await page.getByRole("checkbox", { name: "Description column" }).uncheck();
  await expect(page.locator(".stat-desc")).toHaveCount(0);
});

test("the old Console page lands on Statistics with the Δ column on", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", F.allReplies());
  await openConsole(page, "pagespeed_admin", "#/console");
  await expect(page).toHaveURL(/#\/statistics\?delta=1$/);
  await expect(page.getByRole("heading", { level: 1, name: "Statistics" })).toBeVisible();
  await expect(page.getByRole("checkbox", { name: "Δ since open" })).toBeChecked();
  await expect(page.getByRole("button", { name: "Δ since open" }).first()).toBeVisible();
});
