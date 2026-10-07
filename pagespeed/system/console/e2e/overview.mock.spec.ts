// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Self-contained: serves the committed bundle and answers every admin
// request from fixtures (see support/mock-admin.ts). Run after build.sh.

import { expect, test, type Page } from "@playwright/test";
import * as F from "./support/fixtures";
import { mockAdmin, openConsole, type Admin, type Leaf, type Responder } from "./support/mock-admin";

// Grouped figures ("3,122") and timestamps must not depend on the machine's locale.
test.use({ locale: "en-US", timezoneId: "UTC" });

function healthy(extra: Partial<Record<Leaf, Responder>> = {}): Partial<Record<Leaf, Responder>> {
  return {
    stats_json: F.STATS_GLOBAL,
    config: F.CONFIG_GLOBAL,
    "v1/daemon/health": F.HEALTH_OK,
    "v1/daemon/stats": F.DAEMON_STATS,
    message_history: F.messageGroups([]),
    ...extra,
  };
}

async function open(page: Page, admin: Admin, replies: Partial<Record<Leaf, Responder>>) {
  const mock = await mockAdmin(page, admin, replies);
  const errors: string[] = [];
  page.on("pageerror", (e) => errors.push(e.message));
  await openConsole(page, admin);
  await expect(page.getByTestId("module-card")).toBeVisible();
  return { mock, errors };
}

async function expectNoJunk(page: Page) {
  const text = await page.locator("main").innerText();
  expect(text).not.toMatch(/NaN|undefined|null|\[object Object\]/);
}

test.describe("Overview", () => {
  test("is the landing page and the first navigation item", async ({ page }) => {
    const { errors } = await open(page, "pagespeed_admin", healthy({ config: F.CONFIG_VHOST, stats_json: F.STATS_VHOST }));
    await expect(page.getByRole("heading", { level: 1, name: "Overview" })).toBeVisible();
    await expect(page.locator(".nav-item").first()).toHaveText("Overview");
    await expect(page.locator(".nav-item").nth(1)).toHaveText("Savings");
    await expect(page.locator(".nav-item").nth(2)).toHaveText("Statistics");
    expect(errors).toEqual([]);
  });

  test("global admin: whole-server scope, module and optimizer figures", async ({ page }) => {
    await open(page, "pagespeed_global_admin", healthy());
    await expect(page.getByTestId("overview-scope")).toHaveText("All virtual hosts (the whole server)");
    await expect(page.getByTestId("module-bytes-saved")).toContainText("12.3 MB");
    await expect(page.getByTestId("module-bytes-saved")).toContainText("47% of 26.4 MB");
    await expect(page.getByTestId("module-served")).toContainText("3,122");
    await expect(page.getByTestId("optimizer-state")).toHaveText("Running");
    await expect(page.getByTestId("optimizer-card")).toContainText("2.0.41");
    await expect(page.getByTestId("optimizer-savings")).toContainText("673 KB");
    await expectNoJunk(page);
  });

  test("per-vhost admin: names the host and shows its own figures", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({ config: F.CONFIG_VHOST, stats_json: F.STATS_VHOST }));
    await expect(page.getByTestId("overview-scope")).toContainText("This virtual host (www.example.test:80)");
    await expect(page.getByTestId("module-bytes-saved")).toContainText("281 KB");
    await expect(page.getByTestId("module-served")).toHaveText(/^7/);
    await expect(page.getByTestId("module-served")).toContainText("of 1,651 in-place requests");
    await expect(page.getByTestId("optimizer-state")).toHaveText("Running");
    await expectNoJunk(page);
  });

  test("an older module build: unreported counters read as a dash", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({ config: F.CONFIG_VHOST, stats_json: F.STATS_OLD_BUILD }));
    await expect(page.getByTestId("module-served")).toHaveText("—");
    await expect(page.getByTestId("module-bytes-saved")).toContainText("281 KB");
    await expectNoJunk(page);
  });

  test("scope falls back to the admin path when the configuration cannot be read", async ({ page }) => {
    await open(page, "pagespeed_global_admin", healthy({ config: { status: 500, body: { error: "x" } } }));
    await expect(page.getByTestId("overview-scope")).toHaveText("All virtual hosts (the whole server)");
  });

  test("daemon absent (unreachable): the module card keeps working", async ({ page }) => {
    const { errors } = await open(page, "pagespeed_admin", healthy({ "v1/daemon/health": F.UNREACHABLE, "v1/daemon/stats": F.UNREACHABLE }));
    await expect(page.getByTestId("module-bytes-saved")).toContainText("12.3 MB");
    await expect(page.getByTestId("optimizer-state")).toHaveText("Unreachable");
    await expect(page.getByTestId("optimizer-card")).toContainText("configured but not answering");
    await expectNoJunk(page);
    expect(errors).toEqual([]);
  });

  test("daemon absent (not configured): an ordinary module-only server", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({
      stats_json: F.STATS_MODULE_ONLY,
      "v1/daemon/health": F.NOT_CONFIGURED,
      "v1/daemon/stats": F.NOT_CONFIGURED,
    }));
    await expect(page.getByTestId("optimizer-state")).toHaveText("Not configured");
    await expect(page.getByTestId("module-bytes-saved")).toContainText("12.3 MB");
    await expect(page.getByTestId("module-served")).toHaveCount(0);
  });

  test("a non-string health version does not render junk", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({ "v1/daemon/health": F.HEALTH_BAD_VERSION }));
    await expect(page.getByTestId("optimizer-card")).not.toContainText("[object Object]");
    await expectNoJunk(page);
  });

  test("empty statistics: onboarding card, no figures", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({ stats_json: F.STATS_EMPTY, "v1/daemon/stats": F.DAEMON_STATS_EMPTY }));
    await expect(page.getByTestId("overview-empty")).toContainText("The module is running and ready.");
    await expect(page.getByTestId("module-bytes-saved")).toHaveCount(0);
    await expect(page.getByTestId("optimizer-savings")).toHaveCount(0);
    await expectNoJunk(page);
  });

  test("optimizer below the floor (endpoint not provided): module card intact", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({ "v1/daemon/stats": F.UNSUPPORTED }));
    await expect(page.getByTestId("module-bytes-saved")).toContainText("12.3 MB");
    await expect(page.getByTestId("optimizer-state")).toHaveText("Running");
    await expect(page.getByTestId("optimizer-outdated")).toBeVisible();
    await expect(page.getByTestId("optimizer-below-floor")).toHaveCount(0);
    await expect(page.getByTestId("optimizer-savings")).toHaveCount(0);
  });

  test("optimizer below the floor (version older than the minimum): module card intact", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({ "v1/daemon/health": F.HEALTH_BELOW_FLOOR }));
    await expect(page.getByTestId("module-bytes-saved")).toContainText("12.3 MB");
    await expect(page.getByTestId("optimizer-state")).toHaveText("Running");
    await expect(page.getByTestId("optimizer-card")).toContainText("2.0.3");
    await expect(page.getByTestId("optimizer-below-floor")).toContainText("2.0.41");
    await expect(page.getByTestId("optimizer-outdated")).toHaveCount(0);
  });

  test("a busy statistics read keeps the optimizer figures", async ({ page }) => {
    // Deterministic by construction: the flag is armed right before the
    // action that triggers the next poll, and the mock answers 429 to the
    // next stats request while it is armed -- not tied to a call index,
    // which a slow run could shift past the click.
    let busy = false;
    await open(page, "pagespeed_admin", healthy({
      "v1/daemon/stats": () => {
        if (busy) {
          busy = false;
          return F.BUSY;
        }
        return F.DAEMON_STATS;
      },
    }));
    await expect(page.getByTestId("optimizer-savings")).toContainText("673 KB");
    busy = true;
    await Promise.all([
      page.waitForResponse((r) => r.url().endsWith("/v1/daemon/stats") && r.status() === 429),
      page.getByRole("button", { name: "Refresh", exact: true }).click(),
    ]);
    await page.waitForTimeout(300);
    await expect(page.getByTestId("optimizer-savings")).toContainText("673 KB");
    await expect(page.getByTestId("optimizer-state")).toHaveText("Running");
  });

  test("all worker threads busy is a note, not a start-up state", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({ "v1/daemon/health": F.HEALTH_BUSY }));
    await expect(page.getByTestId("optimizer-state")).toHaveText("Running");
    await expect(page.getByTestId("optimizer-busy")).toHaveText("All worker threads busy.");
  });

  test("module statistics unavailable: an error, and the optimizer card still renders", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", healthy({ stats_json: F.MODULE_FAILURE }));
    await openConsole(page, "pagespeed_admin");
    await expect(page.getByTestId("module-error")).toContainText("statistics unavailable");
    await expect(page.getByTestId("optimizer-state")).toHaveText("Running");
  });

  test("links lead to the detail pages", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy());
    await page.getByTestId("module-fetch-failures").getByRole("link").click();
    await expect(page).toHaveURL(/#\/logs\?source=module&level=warning$/);
  });

  test("one sample per refresh interval: no request storm", async ({ page }) => {
    const { mock } = await open(page, "pagespeed_admin", healthy());
    await page.waitForTimeout(1500);
    expect(mock.calls("stats_json")).toBeLessThanOrEqual(2);
    await page.waitForTimeout(11_000);
    const n = mock.calls("stats_json");
    expect(n).toBeGreaterThanOrEqual(2);
    expect(n).toBeLessThanOrEqual(5);
    // One sample = one read of each source (allow one sample in flight).
    expect(Math.abs(mock.calls("v1/daemon/health") - n)).toBeLessThanOrEqual(1);
    expect(Math.abs(mock.calls("v1/daemon/stats") - n)).toBeLessThanOrEqual(1);
    expect(mock.calls("config")).toBe(1);
  });
});

test.describe("Overview time bases and scope", () => {
  const golden = (): Partial<Record<Leaf, Responder>> => ({
    ...F.allReplies(),
    config: F.CONFIG_GLOBAL,
    stats_json: F.STATS_JSON_GLOBAL,
    "v1/daemon/stats": F.OPT_STATS_PAGE,
  });

  test("the cache-serve line is a link with its percentage", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", golden());
    await openConsole(page, "pagespeed_global_admin", "#/overview");
    const link = page.getByTestId("module-served-link");
    await expect(link).toHaveAttribute("href", "#/savings");
    await expect(link).toContainText("%");
    // The same counter caveat as the Savings page: the rate leans upward.
    await expect(link).toContainText(
      "cacheable resource requests; conditional and HEAD requests count as served",
    );
    await expect(link).toHaveAttribute(
      "title",
      "Share of cacheable resource requests (CSS, JavaScript, images) answered from the optimizer's cache; conditional and HEAD requests count as served",
    );
    await link.click();
    await expect(page).toHaveURL(/#\/savings$/);
  });

  test("both cards carry their own since stamps; the whole-server console needs no scope chips", async ({ page }) => {
    await mockAdmin(page, "pagespeed_global_admin", golden());
    await openConsole(page, "pagespeed_global_admin", "#/overview");
    await expect(page.getByTestId("module-card").locator(".scope-chip")).toHaveCount(0);
    await expect(page.getByTestId("optimizer-card").locator(".scope-chip")).toHaveCount(0);
    await expect(page.getByTestId("since-module")).toContainText("(web server restart)");
    await expect(page.getByTestId("since-optimizer")).toContainText("(optimizer restart)");
  });

  test("the per-host console mutes the optimizer card and names the host scope", async ({ page }) => {
    await mockAdmin(page, "pagespeed_admin", { ...golden(), config: F.CONFIG_VHOST });
    await openConsole(page, "pagespeed_admin", "#/overview");
    await expect(page.getByTestId("module-card").locator(".scope-chip")).toHaveCount(0);
    await expect(page.getByTestId("optimizer-card").locator(".scope-chip")).toHaveText("whole server");
    await expect(page.getByTestId("optimizer-card")).toHaveClass(/card--muted/);
    await expect(page.getByTestId("optimizer-scope-note").locator("a")).toHaveAttribute(
      "href",
      /pagespeed_global_admin\/#\/savings$/,
    );
  });
});

for (const scheme of ["light", "dark"] as const) {
  test.describe(`Overview muted per-host optimizer card (${scheme})`, () => {
    test.use({ colorScheme: scheme });
    test("is visibly set apart from the host's own card", async ({ page }) => {
      await mockAdmin(page, "pagespeed_admin", {
        ...F.allReplies(),
        config: F.CONFIG_VHOST,
        stats_json: F.STATS_JSON_GLOBAL,
        "v1/daemon/stats": F.OPT_STATS_PAGE,
      });
      await openConsole(page, "pagespeed_admin", "#/overview");
      const muted = page.getByTestId("optimizer-card");
      await expect(muted).toHaveClass(/card--muted/);
      await expect(muted).toHaveCSS("border-top-style", "dashed");
      await expect(page.getByTestId("module-card")).toHaveCSS("border-top-style", "solid");
      if (scheme === "dark") {
        const opacity = Number(await muted.evaluate((el) => getComputedStyle(el).opacity));
        expect(opacity).toBeLessThan(1);
      }
    });
  });
}

test.describe("Overview alerts", () => {
  // Take the next sample now and wait until its optimizer read was answered,
  // so consecutive refreshes are processed in order.
  const refresh = async (page: Page) => {
    await Promise.all([
      page.waitForResponse((r) => r.url().endsWith("/v1/daemon/stats")),
      page.getByRole("button", { name: "Refresh", exact: true }).click(),
    ]);
  };

  test("healthy: no alerts, a healthy summary", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy());
    await expect(page.getByTestId("health-summary")).toHaveText("No findings");
    await expect(page.getByTestId("alerts")).toHaveCount(0);
  });

  test("daemon absent (unreachable): one warning linking to Optimizer status", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({ "v1/daemon/health": F.UNREACHABLE, "v1/daemon/stats": F.UNREACHABLE }));
    const alert = page.getByTestId("alert-daemon-unreachable");
    await expect(alert).toContainText("configured but not answering");
    await expect(page.getByTestId("alerts").locator('[data-testid^="alert-"]')).toHaveCount(1);
    // The banner is one polite live region; individual alerts do not each
    // re-announce on every poll.
    await expect(page.getByTestId("alerts")).toHaveAttribute("role", "status");
    await expect(page.getByTestId("alerts")).toHaveAttribute("aria-live", "polite");
    await expect(alert).not.toHaveAttribute("role", "alert");
    await expect(page.getByTestId("health-summary")).toHaveText("1 finding · 1 needs action");
    await alert.getByRole("link", { name: "Details" }).click();
    await expect(page).toHaveURL(/#\/optimizer$/);
  });

  test("daemon absent (not configured): nothing to alert", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({ "v1/daemon/health": F.NOT_CONFIGURED, "v1/daemon/stats": F.NOT_CONFIGURED }));
    await expect(page.getByTestId("health-summary")).toHaveText("No findings");
    await expect(page.getByTestId("alerts")).toHaveCount(0);
  });

  test("per-vhost and global consoles raise the same optimizer alert", async ({ page }) => {
    for (const admin of ["pagespeed_admin", "pagespeed_global_admin"] as const) {
      await page.unrouteAll({ behavior: "ignoreErrors" });
      await open(page, admin, healthy({ "v1/daemon/health": F.HEALTH_CHECK_FAILING }));
      await expect(page.getByTestId("alert-daemon-check-failed")).toContainText("cache_open");
    }
  });

  test("empty statistics: nothing to alert", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({ stats_json: F.STATS_EMPTY, "v1/daemon/stats": F.DAEMON_STATS_EMPTY }));
    await expect(page.getByTestId("health-summary")).toHaveText("No findings");
  });

  test("optimizer below the floor (endpoint not provided): the update warning", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({ "v1/daemon/stats": F.UNSUPPORTED }));
    await expect(page.getByTestId("alert-daemon-outdated")).toContainText("(2.0.41) does not provide");
  });

  test("optimizer below the floor (version older than the minimum): the update warning", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({ "v1/daemon/health": F.HEALTH_BELOW_FLOOR }));
    await expect(page.getByTestId("alert-daemon-outdated")).toContainText("(2.0.3) is older than the oldest this console supports");
  });

  test("rate rule: fires on an increase between two samples, never on the first", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({ "v1/daemon/stats": (n) => F.daemonStats({ errorsTotal: n === 0 ? 5 : 8 }) }));
    await expect(page.getByTestId("alerts")).toHaveCount(0);
    await refresh(page);
    await expect(page.getByTestId("alert-error-rate")).toContainText("3 new errors (8 since it started)");
  });

  test("module fetch failures link to the module's messages in Logs", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({ stats_json: (n) => F.statsWithFetchFailures(n === 0 ? 0 : 2) }));
    await refresh(page);
    const alert = page.getByTestId("alert-fetch-failures");
    await expect(alert).toBeVisible();
    await alert.getByRole("link", { name: "Details" }).click();
    await expect(page).toHaveURL(/#\/logs\?source=module&level=warning$/);
  });

  test("errors rank before warnings", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({
      "v1/daemon/health": F.HEALTH_CHECK_FAILING,
      "v1/daemon/stats": F.daemonStats({ originMisconfiguration: 2 }),
    }));
    const alerts = page.getByTestId("alerts").locator('[data-testid^="alert-"]');
    await expect(alerts).toHaveCount(2);
    await expect(alerts.first()).toHaveAttribute("data-testid", "alert-daemon-check-failed");
    await expect(page.getByTestId("health-summary")).toHaveText("2 findings · 2 need action");
  });

  // Weakly ordered by design: a regular 5 s refresh can consume a responder
  // index, so the "still hidden" check may pass because the condition already
  // cleared. The acknowledgement semantics are pinned exactly by the alert
  // tracker's own unit tests; this test pins the wiring (button, re-arm,
  // final reappearance).
  test("acknowledgement: stays hidden while the condition holds, returns after it clears", async ({ page }) => {
    // Samples 0-1: condition holds; 2: clears; 3 and later: holds again.
    await open(page, "pagespeed_admin", healthy({
      "v1/daemon/stats": (n) => F.daemonStats({ originMisconfiguration: n === 2 ? 0 : 2 }),
    }));
    const alert = page.getByTestId("alert-origin-misconfiguration");
    await expect(alert).toBeVisible();
    await alert.getByRole("button", { name: /^Acknowledge / }).click();
    await expect(alert).toHaveCount(0);
    await expect(page.getByTestId("health-summary")).toHaveText("No findings · 1 acknowledged");
    await refresh(page);
    await expect(alert).toHaveCount(0);
    await refresh(page);
    await refresh(page);
    // A regular 5 s refresh may interleave; it only moves the sequence on.
    await expect(alert).toBeVisible({ timeout: 12_000 });
  });

  test("an acknowledgement survives moving to another page and back", async ({ page }) => {
    await open(page, "pagespeed_admin", healthy({ "v1/daemon/stats": F.daemonStats({ originMisconfiguration: 2 }) }));
    await page.getByTestId("alert-origin-misconfiguration").getByRole("button", { name: /^Acknowledge / }).click();
    await page.getByRole("link", { name: "About", exact: true }).click();
    await page.getByRole("link", { name: "Overview", exact: true }).click();
    await expect(page.getByTestId("module-card")).toBeVisible();
    await expect(page.getByTestId("alert-origin-misconfiguration")).toHaveCount(0);
  });
});
