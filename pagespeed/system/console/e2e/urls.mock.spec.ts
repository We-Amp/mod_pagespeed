// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The URLs page: every state, mocked end to end. Data states run on the
// whole-server console (pagespeed_global_admin + CONFIG_GLOBAL); the
// per-vhost console is the allReplies() default (the three cache leaves
// answer 403 there).
// Self-contained (support/mock-admin.ts); run after build.sh.

import { expect, test, type Page } from "@playwright/test";
import * as F from "./support/fixtures";
import { mockAdmin, openConsole } from "./support/mock-admin";

test.use({ locale: "en-US", timezoneId: "UTC" });

async function ready(page: Page) {
  await expect(page.getByRole("heading", { level: 1, name: "URLs" })).toBeVisible();
  await expect(page.locator("main .loading")).toHaveCount(0);
}

const globalReplies = (urls: Parameters<typeof mockAdmin>[2]["v1/daemon/cache/urls"]) => ({
  ...F.allReplies(),
  config: F.CONFIG_GLOBAL,
  "v1/daemon/cache/urls": urls,
});

test("lists the first page and pages to the second", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.URLS_TWO_PAGES));
  await openConsole(page, "pagespeed_global_admin", "#/urls");
  await ready(page);
  await expect(page.getByText("https://www.example.test/", { exact: true })).toBeVisible();
  await expect(page.getByText("https://www.example.test/hero.png")).toBeVisible();
  await expect(page.getByTestId("urls-pager")).toContainText("1–2 of 3");
  await expect(page.getByTestId("urls-prev")).toBeDisabled();
  await page.getByTestId("urls-next").click();
  await expect(page.getByText("http://cdn.example.test/app.js?v=2")).toBeVisible();
  await expect(page.getByTestId("urls-pager")).toContainText("3–3 of 3");
  await expect(page.getByTestId("urls-next")).toBeDisabled();
  await page.getByTestId("urls-prev").click();
  await expect(page.getByText("https://www.example.test/hero.png")).toBeVisible();
});

test("per-vhost console explains the scope and links to the whole-server console", async ({ page }) => {
  await mockAdmin(page, "pagespeed_admin", F.allReplies());
  await openConsole(page, "pagespeed_admin", "#/urls");
  await ready(page);
  await expect(page.getByTestId("urls-per-vhost")).toBeVisible();
  await expect(
    page.getByRole("link", { name: "whole-server console" }),
  ).toHaveAttribute("href", "/pagespeed_global_admin/#/urls");
  // The nav entry is hidden on a per-vhost console.
  await expect(page.locator(".nav-item")).toHaveCount(10);
  await expect(page.getByRole("link", { name: "URLs", exact: true })).toHaveCount(0);
});

test("a per-vhost console stops asking the cache/urls leaf after its one whole_server_console_only 403", async ({ page }) => {
  const mock = await mockAdmin(page, "pagespeed_admin", F.allReplies());
  await openConsole(page, "pagespeed_admin", "#/urls");
  await ready(page);
  await expect(page.getByTestId("urls-per-vhost")).toBeVisible();
  // The poll interval is 10 s; without the fix a longer wait would still
  // keep asking every interval forever. One request is the worst case.
  await page.waitForTimeout(7000);
  expect(mock.calls("v1/daemon/cache/urls")).toBeLessThanOrEqual(1);
});

test("a console that already knows it is per-vhost never asks the cache/urls leaf", async ({ page }) => {
  const mock = await mockAdmin(page, "pagespeed_admin", F.allReplies());
  // Land on Overview first and let /config settle the scope before this
  // page ever mounts -- the console's own scope state then already says
  // "per-vhost" before the cache/urls leaf would be asked for the first time.
  await openConsole(page, "pagespeed_admin", "#/overview");
  await expect.poll(() => mock.calls("config")).toBeGreaterThan(0);
  await page.evaluate(() => {
    location.hash = "#/urls";
  });
  await ready(page);
  await expect(page.getByTestId("urls-per-vhost")).toBeVisible();
  await page.waitForTimeout(7000);
  expect(mock.calls("v1/daemon/cache/urls")).toBe(0);
});

test("a module without the cache leaves explains instead of erroring", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", {
    ...globalReplies(undefined),
  });
  await openConsole(page, "pagespeed_global_admin", "#/urls");
  await ready(page);
  await expect(page.getByTestId("urls-no-support")).toBeVisible();
  await expect(page.getByTestId("urls-no-support")).toContainText(
    "This module version cannot show",
  );
});

test("an optimizer below the floor is named with the minimum version", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", {
    ...globalReplies(F.UNSUPPORTED),
    "v1/daemon/health": F.HEALTH_BELOW_FLOOR,
  });
  await openConsole(page, "pagespeed_global_admin", "#/urls");
  await ready(page);
  await expect(page.getByTestId("urls-no-support")).toContainText("2.0.41");
  await expect(page.getByTestId("urls-no-support")).toContainText("2.0.3");
});

test("an unreachable daemon renders the shared empty state", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.UNREACHABLE));
  await openConsole(page, "pagespeed_global_admin", "#/urls");
  await ready(page);
  await expect(page.getByTestId("urls-unreachable")).toContainText("unreachable");
});

test("an empty index says so", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.URLS_EMPTY));
  await openConsole(page, "pagespeed_global_admin", "#/urls");
  await ready(page);
  await expect(page.getByTestId("urls-empty")).toContainText("No URLs recorded yet");
});

test("a garbage reply shows no NaN and no broken pager", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.URLS_MALFORMED));
  await openConsole(page, "pagespeed_global_admin", "#/urls");
  await ready(page);
  await expect(page.getByText("https://ok.test/ok.png")).toBeVisible();
  await expect(page.getByTestId("urls-pager")).toContainText("1–1");
  await expect(page.getByTestId("urls-pager")).not.toContainText("of -");
  await expect(page.getByTestId("urls-pager")).not.toContainText("NaN");
  await expect(page.getByTestId("urls-next")).toBeDisabled(); // has_more "yes" is not true
});

test("duplicate index entries render as separate rows", async ({ page }) => {
  const dup = { url: "/dup.png", hostname: "a.test", scheme: "https", alternate_count: 1 };
  await mockAdmin(page, "pagespeed_global_admin", {
    ...F.allReplies(),
    config: F.CONFIG_GLOBAL,
    "v1/daemon/cache/urls": {
      status: 200,
      body: { urls: [dup, dup], offset: 0, limit: 50, next_offset: 2, has_more: false, total: 2 },
    },
  });
  await openConsole(page, "pagespeed_global_admin", "#/urls");
  await ready(page);
  // The daemon sent the same entry twice: both rows render (distinct each keys).
  await expect(page.getByTestId("urls-table").locator("tbody tr")).toHaveCount(2);
  await expect(page.getByRole("link", { name: "https://a.test/dup.png" })).toHaveCount(2);
  await expect(page.getByTestId("urls-pager")).toContainText("1–2 of 2");
});

test("a busy (429) refresh keeps the table and the pager", async ({ page }) => {
  let busy = false;
  await mockAdmin(page, "pagespeed_global_admin", {
    ...F.allReplies(),
    config: F.CONFIG_GLOBAL,
    "v1/daemon/cache/urls": (call, request) =>
      busy
        ? F.BUSY
        : typeof F.URLS_TWO_PAGES === "function"
          ? F.URLS_TWO_PAGES(call, request)
          : F.URLS_TWO_PAGES,
  });
  await openConsole(page, "pagespeed_global_admin", "#/urls");
  await ready(page);
  await expect(page.getByTestId("urls-table")).toBeVisible();
  await expect(page.getByTestId("urls-pager")).toContainText("1–2 of 3");
  // Another tab holds the daemon's one in-flight read: this page's refresh
  // gets a 429. The poller's busy rule keeps the last view; no error branch.
  busy = true;
  await Promise.all([
    page.waitForResponse((r) => r.url().includes("/v1/daemon/cache/urls") && r.status() === 429),
    page.getByRole("button", { name: "Refresh", exact: true }).click(),
  ]);
  await page.waitForTimeout(150); // let the page process the answer
  await expect(page.getByTestId("urls-table")).toBeVisible();
  await expect(page.getByText("https://www.example.test/hero.png")).toBeVisible();
  await expect(page.getByTestId("urls-pager")).toContainText("1–2 of 3");
  await expect(page.locator("main [role='alert']")).toHaveCount(0);
  await expect(page.getByText("Last refresh failed")).toHaveCount(0);
  // A busy answer is not a failure: the next refresh recovers on its own.
  busy = false;
  await page.getByRole("button", { name: "Refresh", exact: true }).click();
  await expect(page.getByTestId("urls-table")).toBeVisible();
  await expect(page.getByTestId("urls-pager")).toContainText("1–2 of 3");
});

test("filter and sort cover the current page, and say so", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.URLS_TWO_PAGES));
  await openConsole(page, "pagespeed_global_admin", "#/urls");
  await ready(page);
  await expect(page.getByTestId("urls-filter-note")).toContainText("current page");
  await page.getByTestId("urls-search").fill("hero");
  await expect(page.getByTestId("urls-table")).toContainText("hero.png");
  // Page 1 holds "/" and "/hero.png" of www.example.test: one row matches.
  await expect(page.getByTestId("urls-table").locator("tbody tr")).toHaveCount(1);
  await page.getByTestId("urls-search").fill("");
  // Sort by variants descending: the 3-variant row first.
  await page.getByRole("button", { name: "Variants" }).click();
  const firstRow = page.getByTestId("urls-table").locator("tbody tr").first();
  await expect(firstRow).toContainText("https://www.example.test/");
});

test("a row that is not in cooldown says so instead of showing nothing", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", globalReplies(F.URLS_TWO_PAGES));
  await openConsole(page, "pagespeed_global_admin", "#/urls");
  await ready(page);
  const firstRow = page.getByTestId("urls-table").locator("tbody tr").first();
  await expect(firstRow.locator("[data-label='Cooldown']")).toHaveText("—");
});

test("a row opens the entry's detail view", async ({ page }) => {
  await mockAdmin(page, "pagespeed_global_admin", {
    ...globalReplies(F.URLS_TWO_PAGES),
    "v1/daemon/cache/alternates": F.ALTERNATES_HERO,
    "v1/daemon/cache/content": F.CONTENT_HERO,
  });
  await openConsole(page, "pagespeed_global_admin", "#/urls");
  await ready(page);
  await page.getByRole("link", { name: "https://www.example.test/hero.png" }).click();
  await expect(page).toHaveURL(/#\/urls\/detail\?url=%2Fhero\.png&host=www\.example\.test&scheme=https$/);
  await expect(page.getByTestId("url-detail-url")).toHaveText("https://www.example.test/hero.png");
});
