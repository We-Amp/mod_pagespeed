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

  test("the Overview claims nothing while the server is down: no fresh stamp, no verdict, no green", async ({ page }) => {
    await page.clock.install();
    let down = false;
    await mockAdmin(page, "pagespeed_admin", F.allReplies(), { down: () => down });
    await openConsole(page, "pagespeed_admin");
    await expect(page.getByTestId("module-bytes-saved")).toBeVisible();

    const health = page.getByTestId("health-summary");
    const optimizer = page.getByTestId("optimizer-state");
    const stamp = page.getByTestId("page-updated");
    const strip = page.getByTestId("health-strip");
    const dot = optimizer.locator(".status-dot");
    const dotColour = () => dot.evaluate((el) => getComputedStyle(el).backgroundColor);
    const stripDotColour = () => page.getByTestId("health-dot").evaluate((el) => getComputedStyle(el).backgroundColor);
    const borderColour = () => strip.evaluate((el) => getComputedStyle(el).borderLeftColor);

    // Up: a verdict, a green dot, a fresh stamp.
    await expect(optimizer).toHaveText("Running");
    await expect(dot).toHaveClass(/status-dot-ok/);
    await expect(health).toHaveText(/^Working\. /);
    await expect(strip).toHaveClass(/health-strip-ok/);
    await expect(stamp).toHaveText(/^updated \d s ago$/);
    const green = await dotColour();
    const verdictBorder = await borderColour();
    expect(await stripDotColour()).toBe(green);

    // Down for 20 s: every sample fails, and none of them counts as an update.
    down = true;
    await runFor(page, 20_000);
    await expect(page.getByTestId("connection-banner")).toBeVisible();
    await expect(stamp).toHaveText(/^refresh overdue · updated \d+ s ago$/);
    const age = Number(/updated (\d+) s ago/.exec((await stamp.textContent()) ?? "")?.[1]);
    expect(age).toBeGreaterThanOrEqual(20);

    await expect(health).toHaveText(/^Status unknown — last answer \d/);
    // No figure from before the outage, and no findings count either.
    await expect(health).not.toContainText(/saved|findings?/);
    await expect(strip).not.toHaveClass(/health-strip-(ok|info|warning|error)/);
    await expect(page.getByTestId("health-dot")).not.toHaveClass(/status-dot-(ok|info|warning|error)/);
    expect(await borderColour()).not.toBe(verdictBorder);
    expect(await stripDotColour()).not.toBe(green);
    // The value stays the state word; that it is the last known state is a line of context.
    await expect(optimizer).toHaveText("Running");
    await expect(page.getByTestId("optimizer-last-known")).toHaveText("Last known state");
    expect((await optimizer.boundingBox())!.height, "the state word is one line").toBeLessThan(45);
    await expect(dot).not.toHaveClass(/status-dot-ok|status-dot-warning/);
    expect(await dotColour()).not.toBe(green);
    // The last figures stay on screen.
    await expect(page.getByTestId("module-bytes-saved")).toBeVisible();

    // Back: the verdict, the green dot and a fresh stamp return.
    down = false;
    await page.getByTestId("connection-banner").getByRole("button", { name: "Retry now" }).click();
    await expect(page.getByTestId("connection-banner")).toHaveCount(0);
    await expect(health).toHaveText(/^Working\. /);
    await expect(optimizer).toHaveText("Running");
    await expect(page.getByTestId("optimizer-last-known")).toHaveCount(0);
    await expect(dot).toHaveClass(/status-dot-ok/);
    expect(await dotColour()).toBe(green);
    await expect(stamp).toHaveText(/^updated \d s ago$/);
  });

  test("a console that never reached the server shows no verdict on the Overview", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", F.allReplies(), { down: () => true });
    await openConsole(page, "pagespeed_admin");
    await expect(page.getByTestId("connection-banner")).toBeVisible();
    await expect(page.getByTestId("health-summary")).toHaveText("Status unknown — the server has not answered.");
    await expect(page.getByTestId("page-updated")).toHaveText("updated —");
  });

  test("a server that answered with an error and then went down still has a last answer", async ({ page }) => {
    let down = false;
    await mockAdmin(
      page,
      "pagespeed_admin",
      { ...F.allReplies(), stats_json: { status: 404, body: "Unknown admin page: stats_json" } },
      { down: () => down },
    );
    await openConsole(page, "pagespeed_admin");
    const health = page.getByTestId("health-summary");
    // A failed module read states no figure: the state is unknown, and the count line says why.
    await expect(health).toHaveText("Status unknown. Module statistics unavailable.");
    await expect(page.getByTestId("health-strip")).not.toHaveClass(/health-strip-(ok|info)/);
    // No figures were ever read: the stamp has nothing to age.
    await expect(page.getByTestId("page-updated")).toHaveText("updated —");

    down = true;
    await page.getByRole("button", { name: "Refresh", exact: true }).click();
    await expect(page.getByTestId("connection-banner")).toBeVisible();
    // The server did answer before it went down, so the line carries that time.
    await expect(health).toHaveText(/^Status unknown — last answer \d/);
    await expect(health).not.toContainText("has not answered");
    await expect(page.getByTestId("page-updated")).toHaveText("updated —");
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
