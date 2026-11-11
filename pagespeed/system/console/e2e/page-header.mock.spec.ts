// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Every page carries the shared header. Self-contained
// (support/mock-admin.ts); run after build.sh.

import { expect, test } from "@playwright/test";
import * as F from "./support/fixtures";
import { mockAdmin, openConsole, type Leaf, type Responder } from "./support/mock-admin";

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
      // On by default, said in words, without the accent of the page's action.
      await expect(toggle).toContainText("Auto-refresh on");
      await expect(page.getByRole("button", { name: "Refresh", exact: true })).toBeVisible();
      await expect(page.getByText(/^updated /)).toBeVisible();
    } else {
      await expect(toggle).toHaveCount(0);
    }
  });
}

test("the stamp counts the seconds, also while auto-refresh is paused", async ({ page }) => {
  await page.clock.install();
  const mock = await mockAdmin(page, "pagespeed_admin", F.allReplies());
  await openConsole(page, "pagespeed_admin", "#/statistics");
  const stamp = page.getByTestId("page-updated");
  await expect(stamp).toHaveText(/^updated \d s ago$/);
  await page.getByRole("button", { name: /Auto-refresh · \d+ s/ }).click();
  await expect(stamp).toHaveText(/^paused · updated \d+ s ago$/);
  const age = async () => Number(/updated (\d+) s ago/.exec((await stamp.textContent()) ?? "")?.[1]);
  const start = await age();
  const calls = mock.calls("stats_json");

  await page.clock.runFor(7_000);
  await expect.poll(age).toBeGreaterThanOrEqual(start + 7);
  await page.clock.runFor(60_000);
  await expect(stamp).toHaveText(/^paused · updated 1 min ago$/);
  // Counting is not refreshing: nothing was asked meanwhile.
  expect(mock.calls("stats_json")).toBe(calls);
  // The stamp is not announced: it sits in no live region.
  expect(await stamp.evaluate((el) => el.closest("[aria-live], [role=status], [role=alert], [role=log]") === null)).toBe(true);
});

test("Configuration has the shared Refresh button and no auto-refresh; it and r refetch", async ({ page }) => {
  await page.clock.install();
  const mock = await mockAdmin(page, "pagespeed_admin", F.allReplies());
  await openConsole(page, "pagespeed_admin", "#/configuration");
  await expect(page.getByRole("heading", { level: 1, name: "Configuration" })).toBeVisible();
  const header = page.locator("main .page-header");
  // One Refresh, the header's icon button; no text button of its own.
  await expect(page.getByRole("button", { name: /refresh/i })).toHaveCount(1);
  const refresh = header.getByRole("button", { name: "Refresh", exact: true });
  await expect(header.getByTestId("page-updated")).toHaveText(/^updated /);
  // Nothing on a timer.
  const loaded = mock.calls("config");
  await page.clock.runFor(30_000);
  expect(mock.calls("config")).toBe(loaded);
  await refresh.click();
  await expect.poll(() => mock.calls("config")).toBe(loaded + 1);
  await expect(header.getByRole("status")).toHaveText("Refreshed");
  await page.locator("main h1").click();
  await page.keyboard.press("r");
  await expect.poll(() => mock.calls("config")).toBe(loaded + 2);
  await expect(header).toContainText("this virtual host");
});

test("Refresh says what it came to, and a timer's refresh says nothing", async ({ page }) => {
  await page.clock.install();
  let failing = false;
  const mock = await mockAdmin(page, "pagespeed_admin", {
    ...F.allReplies(),
    stats_json: () => (failing ? F.MODULE_FAILURE : F.STATS_VHOST),
  });
  await openConsole(page, "pagespeed_admin", "#/statistics");
  await expect(page.getByText("css_filter_total_bytes_saved")).toBeVisible();
  const result = page.locator("main .page-header").getByRole("status");
  const refresh = page.getByRole("button", { name: "Refresh", exact: true });
  // Timer refreshes leave the status region alone (no announcement every few seconds).
  const before = mock.calls("stats_json");
  await page.clock.runFor(15_000);
  await expect.poll(() => mock.calls("stats_json")).toBeGreaterThan(before);
  await expect(result).toHaveText("");
  await refresh.click();
  await expect(result).toHaveText("Refreshed");
  await expect(refresh).toHaveAttribute("aria-busy", "false");
  // It goes after a few seconds, so the next one is announced again.
  await page.clock.runFor(6_000);
  await expect(result).toHaveText("");
  failing = true;
  await refresh.click();
  await expect(result).toHaveText("Refresh failed");
  // The refresh shortcut gives the same feedback.
  failing = false;
  await page.locator("main h1").click();
  await page.keyboard.press("r");
  await expect(result).toHaveText("Refreshed");
});

// Pages that read several parts settle their read even when a part fails,
// and show that part's failure in place: the header then never says
// "Refreshed" above an error notice.
interface PartCase {
  name: string;
  admin: "pagespeed_admin" | "pagespeed_global_admin";
  hash: string;
  replies: Partial<Record<Leaf, Responder>>;
  leaf: Leaf;
  broken: Responder;
  says: string;
}

const GLOBAL = { ...F.allReplies(), config: F.CONFIG_GLOBAL };
const PART_CASES: PartCase[] = [
  { name: "Graphs, statistics failing", admin: "pagespeed_admin", hash: "#/graphs", replies: F.allReplies(),
    leaf: "stats_json", broken: F.MODULE_FAILURE, says: "Refresh failed" },
  { name: "Optimizer status, statistics failing", admin: "pagespeed_admin", hash: "#/optimizer", replies: F.allReplies(),
    leaf: "v1/daemon/stats", broken: F.MODULE_FAILURE, says: "Partly refreshed; see the notice" },
  { name: "URLs, the list failing", admin: "pagespeed_global_admin", hash: "#/urls",
    replies: { ...GLOBAL, "v1/daemon/cache/urls": F.URLS_TWO_PAGES },
    leaf: "v1/daemon/cache/urls", broken: F.MODULE_FAILURE, says: "Refresh failed" },
  { name: "URL detail, the alternates failing", admin: "pagespeed_global_admin",
    hash: "#/urls/detail?url=%2Fhero.png&host=www.example.test&scheme=https",
    replies: { ...GLOBAL, "v1/daemon/cache/alternates": F.ALTERNATES_HERO, "v1/daemon/cache/content": F.CONTENT_HERO },
    leaf: "v1/daemon/cache/alternates", broken: F.MODULE_FAILURE, says: "Partly refreshed; see the notice" },
  { name: "Logs, the module's messages failing", admin: "pagespeed_global_admin", hash: "#/logs",
    replies: { ...GLOBAL, "v1/daemon/logs": F.logsDaemon() },
    leaf: "message_history", broken: F.MODULE_FAILURE, says: "Partly refreshed; see the notice" },
  { name: "Overview, the module's statistics failing", admin: "pagespeed_admin", hash: "#/overview", replies: F.allReplies(),
    leaf: "stats_json", broken: F.MODULE_FAILURE, says: "Partly refreshed; see the notice" },
  { name: "Overview, the optimizer not answering", admin: "pagespeed_admin", hash: "#/overview", replies: F.allReplies(),
    leaf: "v1/daemon/health", broken: F.UNREACHABLE, says: "Partly refreshed; see the notice" },
  { name: "Savings, the module's statistics failing", admin: "pagespeed_admin", hash: "#/savings", replies: F.allReplies(),
    leaf: "stats_json", broken: F.MODULE_FAILURE, says: "Partly refreshed; see the notice" },
];

for (const c of PART_CASES) {
  test(`Refresh is honest when a part fails: ${c.name}`, async ({ page }) => {
    let broken = false;
    const healthy = c.replies[c.leaf];
    const replies: Partial<Record<Leaf, Responder>> = { ...c.replies, [c.leaf]: ((call, request) =>
      broken
        ? typeof c.broken === "function" ? c.broken(call, request) : c.broken
        : typeof healthy === "function" ? healthy(call, request) : healthy) as Responder };
    if (c.leaf === "v1/daemon/health") {
      const stats = c.replies["v1/daemon/stats"];
      replies["v1/daemon/stats"] = ((call, request) =>
        broken ? F.UNREACHABLE : typeof stats === "function" ? stats(call, request) : stats) as Responder;
    }
    await mockAdmin(page, c.admin, replies);
    await openConsole(page, c.admin, c.hash);
    const result = page.locator("main .page-header").getByRole("status");
    const refresh = page.getByRole("button", { name: "Refresh", exact: true });
    await expect(page.locator("main .loading")).toHaveCount(0);
    await refresh.click();
    await expect(result).toHaveText("Refreshed");
    broken = true;
    await refresh.click();
    await expect(result).toHaveText(c.says);
  });
}
