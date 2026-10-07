// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Optimizer status: health, load and cache on one page, and the retired
// optimizer pages landing on their sections. Self-contained
// (support/mock-admin.ts); run after build.sh.

import { expect, test, type Page } from "@playwright/test";
import * as F from "./support/fixtures";
import { mockAdmin, openConsole } from "./support/mock-admin";

test.use({ locale: "en-US", timezoneId: "UTC" });

async function ready(page: Page) {
  await expect(page.getByRole("heading", { level: 1, name: "Optimizer status" })).toBeVisible();
  await expect(page.locator("main .loading")).toHaveCount(0);
}

test("health, load and cache on one page, every leaf read once per refresh", async ({ page }) => {
  const mock = await mockAdmin(page, "pagespeed_admin", F.allReplies());
  await openConsole(page, "pagespeed_admin", "#/optimizer");
  await ready(page);
  for (const name of ["Health", "Load", "Cache"]) {
    await expect(page.getByRole("heading", { level: 2, name, exact: true })).toBeVisible();
  }
  await expect(page.locator(".status-badge")).toHaveText("ok");
  await expect(page.getByText("Notifications received")).toBeVisible();
  await expect(page.getByText("Cache entries")).toBeVisible();
  await page.getByRole("button", { name: "Refresh", exact: true }).click();
  await expect.poll(() => mock.calls("v1/daemon/stats")).toBeGreaterThanOrEqual(2);
  await page.waitForTimeout(300);
  expect(mock.calls("v1/daemon/health")).toBe(mock.calls("v1/daemon/stats"));
  expect(mock.calls("v1/daemon/cooldowns")).toBe(mock.calls("v1/daemon/stats"));
  await expect(page).toHaveTitle("Optimizer status — mod_pagespeed Admin Console");
  await expect(page.getByRole("link", { name: "Status", exact: true })).toHaveAttribute("aria-current", "page");
});

for (const [old, section, text] of [
  ["#/daemon/back-pressure", "load", "Notifications received"],
  ["#/daemon/cache", "cache", "Cache entries"],
] as const) {
  test(`${old} lands on the ${section} section`, async ({ page }) => {
    await page.setViewportSize({ width: 1280, height: 600 });
    await mockAdmin(page, "pagespeed_admin", F.allReplies());
    await openConsole(page, "pagespeed_admin", old);
    await expect(page).toHaveURL(new RegExp(`#/optimizer\\?section=${section}$`));
    await ready(page);
    await expect(page.getByText(text, { exact: true })).toBeVisible();
    await expect(page.locator(`#optimizer-${section}`)).toBeInViewport();
  });
}

test("#/daemon/status lands on the page itself", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", F.allReplies());
  await openConsole(page, "pagespeed_admin", "#/daemon/status");
  await expect(page).toHaveURL(/#\/optimizer$/);
  await ready(page);
});

test("Back after a redirect returns to the page before it", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", F.allReplies());
  await openConsole(page, "pagespeed_admin", "#/statistics");
  await expect(page.getByRole("heading", { level: 1, name: "Statistics" })).toBeVisible();
  await page.evaluate(() => {
    location.hash = "#/daemon/status";
  });
  await expect(page).toHaveURL(/#\/optimizer$/);
  await ready(page);
  await page.goBack();
  await expect(page).toHaveURL(/#\/statistics$/);
  await expect(page.getByRole("heading", { level: 1, name: "Statistics" })).toBeVisible();
});

test("a section link from another page moves focus to that section's heading", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", F.allReplies());
  await openConsole(page, "pagespeed_admin", "#/statistics");
  await expect(page.getByRole("heading", { level: 1, name: "Statistics" })).toBeVisible();
  await page.evaluate(() => {
    location.hash = "#/optimizer?section=cache";
  });
  await expect(page.getByRole("heading", { level: 2, name: "Cache", exact: true })).toBeFocused();
});

test("no optimizer at all: one explanation for the whole page", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", {
    ...F.allReplies(),
    "v1/daemon/health": F.UNREACHABLE,
    "v1/daemon/stats": F.UNREACHABLE,
    "v1/daemon/cooldowns": F.UNREACHABLE,
  });
  await openConsole(page, "pagespeed_admin", "#/optimizer");
  await ready(page);
  await expect(page.getByTestId("daemon-unreachable")).toHaveCount(1);
  await expect(page.getByTestId("daemon-unreachable")).toContainText("unreachable");
  await expect(page.getByTestId("health-unavailable")).toHaveCount(0);
  await expect(page.getByTestId("cache-unavailable")).toHaveCount(0);
});

test("health answers and statistics do not: each section says what it has", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", {
    config: F.CONFIG_VHOST,
    "v1/daemon/health": F.HEALTH_OK,
    "v1/daemon/stats": F.UNSUPPORTED,
  });
  await openConsole(page, "pagespeed_admin", "#/optimizer");
  await ready(page);
  await expect(page.locator(".status-badge")).toHaveText("ok");
  await expect(page.getByTestId("daemon-unreachable")).toHaveCount(1);
  await expect(page.getByTestId("daemon-unreachable")).toContainText("this optimizer version does not provide this panel");
  await expect(page.getByTestId("cache-unavailable")).toBeVisible();
});

test("statistics answer and health does not: the health section says so", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", { config: F.CONFIG_VHOST, "v1/daemon/stats": F.DAEMON_STATS });
  await openConsole(page, "pagespeed_admin", "#/optimizer");
  await ready(page);
  await expect(page.getByTestId("health-unavailable")).toBeVisible();
  await expect(page.getByText("Cache entries")).toBeVisible();
  await expect(page.getByTestId("daemon-unreachable")).toHaveCount(0);
});

test("phone: the status page does not scroll sideways", async ({ page }) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await mockAdmin(page, "pagespeed_admin", {
    ...F.allReplies(),
    "v1/daemon/cooldowns": F.ok({
      cooldowns: [{ url: `https://www.example.test/${"x".repeat(300)}.png`, reason: "processing", remaining_seconds: 30, duration_seconds: 60 }],
    }),
  });
  await openConsole(page, "pagespeed_admin", "#/optimizer");
  await ready(page);
  expect(
    await page.evaluate(() => document.scrollingElement!.scrollWidth <= document.scrollingElement!.clientWidth + 1),
  ).toBe(true);
});
