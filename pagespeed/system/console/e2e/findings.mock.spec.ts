// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The overview's findings: what needs attention, worst first, each with the
// fix, a documentation link and an acknowledge control. Self-contained
// (support/mock-admin.ts); run after build.sh.

import { expect, test, type Page } from "@playwright/test";
import * as F from "./support/fixtures";
import {
  mockAdmin,
  openConsole,
  type Leaf,
  type MockOptions,
  type Reply,
  type Responder,
} from "./support/mock-admin";

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

async function open(page: Page, extra: Partial<Record<Leaf, Responder>> = {}, options: MockOptions = {}) {
  const mock = await mockAdmin(page, ADMIN, replies(extra), options);
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
    await expect(finding.getByTestId("finding-fix")).toHaveText(
      "Fix: Restrict the admin paths to trusted addresses at the web server, or acknowledge this if the console is " +
        "public on purpose.",
    );
    await expect(finding.getByTestId("snippet-code")).toContainText("Require local");
    await expect(finding.locator(".level-badge-warning")).toHaveText(/warning/i);
    await expect(finding.getByRole("link", { name: "Documentation" })).toHaveAttribute(
      "href",
      "https://modpagespeed.com/docs/admin-console/#access-control",
    );
    await expect(finding.getByRole("link", { name: "Documentation" })).toHaveAttribute("rel", "noopener noreferrer");
    await expect(finding.getByRole("link", { name: "Details" })).toHaveAttribute("href", "#/logs?source=module&level=warning");
    await expect(page.getByTestId("findings-summary")).toHaveText("1 finding · 1 needs action");
    expect(errors).toEqual([]);
  });

  test("the optimizer cache not in use is an error and ranks first", async ({ page }) => {
    await open(page, { message_history: F.messageGroups([aclRow(), volumeRow()]) });
    const findings = page.getByTestId("alerts").locator('[data-testid^="alert-"]');
    // The recent refusal is this finding's evidence, not a warning listed a second time.
    await expect(findings).toHaveCount(2);
    await expect(findings.first()).toHaveAttribute("data-testid", "alert-volume-not-opened");
    await expect(findings.first().locator(".level-badge-error")).toBeVisible();
    await expect(findings.first().getByTestId("finding-fix")).toHaveText(
      "Fix: Install module and optimizer packages from the same release, then restart the web server and the optimizer.",
    );
    // The sentence, the fix and the folded log line name one cause: the packages.
    await expect(findings.first()).not.toContainText("DaemonVolumePath");
    // So no path is offered to paste; the access finding below it has its snippet.
    await expect(findings.first().getByTestId("snippet")).toHaveCount(0);
    await expect(findings.nth(1).getByTestId("snippet")).toBeVisible();
    await expect(page.getByTestId("findings-summary")).toHaveText("2 findings · 2 need action");
  });

  test("one cause is one finding, and the log's own line is folded behind the written sentence", async ({ page }) => {
    const now = Date.now();
    await open(page, {
      message_history: F.messageGroups([
        { level: "warning", template: F.ACL_TEMPLATE, count: 6, recent: 4, last_ms: now - 30_000 },
        volumeRow(),
        { level: "warning", template: "Slow origin response for URL", count: 5, recent: 3, last_ms: now - 60_000 },
      ]),
    });
    const all = page.getByTestId("alerts");
    await expect(all.locator('[data-testid^="alert-"]')).toHaveCount(3);
    // Each cause once: the admin warning and the refusal are not repeated under "Warnings in the log".
    const warnings = page.getByTestId("alert-recent-warnings");
    await expect(warnings).toContainText("3 warnings in the last 15 minutes, of 1 kind, besides those with a finding of their own.");
    await expect(warnings.getByTestId("finding-details").locator("li")).toHaveText(["×3 Slow origin response for URL"]);
    await expect(page.getByTestId("findings-summary")).toHaveText("3 findings · 2 need action · 3 warnings in the last 15 min");
    const count = (text: string, needle: string) => text.split(needle).length - 1;
    const folded = await all.evaluate((el) => {
      const clone = el.cloneNode(true) as HTMLElement;
      clone.querySelectorAll("details").forEach((d) => d.remove());
      return { outside: clone.textContent ?? "", everything: el.textContent ?? "" };
    });
    expect(count(folded.everything, "admin handler")).toBe(0);
    expect(count(folded.everything, "in-place optimization is OFF")).toBe(1);
    // No log template in the open: a sentence states the cause.
    expect(folded.outside).not.toContain("in-place optimization is OFF");
    expect(folded.outside).not.toContain("Slow origin response");
    const cache = page.getByTestId("alert-volume-not-opened");
    await expect(cache.locator(".alert-message")).toHaveText(
      "The web server is not using the optimizer's cache: the module logged that the module and optimizer " +
        "packages use different cache layouts, so it opened nothing.",
    );
    // The line itself is one activation away, by pointer or keyboard.
    const line = cache.getByTestId("finding-details").locator("li");
    await expect(line).toBeHidden();
    await cache.locator("summary").focus();
    await page.keyboard.press("Enter");
    await expect(line).toBeVisible();
    await expect(line).toHaveText(F.VOLUME_TEMPLATE);
  });

  test("an old refusal or the start-up race in the log is not the cache finding", async ({ page }) => {
    await open(page, {
      message_history: F.messageGroups([
        { level: "error", template: F.VOLUME_TEMPLATE, count: 1, recent: 0, last_ms: Date.now() - 3_600_000 },
        {
          level: "warning",
          template:
            "nothing will be recorded for in-place optimization: cannot open the optimizer worker's cache volume at " +
            "/var/cache/pagespeed-optimizer/vN: permission denied (attempt N of N)",
          count: 1,
          recent: 1,
          last_ms: Date.now() - 30_000,
        },
      ]),
    });
    await expect(page.getByTestId("alert-recent-warnings")).toBeVisible();
    await expect(page.getByTestId("alert-volume-not-opened")).toHaveCount(0);
    await expect(page.getByTestId("findings-summary")).toHaveText("1 finding · 1 warning in the last 15 min");
  });

  test("a log that cannot be read is never \"No findings\"", async ({ page }) => {
    await open(page, { message_history: { status: 500, body: { success: false, error: "internal" } } });
    await expect(page.getByTestId("findings-summary")).toHaveText("Findings unavailable: the message log could not be read");
    // The strip's colour is the verdict's; the findings clause says in words that it has no findings to show.
    await expect(page.getByTestId("health-summary")).toContainText(/^Working\. .*Findings unavailable: the message log could not be read\.$/);
    await expect(page.getByTestId("findings-mark")).toHaveCount(0);
    await expect(page.getByTestId("alerts")).toHaveCount(0);
  });

  test("acknowledge moves a finding to the acknowledged group, and un-acknowledge brings it back", async ({ page }) => {
    await open(page, { message_history: F.messageGroups([aclRow()]) });
    await page.getByRole("button", { name: "Acknowledge Admin console reachable from the network" }).click();
    await expect(page.getByTestId("alert-admin-exposed")).toHaveCount(0);
    await expect(page.getByTestId("findings-summary")).toHaveText("No findings · 1 acknowledged");
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
    await expect(page.getByTestId("findings-summary")).toHaveText("1 finding · 14 warnings and errors in the last 15 min");
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
    await expect(page.getByTestId("findings-summary")).toHaveText(
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
    await expect(page.getByTestId("findings-summary")).toHaveText("1 finding · 15 warnings in the last 15 min");
    await page.getByRole("button", { name: "Acknowledge Warnings in the log" }).click();
    await expect(page.getByTestId("findings-summary")).toHaveText(
      "No findings · 15 warnings in the last 15 min · 1 acknowledged",
    );
  });

  test("no findings: one calm line with the counters' time base", async ({ page }) => {
    const vars = (F.STATS_GLOBAL.body as { variables: Record<string, number> }).variables;
    await open(page, {
      stats_json: () => withBody(F.STATS_GLOBAL, { variables: { ...vars, process_start_ms: Date.now() - 3 * 3_600_000 } }),
      "v1/daemon/stats": () => withBody(F.DAEMON_STATS, { started_at_ms: Date.now() - 2 * 3_600_000 }),
    });
    await expect(page.getByTestId("findings-summary")).toHaveText("No findings");
    // Each source's time base is stated once, in its tile, in the sentence's words.
    await expect(page.getByTestId("since-module")).toHaveText("since the web server started 3 h ago");
    await expect(page.getByTestId("since-optimizer")).toHaveText("since the optimizer started 2 h ago");
    await expect(page.getByTestId("since-module")).toHaveCount(1);
    await expect(page.getByTestId("since-optimizer")).toHaveCount(1);
    await expect(page.getByTestId("health-summary")).toContainText("since the optimizer started 2 h ago");
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
    await expect(page.getByTestId("findings-summary")).toHaveText("1 finding");
  });

  test("a recently started optimizer is an info finding, not an action", async ({ page }) => {
    await open(page, { "v1/daemon/stats": () => withBody(F.DAEMON_STATS, { started_at_ms: Date.now() - 120_000 }) });
    const finding = page.getByTestId("alert-optimizer-restarted");
    await expect(finding).toContainText("The optimizer started 2 min ago");
    await expect(finding.locator(".level-badge-info")).toBeVisible();
    await expect(page.getByTestId("findings-summary")).toHaveText("1 finding");
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

// A fix that names a setting comes with the setting to paste, in the web
// server's own syntax.
test.describe("Overview findings: a setting to paste", () => {
  const unreachable = { "v1/daemon/health": F.UNREACHABLE, "v1/daemon/stats": F.UNREACHABLE };
  const both = () => ({ ...unreachable, message_history: F.messageGroups([aclRow()]) });
  const APACHE_SOCKET = "ModPagespeedDaemonApiSocketPath /run/pagespeed-optimizer/api.sock";
  const NGINX_SOCKET = "pagespeed DaemonApiSocketPath /run/pagespeed-optimizer/api.sock;";

  /** The clipboard a secure page has; what it was handed is read back with copied(). */
  async function grantClipboard(page: Page) {
    await page.addInitScript(() => {
      Object.defineProperty(navigator, "clipboard", {
        configurable: true,
        value: {
          writeText: async (text: string) => {
            (window as unknown as { __copied: string }).__copied = text;
          },
        },
      });
    });
  }
  const copied = (page: Page) => page.evaluate(() => (window as unknown as { __copied?: string }).__copied);
  const selected = (page: Page, name: string) =>
    page.getByRole("tab", { name, exact: true }).and(page.locator('[aria-selected="true"]'));

  test("no Server header and nothing remembered: the first tab, one tab per server that has the setting", async ({ page }) => {
    const { errors } = await open(page, both());
    const socket = page.getByTestId("alert-daemon-unreachable");
    await expect(socket.getByRole("tab")).toHaveText(["Apache", "nginx"]);
    await expect(socket.getByRole("tab", { name: "Apache" })).toHaveAttribute("aria-selected", "true");
    await expect(socket.getByTestId("snippet-code")).toHaveText(APACHE_SOCKET);
    const access = page.getByTestId("alert-admin-exposed");
    await expect(access.getByRole("tab")).toHaveText(["Apache", "nginx", "IIS", "Envoy"]);
    await expect(access.getByTestId("snippet-code")).toHaveText(
      "<Location /pagespeed_admin>\n    Require local\n</Location>\n" +
        "<Location /pagespeed_global_admin>\n    Require local\n</Location>",
    );
    await expect(page.getByTestId("snippet-detected")).toHaveCount(0);
    // The panel is named by its selected tab.
    await expect(socket.getByRole("tabpanel", { name: "Apache" })).toBeVisible();
    // Findings without a setting in their fix have nothing to paste.
    await expect(page.getByTestId("snippet")).toHaveCount(2);
    expect(errors).toEqual([]);
  });

  test("a tab chosen in one finding moves every snippet, by mouse and by keyboard", async ({ page }) => {
    await open(page, both());
    const socket = page.getByTestId("alert-daemon-unreachable");
    const access = page.getByTestId("alert-admin-exposed");
    await socket.getByRole("tab", { name: "nginx" }).click();
    await expect(socket.getByTestId("snippet-code")).toHaveText(NGINX_SOCKET);
    await expect(access.getByRole("tab", { name: "nginx" })).toHaveAttribute("aria-selected", "true");
    await expect(access.getByTestId("snippet-code")).toContainText(
      "location = /pagespeed_admin { allow 127.0.0.1; allow ::1; deny all; }",
    );
    // Arrow keys move and select, and focus follows; only the selected tab is a tab stop.
    await access.getByRole("tab", { name: "nginx" }).focus();
    await page.keyboard.press("ArrowRight");
    await expect(access.getByRole("tab", { name: "IIS" })).toBeFocused();
    await expect(access.getByTestId("snippet-code")).toHaveText("pagespeed InfoUrlsLocalOnly on");
    await expect(access.locator('[role="tab"][tabindex="0"]')).toHaveText("IIS");
    await page.keyboard.press("ArrowRight");
    await expect(access.getByTestId("snippet-code")).toHaveText(
      'admin_auth:\n  enabled: true\n  allowed_ips:\n    - "127.0.0.1/32"',
    );
    // A finding whose setting that server does not have falls back to its first tab.
    await expect(socket.getByRole("tab", { name: "Apache" })).toHaveAttribute("aria-selected", "true");
    await expect(socket.getByTestId("snippet-code")).toHaveText(APACHE_SOCKET);
  });

  test("the Server header preselects its server and says so", async ({ page }) => {
    await open(page, both(), { server: "nginx/1.26.2" });
    for (const id of ["alert-daemon-unreachable", "alert-admin-exposed"]) {
      const finding = page.getByTestId(id);
      await expect(finding.getByRole("tab", { name: "nginx" })).toHaveAttribute("aria-selected", "true");
      await expect(finding.getByTestId("snippet-detected")).toHaveText("Detected: nginx");
    }
    await expect(page.getByTestId("alert-daemon-unreachable").getByTestId("snippet-code")).toHaveText(NGINX_SOCKET);
    // The header is a hint: another tab can still be chosen, and the hint stays in view.
    await page.getByTestId("alert-admin-exposed").getByRole("tab", { name: "Apache" }).click();
    await expect(page.getByTestId("alert-daemon-unreachable").getByTestId("snippet-code")).toHaveText(APACHE_SOCKET);
    await expect(page.getByTestId("alert-daemon-unreachable").getByTestId("snippet-detected")).toHaveText("Detected: nginx");
  });

  test("a server the finding has no snippet for is not offered as detected", async ({ page }) => {
    await open(page, both(), { server: "Microsoft-IIS/10.0" });
    await expect(selected(page, "IIS")).toHaveCount(1);
    await expect(page.getByTestId("alert-admin-exposed").getByTestId("snippet-detected")).toHaveText("Detected: IIS");
    const socket = page.getByTestId("alert-daemon-unreachable");
    await expect(socket.getByRole("tab", { name: "Apache" })).toHaveAttribute("aria-selected", "true");
    await expect(socket.getByTestId("snippet-detected")).toHaveCount(0);
  });

  test("the chosen tab is remembered across a reload; a server named by the header comes first", async ({ page }) => {
    await open(page, both());
    await page.getByTestId("alert-admin-exposed").getByRole("tab", { name: "IIS" }).click();
    expect(await page.evaluate(() => localStorage.getItem("pagespeed.snippet.dialect"))).toBe("iis");
    await page.reload();
    await expect(page.getByTestId("alert-admin-exposed").getByRole("tab", { name: "IIS" })).toHaveAttribute(
      "aria-selected",
      "true",
    );
    await expect(page.getByTestId("snippet-detected")).toHaveCount(0);
  });

  test("a remembered tab yields to the server the header names", async ({ page }) => {
    await page.addInitScript(() => localStorage.setItem("pagespeed.snippet.dialect", "iis"));
    await open(page, both(), { server: "Apache/2.4.62 (Debian)" });
    await expect(page.getByTestId("alert-admin-exposed").getByRole("tab", { name: "Apache" })).toHaveAttribute(
      "aria-selected",
      "true",
    );
  });

  test("storage that refuses: the tabs still work for this page view", async ({ page }) => {
    await page.addInitScript(() => {
      const refuse = () => {
        throw new DOMException("blocked", "SecurityError");
      };
      Storage.prototype.getItem = refuse;
      Storage.prototype.setItem = refuse;
    });
    const { errors } = await open(page, unreachable);
    const socket = page.getByTestId("alert-daemon-unreachable");
    await expect(socket.getByTestId("snippet-code")).toHaveText(APACHE_SOCKET);
    await socket.getByRole("tab", { name: "nginx" }).click();
    await expect(socket.getByTestId("snippet-code")).toHaveText(NGINX_SOCKET);
    expect(errors).toEqual([]);
  });

  test("Copy hands the selected snippet to the clipboard and says Copied in a status region", async ({ page }) => {
    await grantClipboard(page);
    const { errors } = await open(page, both());
    const access = page.getByTestId("alert-admin-exposed");
    const status = access.getByTestId("snippet-status");
    await expect(status).toHaveAttribute("role", "status");
    await expect(status).toHaveText("");
    await access.getByRole("tab", { name: "nginx" }).click();
    await access.getByRole("button", { name: "Copy" }).click();
    await expect(status).toHaveText("Copied");
    // Exactly the lines shown, line breaks included.
    expect(await copied(page)).toBe(
      "location = /pagespeed_admin { allow 127.0.0.1; allow ::1; deny all; }\n" +
        "location ^~ /pagespeed_admin/ { allow 127.0.0.1; allow ::1; deny all; }\n" +
        "location = /pagespeed_global_admin { allow 127.0.0.1; allow ::1; deny all; }\n" +
        "location ^~ /pagespeed_global_admin/ { allow 127.0.0.1; allow ::1; deny all; }",
    );
    expect(await copied(page)).toBe(await access.getByTestId("snippet-code").textContent());
    // The other finding did not copy; choosing another tab clears the word.
    await expect(page.getByTestId("alert-daemon-unreachable").getByTestId("snippet-status")).toHaveText("");
    await access.getByRole("tab", { name: "Apache" }).click();
    await expect(status).toHaveText("");
    expect(errors).toEqual([]);
  });

  test("a console over plain HTTP has no clipboard: Copy still copies through the selection", async ({ page }) => {
    await page.addInitScript(() => {
      Object.defineProperty(navigator, "clipboard", { configurable: true, value: undefined });
      document.execCommand = ((command: string) => {
        (window as unknown as { __copied: string }).__copied = `${command}:${String(getSelection())}`;
        return true;
      }) as typeof document.execCommand;
    });
    const { errors } = await open(page, unreachable);
    const socket = page.getByTestId("alert-daemon-unreachable");
    await socket.getByTestId("snippet-copy").click();
    await expect(socket.getByTestId("snippet-status")).toHaveText("Copied");
    expect(await copied(page)).toBe(`copy:${APACHE_SOCKET}`);
    expect(await page.evaluate(() => String(getSelection()))).toBe("");
    expect(errors).toEqual([]);
  });

  test("the browser refuses to copy: the snippet is selected, and the status says to copy it by hand", async ({ page }) => {
    await page.addInitScript(() => {
      Object.defineProperty(navigator, "clipboard", {
        configurable: true,
        value: { writeText: async () => Promise.reject(new Error("denied")) },
      });
      document.execCommand = (() => false) as typeof document.execCommand;
    });
    const { errors } = await open(page, unreachable);
    const socket = page.getByTestId("alert-daemon-unreachable");
    await socket.getByTestId("snippet-copy").click();
    await expect(socket.getByTestId("snippet-status")).toHaveText("Selected: copy it with the keyboard");
    expect(await page.evaluate(() => String(getSelection()))).toBe(APACHE_SOCKET);
    expect(errors).toEqual([]);
  });

  test("the snippet and its tab survive a refresh of the findings", async ({ page }) => {
    await open(page, unreachable);
    const socket = page.getByTestId("alert-daemon-unreachable");
    await socket.getByRole("tab", { name: "nginx" }).click();
    await socket.getByRole("tab", { name: "nginx" }).focus();
    await page.waitForTimeout(6_000);
    await expect(socket.getByRole("tab", { name: "nginx" })).toBeFocused();
    await expect(socket.getByTestId("snippet-code")).toHaveText(NGINX_SOCKET);
  });

  test("phone: a snippet wraps inside its finding, with no sideways scrolling", async ({ page }) => {
    await page.setViewportSize(PHONE);
    await open(page, both(), { server: "nginx" });
    const code = page.getByTestId("alert-admin-exposed").getByTestId("snippet-code");
    await expect(code).toBeVisible();
    expect(await code.evaluate((el) => el.scrollWidth <= el.clientWidth + 1)).toBe(true);
    expect(
      await page.evaluate(() => document.scrollingElement!.scrollWidth <= document.scrollingElement!.clientWidth + 1),
    ).toBe(true);
  });
});

