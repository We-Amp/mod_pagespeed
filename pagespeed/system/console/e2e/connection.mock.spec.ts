// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The console loses and regains the server. Self-contained
// (support/mock-admin.ts); run after build.sh.

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

test.describe("Connection", () => {
  test("the server stops answering: a banner, the last data stays; it comes back: the banner clears", async ({ page }) => {
    let down = false;
    await mockAdmin(page, "pagespeed_admin", {
      config: F.CONFIG_VHOST,
      stats_json: () => (down ? NETWORK_ERROR : F.STATS_VHOST),
    });
    await openConsole(page, "pagespeed_admin", "#/statistics");
    await expect(page.getByText("css_filter_total_bytes_saved")).toBeVisible();
    await expect(page.getByTestId("connection-state")).toHaveText("connected");

    down = true;
    await page.getByRole("button", { name: "Refresh", exact: true }).click();
    const banner = page.getByTestId("connection-banner");
    await expect(banner).toBeVisible();
    await expect(banner).toHaveAttribute("role", "alert");
    await expect(banner).toContainText("Cannot reach the server.");
    await expect(banner).toContainText("the figures below are from");
    await expect(page.getByTestId("connection-state")).toHaveText("reconnecting");
    await expect(page.getByText("css_filter_total_bytes_saved")).toBeVisible();
    await expect(page.getByText("Last refresh failed: cannot reach the server.")).toBeVisible();

    down = false;
    await banner.getByRole("button", { name: "Retry now" }).click();
    await expect(banner).toHaveCount(0);
    await expect(page.getByTestId("connection-state")).toHaveText("connected");
    await expect(page.getByText("Last refresh failed")).toHaveCount(0);
  });

  test("Retry now works on a page that does not poll", async ({ page }) => {
    let down = false;
    // `down` covers every leaf (via MockOptions.down below), not just the two
    // this page polls: navigating to About, which reads the optimizer's
    // health once, must not find a leaf that still answers.
    await mockAdmin(
      page,
      "pagespeed_admin",
      { config: F.CONFIG_VHOST, stats_json: F.STATS_VHOST },
      { down: () => down },
    );
    await openConsole(page, "pagespeed_admin", "#/statistics");
    await expect(page.getByText("css_filter_total_bytes_saved")).toBeVisible();
    down = true;
    await page.getByRole("button", { name: "Refresh", exact: true }).click();
    const banner = page.getByTestId("connection-banner");
    await expect(banner).toBeVisible();
    await page.locator(".nav-item", { hasText: "About" }).click();
    await expect(page.getByRole("heading", { level: 1, name: "About" })).toBeVisible();
    await expect(banner).toBeVisible();
    down = false;
    await banner.getByRole("button", { name: "Retry now" }).click();
    await expect(banner).toHaveCount(0);
  });

  test("Retry now proves the server is back even when only an optimizer page is open", async ({ page }) => {
    // The server is unreachable outright (a real outage), then it recovers
    // while the optimizer keeps answering its own 5xx with no proxy reason
    // code -- the daemon page's own poller alone can never tell the two
    // apart, and must not be the only thing "Retry now" asks.
    let outage = false;
    let daemonOwnFailure = false;
    const daemonGateway5xx = { status: 502, body: { error: "gateway timeout" } };
    await mockAdmin(
      page,
      "pagespeed_admin",
      {
        config: F.CONFIG_VHOST,
        "v1/daemon/health": () => (daemonOwnFailure ? daemonGateway5xx : F.HEALTH_OK),
      },
      { down: () => outage },
    );
    await openConsole(page, "pagespeed_admin", "#/daemon/status");
    await expect(page.getByTestId("connection-state")).toHaveText("connected");

    outage = true;
    await page.getByRole("button", { name: "Refresh", exact: true }).click();
    const banner = page.getByTestId("connection-banner");
    await expect(banner).toBeVisible();
    await expect(page.getByTestId("connection-state")).toHaveText("reconnecting");

    // The server is back, but the optimizer's own health check is still
    // failing on its own terms -- a plain 5xx, not one of the proxy's
    // reason codes, so it stays neutral for the connection state.
    outage = false;
    daemonOwnFailure = true;
    await banner.getByRole("button", { name: "Retry now" }).click();
    await expect(banner).toHaveCount(0);
    await expect(page.getByTestId("connection-state")).toHaveText("connected");
  });

  test("an unreachable optimizer is a panel state, not a lost connection", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { config: F.CONFIG_VHOST, "v1/daemon/health": F.UNREACHABLE });
    await openConsole(page, "pagespeed_admin", "#/daemon/status");
    await expect(page.getByTestId("daemon-unreachable")).toBeVisible();
    await expect(page.getByTestId("connection-banner")).toHaveCount(0);
    await expect(page.getByTestId("connection-state")).toHaveText("connected");
  });

  test("a gateway error in front of the module counts as a lost connection", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", {
      config: F.CONFIG_VHOST,
      stats_json: { status: 504, body: { error: "gateway timeout" } },
    });
    await openConsole(page, "pagespeed_admin", "#/statistics");
    await expect(page.getByTestId("connection-banner")).toBeVisible();
  });

  test("a console that never reached the server says so without a time", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { config: NETWORK_ERROR, stats_json: NETWORK_ERROR });
    await openConsole(page, "pagespeed_admin", "#/statistics");
    const banner = page.getByTestId("connection-banner");
    await expect(banner).toContainText("Cannot reach the server.");
    await expect(banner).not.toContainText("the figures below are from");
  });

  test("a gateway 5xx on every Overview leaf keeps the banner steady and the Overview backs off", async ({ page }) => {
    await page.clock.install();
    let down = false;
    const gateway = { status: 504, body: { error: "gateway timeout" } };
    const mock = await mockAdmin(page, "pagespeed_admin", {
      config: F.CONFIG_VHOST,
      stats_json: () => (down ? gateway : F.STATS_VHOST),
      "v1/daemon/health": () => (down ? gateway : F.HEALTH_OK),
      "v1/daemon/stats": () => (down ? gateway : F.DAEMON_STATS),
      message_history: () => (down ? gateway : F.MESSAGES),
    });
    await openConsole(page, "pagespeed_admin");
    await expect(page.getByTestId("module-bytes-saved")).toBeVisible();
    down = true;

    const banner = page.getByTestId("connection-banner");
    const state = page.getByTestId("connection-state");
    const before = mock.calls("stats_json");

    // First poll cycle behind the gateway: the banner appears and the
    // "figures from" time is pinned to the last real answer, not to the
    // gateway's own 504.
    await runFor(page, 5_000);
    await expect(banner).toBeVisible();
    await expect(state).toHaveText("reconnecting");
    const figuresAtFirstCycle = await banner.textContent();

    // A second cycle: no flicker back to "connected" from the daemon
    // leaves' own 504s, and the same pinned time.
    await runFor(page, 5_000);
    await expect(banner).toBeVisible();
    await expect(state).toHaveText("reconnecting");
    expect(await banner.textContent()).toBe(figuresAtFirstCycle);

    // A fixed five-second cadence would have asked the module leaf about
    // twenty more times over the next 100 s; backing off asks far fewer.
    await runFor(page, 90_000);
    const asked = mock.calls("stats_json") - before;
    expect(asked).toBeGreaterThanOrEqual(3);
    expect(asked).toBeLessThanOrEqual(7);
  });
});
