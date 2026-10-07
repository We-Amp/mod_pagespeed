// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// How the console refreshes: back-off, hidden tabs, busy answers, shared
// reads and range changes. Self-contained (support/mock-admin.ts); run after
// build.sh.

import { expect, test, type Page } from "@playwright/test";
import * as F from "./support/fixtures";
import { NETWORK_ERROR, mockAdmin, openConsole } from "./support/mock-admin";

test.use({ locale: "en-US", timezoneId: "UTC" });

/** Let the page handle replies it already has (real time; the page clock may be paused). */
const settle = (page: Page) => page.waitForTimeout(150);

async function runFor(page: Page, ms: number) {
  for (let t = 0; t < ms; t += 5_000) {
    await page.clock.runFor(5_000);
    await settle(page);
  }
}

async function setHidden(page: Page, hidden: boolean) {
  await page.evaluate((h) => {
    Object.defineProperty(document, "visibilityState", { configurable: true, get: () => (h ? "hidden" : "visible") });
    document.dispatchEvent(new Event("visibilitychange"));
  }, hidden);
}

test.describe("Polling", () => {
  test("a failing page backs off instead of retrying every five seconds", async ({ page }) => {
    await page.clock.install();
    let failing = true;
    const mock = await mockAdmin(page, "pagespeed_admin", {
      config: F.CONFIG_VHOST,
      stats_json: () => (failing ? F.MODULE_FAILURE : F.STATS_VHOST),
    });
    await openConsole(page, "pagespeed_admin", "#/statistics");
    await expect(page.getByText("statistics unavailable")).toBeVisible();
    // Two minutes: a fixed five-second cadence would ask 25 times; backing
    // off (5, 10, 20, 40, 60 s) asks 5 times.
    await runFor(page, 120_000);
    expect(mock.calls("stats_json")).toBeGreaterThanOrEqual(4);
    expect(mock.calls("stats_json")).toBeLessThanOrEqual(7);
    // The server recovers: read again within the capped wait, then every five seconds.
    failing = false;
    await runFor(page, 60_000);
    await expect(page.getByText("css_filter_total_bytes_saved")).toBeVisible();
    // Bounds, not exact counts: the page clock also moves during the real-time settles.
    const recovered = mock.calls("stats_json");
    await runFor(page, 10_000);
    expect(mock.calls("stats_json")).toBeGreaterThanOrEqual(recovered + 1);
    expect(mock.calls("stats_json")).toBeLessThanOrEqual(recovered + 3);
  });

  test("the overview backs off while the server does not answer, and keeps its last view", async ({ page }) => {
    await page.clock.install();
    let down = false;
    const mock = await mockAdmin(page, "pagespeed_admin", {
      config: F.CONFIG_VHOST,
      stats_json: () => (down ? NETWORK_ERROR : F.STATS_VHOST),
      "v1/daemon/health": () => (down ? NETWORK_ERROR : F.HEALTH_OK),
      "v1/daemon/stats": () => (down ? NETWORK_ERROR : F.DAEMON_STATS),
    });
    await openConsole(page, "pagespeed_admin");
    await expect(page.getByTestId("module-bytes-saved")).toBeVisible();
    down = true;
    const before = mock.calls("stats_json");
    // Two minutes: at a fixed cadence 24 samples; backing off, about 5.
    await runFor(page, 120_000);
    const asked = mock.calls("stats_json") - before;
    expect(asked).toBeGreaterThanOrEqual(3);
    expect(asked).toBeLessThanOrEqual(7);
    await expect(page.getByTestId("module-bytes-saved")).toBeVisible();
    await expect(page.getByTestId("optimizer-state")).toHaveText("Running");
  });

  test("a hidden tab makes no requests and refreshes at once when shown", async ({ page }) => {
    await page.clock.install();
    const mock = await mockAdmin(page, "pagespeed_admin", { config: F.CONFIG_VHOST, stats_json: F.STATS_VHOST });
    await openConsole(page, "pagespeed_admin", "#/statistics");
    await expect(page.getByText("css_filter_total_bytes_saved")).toBeVisible();
    await setHidden(page, true);
    const before = mock.calls("stats_json");
    await runFor(page, 60_000);
    expect(mock.calls("stats_json")).toBe(before);
    await setHidden(page, false);
    await expect.poll(() => mock.calls("stats_json")).toBeGreaterThanOrEqual(before + 1);
  });

  test("another tab's busy read is invisible on Daemon Cache", async ({ page }) => {
    let busy = false;
    await mockAdmin(page, "pagespeed_admin", {
      config: F.CONFIG_VHOST,
      "v1/daemon/stats": () => (busy ? F.BUSY : F.DAEMON_STATS),
    });
    await openConsole(page, "pagespeed_admin", "#/daemon/cache");
    await expect(page.getByText("Cache entries")).toBeVisible();
    busy = true;
    await Promise.all([
      page.waitForResponse((r) => r.url().endsWith("/v1/daemon/stats") && r.status() === 429),
      page.getByRole("button", { name: "Refresh", exact: true }).click(),
    ]);
    await settle(page);
    await expect(page.getByText("Last refresh failed")).toHaveCount(0);
    await expect(page.getByText("Cache entries")).toBeVisible();
  });

  test("a busy first read retries within a second", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", {
      config: F.CONFIG_VHOST,
      "v1/daemon/stats": (n) => (n === 0 ? F.BUSY : F.DAEMON_STATS),
    });
    await openConsole(page, "pagespeed_admin", "#/daemon/cache");
    await expect(page.getByText("Cache entries")).toBeVisible({ timeout: 3_000 });
    await expect(page.getByTestId("daemon-unreachable")).toHaveCount(0);
  });

  test("moving from the overview to Daemon Cache mid-read shares the read", async ({ page }) => {
    const mock = await mockAdmin(
      page,
      "pagespeed_admin",
      {
        config: F.CONFIG_VHOST,
        stats_json: F.STATS_VHOST,
        "v1/daemon/health": F.HEALTH_OK,
        "v1/daemon/stats": F.DAEMON_STATS,
      },
      { delayMs: { "v1/daemon/stats": 1_500 }, oneInFlight: true },
    );
    await openConsole(page, "pagespeed_admin");
    await expect.poll(() => mock.calls("v1/daemon/stats")).toBe(1);
    await page.locator(".nav-item", { hasText: "Status" }).click();
    await expect(page.getByText("Cache entries")).toBeVisible();
    expect(mock.busyReplies("v1/daemon/stats")).toBe(0);
    expect(mock.calls("v1/daemon/stats")).toBe(1);
  });

  test("Refresh during a pending read makes no second request", async ({ page }) => {
    const mock = await mockAdmin(
      page,
      "pagespeed_admin",
      { config: F.CONFIG_VHOST, stats_json: F.STATS_VHOST },
      { delayMs: { stats_json: 1_000 } },
    );
    await openConsole(page, "pagespeed_admin", "#/statistics");
    await expect(page.getByText("css_filter_total_bytes_saved")).toBeVisible();
    const before = mock.calls("stats_json");
    const refresh = page.getByRole("button", { name: "Refresh", exact: true });
    await refresh.click();
    await refresh.click();
    await refresh.click();
    await page.waitForTimeout(1_500);
    expect(mock.calls("stats_json")).toBe(before + 1);
  });

  test("changing the range while a read is pending shows only the new range", async ({ page }) => {
    const spans: number[] = [];
    page.on("request", (r) => {
      const u = new URL(r.url());
      if (u.pathname.endsWith("/graphs")) {
        spans.push(Number(u.searchParams.get("end_time")) - Number(u.searchParams.get("start_time")));
      }
    });
    const mock = await mockAdmin(
      page,
      "pagespeed_admin",
      { config: F.CONFIG_VHOST, stats_json: F.STATS_VHOST, graphs: (n) => F.graphsReply([`range_${n}`]) },
      { delayMs: { graphs: 800 } },
    );
    await openConsole(page, "pagespeed_admin", "#/graphs?counters=all");
    // The default 15-minute range reads the log on the first paint. (The
    // card's heading names the graph; the chart's text summary repeats it.)
    await expect(page.getByRole("heading", { name: "range_0" })).toBeVisible();
    await page.getByLabel("Time range").selectOption("360");
    // The 360-minute read is still in flight (800 ms delay) when the range
    // changes again; its answer is dropped when it lands.
    await page.getByLabel("Time range").selectOption("1440");
    await expect(page.getByRole("heading", { name: "range_2" })).toBeVisible();
    await expect(page.getByRole("heading", { name: "range_1" })).toHaveCount(0);
    expect(mock.calls("graphs")).toBe(3);
    expect(spans).toEqual([15 * 60 * 1000, 360 * 60 * 1000, 1440 * 60 * 1000]);
  });

  test("Caches keeps its data when a refresh fails", async ({ page }) => {
    await page.clock.install();
    await mockAdmin(page, "pagespeed_admin", {
      config: F.CONFIG_VHOST,
      cache: (n) => (n === 0 ? F.CACHE_STRUCTURE : F.MODULE_FAILURE),
    });
    await openConsole(page, "pagespeed_admin", "#/caches");
    await expect(page.getByRole("heading", { name: "HTTP Cache" })).toBeVisible();
    await runFor(page, 30_000);
    await expect(page.getByText("Last refresh failed")).toBeVisible();
    await expect(page.getByRole("heading", { name: "HTTP Cache" })).toBeVisible();
  });
});
