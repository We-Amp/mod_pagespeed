// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The overview's findings: what needs attention, worst first, each with the
// fix, a documentation link and an acknowledge control. Self-contained
// (support/mock-admin.ts); run after build.sh.

import { expect, test, type Page } from "@playwright/test";
import * as F from "./support/fixtures";
import { mockAdmin, openConsole, type Leaf, type Reply, type Responder } from "./support/mock-admin";

test.use({ locale: "en-US", timezoneId: "UTC" });

const PHONE = { width: 390, height: 844 } as const;
const ADMIN = "pagespeed_global_admin" as const;

function replies(extra: Partial<Record<Leaf, Responder>> = {}): Partial<Record<Leaf, Responder>> {
  return {
    stats_json: F.STATS_GLOBAL,
    config: F.CONFIG_GLOBAL,
    "v1/daemon/health": F.HEALTH_OK,
    "v1/daemon/stats": F.DAEMON_STATS,
    message_history: F.messageGroups([]),
    ...extra,
  };
}

async function open(page: Page, extra: Partial<Record<Leaf, Responder>> = {}) {
  const mock = await mockAdmin(page, ADMIN, replies(extra));
  const errors: string[] = [];
  page.on("pageerror", (e) => errors.push(e.message));
  await openConsole(page, ADMIN);
  await expect(page.getByTestId("module-card")).toBeVisible();
  return { mock, errors };
}

const aclRow = (): F.GroupRowFixture => ({ level: "warning", template: F.ACL_TEMPLATE, count: 1, last_ms: Date.now() - 60_000 });
// Recent: the refusal a serving process repeats at start-up (warning level),
// one minute ago. Finding (i) reads only a recent refusal.
const volumeRow = (): F.GroupRowFixture => ({
  level: "warning",
  template: F.VOLUME_TEMPLATE,
  count: 1,
  recent: 1,
  last_ms: Date.now() - 60_000,
});

const withBody = (reply: Reply, patch: Record<string, unknown>): Reply => ({
  ...reply,
  body: { ...(reply.body as Record<string, unknown>), ...patch },
});

test.describe("Overview findings", () => {
  test("the admin-exposure warning in the log is a finding with its fix and documentation", async ({ page }) => {
    const { errors } = await open(page, { message_history: F.messageGroups([aclRow()]) });
    const finding = page.getByTestId("alert-admin-exposed");
    await expect(finding).toContainText("requested from an address other than this server itself");
    await expect(finding.getByTestId("finding-fix")).toContainText("Require local");
    await expect(finding.locator(".level-badge-warning")).toHaveText(/warning/i);
    await expect(finding.getByRole("link", { name: "Documentation" })).toHaveAttribute(
      "href",
      "https://modpagespeed.com/1.1/docs/admin-console/#access-control",
    );
    await expect(finding.getByRole("link", { name: "Documentation" })).toHaveAttribute("rel", "noopener noreferrer");
    await expect(finding.getByRole("link", { name: "Details" })).toHaveAttribute("href", "#/logs?source=module&level=warning");
    await expect(page.getByTestId("health-summary")).toHaveText("1 finding · 1 needs action");
    expect(errors).toEqual([]);
  });

  test("the optimizer cache not in use is an error and ranks first", async ({ page }) => {
    await open(page, { message_history: F.messageGroups([aclRow(), volumeRow()]) });
    const findings = page.getByTestId("alerts").locator('[data-testid^="alert-"]');
    // The recent refusal is also a recent warning, so (h) is listed too.
    await expect(findings).toHaveCount(3);
    await expect(findings.first()).toHaveAttribute("data-testid", "alert-volume-not-opened");
    await expect(findings.first().locator(".level-badge-error")).toBeVisible();
    await expect(findings.first().getByTestId("finding-fix")).toHaveText(
      "Fix: Make the web server's DaemonVolumePath and the optimizer's cache directory (--cache-dir) agree.",
    );
    await expect(page.getByTestId("health-summary")).toHaveText(
      "3 findings · 2 need action · 1 warning in the last 15 min",
    );
  });

  test("an old refusal or the start-up race in the log is not the cache finding", async ({ page }) => {
    await open(page, {
      message_history: F.messageGroups([
        { level: "error", template: F.VOLUME_TEMPLATE, count: 1, recent: 0, last_ms: Date.now() - 3_600_000 },
        {
          level: "warning",
          template:
            "nothing will be recorded for in-place optimization: cannot open the optimizer daemon's cache volume at " +
            "/var/cache/pagespeed-optimizer/vN: permission denied (attempt N of N)",
          count: 1,
          recent: 1,
          last_ms: Date.now() - 30_000,
        },
      ]),
    });
    await expect(page.getByTestId("alert-recent-warnings")).toBeVisible();
    await expect(page.getByTestId("alert-volume-not-opened")).toHaveCount(0);
    await expect(page.getByTestId("health-summary")).toHaveText("1 finding · 1 warning in the last 15 min");
  });

  test("a log that cannot be read is never \"No findings\"", async ({ page }) => {
    await open(page, { message_history: { status: 500, body: { success: false, error: "internal" } } });
    await expect(page.getByTestId("health-summary")).toHaveText("Findings unavailable — the message log could not be read");
    await expect(page.getByTestId("health-summary")).toHaveClass(/health-info/);
    await expect(page.getByTestId("alerts")).toHaveCount(0);
  });

  test("acknowledge moves a finding to the acknowledged group, and un-acknowledge brings it back", async ({ page }) => {
    await open(page, { message_history: F.messageGroups([aclRow()]) });
    await page.getByRole("button", { name: "Acknowledge Admin console reachable from the network" }).click();
    await expect(page.getByTestId("alert-admin-exposed")).toHaveCount(0);
    await expect(page.getByTestId("health-summary")).toHaveText("No findings · 1 acknowledged");
    const group = page.getByTestId("findings-acknowledged");
    await expect(group).toBeVisible();
    await group.locator("summary").click();
    await expect(page.getByTestId("acknowledged-admin-exposed")).toContainText("Admin console reachable from the network");
    await page.getByRole("button", { name: "Un-acknowledge Admin console reachable from the network" }).click();
    await expect(page.getByTestId("alert-admin-exposed")).toBeVisible();
    await expect(group).toHaveCount(0);
  });

  test("an acknowledgement survives a reload", async ({ page }) => {
    await open(page, { message_history: F.messageGroups([aclRow()]) });
    await page.getByRole("button", { name: "Acknowledge Admin console reachable from the network" }).click();
    await page.reload();
    await expect(page.getByTestId("module-card")).toBeVisible();
    await expect(page.getByTestId("findings-acknowledged")).toBeVisible();
    await expect(page.getByTestId("alert-admin-exposed")).toHaveCount(0);
  });

  test("a cleared finding forgets its acknowledgement and comes back armed", async ({ page }) => {
    test.setTimeout(45_000);
    // Grouped reads 0-1: the warning is in the log; 2: it has rotated out; 3 and later: logged again.
    const { mock } = await open(page, {
      message_history: F.messageGroups((n) => (n === 2 ? [] : [aclRow()])),
    });
    await page.getByRole("button", { name: "Acknowledge Admin console reachable from the network" }).click();
    await expect(page.getByTestId("alert-admin-exposed")).toHaveCount(0);
    await expect.poll(() => mock.calls("message_history"), { timeout: 25_000 }).toBeGreaterThanOrEqual(4);
    await expect(page.getByTestId("alert-admin-exposed")).toBeVisible({ timeout: 6_000 });
  });

  test("warnings in the last 15 minutes are one finding with the count and a link to warnings", async ({ page }) => {
    const now = Date.now();
    await open(page, {
      message_history: F.messageGroups([
        { level: "warning", template: "Slow origin response for URL", count: 30, recent: 12, last_ms: now - 60_000 },
        { level: "error", template: "Fetch of URL failed", count: 3, recent: 2, last_ms: now - 120_000 },
        { level: "info", template: "CycloneCache enabled at /var/cache/mod_pagespeed/", count: 1, recent: 1, last_ms: now },
      ]),
    });
    const finding = page.getByTestId("alert-recent-warnings");
    await expect(finding).toContainText("14 warnings and errors in the last 15 minutes, of 2 kinds.");
    await expect(finding.getByTestId("finding-details").locator("li")).toHaveText([
      "×12 Slow origin response for URL",
      "×2 Fetch of URL failed",
    ]);
    await expect(page.getByTestId("health-summary")).toHaveText("1 finding · 14 warnings and errors in the last 15 min");
    await finding.getByRole("link", { name: "Details" }).click();
    await expect(page).toHaveURL(/#\/logs\?source=module&level=warning$/);
  });

  test("a finding's details render log text as text", async ({ page }) => {
    const dialogs: string[] = [];
    page.on("dialog", (d) => {
      dialogs.push(d.message());
      void d.dismiss();
    });
    await open(page, {
      message_history: F.messageGroups([
        { level: "warning", template: "<img src=x onerror=alert(N)> javascript:alert(N)", count: 1, recent: 1, last_ms: Date.now() },
      ]),
    });
    const finding = page.getByTestId("alert-recent-warnings");
    await expect(finding.getByTestId("finding-details")).toContainText("<img src=x onerror=alert(N)> javascript:alert(N)");
    await expect(finding.locator("img")).toHaveCount(0);
    await expect(finding.locator("a[href^='javascript']")).toHaveCount(0);
    expect(dialogs).toEqual([]);
  });

  test("an older module without grouped answers still raises the log findings", async ({ page }) => {
    await open(page, {
      message_history: F.plainMessages([
        { severity: "warning", message: F.ACL_LINE },
        { severity: "warning", message: F.recentLine(60_000, "Warning", "Slow origin response for https://www.example.test/b.js") },
      ]),
    });
    await expect(page.getByTestId("alert-admin-exposed")).toBeVisible();
    await expect(page.getByTestId("alert-recent-warnings")).toContainText(
      "1 warning in the last 15 minutes, of 1 kind.",
    );
    await expect(page.getByTestId("health-summary")).toHaveText(
      "2 findings · 1 needs action · 1 warning in the last 15 min",
    );
  });

  test("an acknowledged warnings finding still has its count on the summary line", async ({ page }) => {
    const now = Date.now();
    await open(page, {
      message_history: F.messageGroups([
        { level: "warning", template: "Slow origin response for URL", count: 30, recent: 15, last_ms: now - 60_000 },
      ]),
    });
    await expect(page.getByTestId("health-summary")).toHaveText("1 finding · 15 warnings in the last 15 min");
    await page.getByRole("button", { name: "Acknowledge Warnings in the log" }).click();
    await expect(page.getByTestId("health-summary")).toHaveText(
      "No findings · 15 warnings in the last 15 min · 1 acknowledged",
    );
  });

  test("no findings: one calm line with the counters' time base", async ({ page }) => {
    const vars = (F.STATS_GLOBAL.body as { variables: Record<string, number> }).variables;
    await open(page, {
      stats_json: () => withBody(F.STATS_GLOBAL, { variables: { ...vars, process_start_ms: Date.now() - 3 * 3_600_000 } }),
      "v1/daemon/stats": () => withBody(F.DAEMON_STATS, { started_at_ms: Date.now() - 2 * 3_600_000 }),
    });
    await expect(page.getByTestId("health-summary")).toHaveText("No findings");
    await expect(page.getByTestId("findings-since")).toHaveText("Counters: module since 3 h · optimizer since 2 h");
    await expect(page.getByTestId("alerts")).toHaveCount(0);
  });

  test("the optimizer's failure on the first load is remembered after it answers again", async ({ page }) => {
    const firstDown = (ok: Responder): Responder => (call, request) =>
      call === 0 ? F.UNREACHABLE : typeof ok === "function" ? ok(call, request) : ok;
    await open(page, {
      "v1/daemon/health": firstDown(F.HEALTH_OK),
      "v1/daemon/stats": firstDown(F.DAEMON_STATS),
    });
    await expect(page.getByTestId("alert-daemon-unreachable")).toBeVisible();
    const recovered = page.getByTestId("alert-daemon-recovered");
    await expect(recovered).toContainText("did not answer at", { timeout: 12_000 });
    await expect(page.getByTestId("alert-daemon-unreachable")).toHaveCount(0);
    await expect(page.getByTestId("health-summary")).toHaveText("1 finding");
  });

  test("a recently started optimizer is an info finding, not an action", async ({ page }) => {
    await open(page, { "v1/daemon/stats": () => withBody(F.DAEMON_STATS, { started_at_ms: Date.now() - 120_000 }) });
    const finding = page.getByTestId("alert-optimizer-restarted");
    await expect(finding).toContainText("The optimizer started 2 min ago");
    await expect(finding.locator(".level-badge-info")).toBeVisible();
    await expect(page.getByTestId("health-summary")).toHaveText("1 finding");
  });

  test("one log read per sample", async ({ page }) => {
    const { mock } = await open(page);
    await page.waitForTimeout(11_000);
    const n = mock.calls("stats_json");
    expect(n).toBeGreaterThanOrEqual(2);
    expect(Math.abs(mock.calls("message_history") - n)).toBeLessThanOrEqual(1);
  });

  test("phone: findings stack without sideways scrolling", async ({ page }) => {
    await page.setViewportSize(PHONE);
    await open(page, { message_history: F.messageGroups([aclRow(), volumeRow()]) });
    await expect(page.getByTestId("alert-volume-not-opened")).toBeVisible();
    await expect(page.getByRole("button", { name: "Acknowledge Optimizer cache not in use" })).toBeVisible();
    expect(
      await page.evaluate(() => document.scrollingElement!.scrollWidth <= document.scrollingElement!.clientWidth + 1),
    ).toBe(true);
  });
});
